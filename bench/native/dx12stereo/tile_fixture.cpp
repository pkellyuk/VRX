// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Real GPU sparse mappings: permuted pages, copied mappings and shared tiles.
#include "mirror_scene.h"
#include <array>
namespace {
constexpr UINT64 tile=D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
void Transition(Scene& scene,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};scene.commands->ResourceBarrier(1,&barrier);
}
void Alias(Scene& scene,ID3D12Resource* before,ID3D12Resource* after){
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING;barrier.Aliasing={before,after};scene.commands->ResourceBarrier(1,&barrier);
}
void Begin(Scene& scene){scene.Wait();Hr(scene.allocator->Reset());Hr(scene.commands->Reset(scene.allocator.Get(),nullptr));}
void Submit(Scene& scene){Hr(scene.commands->Close());ID3D12CommandList* lists[]={scene.commands.Get()};scene.queue->ExecuteCommandLists(1,lists);scene.Wait();scene.DebugCheck();}
void Verify(ID3D12Resource* resource,const std::vector<uint8_t>& expected){
    void* mapped=nullptr;Hr(resource->Map(0,nullptr,&mapped));const bool same=!std::memcmp(mapped,expected.data(),expected.size());resource->Unmap(0,nullptr);
    Check(same,"GPU page contents match the independently calculated mapping");
}
}
int main(int argc,char** argv){try {
    const bool warp=argc==3 && std::strcmp(argv[2],"--warp")==0;
    Check(argc==2 || warp,"known tile fixture adapter");
    const auto module=GetModuleHandleW(L"d3d12.dll");
    counts=reinterpret_cast<CountsFn>(GetProcAddress(module,"VRX_DX12_GetCounts"));twin=reinterpret_cast<TwinFn>(GetProcAddress(module,"VRX_DX12_GetTwin"));scope=reinterpret_cast<ScopeFn>(GetProcAddress(module,"VRX_DX12_DiagnosticScope"));
    Check(counts && twin && scope,"native tile bridge loaded");
    ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    {
        ComPtr<IDXGIAdapter> adapter;if(warp){ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));}
        Scene scene;scene.Init(adapter.Get());D3D12_FEATURE_DATA_D3D12_OPTIONS options{};Hr(scene.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,&options,sizeof(options)));
        Check(options.TiledResourcesTier!=D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED,"adapter supports sparse resources");
        D3D12_HEAP_DESC heap{};heap.SizeInBytes=4*tile;heap.Properties.Type=D3D12_HEAP_TYPE_DEFAULT;heap.Flags=D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        ComPtr<ID3D12Heap> memory;Hr(scene.device->CreateHeap(&heap,IID_PPV_ARGS(&memory)));
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=4*tile;desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> a,b,upload,readback;
        Hr(scene.device->CreateReservedResource(&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&a)));
        ComPtr<ID3D12Device4> modern;Hr(scene.device.As(&modern));ComPtr<ID3D12Resource1> newer;
        Hr(modern->CreateReservedResource1(&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,nullptr,IID_PPV_ARGS(&newer)));Hr(newer.As(&b));
        D3D12_HEAP_PROPERTIES cpu{};cpu.Type=D3D12_HEAP_TYPE_UPLOAD;Hr(scene.device->CreateCommittedResource(&cpu,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)));
        cpu.Type=D3D12_HEAP_TYPE_READBACK;Hr(scene.device->CreateCommittedResource(&cpu,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
        std::vector<uint8_t> expected(size_t(4*tile));for(size_t i=0;i<expected.size();++i)expected[i]=uint8_t((i*13+i/tile*71+17)%251);
        void* mapped=nullptr;Hr(upload->Map(0,nullptr,&mapped));std::memcpy(mapped,expected.data(),expected.size());upload->Unmap(0,nullptr);
        const D3D12_TILED_RESOURCE_COORDINATE starts[]={{0,0,0,0},{1,0,0,0},{2,0,0,0},{3,0,0,0}};const UINT offsets[]={2,0,3,1};
        const UINT oneTile[]={1,1,1,1};scene.queue->UpdateTileMappings(a.Get(),4,starts,nullptr,memory.Get(),4,nullptr,offsets,oneTile,D3D12_TILE_MAPPING_FLAG_NONE);
        D3D12_TILE_REGION_SIZE all{};all.NumTiles=4;
        scene.queue->CopyTileMappings(b.Get(),starts,a.Get(),starts,&all,D3D12_TILE_MAPPING_FLAG_NONE);
        Begin(scene);Alias(scene,nullptr,a.Get());
        scene.commands->CopyTiles(a.Get(),starts,&all,upload.Get(),0,D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
        Alias(scene,a.Get(),b.Get());Transition(scene,b.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        scene.commands->CopyTiles(b.Get(),starts,&all,readback.Get(),0,D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);Submit(scene);
        Verify(readback.Get(),expected);
        {Diagnostic diagnostic;ComPtr<ID3D12Resource> right;Hr(twin(readback.Get(),&right));Check(right.Get()!=readback.Get(),"right-eye readback is independent");Verify(right.Get(),expected);}
        // Share one physical tile twice, retain the third mapping with SKIP,
        // and remap the last to a different original heap page.
        const D3D12_TILE_RANGE_FLAGS flags[]={D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE,D3D12_TILE_RANGE_FLAG_SKIP,D3D12_TILE_RANGE_FLAG_NONE};
        const UINT remap[]={1,0,2},lengths[]={2,1,1};
        scene.queue->UpdateTileMappings(b.Get(),1,nullptr,nullptr,memory.Get(),3,flags,remap,lengths,D3D12_TILE_MAPPING_FLAG_NONE);
        Begin(scene);Alias(scene,nullptr,b.Get());scene.commands->CopyTiles(b.Get(),starts,&all,readback.Get(),0,D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);Submit(scene);
        const auto initialBytes=expected;const UINT pages[]={3,3,2,0};for(UINT i=0;i<4;++i)std::memcpy(expected.data()+size_t(i*tile),initialBytes.data()+size_t(pages[i]*tile),size_t(tile));
        Verify(readback.Get(),expected);{Diagnostic diagnostic;ComPtr<ID3D12Resource> right;Hr(twin(readback.Get(),&right));Verify(right.Get(),expected);}
        // Null mappings must accept a null heap and optional range arrays.
        const auto nullRange=D3D12_TILE_RANGE_FLAG_NULL;scene.queue->UpdateTileMappings(b.Get(),1,nullptr,nullptr,nullptr,1,&nullRange,nullptr,nullptr,D3D12_TILE_MAPPING_FLAG_NONE);scene.Wait();scene.DebugCheck();
    }
    vrx::dx12::Counts result;Hr(counts(&result,sizeof(result)));
    Check(result.mirrorReplays==2 && !result.mirrorStops && !result.hookFailures && !result.mirrorLiveObjects && result.modernResources>=1,"tile commands replay twice and all shadow identities retire");
    std::puts("PASS: native DX12 reserved resources, permuted/copied tile mappings, reuse/skip/null ranges and bidirectional GPU tile copies");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
