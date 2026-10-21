#include "gpu_transport.h"
#include <d3d12sdklayers.h>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <future>
#include <memory>

static void Check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
// Child process: register as the consumer and claim the published pair, but
// never queue its release. Either exit normally (objects destroyed) or die
// without any cleanup, like an engine killed from Task Manager.
static int ConsumerHoldingLease(const std::wstring& session, bool kill)
{
    xg::ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return 2;
    auto* reader = new xg::GpuReader; // deleted only on the normal path
    if (!reader->Open(device.Get(), session)) return 4;
    xg::GpuSlotInfo frame; int slot = -1;
    if (reader->Status(0, frame, &slot) != xg::PairRead::NewPair || slot < 0) return 5;
    if (kill) TerminateProcess(GetCurrentProcess(), 3);
    delete reader;
    return 3;
}
// Starts this executable as a consumer that leaves holding a lease.
static void RunConsumer(const std::wstring& session, bool kill)
{
    wchar_t executable[MAX_PATH]{};
    Check(GetModuleFileNameW(nullptr, executable, MAX_PATH) != 0, "test executable path");
    std::wstring command = L"\"" + std::wstring(executable) + (kill ? L"\" --kill-consumer=" : L"\" --exit-consumer=") + session;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    Check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) != FALSE, "start doomed consumer");
    const bool finished = WaitForSingleObject(process.hProcess, 20000) == WAIT_OBJECT_0;
    DWORD code = 0; GetExitCodeProcess(process.hProcess, &code);
    if (!finished) TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    Check(finished && code == 3, "consumer claimed a pair and left without releasing it");
}
int main(int argc, char** argv)
{
    for (const char* mode : {"--kill-consumer=", "--exit-consumer="})
    {
        const std::string prefix(mode);
        if (argc != 2 || std::string(argv[1]).rfind(prefix, 0) != 0) continue;
        const std::string name = std::string(argv[1]).substr(prefix.size());
        return ConsumerHoldingLease(std::wstring(name.begin(), name.end()), prefix == "--kill-consumer=");
    }
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
        {
            const auto resizedSession=session+L"-resize";
            auto oldProducer=std::make_unique<xg::GpuWriter>();auto oldConsumer=std::make_unique<xg::GpuReader>();
            Check(oldProducer->Create(resizedSession,device->GetAdapterLuid(),18,10,xg::GpuFrameKind::SameFrame),"initial resize generation");
            Check(oldConsumer->Open(device.Get(),resizedSession),"old consumer keeps metadata alive");
            Check(!oldConsumer->ReplacementAvailable(),"current live generation is not a replacement");
            oldProducer->Close();xg::GpuWriter replacement;
            Check(replacement.Create(resizedSession,device->GetAdapterLuid(),23,13,xg::GpuFrameKind::SameFrame),"replace own closed generation while old mapping is held");
            Check(oldConsumer->ReplacementAvailable(),"replacement detected without releasing old GPU objects");
            Check(oldConsumer->Status(0,frame)==xg::PairRead::Invalid,"old reader rejects new generation");
            {xg::GpuReader wrongOwner;Check(!wrongOwner.Open(device.Get(),resizedSession,GetCurrentProcessId()+1),"expected producer PID checked before registering a replacement consumer");}
            xg::GpuReader newConsumer;Check(newConsumer.Open(device.Get(),resizedSession),"new reader opens resized generation");
            Check(newConsumer.Header().width==23 && newConsumer.Header().height==13,"new dimensions advertised");
            oldConsumer.reset();oldProducer->Close();oldProducer.reset();
            Check(newConsumer.Status(0,frame)==xg::PairRead::Empty,"old producer and consumer cleanup preserve the new generation");
            {xg::GpuReader duplicate;Check(!duplicate.Open(device.Get(),resizedSession),"old consumer teardown does not detach the new consumer");}
            {xg::GpuWriter duplicate;Check(!duplicate.Create(resizedSession,device->GetAdapterLuid(),23,13,xg::GpuFrameKind::SameFrame),"live resized producer remains exclusive");}
            Check(newConsumer.Status(0,frame)==xg::PairRead::Empty,"failed replacement leaves active stream unchanged");
        }
        {
            const auto detachSession=session+L"-detach";xg::GpuWriter producer;
            Check(producer.Create(detachSession,device->GetAdapterLuid(),18,10,xg::GpuFrameKind::SameFrame),"create detach fixture");
            auto consumer=std::make_unique<xg::GpuReader>();Check(consumer->Open(device.Get(),detachSession),"register disconnecting reader");
            HANDLE held=OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,xg::GpuName(detachSession,L".lock").c_str());Check(held!=nullptr,"open detach mutex");
            std::promise<void> detachLocked,detachRelease,detachBegun;auto detachUnlocked=detachRelease.get_future();
            std::thread owner([&]{xg::PairLock lock(held,1000);Check(lock.Ready(),"hold metadata during detach");detachLocked.set_value();detachUnlocked.wait();});
            detachLocked.get_future().wait();auto detached=std::async(std::launch::async,[&]{detachBegun.set_value();consumer.reset();});detachBegun.get_future().wait();
            const auto waited=detached.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout;
            detachRelease.set_value();owner.join();detached.get();CloseHandle(held);
            xg::GpuReader next;Check(waited && next.Open(device.Get(),detachSession),"contended cleanup releases registration for the next reader");
        }
        {
            const auto startupSession=session+L"-startup";
            xg::GpuWriter splash;
            Check(splash.Create(startupSession,device->GetAdapterLuid(),18,10,xg::GpuFrameKind::SameFrame),"create splash generation");
            auto consumer=std::make_unique<xg::GpuReader>();xg::GpuSlotInfo startupFrame;
            auto waiting=std::async(std::launch::async,[&]{return xg::WaitForGpuPair(device.Get(),startupSession,GetCurrentProcessId(),consumer,startupFrame,3000);});
            // Wait until startup has registered on the splash stream, so this
            // test exercises replacement after Open rather than just retrying.
            HANDLE map=OpenFileMappingW(FILE_MAP_READ,FALSE,xg::GpuName(startupSession,L".frames").c_str());
            auto* metadata=static_cast<const xg::GpuHeader*>(MapViewOfFile(map,FILE_MAP_READ,0,0,sizeof(xg::GpuHeader)));
            HANDLE startupMutex=OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,xg::GpuName(startupSession,L".lock").c_str());
            const auto startupDeadline=GetTickCount64()+1000;
            bool registered=false;
            while(!registered && GetTickCount64()<startupDeadline) {{xg::PairLock lock(startupMutex,1000);registered=lock.Ready() && metadata->consumer==GetCurrentProcessId();}if(!registered)Sleep(1);}
            Check(registered,"startup reader opened splash generation");
            splash.Close();Sleep(30);
            xg::GpuWriter game;
            Check(game.Create(startupSession,device->GetAdapterLuid(),23,13,xg::GpuFrameKind::SameFrame),"create gameplay replacement while startup waits");
            uint64_t startupSequence=0,startupRelease=0;int slot=game.Reserve(startupSequence,startupRelease);
            Check(slot>=0 && game.Publish(uint32_t(slot),startupSequence,7,7) && SUCCEEDED(game.Objects(uint32_t(slot)).ready->Signal(startupSequence)),"complete replacement pair");
            Check(waiting.get()==xg::PairRead::NewPair && consumer->Header().width==23 && startupFrame.leftFrame==7 && startupFrame.rightFrame==7,"startup reconnects to completed replacement without restarting the game");
            consumer.reset();UnmapViewOfFile(metadata);CloseHandle(map);CloseHandle(startupMutex);
            Check(xg::WaitForGpuPair(device.Get(),startupSession,GetCurrentProcessId()+1,consumer,startupFrame,40)!=xg::PairRead::NewPair,"startup retry never accepts a different producer");
        }
        Check(xg::GpuConsumerAlive(GetCurrentProcessId(), xg::GpuProcessStartTime(GetCurrentProcess())), "a running consumer is alive");
        Check(!xg::GpuConsumerAlive(GetCurrentProcessId(), xg::GpuProcessStartTime(GetCurrentProcess()) + 1), "a reused PID with another start time is not the consumer");
        {
            // A consumer exits while holding a lease whose release was never
            // queued: the producer reclaims the slot instead of permanently
            // running with two of three, and the next consumer can attach.
            const auto exitSession = session + L"-exit";
            xg::GpuWriter producer;
            Check(producer.Create(exitSession, device->GetAdapterLuid(), 18, 10, xg::GpuFrameKind::SameFrame), "create exiting-consumer fixture");
            uint64_t exitSequence = 0, exitRelease = 0;
            const int leased = producer.Reserve(exitSequence, exitRelease);
            Check(leased >= 0 && producer.Publish(uint32_t(leased), exitSequence, 1, 1) &&
                SUCCEEDED(producer.Objects(uint32_t(leased)).ready->Signal(exitSequence)), "publish pair for the exiting consumer");
            RunConsumer(exitSession, false);
            const int second = producer.Reserve(exitSequence, exitRelease), third = producer.Reserve(exitSequence, exitRelease);
            Check(second >= 0 && third >= 0 && second != leased && third != leased && !producer.Lost(), "free slots stay usable");
            const int recovered = producer.Reserve(exitSequence, exitRelease);
            Check(recovered == leased && exitRelease == 0 && producer.ReclaimedConsumers() == 1, "producer reclaims the exited consumer's lease");
            xg::GpuReader next;
            Check(next.Open(device.Get(), exitSession), "a new consumer attaches after the exited one");
            { xg::GpuReader duplicate; Check(!duplicate.Open(device.Get(), exitSession), "a live consumer still excludes others"); }
        }
        {
            // Engine killed (TerminateProcess): Windows abandons the shared
            // fences it held. The registration of the dead engine no longer
            // blocks Open, and the producer replaces the lost generation so a
            // restarted engine receives pairs again without restarting the game.
            const auto killSession = session + L"-kill";
            auto producer = std::make_unique<xg::GpuWriter>();
            Check(producer->Create(killSession, device->GetAdapterLuid(), 18, 10, xg::GpuFrameKind::SameFrame), "create killed-consumer fixture");
            uint64_t killSequence = 0, killRelease = 0;
            const int leased = producer->Reserve(killSequence, killRelease);
            Check(leased >= 0 && producer->Publish(uint32_t(leased), killSequence, 1, 1) &&
                SUCCEEDED(producer->Objects(uint32_t(leased)).ready->Signal(killSequence)), "publish pair before the engine dies");
            RunConsumer(killSession, true);
            {
                xg::GpuReader stale;
                Check(stale.Open(device.Get(), killSession) && stale.ReclaimedDeadConsumer(), "killed engine's registration is reclaimed on re-attach");
                xg::GpuSlotInfo staleFrame;
                Check(stale.Status(0, staleFrame) == xg::PairRead::Invalid && stale.FenceLost(), "abandoned fences are detected by the consumer");
            }
            auto restarted = std::make_unique<xg::GpuReader>(); xg::GpuSlotInfo restartFrame;
            auto waiting = std::async(std::launch::async, [&] { return xg::WaitForGpuPair(device.Get(), killSession, GetCurrentProcessId(), restarted, restartFrame, 3000); });
            Check(producer->Reserve(killSequence, killRelease) < 0 && producer->Lost(), "producer detects the abandoned generation");
            producer->Close();
            xg::GpuWriter replacement;
            Check(replacement.Create(killSession, device->GetAdapterLuid(), 18, 10, xg::GpuFrameKind::SameFrame), "producer replaces the lost generation");
            const int slot = replacement.Reserve(killSequence, killRelease);
            Check(slot >= 0 && replacement.Publish(uint32_t(slot), killSequence, 9, 9) &&
                SUCCEEDED(replacement.Objects(uint32_t(slot)).ready->Signal(killSequence)), "publish on the replacement generation");
            Check(waiting.get() == xg::PairRead::NewPair && restartFrame.leftFrame == 9, "restarted engine receives the replacement generation");
            restarted.reset(); producer.reset();
        }
        std::puts("PASS: exited/killed consumer lease reclaim, abandoned-fence generation replacement and re-attachment; shared texture leases, GPU completion, nonblocking full ring, strict same-frame metadata, startup generation replacement, closure, stale/size/adapter/owner rejection");
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
