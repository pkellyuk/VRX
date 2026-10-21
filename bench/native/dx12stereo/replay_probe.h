// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Opt-in paired GPU counters. Read only after a queue fence; never wait on it.
#pragma once
#include "native_debug.h"
#include <memory>
#include <cstring>
#include <vector>
#include <algorithm>
#include <charconv>
#ifndef VRX_DX12_DIAGNOSTICS
#define VRX_DX12_DIAGNOSTICS 0
#endif
namespace vrx::dx12::diagnostic {
// Replay probes and pixel traces (pixel_trace.h) are diagnostic-build only:
// without VRX_DX12_DIAGNOSTICS (Debug builds set it) they can never activate.
inline bool ProbesRequested() noexcept {
#if VRX_DX12_DIAGNOSTICS
    wchar_t text[3]{};return Requested() && GetEnvironmentVariableW(L"VRX_DX12_REPLAY_PROBES",text,3)==1 && text[0]==L'1';
#else
    return false;
#endif
}
inline uint64_t ProbeRequest(){
    const auto path=Settings().path+L".request";FILE* file=nullptr;if(_wfopen_s(&file,path.c_str(),L"rb") || !file)return 0;
    char text[64]{};const auto size=std::fread(text,1,sizeof(text),file);std::fclose(file);if(!size || size==sizeof(text))return 0;
    size_t end=size;while(end && (text[end-1]=='\n' || text[end-1]=='\r' || text[end-1]==' ' || text[end-1]=='\t'))--end;
    uint64_t serial=0;const auto parsed=std::from_chars(text,text+end,serial);return parsed.ec==std::errc{} && parsed.ptr==text+end?serial:0;
}
struct ColourTarget {
    Microsoft::WRL::ComPtr<ID3D12Resource> native,right;
    UINT subresource=0,width=0,height=0;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint[2]{};
    UINT rows=0;UINT64 rowBytes=0;
};
struct ReplayProbe {
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    Microsoft::WRL::ComPtr<ID3D12Resource> output;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    Microsoft::WRL::ComPtr<ID3D12Resource> colours;
    std::vector<ColourTarget> targets;
    uint64_t sequence=0,submitted=0;
    bool pending=false,failed=false,paired=true;
    static std::shared_ptr<ReplayProbe> Create(ID3D12Device* device,uint64_t sequence,std::vector<ColourTarget> targets={}) {
        auto probe=std::make_shared<ReplayProbe>();probe->sequence=sequence;
        const D3D12_QUERY_HEAP_DESC query{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS,2,0};
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=2*sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(FAILED(device->CreateQueryHeap(&query,IID_PPV_ARGS(&probe->queries))) || FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&probe->output))) || FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&probe->fence)))){Write(device,1,0,"VRX replay probe allocation failed");return {};}
        UINT64 bytes=0;
        for(auto& target:targets){
            D3D12_RESOURCE_DESC patch{};patch.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;patch.Width=std::min(UINT(16),target.width);patch.Height=std::min(UINT(16),target.height);patch.DepthOrArraySize=patch.MipLevels=1;patch.Format=target.format;patch.SampleDesc.Count=1;
            for(UINT eye=0;eye<2;++eye){device->GetCopyableFootprints(&patch,0,1,bytes,&target.footprint[eye],&target.rows,&target.rowBytes,nullptr);if(!target.rows || target.rows>16 || !target.rowBytes || target.rowBytes>4096 || target.footprint[eye].Footprint.RowPitch==UINT_MAX)return {};bytes=(target.footprint[eye].Offset+UINT64(target.footprint[eye].Footprint.RowPitch)*target.rows+511)&~UINT64(511);}
        }
        if(bytes){desc.Width=bytes;if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&probe->colours)))){Write(device,1,0,"VRX replay colour allocation failed");return {};}}
        probe->targets=std::move(targets);
        return probe;
    }
    void Begin(ID3D12GraphicsCommandList* native,ID3D12GraphicsCommandList* right){native->BeginQuery(queries.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0);right->BeginQuery(queries.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,1);}
    void End(ID3D12GraphicsCommandList* native,ID3D12GraphicsCommandList* right){
        ID3D12GraphicsCommandList* lists[]={native,right};for(UINT eye=0;eye<2;++eye){lists[eye]->EndQuery(queries.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,eye);lists[eye]->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,eye,1,output.Get(),eye*sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));}
        for(auto& target:targets)for(UINT eye=0;eye<2;++eye){
            auto* resource=eye?target.right.Get():target.native.Get();D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={resource,target.subresource,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};lists[eye]->ResourceBarrier(1,&barrier);
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=resource;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=target.subresource;dst.pResource=colours.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=target.footprint[eye];
            const auto w=target.footprint[eye].Footprint.Width,h=target.footprint[eye].Footprint.Height,x=(target.width-w)/2,y=(target.height-h)/2;const D3D12_BOX box{x,y,0,x+w,y+h,1};lists[eye]->CopyTextureRegion(&dst,0,0,0,&src,&box);std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);lists[eye]->ResourceBarrier(1,&barrier);
        }
    }
    bool Poll() {
        if(failed)return false; // Retain resources if submission cannot be fenced.
        const auto done=fence->GetCompletedValue();if(done<submitted)return false;
        if(done==UINT64_MAX){targets.clear();Write(nullptr,1,0,"VRX replay probe device removed");return true;}
        if(!paired){targets.clear();Write(nullptr,3,0,"VRX unpaired replay probe omitted after stereo stop");return true;}
        void* mapped=nullptr;const D3D12_RANGE read{0,2*sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)};
        if(FAILED(output->Map(0,&read,&mapped))){targets.clear();Write(nullptr,1,0,"VRX replay probe readback failed");return true;}
        D3D12_QUERY_DATA_PIPELINE_STATISTICS stats[2]{};std::memcpy(stats,mapped,sizeof(stats));const D3D12_RANGE noWrite{0,0};output->Unmap(0,&noWrite);
        char text[512]{};sprintf_s(text,"VRX replay probe %llu: iaV=%llu/%llu iaP=%llu/%llu vs=%llu/%llu clipIn=%llu/%llu clipOut=%llu/%llu ps=%llu/%llu",static_cast<unsigned long long>(sequence),stats[0].IAVertices,stats[1].IAVertices,stats[0].IAPrimitives,stats[1].IAPrimitives,stats[0].VSInvocations,stats[1].VSInvocations,stats[0].CInvocations,stats[1].CInvocations,stats[0].CPrimitives,stats[1].CPrimitives,stats[0].PSInvocations,stats[1].PSInvocations);Write(nullptr,3,0,text);
        if(colours && !targets.empty()){
            const D3D12_RANGE readColours{0,SIZE_T(colours->GetDesc().Width)};
            if(SUCCEEDED(colours->Map(0,&readColours,&mapped))){
                for(size_t i=0;i<targets.size();++i){const auto& target=targets[i];UINT64 hash[]={14695981039346656037ull,14695981039346656037ull},nonzero[2]{};
                    for(UINT eye=0;eye<2;++eye)for(UINT row=0;row<target.rows;++row){const auto* bytes=static_cast<const BYTE*>(mapped)+target.footprint[eye].Offset+UINT64(row)*target.footprint[eye].Footprint.RowPitch;for(UINT64 col=0;col<target.rowBytes;++col){hash[eye]=(hash[eye]^bytes[col])*1099511628211ull;if(bytes[col])++nonzero[eye];}}
                    sprintf_s(text,"VRX replay colour %llu target=%zu format=%u subresource=%u size=%ux%u resource=%p/%p hash=%llx/%llx nonzero=%llu/%llu",static_cast<unsigned long long>(sequence),i,UINT(target.format),target.subresource,target.width,target.height,target.native.Get(),target.right.Get(),hash[0],hash[1],nonzero[0],nonzero[1]);Write(nullptr,3,0,text);
                }
                colours->Unmap(0,&noWrite);
            }else Write(nullptr,1,0,"VRX replay colour readback failed");
        }
        targets.clear();return true;
    }
};
} // namespace vrx::dx12::diagnostic
