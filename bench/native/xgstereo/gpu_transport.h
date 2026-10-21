// Shared D3D12 textures and fences. Only bounded metadata crosses CPU memory.
#pragma once
#include "pair_transport.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <memory>
#include "../common/system_d3d12.h"

namespace xg {
using Microsoft::WRL::ComPtr;
constexpr uint32_t GpuMagic = 0x47534758, GpuVersion = 1, GpuSlots = 3;
enum class GpuPublish { Published, Busy, Invalid };
enum class GpuFrameKind : uint32_t { Alternating = 1, SameFrame = 2 };
struct GpuSlotInfo {
    uint64_t sequence = 0, leftFrame = 0, rightFrame = 0, publishedMs = 0, claimed = 0;
};
struct GpuHeader {
    uint32_t magic = GpuMagic, version = GpuVersion, bytes = sizeof(GpuHeader), active = 1;
    uint32_t pid = 0, width = 0, height = 0, format = DXGI_FORMAT_R8G8B8A8_UNORM;
    LUID adapter{};
    uint64_t generation = 0;
    uint32_t kind = 0, consumer = 0;
    GpuSlotInfo slots[GpuSlots]{};
};
static_assert(sizeof(GpuSlotInfo) == 40 && sizeof(GpuHeader) == 176, "GPU transport metadata ABI");
inline bool GpuShape(uint32_t w, uint32_t h) { return w >= 2 && h >= 2 && w <= 4096 && h <= 4096; }
inline std::wstring GpuName(const std::wstring& session, const wchar_t* suffix)
{ return L"Local\\VRX.xgstereo." + session + L".gpu" + suffix; }
inline std::wstring GpuObjectName(const std::wstring& session, const GpuHeader& h, uint32_t slot, const wchar_t* suffix)
{ return GpuName(session, L".") + std::to_wstring(h.pid) + L"." + std::to_wstring(h.generation) + L"." + std::to_wstring(slot) + suffix; }
inline bool ValidGpuHeader(const GpuHeader& h)
{
    return h.magic == GpuMagic && h.version == GpuVersion && h.bytes == sizeof(h) && h.pid && h.generation &&
        GpuShape(h.width, h.height) && h.format == DXGI_FORMAT_R8G8B8A8_UNORM &&
        (h.kind == uint32_t(GpuFrameKind::Alternating) || h.kind == uint32_t(GpuFrameKind::SameFrame));
}
struct GpuObjects { ComPtr<ID3D12Resource> texture; ComPtr<ID3D12Fence> ready, released; };
class GpuMapping {
protected:
    HANDLE mutex_ = nullptr, mapping_ = nullptr;
    GpuHeader* view_ = nullptr;
public:
    GpuMapping() = default;
    GpuMapping(const GpuMapping&) = delete;
    GpuMapping& operator=(const GpuMapping&) = delete;
    ~GpuMapping() { if (view_) UnmapViewOfFile(view_); if (mapping_) CloseHandle(mapping_); if (mutex_) CloseHandle(mutex_); }
};
class GpuWriter : public GpuMapping {
    ComPtr<ID3D12Device> device_;
    std::array<GpuObjects, GpuSlots> objects_;
    uint64_t sequence_ = 0;
    uint64_t generation_ = 0;
    uint32_t nextSlot_ = 0;
    std::array<uint64_t, GpuSlots> pending_{};
    std::vector<HANDLE> handles_;
public:
    ~GpuWriter() { Close(); for (HANDLE h : handles_) CloseHandle(h); }
    ID3D12Device* Device() const { return device_.Get(); }
    const GpuObjects& Objects(uint32_t slot) const { return objects_[slot]; }
    HANDLE Handle(uint32_t slot, uint32_t object) const { return handles_[slot * 3 + object]; }
    bool Create(const std::wstring& session, LUID adapter, uint32_t w, uint32_t h, GpuFrameKind kind, bool unorderedAccess=false)
    {
        if (!ValidSession(session) || !GpuShape(w, h)) return false;
        ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> gpu;
        if (FAILED(vrx::SystemDxgiFactory(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByLuid(adapter, IID_PPV_ARGS(&gpu))) ||
            FAILED(vrx::SystemD3d12Device(gpu.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) return false;
        mutex_ = CreateMutexW(nullptr, FALSE, GpuName(session, L".lock").c_str());
        if (!mutex_) return false;
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(GpuHeader), GpuName(session, L".frames").c_str());
        if (!mapping_) return false;
        const bool existing = GetLastError() == ERROR_ALREADY_EXISTS;
        view_ = static_cast<GpuHeader*>(MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, sizeof(GpuHeader)));
        if (!view_) return false;
        PairLock lock(mutex_, 1000);
        if (!lock.Ready()) return false;
        // A resize can leave the closed metadata mapping alive in a consumer.
        // Only the same producer process may replace its inactive generation.
        if (existing && (!ValidGpuHeader(*view_) || view_->active || view_->pid != GetCurrentProcessId())) return false;
        GpuHeader header; header.pid = GetCurrentProcessId(); header.adapter = adapter; header.width = w; header.height = h; header.kind = uint32_t(kind);
        LARGE_INTEGER clock{}; QueryPerformanceCounter(&clock); header.generation = uint64_t(clock.QuadPart);
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = w; desc.Height = h;
        desc.DepthOrArraySize = 2; desc.MipLevels = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        if(unorderedAccess)desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        for (uint32_t i = 0; i < GpuSlots; ++i)
        {
            auto& o = objects_[i];
            if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&o.texture))) ||
                FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&o.ready))) ||
                FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&o.released)))) return false;
            ID3D12DeviceChild* resources[] = {o.texture.Get(), o.ready.Get(), o.released.Get()};
            const wchar_t* suffixes[] = {L".texture", L".ready", L".released"};
            for (uint32_t n = 0; n < 3; ++n)
            {
                HANDLE handle = nullptr;
                if (FAILED(device_->CreateSharedHandle(resources[n], nullptr, GENERIC_ALL, GpuObjectName(session, header, i, suffixes[n]).c_str(), &handle))) return false;
                handles_.push_back(handle);
            }
        }
        *view_ = header; generation_ = header.generation;
        return true;
    }
    // Never wait for a consumer. A leased texture cannot be reused until the
    // consumer's GPU fence has completed; an unclaimed old frame can be dropped.
    int Reserve(uint64_t& sequence, uint64_t& releasedValue)
    {
        PairLock lock(mutex_);
        if (!lock.Ready() || !view_ || !view_->active || !generation_ || view_->generation != generation_ || sequence_ == UINT64_MAX) return -1;
        for (uint32_t n = 0; n < GpuSlots; ++n)
        {
            const uint32_t i = (nextSlot_ + n) % GpuSlots;
            auto& s = view_->slots[i];
            const auto ready = objects_[i].ready->GetCompletedValue(), released = objects_[i].released->GetCompletedValue();
            if (ready == UINT64_MAX || released == UINT64_MAX) return -1;
            if (ready < pending_[i] || (s.claimed && released < s.claimed)) continue;
            releasedValue = s.claimed; sequence = ++sequence_; pending_[i] = sequence;
            nextSlot_ = (i + 1) % GpuSlots;
            s = {}; // revoke old metadata before GPU writes can begin
            return int(i);
        }
        return -1;
    }
    GpuPublish TryPublish(uint32_t slot, uint64_t sequence, uint64_t left, uint64_t right)
    {
        PairLock lock(mutex_);
        if (lock.Busy()) return GpuPublish::Busy;
        if (!lock.Ready() || !view_ || !view_->active || !generation_ || view_->generation != generation_ || slot >= GpuSlots || pending_[slot] != sequence || !left ||
            right < left || right - left > 1 || (view_->kind == uint32_t(GpuFrameKind::SameFrame) && right != left)) return GpuPublish::Invalid;
        view_->slots[slot] = {sequence, left, right, GetTickCount64(), 0}; return GpuPublish::Published;
    }
    bool Publish(uint32_t slot, uint64_t sequence, uint64_t left, uint64_t right)
    { return TryPublish(slot, sequence, left, right) == GpuPublish::Published; }
    void Close() { PairLock lock(mutex_, 1000); if (lock.Ready() && view_ && generation_ && view_->generation == generation_) view_->active = 0; }
};

class GpuReader : public GpuMapping {
    std::array<GpuObjects, GpuSlots> objects_;
    GpuHeader header_{};
    bool tainted_ = false;
    bool registered_ = false;
    int pendingSlot_ = -1;
    uint64_t pendingSequence_ = 0;
public:
    const GpuHeader& Header() const { return header_; }
    bool Open(ID3D12Device* device, const std::wstring& session, uint32_t expectedPid = 0)
    {
        if (!device || !ValidSession(session) || mutex_ || mapping_ || view_) return false;
        mutex_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, GpuName(session, L".lock").c_str());
        mapping_ = OpenFileMappingW(FILE_MAP_WRITE, FALSE, GpuName(session, L".frames").c_str());
        if (!mutex_ || !mapping_) return false;
        view_ = static_cast<GpuHeader*>(MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, sizeof(GpuHeader)));
        if (!view_) return false;
        PairLock lock(mutex_, 1000);
        if (!lock.Ready() || !ValidGpuHeader(*view_) || !view_->active || view_->consumer || (expectedPid && view_->pid != expectedPid)) return false;
        const LUID luid = device->GetAdapterLuid();
        if (luid.LowPart != view_->adapter.LowPart || luid.HighPart != view_->adapter.HighPart) return false;
        header_ = *view_;
        for (uint32_t i = 0; i < GpuSlots; ++i)
        {
            auto open = [&](const wchar_t* suffix, auto& object) {
                HANDLE handle = nullptr;
                if (FAILED(device->OpenSharedHandleByName(GpuObjectName(session, header_, i, suffix).c_str(), GENERIC_ALL, &handle))) return false;
                const HRESULT hr = device->OpenSharedHandle(handle, IID_PPV_ARGS(&object)); CloseHandle(handle); return SUCCEEDED(hr);
            };
            if (!open(L".texture", objects_[i].texture) || !open(L".ready", objects_[i].ready) || !open(L".released", objects_[i].released)) return false;
            const auto d = objects_[i].texture->GetDesc();
            if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width != header_.width || d.Height != header_.height ||
                d.DepthOrArraySize != 2 || d.MipLevels != 1 || d.SampleDesc.Count != 1 || d.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return false;
        }
        view_->consumer = GetCurrentProcessId();
        registered_ = true;
        return true;
    }
    bool ReplacementAvailable() const {
        PairLock lock(mutex_);
        return lock.Ready() && !tainted_ && view_ && view_->active && ValidGpuHeader(*view_) &&
            view_->pid == header_.pid && view_->generation != header_.generation &&
            view_->adapter.LowPart == header_.adapter.LowPart && view_->adapter.HighPart == header_.adapter.HighPart;
    }
    ~GpuReader()
    {
        if(!registered_)return;
        // Dropping a candidate after a Busy status must not strand its consumer
        // registration. Cleanup is rare; allow a bounded metadata-lock wait.
        PairLock lock(mutex_,1000);
        if (registered_ && lock.Ready() && view_ && view_->generation == header_.generation && view_->pid == header_.pid && view_->consumer == GetCurrentProcessId()) view_->consumer = 0;
        // Leases deliberately survive disconnect. Only completed GPU signals
        // permit reuse, including when a reader dies during a submission.
    }
    PairRead Status(uint64_t previous, GpuSlotInfo& frame, int* slot = nullptr)
    {
        PairLock lock(mutex_);
        if (lock.Abandoned()) tainted_ = true;
        if (tainted_) return PairRead::Invalid;
        if (!lock.Ready()) return PairRead::Busy;
        if (!view_ || !view_->active) return PairRead::Closed;
        if (!ValidGpuHeader(*view_) || view_->generation != header_.generation || view_->pid != header_.pid ||
            view_->width != header_.width || view_->height != header_.height || view_->kind != header_.kind) return PairRead::Invalid;
        int best = -1;
        for (uint32_t i = 0; i < GpuSlots; ++i)
        {
            const auto& s = view_->slots[i];
            const auto ready = objects_[i].ready->GetCompletedValue();
            if (ready == UINT64_MAX) return PairRead::Invalid;
            if (!s.sequence || ready < s.sequence || s.claimed || (best >= 0 && s.sequence <= view_->slots[best].sequence)) continue;
            best = int(i);
        }
        if (best < 0) return PairRead::Empty;
        const auto& s = view_->slots[best];
        if (!s.leftFrame || s.rightFrame < s.leftFrame || s.rightFrame - s.leftFrame > 1 ||
            (header_.kind == uint32_t(GpuFrameKind::SameFrame) && s.leftFrame != s.rightFrame)) return PairRead::Invalid;
        const auto now = GetTickCount64();
        if (s.publishedMs > now || now - s.publishedMs > PairMaxAgeMs) return PairRead::Stale;
        frame = s;
        if (s.sequence <= previous) return PairRead::Unchanged;
        if (slot)
        {
            if (pendingSlot_ >= 0) return PairRead::Busy;
            view_->slots[best].claimed = s.sequence;
            pendingSlot_ = best; pendingSequence_ = s.sequence; *slot = best;
        }
        return PairRead::NewPair;
    }
    bool Record(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* commands, ID3D12Resource* sbs,
        uint64_t previous, GpuSlotInfo& frame)
    {
        if (!queue || !commands || !sbs) return false;
        const auto d = sbs->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width != uint64_t(header_.width) * 2 ||
            d.Height != header_.height || d.DepthOrArraySize != 1 || d.MipLevels != 1 ||
            d.SampleDesc.Count != 1 || d.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return false;
        int slot = -1;
        if (Status(previous, frame, &slot) != PairRead::NewPair) return false;
        auto& o = objects_[slot];
        if (FAILED(queue->Wait(o.ready.Get(), frame.sequence))) return false;
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {o.texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE};
        commands->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{}; src.pResource = o.texture.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = sbs; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        for (UINT eye = 0; eye < 2; ++eye) { src.SubresourceIndex = eye; commands->CopyTextureRegion(&dst, eye * header_.width, 0, 0, &src, nullptr); }
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); commands->ResourceBarrier(1, &barrier);
        return true;
    }
    bool Submitted(ID3D12CommandQueue* queue)
    {
        if (pendingSlot_ < 0) return true;
        if (FAILED(queue->Signal(objects_[pendingSlot_].released.Get(), pendingSequence_))) return false;
        pendingSlot_ = -1; pendingSequence_ = 0; return true;
    }
};
// Startup may observe the short closed generation between a game's splash
// swapchain and its real one. Reopen until a completed pair arrives, retaining
// the first producer's identity and the device's adapter check throughout.
inline PairRead WaitForGpuPair(ID3D12Device* device,const std::wstring& session,uint32_t expectedPid,
    std::unique_ptr<GpuReader>& reader,GpuSlotInfo& frame,uint64_t timeoutMs)
{
    const auto until=GetTickCount64()+timeoutMs;
    uint32_t owner=expectedPid;PairRead status=PairRead::Closed;
    reader.reset();
    do {
        if(!reader) {
            auto candidate=std::make_unique<GpuReader>();
            if(candidate->Open(device,session,owner)) {
                owner=candidate->Header().pid;reader=std::move(candidate);
            }
        }
        if(reader) {
            status=reader->Status(0,frame);
            if(status==PairRead::NewPair) return status;
            if(status==PairRead::Invalid && !reader->ReplacementAvailable()) return status;
            if(status==PairRead::Closed || status==PairRead::Invalid) reader.reset();
        }
        Sleep(5);
    } while(GetTickCount64()<until);
    return status;
}
} // namespace xg
