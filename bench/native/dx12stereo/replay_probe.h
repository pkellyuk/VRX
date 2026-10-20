// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Opt-in paired GPU counters. Read only after a queue fence; never wait on it.
#pragma once
#include "native_debug.h"
#include <memory>
#include <cstring>
namespace vrx::dx12::diagnostic {
inline bool ProbesRequested() noexcept {wchar_t text[3]{};return Requested() && GetEnvironmentVariableW(L"VRX_DX12_REPLAY_PROBES",text,3)==1 && text[0]==L'1';}
struct ReplayProbe {
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    Microsoft::WRL::ComPtr<ID3D12Resource> output;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    uint64_t sequence=0,submitted=0;
    bool pending=false,failed=false,paired=true;
    static std::shared_ptr<ReplayProbe> Create(ID3D12Device* device,uint64_t sequence) {
        auto probe=std::make_shared<ReplayProbe>();probe->sequence=sequence;
        const D3D12_QUERY_HEAP_DESC query{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS,2,0};
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=2*sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(FAILED(device->CreateQueryHeap(&query,IID_PPV_ARGS(&probe->queries))) || FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&probe->output))) || FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&probe->fence)))){Write(device,1,0,"VRX replay probe allocation failed");return {};}
        return probe;
    }
    void Begin(ID3D12GraphicsCommandList* native,ID3D12GraphicsCommandList* right){native->BeginQuery(queries.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0);right->BeginQuery(queries.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,1);}
    void End(ID3D12GraphicsCommandList* native,ID3D12GraphicsCommandList* right){
        ID3D12GraphicsCommandList* lists[]={native,right};for(UINT eye=0;eye<2;++eye){lists[eye]->EndQuery(queries.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,eye);lists[eye]->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,eye,1,output.Get(),eye*sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));}
    }
    bool Poll() {
        if(failed)return false; // Retain resources if submission cannot be fenced.
        const auto done=fence->GetCompletedValue();if(done<submitted)return false;
        if(done==UINT64_MAX){Write(nullptr,1,0,"VRX replay probe device removed");return true;}
        if(!paired){Write(nullptr,3,0,"VRX unpaired replay probe omitted after stereo stop");return true;}
        void* mapped=nullptr;const D3D12_RANGE read{0,2*sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)};
        if(FAILED(output->Map(0,&read,&mapped))){Write(nullptr,1,0,"VRX replay probe readback failed");return true;}
        D3D12_QUERY_DATA_PIPELINE_STATISTICS stats[2]{};std::memcpy(stats,mapped,sizeof(stats));const D3D12_RANGE noWrite{0,0};output->Unmap(0,&noWrite);
        char text[512]{};sprintf_s(text,"VRX replay probe %llu: iaV=%llu/%llu iaP=%llu/%llu vs=%llu/%llu clipIn=%llu/%llu clipOut=%llu/%llu ps=%llu/%llu",static_cast<unsigned long long>(sequence),stats[0].IAVertices,stats[1].IAVertices,stats[0].IAPrimitives,stats[1].IAPrimitives,stats[0].VSInvocations,stats[1].VSInvocations,stats[0].CInvocations,stats[1].CInvocations,stats[0].CPrimitives,stats[1].CPrimitives,stats[0].PSInvocations,stats[1].PSInvocations);Write(nullptr,3,0,text);return true;
    }
};
} // namespace vrx::dx12::diagnostic
