#include "source_ring.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

static void Check(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::abort(); }
}

static void TestOwnershipAndFences()
{
    SourceRing<2> ring;
    auto a = ring.Reserve(0, 0, 0);
    auto b = ring.Reserve(0, 0, 0);
    Check(a && b && a->index != b->index, "reservations must be exclusive");
    Check(!ring.Reserve(100, 100, 100), "held CPU references must prevent reuse");
    const int aIndex = a->index;
    const uint64_t aSeq = a->seq;
    ring.Publish(a, 10, 1.25);
    auto paired = ring.Latest();
    ring.MarkRead(paired, SourceRing<2>::Reader::Graphics, 20);
    ring.MarkRead(paired, SourceRing<2>::Reader::Model, 30);
    // A later registration of an earlier fence must not shorten protection.
    ring.MarkRead(paired, SourceRing<2>::Reader::Graphics, 2);
    ring.Publish(b, 11, 2.5);
    a.reset(); b.reset();
    Check(!ring.Reserve(100, 100, 100), "paired and latest publications retain their frames");
    Check(paired->seq == aSeq && paired->value == 10 && paired->time == 1.25,
        "new publications must not change retained frame identity");
    paired.reset();
    Check(!ring.Reserve(9, 20, 30), "producer GPU write must complete");
    Check(!ring.Reserve(10, 19, 30), "graphics GPU read must complete");
    Check(!ring.Reserve(10, 20, 29), "model GPU read must complete");
    auto recycled = ring.Reserve(10, 20, 30);
    Check(recycled && recycled->index == aIndex && recycled->seq > aSeq,
        "slot may be reused only after all three fences and CPU readers finish");
    recycled.reset(); // cancelled before GPU submission
    Check(bool(ring.Reserve(10, 20, 30)), "cancelled reservation should not leak a slot");
    Check(!ring.Reserve(UINT64_MAX, 100, 100), "device removal is not producer completion");
    Check(!ring.Reserve(100, UINT64_MAX, 100), "device removal is not graphics completion");
    Check(!ring.Reserve(100, 100, UINT64_MAX), "device removal is not model completion");
}

// The copy-engine hand-over reads the frame on its own timeline: the slot must
// stay protected until that copy completes, whatever the other timelines say.
static void TestTransferTimeline()
{
    SourceRing<2> ring;
    auto frame = ring.Reserve(0, 0, 0, 0);
    Check(bool(frame), "first reservation");
    const int first = frame->index;
    ring.Publish(frame, 5, 0);
    auto read = ring.Latest();
    frame.reset();
    ring.MarkRead(read, SourceRing<2>::Reader::Transfer, 40);
    ring.MarkRead(read, SourceRing<2>::Reader::Transfer, 3);   // earlier value must not shorten protection
    read.reset();
    auto next = ring.Reserve(5, 0, 0, 0);                       // the other slot; the first is transfer-protected
    Check(next && next->index != first, "a fresh slot is available while the copy is pending");
    ring.Publish(next, 6, 0);                                   // the first frame is no longer the latest
    next.reset();
    Check(!ring.Reserve(6, 100, 100, 39), "copy-engine read must complete before reuse");
    Check(!ring.Reserve(6, 100, 100), "callers without a transfer timeline cannot skip a recorded copy");
    Check(!ring.Reserve(6, 100, 100, UINT64_MAX), "device removal is not transfer completion");
    auto reused = ring.Reserve(6, 0, 0, 40);
    Check(reused && reused->index == first, "slot reusable once the copy completes");
}

static void TestLongLivedPairedFrame()
{
    SourceRing<8> ring;
    uint64_t pixels[8] = {};
    auto first = ring.Reserve(0, 0, 0);
    pixels[first->index] = first->seq;
    ring.Publish(first, 1, 0);
    auto paired = ring.Latest();
    first.reset();
    // Keep the paired frame alive for far longer than an eight-frame rotation.
    for (uint64_t i = 2; i <= 10000; ++i)
    {
        auto frame = ring.Reserve(i, i, i);
        Check(bool(frame), "other slots should keep capture progressing");
        pixels[frame->index] = frame->seq;
        ring.Publish(frame, i, static_cast<double>(i));
        Check(pixels[paired->index] == paired->seq, "paired colour changed underneath its depth");
    }
}

static void TestGpuBackpressure()
{
    SourceRing<8> ring;
    for (uint64_t i = 1; i <= 8; ++i)
    {
        auto frame = ring.Reserve(100, 0, 0);
        Check(bool(frame), "initial free slots");
        ring.Publish(frame, i, 0);
        ring.MarkRead(frame, SourceRing<8>::Reader::Graphics, i);
        ring.MarkRead(frame, SourceRing<8>::Reader::Model, i + 10);
    }
    for (int i = 0; i < 10000; ++i)
        Check(!ring.Reserve(100, 0, 0), "stalled GPU queues must cause drops, never overwrite");
    Check(!ring.Reserve(100, 8, 10), "graphics completion alone cannot free a model input");
    auto frame = ring.Reserve(100, 1, 11);
    Check(frame && frame->index == 0, "capture should resume when both readers release one slot");
}

static void TestConcurrentReaders()
{
    SourceRing<8> ring;
    std::atomic<uint64_t> pixels[8]{};
    std::atomic<bool> start{ false }, done{ false };
    std::atomic<uint64_t> reads{ 0 }, published{ 0 }, drops{ 0 };
    auto reader = [&](SourceRing<8>::Reader queue) {
        while (!start.load()) std::this_thread::yield();
        uint64_t localReads = 0;
        while (!done.load() || localReads < 1000)
        {
            auto frame = ring.Latest();
            if (!frame) { std::this_thread::yield(); continue; }
            const auto expected = frame->seq;
            Check(pixels[frame->index].load() == expected, "reader acquired wrong frame contents");
            for (int i = 0; i < 4; ++i) std::this_thread::yield();
            Check(pixels[frame->index].load() == expected, "producer overwrote a delayed CPU reader");
            ring.MarkRead(frame, queue, 0);
            ++localReads;
        }
        reads += localReads;
    };
    std::thread model(reader, SourceRing<8>::Reader::Model);
    std::thread graphics(reader, SourceRing<8>::Reader::Graphics);
    start = true;
    for (int i = 0; i < 100000; ++i)
    {
        auto frame = ring.Reserve(0, 0, 0);
        if (!frame) { ++drops; std::this_thread::yield(); continue; }
        pixels[frame->index] = frame->seq;
        ring.Publish(frame, 0, static_cast<double>(i));
        ++published;
    }
    done = true;
    model.join(); graphics.join();
    Check(published > 0 && reads >= 2000, "concurrent stress must exercise both readers");
    std::printf("Concurrent stress: %llu publications, %llu reads, %llu drops\n",
        (unsigned long long)published.load(), (unsigned long long)reads.load(),
        (unsigned long long)drops.load());
}

int main()
{
    TestTransferTimeline();
    TestOwnershipAndFences();
    TestLongLivedPairedFrame();
    TestGpuBackpressure();
    TestConcurrentReaders();
    std::puts("PASS: source ownership, four GPU timelines (incl. copy-engine transfer), long-lived pairing, backpressure, concurrent readers");
}
