// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
#include "dxbc_patch.h"
#include "test_scene.h"
#include <dxgi1_6.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <limits>
#include <filesystem>
#include <fstream>

using namespace vrx::dx12::fixture;
namespace patch=vrx::dx12::dxbc;
void ComputeHashRetail(const BYTE*,UINT,BYTE*);
static constexpr char shader[]=R"(
cbuffer Calibration : register(b0) { float clipW; float3 color; };
struct V { float4 position:SV_Position; float2 uv:TEXCOORD; };
V VS(uint id:SV_VertexID) {
    float2 p[3]={float2(-0.35,-0.5),float2(0,0.5),float2(0.35,-0.5)};
    V o; o.position=float4(p[id]*clipW,0.5*clipW,clipW); o.uv=float2(0.25,0.75);
    if(id==1) { o.position.z=0.6*clipW; return o; }
    return o;
}
float4 PS(V input):SV_Target { return float4(color*float3(input.uv.x*4,1,input.uv.y),1); }
)";
static ComPtr<ID3DBlob> Compile(const char* source,const char* entry,const char* profile,UINT flags=0) {
    ComPtr<ID3DBlob> blob,error;
    const auto hr=D3DCompile(source,std::strlen(source),nullptr,nullptr,nullptr,entry,profile,flags|D3DCOMPILE_WARNINGS_ARE_ERRORS,0,&blob,&error);
    if(FAILED(hr)) throw std::runtime_error(error?static_cast<char*>(error->GetBufferPointer()):"compile failed");
    return blob;
}
static std::vector<uint8_t> Bytes(ID3DBlob* blob) {
    auto* p=static_cast<const uint8_t*>(blob->GetBufferPointer()); return {p,p+blob->GetBufferSize()};
}
static void Sign(std::vector<uint8_t>& bytes) { ComputeHashRetail(bytes.data()+20,UINT(bytes.size()-20),bytes.data()+4); }
static uint32_t U(const uint8_t* p) { uint32_t word; std::memcpy(&word,p,4); return word; }
static void Store(std::vector<uint8_t>& bytes,size_t at,uint32_t word) { std::memcpy(bytes.data()+at,&word,4); }
static size_t Program(const std::vector<uint8_t>& bytes) {
    for(uint32_t i=0;i<U(bytes.data()+28);++i) {
        const auto at=U(bytes.data()+32+4*i),type=U(bytes.data()+at);
        if(type==0x58454853 || type==0x52444853) return at;
    }
    throw std::runtime_error("fixture program absent");
}
static void Signatures(const std::vector<uint8_t>& a,const std::vector<uint8_t>& b) {
    // Compare parts separately. The combined query also failed on the original VS.
    for(auto part:{D3D_BLOB_INPUT_SIGNATURE_BLOB,D3D_BLOB_OUTPUT_SIGNATURE_BLOB}) {
    ComPtr<ID3DBlob> original,changed;
    Hr(D3DGetBlobPart(a.data(),a.size(),part,0,&original));
    Hr(D3DGetBlobPart(b.data(),b.size(),part,0,&changed));
    Check(original->GetBufferSize()==changed->GetBufferSize() &&
        !std::memcmp(original->GetBufferPointer(),changed->GetBufferPointer(),original->GetBufferSize()),"shader signatures are preserved byte-for-byte");
    }
}
static void Validate(const std::vector<uint8_t>& bytes) {
    Check(patch::ValidChecksum(bytes.data(),bytes.size()),"rewritten checksum is valid");
    ComPtr<ID3DBlob> disassembly;
    const auto hr=D3DDisassemble(bytes.data(),bytes.size(),0,nullptr,&disassembly);
    if(FAILED(hr) && disassembly) std::fprintf(stderr,"Disassembly: %s\n",static_cast<char*>(disassembly->GetBufferPointer()));
    if(FAILED(hr)) {
        wchar_t exe[32768]{}; GetModuleFileNameW(nullptr,exe,32768);
        auto file=std::filesystem::path(exe).parent_path()/L"failed-rewrite.dxbc";
        std::ofstream output(file,std::ios::binary); output.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
    }
    Hr(hr);
}
struct Draw : Scene {
    ComPtr<ID3DBlob> pixel;
    void Init(const char* profile,IUnknown* adapter) { Scene::Init(adapter); pixel=Compile(shader,"PS",profile); }
    ComPtr<ID3D12PipelineState> Pipeline(const std::vector<uint8_t>& vertex) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{}; p.pRootSignature=root.Get(); p.VS={vertex.data(),vertex.size()};
        p.PS={pixel->GetBufferPointer(),pixel->GetBufferSize()}; p.SampleMask=UINT_MAX;
        p.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE; p.RasterizerState.DepthClipEnable=TRUE;
        p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; p.NumRenderTargets=1; p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM; p.SampleDesc.Count=1;
        ComPtr<ID3D12PipelineState> result; const auto hr=device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&result));
        if(FAILED(hr)) DebugCheck(); Hr(hr); return result;
    }
    std::vector<uint8_t> Frame(ID3D12PipelineState* left,ID3D12PipelineState* right,float w) {
        Wait(); Hr(allocator->Reset()); Hr(commands->Reset(allocator.Get(),nullptr)); commands->SetGraphicsRootSignature(root.Get());
        D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1}; D3D12_RECT rect{0,0,LONG(width),LONG(height)};
        commands->RSSetViewports(1,&viewport); commands->RSSetScissorRects(1,&rect); commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for(UINT eye=0;eye<2;++eye) {
            auto target=rtv->GetCPUDescriptorHandleForHeapStart(); target.ptr+=SIZE_T(eye)*increment;
            const float black[]={0,0,0,1}; commands->ClearRenderTargetView(target,black,0,nullptr); commands->OMSetRenderTargets(1,&target,FALSE,nullptr);
            commands->SetPipelineState(eye?right:left); const float values[]={w,.25f,.5f,.75f};
            commands->SetGraphicsRoot32BitConstants(0,4,values,0); commands->DrawInstanced(3,1,0,0);
        }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint[2]{}; const auto desc=eyes->GetDesc(); UINT64 bytes=0;
        device->GetCopyableFootprints(&desc,0,2,0,footprint,nullptr,nullptr,&bytes);
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width=bytes;
        buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1; buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback; Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
        D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={eyes.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE}; commands->ResourceBarrier(1,&b);
        for(UINT eye=0;eye<2;++eye) {
            D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.pResource=eyes.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex=eye;
            dst.pResource=readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=footprint[eye];
            commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        }
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter); commands->ResourceBarrier(1,&b); Hr(commands->Close());
        ID3D12CommandList* lists[]={commands.Get()}; queue->ExecuteCommandLists(1,lists); Wait();
        const uint8_t* mapped=nullptr; Hr(readback->Map(0,nullptr,reinterpret_cast<void**>(const_cast<uint8_t**>(&mapped))));
        std::vector<uint8_t> pixels(size_t(width)*height*8);
        for(UINT eye=0;eye<2;++eye) for(UINT y=0;y<height;++y)
            std::memcpy(pixels.data()+(size_t(y)*width*2+eye*width)*4,mapped+footprint[eye].Offset+size_t(y)*footprint[eye].Footprint.RowPitch,width*4);
        readback->Unmap(0,nullptr); DebugCheck(); return pixels;
    }
    double Disparity(const std::vector<uint8_t>& pixels) {
        uint64_t sum[2]{},count[2]{};
        for(UINT eye=0;eye<2;++eye) for(UINT y=0;y<height;++y) for(UINT x=0;x<width;++x) {
            auto* p=pixels.data()+(size_t(y)*width*2+eye*width+x)*4; Check(p[3]==255,"alpha preserved");
            if(p[1]>100) {
                Check(std::abs(int(p[0])-64)<=1 && std::abs(int(p[1])-128)<=1 && std::abs(int(p[2])-143)<=1,"non-position varyings and color preserved within UNORM rounding");
                sum[eye]+=x; ++count[eye];
            }
        }
        Check(count[0]>100 && count[1]>100,"both eye triangles are populated");
        return double(sum[1])/count[1]-double(sum[0])/count[0];
    }
};
static void Boundaries(const std::vector<uint8_t>& input) {
    Check(patch::ValidChecksum(input.data(),input.size()),"DXC hash matches original Microsoft compiler output");
    const auto before=input;
    Check(patch::Rewrite(nullptr,0,{.4f,0,true}).status==patch::Status::Invalid,"null input rejected");
    Check(patch::Rewrite(input.data(),input.size(),{std::numeric_limits<float>::infinity(),0,true}).status==patch::Status::Invalid,"infinite calibration rejected");
    for(size_t size=0;size<32;++size) Check(patch::Rewrite(input.data(),size,{}).status==patch::Status::Invalid,"truncated header rejected");
    auto bad=input; bad[4]^=1; Check(patch::Rewrite(bad.data(),bad.size(),{}).status==patch::Status::Invalid,"wrong checksum rejected");
    bad=input; Store(bad,32,UINT32_MAX); Sign(bad); Check(patch::Rewrite(bad.data(),bad.size(),{}).status==patch::Status::Invalid,"out-of-bounds chunk rejected");
    bad=input; Store(bad,28,65); Sign(bad); Check(patch::Rewrite(bad.data(),bad.size(),{}).status==patch::Status::Invalid,"chunk count is bounded");
    bad=input; const auto program=Program(bad); Store(bad,program,0x4c495844); Sign(bad);
    Check(patch::Rewrite(bad.data(),bad.size(),{}).status==patch::Status::Unsupported,"DXIL is not reported as rewritten");
    bad=input; Store(bad,program+12,UINT32_MAX); Sign(bad);
    Check(patch::Rewrite(bad.data(),bad.size(),{}).status==patch::Status::Invalid,"incorrect program length rejected");
    Check(before==input,"input bytecode is never modified");
    auto literal=Bytes(Compile("float4 VS(uint id:SV_VertexID):SV_Position{return float4(id^0x001020f2,0,0.5,2);}","VS","vs_5_1").Get());
    uint32_t marker[]={0x00004001,0x001020f2}; const auto* first=reinterpret_cast<const uint8_t*>(marker);
    Check(std::search(literal.begin(),literal.end(),first,first+sizeof(marker))!=literal.end(),"fixture contains an output-looking immediate literal");
    auto changed=patch::Rewrite(literal.data(),literal.size(),{.4f,0,true});
    Check(changed.status==patch::Status::Patched && std::search(changed.bytes.begin(),changed.bytes.end(),first,first+sizeof(marker))!=changed.bytes.end(),"immediate literal is not mistaken for a register");
    Validate(changed.bytes);
    auto constant=Bytes(Compile("float4 VS():SV_Position{return float4(0,0,1,2);}","VS","vs_5_1").Get());
    ComPtr<ID3DBlob> assembly; Hr(D3DDisassemble(constant.data(),constant.size(),0,nullptr,&assembly));
    Check(std::string(static_cast<char*>(assembly->GetBufferPointer())).find("dcl_temps ")==std::string::npos,"constant fixture has no temporary declaration");
    auto declared=patch::Rewrite(constant.data(),constant.size(),{.4f,0,true});
    Check(declared.status==patch::Status::Patched,"spare temporary declaration is introduced when absent"); Validate(declared.bytes);
}
int main(int argc,char** argv) {
    try {
        ComPtr<ID3D12Debug> debug; if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        Check(argc==1 || (argc==2 && std::strcmp(argv[1],"--warp")==0),"expected optional --warp");
        ComPtr<IDXGIAdapter> adapter;
        if(argc==2) { ComPtr<IDXGIFactory4> factory; Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory))); Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))); }
        for(bool modern:{false,true}) {
            Draw draw; draw.Init(modern?"ps_5_1":"ps_5_0",adapter.Get());
            const auto original=Bytes(Compile(shader,"VS",modern?"vs_5_1":"vs_5_0",D3DCOMPILE_SKIP_OPTIMIZATION).Get()); Boundaries(original);
            auto make=[&](float shift,float convergence,bool hud=true) {
                auto result=patch::Rewrite(original.data(),original.size(),{shift,convergence,hud});
                Check(result.status==patch::Status::Patched,result.reason.c_str());
                Validate(result.bytes); Signatures(original,result.bytes); return draw.Pipeline(result.bytes);
            };
            auto unchanged=draw.Pipeline(original),left=make(-.4f,0),right=make(.4f,0);
            const auto nearDisparity=draw.Disparity(draw.Frame(left.Get(),right.Get(),2));
            const auto farDisparity=draw.Disparity(draw.Frame(left.Get(),right.Get(),4));
            Check(nearDisparity>15 && farDisparity>7 && nearDisparity/farDisparity>1.7 && nearDisparity/farDisparity<2.3,"parallax falls with depth instead of shifting a flat image");
            auto hud=draw.Frame(left.Get(),right.Get(),1); Check(hud==draw.Frame(unchanged.Get(),unchanged.Get(),1),"W=1 HUD is pixel-identical to unmodified shader");
            auto zero=make(0,0); Check(draw.Frame(zero.Get(),zero.Get(),4)==draw.Frame(unchanged.Get(),unchanged.Get(),4),"zero calibration is pixel-identical to original geometry");
            auto anchorLeft=make(-.4f,.1f),anchorRight=make(.4f,.1f);
            Check(std::abs(draw.Disparity(draw.Frame(anchorLeft.Get(),anchorRight.Get(),4)))<.1,"convergence aligns the chosen depth in both eyes");
            auto orthoLeft=make(-.4f,0,false),orthoRight=make(.4f,0,false);
            // Ortho +/-0.4 at w=1 is clamped to +/-0.25 NDC; near (w=2) is +/-0.2 NDC.
            const auto orthoDisparity=draw.Disparity(draw.Frame(orthoLeft.Get(),orthoRight.Get(),1));
            Check(orthoDisparity>nearDisparity*1.15 && orthoDisparity<nearDisparity*1.35,"orthographic geometry can opt out of the HUD heuristic; its screen shift is clamped");
            auto pixel=Bytes(draw.pixel.Get()); Check(patch::Rewrite(pixel.data(),pixel.size(),{}).status==patch::Status::Unsupported,"pixel shaders are left alone");
            std::printf("PASS: DXBC %s %s stereo, near/far disparity %.2f/%.2f; convergence, HUD, signatures, malformed input, literal preservation\n",modern?"5.1":"5.0",adapter?"WARP":"GPU",nearDisparity,farDisparity);
        }
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
