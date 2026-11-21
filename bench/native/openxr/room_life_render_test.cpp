#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "room.h"
#include "room_life_shader.h"
#include "floating_panel_shader.h"
#include <fstream>
#include <chrono>
#include <vector>
using Microsoft::WRL::ComPtr;
static void Need(HRESULT hr) { if(FAILED(hr)) { printf("FAIL 0x%08X\n",(unsigned)hr); exit(1); } }

static bool TestFloatingPanels(ID3D11Device* device, ID3D11DeviceContext* context)
{
    std::string source = R"HLSL(
Texture2D<float4> panelAtlas : register(t0);
SamplerState samp : register(s0);
RWTexture2D<float4> output : register(u0);
static const uint vrexPanelCount=1;
static const uint smoothPanels=1;
static const float4 vrexPanels[3]={float4(0,0,-2,0),float4(1,0,0,1),float4(0,1,0,1)};
float3 Dec(float3 c) { return c; }
float3 Enc(float3 c) { return c; }
)HLSL";
    source += kFloatingPanelHlsl;
    source += R"HLSL(
[numthreads(4,1,1)] void main(uint3 id : SV_DispatchThreadID) {
    const float3 colour=float3(0,1,0), dx=float3(.01,0,0), dy=float3(0,.01,0);
    float3 ray=id.x==3 ? normalize(float3(.5,0,-1)) : float3(0,0,-1);
    float depth=id.x==0 ? 1 : id.x==1 ? 3 : 0;
    output[id.xy]=float4(ComposePanels(colour,0,ray,dx,dy,depth),1);
})HLSL";
    ComPtr<ID3DBlob> code, errors;
    HRESULT compiled = D3DCompile(source.data(),source.size(),"floating-panels",nullptr,nullptr,"main","cs_5_0",0,0,&code,&errors);
    if (errors) puts((const char*)errors->GetBufferPointer());
    Need(compiled);
    ComPtr<ID3D11ComputeShader> shader;
    Need(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = texture.Height = texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1;
    texture.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const float red[4]{1,0,0,1};
    D3D11_SUBRESOURCE_DATA texels{red,sizeof(red),0};
    ComPtr<ID3D11Texture2D> atlas, result, cpu;
    Need(device->CreateTexture2D(&texture,&texels,&atlas));
    ComPtr<ID3D11ShaderResourceView> atlasView;
    Need(device->CreateShaderResourceView(atlas.Get(),nullptr,&atlasView));
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ComPtr<ID3D11SamplerState> sampling;
    Need(device->CreateSamplerState(&sampler,&sampling));
    texture.Width = 4;
    texture.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    Need(device->CreateTexture2D(&texture,nullptr,&result));
    ComPtr<ID3D11UnorderedAccessView> output;
    Need(device->CreateUnorderedAccessView(result.Get(),nullptr,&output));
    texture.BindFlags = 0;
    texture.Usage = D3D11_USAGE_STAGING;
    texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Need(device->CreateTexture2D(&texture,nullptr,&cpu));

    auto* resource = atlasView.Get();
    auto* sample = sampling.Get();
    auto* write = output.Get();
    context->CSSetShader(shader.Get(),nullptr,0);
    context->CSSetShaderResources(0,1,&resource);
    context->CSSetSamplers(0,1,&sample);
    context->CSSetUnorderedAccessViews(0,1,&write,nullptr);
    context->Dispatch(1,1,1);
    context->CopyResource(cpu.Get(),result.Get());
    D3D11_MAPPED_SUBRESOURCE read{};
    Need(context->Map(cpu.Get(),0,D3D11_MAP_READ,0,&read));
    const auto* pixels = static_cast<const float*>(read.pData);
    const bool passed = pixels[0] < .01f && pixels[1] > .99f &&
        pixels[4] > .99f && pixels[5] < .01f &&
        pixels[8] > .99f && pixels[9] < .01f &&
        pixels[12] > .45f && pixels[12] < .55f && pixels[13] > .45f && pixels[13] < .55f;
    context->Unmap(cpu.Get(),0);
    context->ClearState();
    puts(passed ? "PASS: companion in front, panel in front, and antialiased coverage at the panel edge"
                : "FAIL: panel depth order or edge coverage");
    return passed;
}

// A deliberately small GPU gallery, using the production SDFs, tracing, lighting
// and constant layout. No headset, depth models or source capture required.
int main(int argc, char** argv)
{
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    Need(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,nullptr,&ctx));
    if (!TestFloatingPanels(dev.Get(),ctx.Get())) return 1;
    if (argc == 2 && std::string(argv[1]) == "--panels") return 0;
    std::string src=R"(
#define ROOM_STANDS 0
#define ROOM_FACE_FLOOR 3
#define ROOM_FACE_CEILING 4
#define ROOM_FACE_LEFT 1
#define ROOM_FACE_RIGHT 2
#define ROOM_FACES 6
#define ROOM_KIND_FOOTPRINT 7
#define ROOM_KIND_OUTSIDE 8
cbuffer RoomC : register(b0) {
float4 ignored[36];
float lifeTime,audioBass,audioMid,audioHigh;
float lifeBeat,audioLevel,lifeGain,particleDensity;
uint lifePreset,lifeSpecies,lifeCount,lifeFlags;
float4 lifeSpectrum[4]; float4 lifeObjects[100]; float4 lifeWaveform[12];
float visualReactivity;uint visualLayers;float vrexChat,vrexState;
};
#define X ignored[0].x
#define yF ignored[0].y
#define yC ignored[0].z
#define zB ignored[0].w
float3 Enc(float3 c) { return pow(saturate(c),1/2.2); }
float3 Dec(float3 c) { return pow(max(c,0),2.2); }
bool InsideH(float3 o) { return true; }
static const float R=0,scrTanWrap=0,halfH=0;
bool RoomArcRoot(float a,float b,float c,float e,float3 o,float3 d,out float t,out float3 hit) {t=1000;hit=0;return false;}
float FrontDepthR(float x) { return -4; }
void FacePointH(uint face,float u,float v,out float3 p,out float3 n) { p=face==3?float3(u,yF,v):float3(u,v,-4); n=face==3?float3(0,1,0):float3(0,0,1); }
float3 BaseLookColour(int kind,float2 uv,float2 guv,uint e,float3 o,float3 d,float3 Dx,float3 Dy) { return float3(.065,.075,.10); }
)";
    src+=RoomLifeHlsl();
    src+=R"(
RWTexture2D<float4> output : register(u0);
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
if(id.x>=960 || id.y>=540) return;
float2 uv=(float2(id.xy)+.5)/float2(960,540)*2-1;
float3 o=float3(0,0,3),d=normalize(float3(uv.x*1.78,-uv.y,-1.5));
bool floorView=lifePreset==98;
if(floorView) { o=float3(0,1,-3);d=normalize(float3(uv.x*1.78,-.75,1.5+uv.y)); }
float t=floorView?(yF-o.y)/d.y:(-4-o.z)/d.z; float3 p=o+d*t;
float companionDepth;
float3 c=LifeCompose(float3(.065,.075,.10),lifePreset==99?0:floorView?4:6,floorView?p.xz:p.xy,0,0,o,d,float3(.0037,0,0),float3(0,.0037,0),companionDepth);
output[id.xy]=float4(c,1);
})";
    ComPtr<ID3DBlob> code,errors;
    HRESULT hr=D3DCompile(src.data(),src.size(),"room-life-gallery",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    if(errors) printf("%s\n",(char*)errors->GetBufferPointer()); Need(hr);
    ComPtr<ID3D11ComputeShader> shader; Need(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
    D3D11_TEXTURE2D_DESC td{};td.Width=960;td.Height=540;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> image,read; Need(dev->CreateTexture2D(&td,nullptr,&image));
    ComPtr<ID3D11UnorderedAccessView> uav; Need(dev->CreateUnorderedAccessView(image.Get(),nullptr,&uav));
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Need(dev->CreateTexture2D(&td,nullptr,&read));
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth=sizeof(RoomConstants);bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> buffer;Need(dev->CreateBuffer(&bd,nullptr,&buffer));
    RoomConstants rc;rc.X=5;rc.yF=-2;rc.yC=2;rc.zB=4;rc.lifeFlags=4;rc.lifeGain=1;rc.lifeTime=2;rc.audioBass=.3f;rc.visualReactivity=1;rc.visualLayers=1;
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP,0};
    ComPtr<ID3D11Query> qStart,qEnd,qRange;
    Need(dev->CreateQuery(&qd,&qStart));Need(dev->CreateQuery(&qd,&qEnd));
    qd.Query=D3D11_QUERY_TIMESTAMP_DISJOINT;Need(dev->CreateQuery(&qd,&qRange));
    std::vector<float> shadowReference(960*540*3);
    std::vector<float> avatarReference(960*540*3);
    for(int scene=0;scene<30;++scene)
    {
        rc.lifeCount=scene==0?8:0;rc.lifeFlags=scene==0?4:2;rc.lifePreset=scene?scene-1:0;rc.audioLevel=.5f;rc.visualLayers=1u<<rc.lifePreset;
        for(int b=0;b<48;++b) rc.lifeWaveform[b]=sinf(b*.4f)*.6f;
        for(int b=0;b<16;++b) rc.lifeSpectrum[b]=.2f+.35f*sinf((float)b)*sinf((float)b);
        for(int i=0;i<8;++i)
        {
            auto& o=rc.lifeObjects[i];o[0]=(i%4-1.5f)*1.25f;o[1]=i<4?.6f:-.6f;o[2]=0;o[3]=.53f;
            o[4]=.75f;o[6]=.66f;o[7]=(float)i;o[8]=.3f;o[9]=.6f;o[10]=1;o[11]=i*.6f;o[12]=2;
        }
        if(scene==13 || scene==14) { rc.lifeCount=1; rc.lifeFlags=4; rc.lifePreset=99; rc.lifeObjects[0][0]=0;rc.lifeObjects[0][1]=0;rc.lifeObjects[0][2]=scene==13?1.f:-1.f; }
        if(scene==15) {rc.lifeCount=0;rc.lifeFlags=8;rc.particleDensity=100;rc.lifePreset=0;}
        if(scene==16 || scene==17) {rc.lifeCount=1;rc.lifeFlags=scene==16?20:52;rc.lifePreset=98;rc.lifeObjects[0][0]=2.8f;rc.lifeObjects[0][1]=-.5f;rc.lifeObjects[0][2]=2.6f;}
        if(scene==18) {rc.lifeCount=1;rc.lifeFlags=128;rc.lifePreset=0;rc.lifeObjects[0][0]=0;rc.lifeObjects[0][1]=0;rc.lifeObjects[0][2]=0;rc.lifeObjects[0][3]=1.0f;rc.lifeObjects[0][4]=.25f;rc.lifeObjects[0][6]=.968f;rc.lifeObjects[0][7]=8;rc.vrexChat=0;}
        if(scene==19) {rc.lifeCount=0;rc.lifeFlags=2;rc.lifePreset=0;rc.visualLayers=(1<<1)|(1<<3)|(1<<5);}
        if(scene==20) {rc.lifeCount=1;rc.lifeFlags=128;rc.lifePreset=0;rc.lifeObjects[0][0]=0;rc.lifeObjects[0][1]=0;rc.lifeObjects[0][2]=2;rc.lifeObjects[0][3]=.14f;rc.lifeObjects[0][4]=0;rc.lifeObjects[0][6]=1;rc.lifeObjects[0][7]=8;rc.vrexChat=4;}
        if(scene>=21)
        {
            rc.lifeCount=1;rc.lifeFlags=128;rc.lifePreset=0;auto& pal=rc.lifeObjects[0];
            pal[0]=pal[1]=pal[4]=0;pal[2]=2;pal[3]=.14f;pal[6]=1;pal[7]=8;pal[12]=1.2f;
            rc.vrexChat=scene==21 || scene==23?1.f:7.f;
            rc.vrexState=(float)(5+(scene==21?16+32+256:scene==22?16+64+128+512:scene==23?256:scene==24?512:768));
        }
        if(scene>=26)
        {
            auto& pal=rc.lifeObjects[0];
            pal[2]=1.8f;pal[3]=.75f;pal[7]=scene<28?9.f:10.f;
            pal[12]=(scene%2==0)?.15f:1.75f;
            pal[14]=(scene%2==0)?-.06f:.18f;
            rc.vrexChat=0;
            rc.vrexState=(scene%2==0)?0.f:256.f;
        }
        auto start=std::chrono::steady_clock::now();
        ctx->UpdateSubresource(buffer.Get(),0,nullptr,&rc,0,0);auto* cb=buffer.Get();ctx->CSSetConstantBuffers(0,1,&cb);
        auto* view=uav.Get();ctx->CSSetUnorderedAccessViews(0,1,&view,nullptr);ctx->CSSetShader(shader.Get(),nullptr,0);
        ctx->Begin(qRange.Get());ctx->End(qStart.Get());ctx->Dispatch(120,68,1);ctx->End(qEnd.Get());ctx->End(qRange.Get());
        ctx->CopyResource(read.Get(),image.Get());D3D11_MAPPED_SUBRESOURCE map{};Need(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&map));
        std::ofstream f("bench/native/openxr/out/life-preview-"+std::to_string(scene)+".ppm",std::ios::binary);f<<"P6\n960 540\n255\n";
        int bright=0,shadowPixels=0,movingPixels=0;
        for(int y=0;y<540;++y) for(int x=0;x<960;++x)
        {
            const float* pixel=(const float*)((const char*)map.pData+y*map.RowPitch)+x*4;
            for(int c=0;c<3;++c) { if(!std::isfinite(pixel[c])) { puts("FAIL nonfinite pixel"); return 1; } char v=(char)(std::clamp(pixel[c],0.f,1.f)*255);f.write(&v,1); }
            for(int c=0;c<3;++c) { size_t index=((size_t)y*960+x)*3+c; if(scene==16) shadowReference[index]=pixel[c]; if(scene==17 && shadowReference[index]-pixel[c]>.01f) ++shadowPixels; }
            if(scene>=26) for(int c=0;c<3;++c)
            {
                size_t index=((size_t)y*960+x)*3+c;
                if(scene%2==0) avatarReference[index]=pixel[c];
                else if(std::abs(avatarReference[index]-pixel[c])>.08f) ++movingPixels;
            }
            if(std::max({pixel[0],pixel[1],pixel[2]})>.2) ++bright;
        }
        ctx->Unmap(read.Get(),0);
        printf("gallery %d: %.2f ms dispatch + synchronous readback + file write\n",scene,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
        UINT64 gpuStart=0,gpuEnd=0;D3D11_QUERY_DATA_TIMESTAMP_DISJOINT range{};
        Need(ctx->GetData(qStart.Get(),&gpuStart,sizeof(gpuStart),0));Need(ctx->GetData(qEnd.Get(),&gpuEnd,sizeof(gpuEnd),0));Need(ctx->GetData(qRange.Get(),&range,sizeof(range),0));
        if(!range.Disjoint) printf("  GPU %.3f ms at 960x540\n",1000.0*(gpuEnd-gpuStart)/range.Frequency);
        if(scene==17 && shadowPixels<10) { puts("FAIL traced shadows must darken the floor"); return 1; }
        if(scene==14 && bright!=0) { puts("FAIL screen must occlude creatures behind it"); return 1; }
        if(scene>=26 && scene%2 && movingPixels<1500) { puts("FAIL companion pose changes are not visible"); return 1; }
        if(scene!=14 && bright<100) { puts("FAIL effect did not draw"); return 1; }
    }
    puts("PASS: GPU creature gallery and all 12 visualizers render finite, visible pixels");
}
