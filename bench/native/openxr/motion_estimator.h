#pragma once
// Hardware block motion estimation (D3D12 video motion estimation: fixed function,
// usually the GPU's video encoder - not the shader cores) for depth steadying and
// fusion. Measured on an RTX 3090 and RTX 3060: 8x8 and 16x16 blocks, quarter-pel,
// 32x32..4096x4096; ~0.45 ms per 686x392 estimate (bench/native/xmmodel/me_probe).
//
// Owns its own VIDEO_ENCODE and COPY queues and a few NV12 frame slots. Luma (0..255)
// is uploaded from the CPU into a slot; Estimate(cur, ref) returns 8x8-block vectors in
// quarter pels, pointing from each block of `cur` to where its content was in `ref`.
// EstimateMany runs up to MAX_BATCH estimates in one submission with one CPU wait.
// Single-threaded: call everything from one thread (the depth worker).
#include <windows.h>
#include <d3d12.h>
#include <d3d12video.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

class MotionEstimator
{
public:
    static const int SLOTS = 3;
    static const int MAX_BATCH = 2;
    struct Pair { int cur, ref; };

    // log: printf-style logger (may be null). False when this GPU cannot estimate
    // motion at w x h; the reason is logged.
    bool Init(ID3D12Device* device, int w, int h, void (*log)(const char*, ...))
    {
        log_ = log;
        Say("MotionEstimator::Init: enter %dx%d", w, h);
        if (!device || w < 32 || h < 32 || (w & 1) || (h & 1)) { Say("MotionEstimator::Init: FAIL bad arguments"); return false; }
        device_ = device;
        w_ = w; h_ = h;
        bw_ = (w + 7) / 8; bh_ = (h + 7) / 8;

        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&video_)))) { Say("MotionEstimator::Init: UNSUPPORTED no ID3D12VideoDevice1"); return false; }
        D3D12_FEATURE_DATA_VIDEO_MOTION_ESTIMATOR caps{};
        caps.InputFormat = DXGI_FORMAT_NV12;
        if (FAILED(video_->CheckFeatureSupport(D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR, &caps, sizeof(caps))))
        { Say("MotionEstimator::Init: UNSUPPORTED no motion estimation feature"); return false; }
        const bool b8 = (caps.BlockSizeFlags & D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_FLAG_8X8) != 0;
        const bool qpel = (caps.PrecisionFlags & D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION_FLAG_QUARTER_PEL) != 0;
        if (!b8 || !qpel) { Say("MotionEstimator::Init: UNSUPPORTED no 8x8 quarter-pel search"); return false; }
        if ((UINT)w < caps.SizeRange.MinWidth || (UINT)h < caps.SizeRange.MinHeight || (UINT)w > caps.SizeRange.MaxWidth || (UINT)h > caps.SizeRange.MaxHeight)
        { Say("MotionEstimator::Init: UNSUPPORTED %dx%d outside %ux%u..%ux%u", w, h, caps.SizeRange.MinWidth, caps.SizeRange.MinHeight, caps.SizeRange.MaxWidth, caps.SizeRange.MaxHeight); return false; }

        const D3D12_VIDEO_SIZE_RANGE range{ (UINT)w, (UINT)h, (UINT)w, (UINT)h };
        D3D12_VIDEO_MOTION_ESTIMATOR_DESC ed{ 0, DXGI_FORMAT_NV12, D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_8X8, D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION_QUARTER_PEL, range };
        if (FAILED(video_->CreateVideoMotionEstimator(&ed, nullptr, IID_PPV_ARGS(&est_)))) { Say("MotionEstimator::Init: FAIL CreateVideoMotionEstimator"); return false; }
        D3D12_VIDEO_MOTION_VECTOR_HEAP_DESC hd{ 0, DXGI_FORMAT_NV12, D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_8X8, D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION_QUARTER_PEL, range };
        for (int b = 0; b < MAX_BATCH; b++)
            if (FAILED(video_->CreateVideoMotionVectorHeap(&hd, nullptr, IID_PPV_ARGS(&heap_[b])))) { Say("MotionEstimator::Init: FAIL CreateVideoMotionVectorHeap"); return false; }

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&encodeQueue_)))) { Say("MotionEstimator::Init: FAIL video encode queue"); return false; }
        qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&copyQueue_)))) { Say("MotionEstimator::Init: FAIL copy queue"); return false; }
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, IID_PPV_ARGS(&encodeAlloc_))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, encodeAlloc_.Get(), nullptr, IID_PPV_ARGS(&encodeList_))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&copyAlloc_))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, copyAlloc_.Get(), nullptr, IID_PPV_ARGS(&copyList_))))
        { Say("MotionEstimator::Init: FAIL command lists"); return false; }
        encodeList_->Close();
        copyList_->Close();
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) { Say("MotionEstimator::Init: FAIL fence"); return false; }
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) { Say("MotionEstimator::Init: FAIL event"); return false; }

        // NV12 frame slots (chroma set once to neutral) and their upload buffers.
        D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT }, up{ D3D12_HEAP_TYPE_UPLOAD }, rb{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = (UINT64)w; td.Height = (UINT)h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_NV12; td.SampleDesc.Count = 1;
        UINT64 total = 0;
        device->GetCopyableFootprints(&td, 0, 2, 0, planeFootprint_, planeRows_, planeRowBytes_, &total);
        for (int s = 0; s < SLOTS; s++)
        {
            if (FAILED(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&frames_[s]))) ||
                !MakeBuffer(&up, total, D3D12_RESOURCE_STATE_GENERIC_READ, upload_[s]) ||
                FAILED(upload_[s]->Map(0, nullptr, (void**)&uploadMapped_[s])))
            { Say("MotionEstimator::Init: FAIL frame slot %d", s); return false; }
            for (UINT y = 0; y < planeRows_[1]; y++)
                memset(uploadMapped_[s] + planeFootprint_[1].Offset + (UINT64)y * planeFootprint_[1].Footprint.RowPitch, 128, (size_t)planeRowBytes_[1]);
            if (!CopyUpload(s, true)) return false;
        }

        D3D12_RESOURCE_DESC vd{};
        vd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; vd.Width = (UINT64)bw_; vd.Height = (UINT)bh_; vd.DepthOrArraySize = 1;
        vd.MipLevels = 1; vd.Format = DXGI_FORMAT_R16G16_SINT; vd.SampleDesc.Count = 1;
        for (int b = 0; b < MAX_BATCH; b++)
            if (FAILED(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &vd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&vectors_[b]))))
            { Say("MotionEstimator::Init: FAIL vector texture"); return false; }
        UINT64 vbytes = 0;
        device->GetCopyableFootprints(&vd, 0, 1, 0, &vectorFootprint_, nullptr, nullptr, &vbytes);
        vectorStride_ = (vbytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) / D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        if (!MakeBuffer(&rb, vectorStride_ * MAX_BATCH, D3D12_RESOURCE_STATE_COPY_DEST, readback_)) { Say("MotionEstimator::Init: FAIL readback"); return false; }

        ok_ = true;
        Say("MotionEstimator::Init: exit ok (%dx%d blocks of 8x8, quarter-pel)", bw_, bh_);
        return true;
    }

    void Release()
    {
        if (fence_ && event_) WaitFor(fenceValue_);
        for (int s = 0; s < SLOTS; s++)
        {
            if (upload_[s] && uploadMapped_[s]) upload_[s]->Unmap(0, nullptr);
            uploadMapped_[s] = nullptr;
            upload_[s].Reset();
            frames_[s].Reset();
        }
        for (int b = 0; b < MAX_BATCH; b++) { vectors_[b].Reset(); heap_[b].Reset(); }
        readback_.Reset(); est_.Reset();
        encodeList_.Reset(); encodeAlloc_.Reset(); copyList_.Reset(); copyAlloc_.Reset();
        encodeQueue_.Reset(); copyQueue_.Reset(); fence_.Reset(); video_.Reset();
        if (event_) { CloseHandle(event_); event_ = nullptr; }
        ok_ = false;
    }

    ~MotionEstimator() { Release(); }

    bool ok() const { return ok_; }
    int blocksW() const { return bw_; }
    int blocksH() const { return bh_; }

    // Luma 0..255 (w*h floats, rounded to bytes) into a frame slot.
    bool Upload(int slot, const float* luma)
    {
        if (!ok_ || !luma || slot < 0 || slot >= SLOTS) return false;
        unsigned char* base = uploadMapped_[slot] + planeFootprint_[0].Offset;
        for (int y = 0; y < h_; y++)
        {
            unsigned char* row = base + (UINT64)y * planeFootprint_[0].Footprint.RowPitch;
            const float* src = luma + (size_t)y * w_;
            for (int x = 0; x < w_; x++)
            {
                const float v = src[x] < 0 ? 0 : (src[x] > 255 ? 255 : src[x]);
                row[x] = (unsigned char)(v + 0.5f);
            }
        }
        return CopyUpload(slot, false);
    }

    // 8x8-block vectors (bw*bh pairs of int16 x, y; quarter pels) from `cur` to `ref`.
    bool Estimate(int cur, int ref, std::vector<int16_t>& out)
    {
        const Pair p{ cur, ref };
        return EstimateMany(&p, 1, &out);
    }

    // Up to MAX_BATCH estimates, recorded in one list, copied out together, one wait.
    bool EstimateMany(const Pair* pairs, int count, std::vector<int16_t>* outs)
    {
        if (!ok_ || !pairs || !outs || count < 1 || count > MAX_BATCH) return false;
        for (int i = 0; i < count; i++)
        {
            const Pair& p = pairs[i];
            if (p.cur < 0 || p.ref < 0 || p.cur >= SLOTS || p.ref >= SLOTS || p.cur == p.ref) return false;
        }
        if (FAILED(encodeAlloc_->Reset()) || FAILED(encodeList_->Reset(encodeAlloc_.Get()))) { Say("MotionEstimator::Estimate: FAIL reset"); return false; }
        for (int i = 0; i < count; i++)
        {
            ID3D12Resource* cur = frames_[pairs[i].cur].Get();
            ID3D12Resource* ref = frames_[pairs[i].ref].Get();
            D3D12_RESOURCE_BARRIER in[3] = {
                Transition(cur, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ),
                Transition(ref, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ),
                Transition(vectors_[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE) };
            encodeList_->ResourceBarrier(3, in);
            D3D12_VIDEO_MOTION_ESTIMATOR_OUTPUT eo{ heap_[i].Get() };
            D3D12_VIDEO_MOTION_ESTIMATOR_INPUT ei{ cur, 0, ref, 0, nullptr };
            encodeList_->EstimateMotion(est_.Get(), &eo, &ei);
            D3D12_RESOLVE_VIDEO_MOTION_VECTOR_HEAP_OUTPUT ro{ vectors_[i].Get(), {} };
            D3D12_RESOLVE_VIDEO_MOTION_VECTOR_HEAP_INPUT ri{ heap_[i].Get(), (UINT)w_, (UINT)h_ };
            encodeList_->ResolveMotionVectorHeap(&ro, &ri);
            D3D12_RESOURCE_BARRIER back[3] = {
                Transition(cur, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ, D3D12_RESOURCE_STATE_COMMON),
                Transition(ref, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ, D3D12_RESOURCE_STATE_COMMON),
                Transition(vectors_[i].Get(), D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE, D3D12_RESOURCE_STATE_COMMON) };
            encodeList_->ResourceBarrier(3, back);
        }
        if (FAILED(encodeList_->Close())) { Say("MotionEstimator::Estimate: FAIL close"); return false; }
        ID3D12CommandList* lists[] = { encodeList_.Get() };
        encodeQueue_->ExecuteCommandLists(1, lists);
        encodeQueue_->Signal(fence_.Get(), ++fenceValue_);

        // Copy the resolved vectors out on the copy queue, after the estimates.
        if (FAILED(copyAlloc_->Reset()) || FAILED(copyList_->Reset(copyAlloc_.Get(), nullptr))) { Say("MotionEstimator::Estimate: FAIL copy reset"); return false; }
        for (int i = 0; i < count; i++)
        {
            D3D12_TEXTURE_COPY_LOCATION dst{ readback_.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            dst.PlacedFootprint = vectorFootprint_;
            dst.PlacedFootprint.Offset = vectorStride_ * i;
            D3D12_TEXTURE_COPY_LOCATION src{ vectors_[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            src.SubresourceIndex = 0;
            copyList_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        if (FAILED(copyList_->Close())) { Say("MotionEstimator::Estimate: FAIL copy close"); return false; }
        copyQueue_->Wait(fence_.Get(), fenceValue_);
        ID3D12CommandList* copies[] = { copyList_.Get() };
        copyQueue_->ExecuteCommandLists(1, copies);
        copyQueue_->Signal(fence_.Get(), ++fenceValue_);
        if (!WaitFor(fenceValue_)) { Say("MotionEstimator::Estimate: FAIL wait (device removed 0x%08lx)", (unsigned long)device_->GetDeviceRemovedReason()); return false; }

        const char* p = nullptr;
        if (FAILED(readback_->Map(0, nullptr, (void**)&p)) || !p) { Say("MotionEstimator::Estimate: FAIL map"); return false; }
        for (int i = 0; i < count; i++)
        {
            std::vector<int16_t>& out = outs[i];
            out.resize((size_t)bw_ * bh_ * 2);
            const char* base = p + vectorStride_ * i;
            for (int y = 0; y < bh_; y++)
                memcpy(&out[(size_t)y * bw_ * 2], base + (UINT64)y * vectorFootprint_.Footprint.RowPitch, (size_t)bw_ * 4);
        }
        readback_->Unmap(0, nullptr);
        return true;
    }

private:
    template <typename... A>
    void Say(const char* fmt, A... args) const { if (log_) log_(fmt, args...); }

    static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        return b;
    }

    bool MakeBuffer(const D3D12_HEAP_PROPERTIES* heap, UINT64 bytes, D3D12_RESOURCE_STATES state, Microsoft::WRL::ComPtr<ID3D12Resource>& out)
    {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = bytes; bd.Height = 1; bd.DepthOrArraySize = 1;
        bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(device_->CreateCommittedResource(heap, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, IID_PPV_ARGS(&out)));
    }

    bool WaitFor(UINT64 value)
    {
        if (fence_->GetCompletedValue() >= value) return true;
        if (FAILED(fence_->SetEventOnCompletion(value, event_))) return false;
        return WaitForSingleObject(event_, 2000) == WAIT_OBJECT_0 && fence_->GetCompletedValue() != UINT64_MAX;
    }

    // Upload buffer -> NV12 slot on the copy queue: luma only, or both planes.
    bool CopyUpload(int slot, bool bothPlanes)
    {
        if (!WaitFor(fenceValue_)) { Say("MotionEstimator::CopyUpload: FAIL wait"); return false; }
        if (FAILED(copyAlloc_->Reset()) || FAILED(copyList_->Reset(copyAlloc_.Get(), nullptr))) { Say("MotionEstimator::CopyUpload: FAIL reset"); return false; }
        for (UINT plane = 0; plane < (bothPlanes ? 2u : 1u); plane++)
        {
            D3D12_TEXTURE_COPY_LOCATION dst{ frames_[slot].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            dst.SubresourceIndex = plane;
            D3D12_TEXTURE_COPY_LOCATION src{ upload_[slot].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            src.PlacedFootprint = planeFootprint_[plane];
            copyList_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        if (FAILED(copyList_->Close())) { Say("MotionEstimator::CopyUpload: FAIL close"); return false; }
        ID3D12CommandList* lists[] = { copyList_.Get() };
        copyQueue_->ExecuteCommandLists(1, lists);
        copyQueue_->Signal(fence_.Get(), ++fenceValue_);
        if (!WaitFor(fenceValue_)) { Say("MotionEstimator::CopyUpload: FAIL copy wait"); return false; }
        return true;
    }

    void (*log_)(const char*, ...) = nullptr;
    bool ok_ = false;
    int w_ = 0, h_ = 0, bw_ = 0, bh_ = 0;
    ID3D12Device* device_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12VideoDevice1> video_;
    Microsoft::WRL::ComPtr<ID3D12VideoMotionEstimator> est_;
    Microsoft::WRL::ComPtr<ID3D12VideoMotionVectorHeap> heap_[MAX_BATCH];
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> encodeQueue_, copyQueue_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> encodeAlloc_, copyAlloc_;
    Microsoft::WRL::ComPtr<ID3D12VideoEncodeCommandList> encodeList_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> copyList_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    UINT64 fenceValue_ = 0;
    HANDLE event_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> frames_[SLOTS], upload_[SLOTS], vectors_[MAX_BATCH], readback_;
    UINT64 vectorStride_ = 0;
    unsigned char* uploadMapped_[SLOTS] = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT planeFootprint_[2]{}, vectorFootprint_{};
    UINT planeRows_[2]{};
    UINT64 planeRowBytes_[2]{};
};
