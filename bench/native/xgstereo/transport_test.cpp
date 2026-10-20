#include "pair_transport.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

static void Check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 3 && std::wstring(argv[1]) == L"--child")
    {
        xg::PairReader reader;
        Check(reader.Open(argv[2]), "child opens parent transport");
        xg::PairFrame frame;
        Check(reader.Read(frame) == xg::PairRead::NewPair && frame.header.sequence == 1 &&
            frame.rgba.size() == 16 * 8 * 8 && frame.rgba.front() == 0x42 && frame.rgba.back() == 0x42,
            "cross-process pair contents");
        return 0;
    }
    const auto session = L"test-" + std::to_wstring(GetCurrentProcessId());
    Check(!xg::ValidSession(L"../unsafe") && !xg::ValidSession(L"") && xg::ValidSession(session), "session name validation");
    xg::PairWriter writer, duplicate;
    Check(writer.Create(session) && !duplicate.Create(session), "one producer owns a session");
    xg::PairReader reader;
    Check(reader.Open(session), "reader opens producer");
    xg::PairFrame frame;
    Check(reader.Read(frame) == xg::PairRead::Empty, "unpublished mapping has no picture");
    std::vector<uint8_t> pixels(16 * 8 * 8, 0x42);
    Check(!writer.Publish(15, 8, pixels, 1, 1) && !writer.Publish(16, 8, pixels, 1, 3), "invalid dimensions and unrelated frames rejected");
    Check(writer.Publish(16, 8, pixels, 1, 2), "publish complete pair");
    Check(reader.Read(frame) == xg::PairRead::NewPair && frame.rgba == pixels && frame.header.rightFrame == 2, "pair metadata and bytes");
    Check(reader.Read(frame, 1) == xg::PairRead::Unchanged, "unchanged pair not recopied");
    Check(reader.Read(frame, 1, frame.header.publishedMs + xg::PairMaxAgeMs + 1) == xg::PairRead::Stale,
        "staleness precedes unchanged detection");
    wchar_t executable[32768]{};
    Check(GetModuleFileNameW(nullptr, executable, 32768) != 0, "child path");
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --child " + session;
    STARTUPINFOW start{sizeof(start)}; PROCESS_INFORMATION process{};
    Check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &start, &process) != 0,
        "start transport test child");
    Check(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0, "child completed");
    DWORD code = 1;
    Check(GetExitCodeProcess(process.hProcess, &code) != 0 && code == 0, "child read real shared memory");
    CloseHandle(process.hThread); CloseHandle(process.hProcess);

    // Concurrent publication with resize and distinct byte patterns: a reader
    // must see an entire publication or a busy result, never mixed eye data.
    std::atomic<bool> finished{false};
    std::thread producer([&] {
        for (uint64_t i = 2; i < 1002; ++i)
        {
            const uint32_t width = (i & 1) ? 16 : 32;
            std::vector<uint8_t> data(size_t(width) * 8 * 8, static_cast<uint8_t>(i));
            while (!writer.Publish(width, 8, data, i, i)) SwitchToThread();
        }
        finished = true;
    });
    unsigned observed = 0;
    do
    {
        const auto result = reader.Read(frame);
        if (result != xg::PairRead::NewPair) continue;
        const uint8_t expected = frame.header.sequence == 1 ? 0x42 : static_cast<uint8_t>(frame.header.leftFrame);
        Check(frame.rgba.size() == size_t(frame.header.eyeWidth) * frame.header.eyeHeight * 8, "resized metadata matches bytes");
        for (uint8_t value : frame.rgba) Check(value == expected, "atomic publication contains no torn pixels");
        ++observed;
    } while (!finished);
    producer.join();
    Check(observed > 0, "concurrent reader observed a complete publication");

    // A short-lived lock holder must cause a whole-pair drop; consumers keep
    // their last complete snapshot and sequence, rather than a partial update.
    const std::wstring lockName = L"Local\\VRX.xgstereo." + session + L".lock";
    std::atomic<bool> holding{false}, release{false};
    std::thread holder([&] {
        HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, lockName.c_str());
        Check(mutex != nullptr, "open contention lock");
        {
            xg::PairLock lock(mutex, 1000);
            Check(lock.Ready(), "hold contention lock");
            holding = true;
            while (!release) SwitchToThread();
        }
        CloseHandle(mutex);
    });
    while (!holding) SwitchToThread();
    const auto oldPixels = frame.rgba;
    const auto oldSequence = frame.header.sequence;
    Check(reader.Read(frame) == xg::PairRead::Busy && frame.rgba == oldPixels && frame.header.sequence == oldSequence &&
        !writer.Publish(16, 8, pixels, 2000, 2000), "contention neither tears nor overwrites a complete snapshot");
    release = true; holder.join();

    // Test reader-side validation independently of the writer's input checks.
    const std::wstring mapName = L"Local\\VRX.xgstereo." + session + L".frames";
    HANDLE mutableMap = OpenFileMappingW(FILE_MAP_WRITE, FALSE, mapName.c_str());
    auto* header = static_cast<xg::PairHeader*>(MapViewOfFile(mutableMap, FILE_MAP_WRITE, 0, 0, xg::PairMappingBytes));
    HANDLE mutationLock = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, lockName.c_str());
    Check(mutableMap && header && mutationLock, "mutable metadata test fixture");
    const auto valid = *header;
    for (int corruption = 0; corruption < 4; ++corruption)
    {
        {
            xg::PairLock lock(mutationLock, 1000); Check(lock.Ready(), "lock metadata mutation");
            *header = valid;
            if (corruption == 0) header->version++;
            if (corruption == 1) header->pixelBytes = UINT32_MAX;
            if (corruption == 2) header->eyeWidth = UINT32_MAX;
            if (corruption == 3) header->stride++;
        }
        Check(reader.Read(frame) == xg::PairRead::Invalid && frame.rgba == oldPixels,
            "reader rejects invalid versions, oversized lengths, overflowing dimensions and stride");
    }
    {
        xg::PairLock lock(mutationLock, 1000); Check(lock.Ready(), "restore metadata"); *header = valid;
    }
    UnmapViewOfFile(header); CloseHandle(mutableMap); CloseHandle(mutationLock);
    writer.Close();
    Check(reader.Read(frame) == xg::PairRead::Closed, "producer shutdown invalidates the picture immediately");

    const auto abandonedSession = session + L"-abandoned";
    xg::PairWriter abandonedWriter;
    xg::PairReader abandonedReader;
    Check(abandonedWriter.Create(abandonedSession) && abandonedWriter.Publish(16, 8, pixels, 1, 1) &&
        abandonedReader.Open(abandonedSession), "abandonment fixture");
    std::thread interrupted([&] {
        const std::wstring name = L"Local\\VRX.xgstereo." + abandonedSession + L".lock";
        HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name.c_str());
        Check(mutex && WaitForSingleObject(mutex, 1000) == WAIT_OBJECT_0, "test owns producer lock");
        CloseHandle(mutex); // owning thread exits without releasing the mutex
    });
    interrupted.join();
    Check(abandonedReader.Read(frame) == xg::PairRead::Invalid && abandonedReader.Read(frame) == xg::PairRead::Invalid,
        "abandoned publication remains invalid across subsequent reads");
    std::puts("PASS: cross-process pairing, bounded dimensions, resize, concurrency, freshness, shutdown, abandoned mutex");
    return 0;
}
