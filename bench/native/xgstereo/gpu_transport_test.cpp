#include "gpu_transport.h"
#include <d3d12sdklayers.h>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <future>

static void Check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
int main()
{
    try
    {
        xg::ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        xg::ComPtr<ID3D12Device> device;
        Check(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "create test device");
        const auto session = L"gpu-lease-" + std::to_wstring(GetCurrentProcessId());
        xg::GpuWriter writer;
        Check(writer.Create(session, device->GetAdapterLuid(), 18, 10, xg::GpuFrameKind::SameFrame), "create bounded GPU transport");
        { xg::GpuWriter duplicate; Check(!duplicate.Create(session, device->GetAdapterLuid(), 18, 10, xg::GpuFrameKind::SameFrame), "refuse duplicate producer"); }
        xg::GpuReader reader;
        Check(reader.Open(device.Get(), session), "open named shared array textures and fences");
        xg::GpuSlotInfo frame;
        Check(reader.Status(0, frame) == xg::PairRead::Empty, "duplicate writer did not close original stream");
        uint64_t sequence = 0, release = 0;
        const int first = writer.Reserve(sequence, release);
        Check(first >= 0 && sequence == 1 && !release, "reserve first GPU slot");
        Check(!writer.Publish(uint32_t(first), sequence, 1, 2), "same-frame mode rejects alternating eye IDs");
        Check(writer.Publish(uint32_t(first), sequence, 1, 1), "publish same-frame metadata");
        Check(reader.Status(0, frame) == xg::PairRead::Empty, "uncompleted GPU texture is not readable");
        Check(SUCCEEDED(writer.Objects(uint32_t(first)).ready->Signal(sequence)), "simulate producer GPU completion");
        int claimed = -1;
        Check(reader.Status(0, frame, &claimed) == xg::PairRead::NewPair && claimed == first, "claim completed GPU pair");
        int other[2];
        for (int& slot : other) { slot = writer.Reserve(sequence, release); Check(slot >= 0 && slot != first, "leased slot not reused"); }
        Check(writer.Reserve(sequence, release) < 0, "full ring drops without waiting or overwriting");
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        xg::ComPtr<ID3D12CommandQueue> queue;
        Check(SUCCEEDED(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue))), "create consumer queue");
        struct Gate { xg::ComPtr<ID3D12Fence> fence; ~Gate() { if (fence) fence->Signal(1); } } gate;
        Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.fence))), "create test GPU gate");
        Check(SUCCEEDED(queue->Wait(gate.fence.Get(), 1)), "hold consumer GPU work");
        Check(reader.Submitted(queue.Get()), "schedule consumer release behind its GPU work");
        Check(writer.Reserve(sequence, release) < 0, "queued release is insufficient until GPU completes");
        gate.fence->Signal(1);
        const auto deadline = GetTickCount64() + 5000;
        while (writer.Objects(uint32_t(first)).released->GetCompletedValue() < 1 && GetTickCount64() < deadline) Sleep(1);
        Check(writer.Reserve(sequence, release) == first && release == 1, "reuse only after consumer GPU release completed");
        Check(sequence == 4, "failed reservations do not consume sequence values");
        writer.Objects(uint32_t(first)).ready->Signal(sequence);
        Check(writer.Publish(uint32_t(first), sequence, 4, 4), "publish next completed same-frame pair");
        Check(reader.Status(1, frame) == xg::PairRead::NewPair && frame.leftFrame == 4 && frame.rightFrame == 4, "new pair has no old eye");

        HANDLE mapping = OpenFileMappingW(FILE_MAP_WRITE, FALSE, xg::GpuName(session, L".frames").c_str());
        HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, xg::GpuName(session, L".lock").c_str());
        Check(mapping && mutex, "open metadata fixture");
        auto* header = static_cast<xg::GpuHeader*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(xg::GpuHeader)));
        Check(header != nullptr, "map metadata fixture");
        std::promise<void> locked, unlock;
        auto unlocked = unlock.get_future();
        std::thread contention([&] {
            xg::PairLock lock(mutex, 1000);
            locked.set_value(); unlocked.wait();
        });
        locked.get_future().wait();
        const auto busy = writer.TryPublish(uint32_t(first), sequence, 4, 4);
        unlock.set_value(); contention.join();
        Check(busy == xg::GpuPublish::Busy, "transient contention is distinct from invalid publication");
        Check(header->active && writer.TryPublish(uint32_t(first), sequence, 4, 4) == xg::GpuPublish::Published,
            "publication recovers after contention without closing the stream");
        {
            xg::PairLock lock(mutex, 1000); Check(lock.Ready(), "lock metadata fixture");
            header->slots[first].publishedMs = GetTickCount64() - xg::PairMaxAgeMs - 1;
        }
        Check(reader.Status(1, frame) == xg::PairRead::Stale, "stale GPU pairs are hidden");
        {
            xg::PairLock lock(mutex, 1000); Check(lock.Ready(), "lock metadata fixture");
            header->slots[first].publishedMs = GetTickCount64(); header->width = 19;
        }
        Check(reader.Status(1, frame) == xg::PairRead::Invalid, "changed dimensions rejected");
        {
            xg::PairLock lock(mutex, 1000); Check(lock.Ready(), "lock metadata fixture");
            header->width = 18; header->adapter.LowPart ^= 1;
        }
        { xg::GpuReader mismatch; Check(!mismatch.Open(device.Get(), session), "wrong adapter rejected"); }
        {
            xg::PairLock lock(mutex, 1000); Check(lock.Ready(), "lock metadata fixture");
            Check(header->consumer == GetCurrentProcessId(), "failed reader must not detach an existing consumer");
            header->adapter.LowPart ^= 1;
        }
        writer.Close(); Check(reader.Status(1, frame) == xg::PairRead::Closed, "producer closure hides frames");
        UnmapViewOfFile(header); CloseHandle(mapping); CloseHandle(mutex);
        Check(xg::GpuShape(2560, 1440) && !xg::GpuShape(16384, 8192), "bounded full resolution GPU dimensions");
        std::puts("PASS: shared texture leases, GPU completion, nonblocking full ring, strict same-frame metadata, closure, stale/size/adapter rejection");
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
