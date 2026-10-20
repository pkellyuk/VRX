#include "render_support.h"
#include "same_frame.h"
#include "gpu_test_support.h"
#include <algorithm>
#include <memory>

using namespace xg::render_test;
int main(int argc, char** argv)
{
    try
    {
        const bool serve = argc == 2 && std::string(argv[1]).rfind("--serve-gpu=", 0) == 0;
        Require(argc == 1 || serve, "usage: same_frame_test [--serve-gpu=session]");
        const std::string session = serve ? std::string(argv[1]).substr(12) : "same-frame-" + std::to_string(GetCurrentProcessId());
        const std::wstring wide(session.begin(), session.end());
        SetEnvironmentVariableA("VRX_XG_EYE", nullptr); SetEnvironmentVariableA("VRX_XG_TRANSPORT", nullptr);
        if (!serve) {
            xg::ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        }
        Gpu gpu; gpu.Init(true, 1, false, true);
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; props.pNext = &id;
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gpu.gipa(gpu.instance, "vkGetPhysicalDeviceProperties2"))(gpu.physical, &props);
        LUID luid{}; std::memcpy(&luid, id.deviceLUID, sizeof(luid));
        xg::SameFrameProducer producer;
        Require(producer.Init(gpu.device, gpu.getDeviceProc, gpu.queue, gpu.family, luid, {Width, Height}, wide), "initialize same-frame shared GPU renderer");
        std::unique_ptr<xg::gpu_test::Snapshot> snapshot;
        if (!serve) {
            snapshot = std::make_unique<xg::gpu_test::Snapshot>(); snapshot->Init(luid, Width, Height, wide);
            Require(snapshot->reader.Header().kind == uint32_t(xg::GpuFrameKind::SameFrame), "transport explicitly identifies same-frame eyes");
        }
        const auto deadline = GetTickCount64() + (serve ? 120000 : 15000);
        uint64_t frame = 1;
        while (GetTickCount64() < deadline && (serve || frame <= 4))
        {
            // Advance simulation ONCE, then render both eyes from its snapshot.
            // Movement of two pixels per simulation step exposes mixed-frame
            // eyes even if their metadata were incorrectly labelled equal.
            const float simulationX = float((frame - 1) % 4) * .03125f;
            Scene left(gpu), right(gpu);
            left.Draw(true, VK_FORMAT_R8G8B8A8_UNORM, false, false, .125f, simulationX);
            right.Draw(true, VK_FORMAT_R8G8B8A8_UNORM, false, false, -.125f, simulationX);
            const bool submitted = producer.Submit(frame, left.command, left.image, right.command, right.image);
            // Serving uses the production drop-under-backpressure contract.
            // A busy ring during consumer reconnect must not kill the producer.
            Require(serve || submitted, "submit both eyes from one simulation snapshot");
            Require(!producer.Submit(frame, left.command, left.image, right.command, right.image), "reject repeated simulation frame");
            if (snapshot)
            {
                xg::PairFrame pair; const auto timeout = GetTickCount64() + 5000;
                while (!snapshot->Read(pair) && GetTickCount64() < timeout) Sleep(1);
                Require(pair.header.leftFrame == frame && pair.header.rightFrame == frame, "both eye IDs identify the same simulation step");
                std::vector<uint8_t> l(size_t(Width) * Height * 4), r(l.size());
                for (uint32_t y = 0; y < Height; ++y) {
                    std::memcpy(l.data() + size_t(y) * Width * 4, pair.rgba.data() + size_t(y) * Width * 8, Width * 4);
                    std::memcpy(r.data() + size_t(y) * Width * 4, pair.rgba.data() + size_t(y) * Width * 8 + Width * 4, Width * 4);
                }
                const double expected[] = {4, 2, 0}; const double center = 63.5 + simulationX * Width / 2;
                for (uint32_t post = 0; post < 3; ++post) {
                    const double a = Centroid(l, post), b = Centroid(r, post);
                    std::printf("same frame %llu, post %u: left %.2f, right %.2f, snapshot center %.2f\n", (unsigned long long)frame, post, a, b, center);
                    Require(std::abs(a - center - expected[post]) < .01 && std::abs(center - b - expected[post]) < .01, "stereo parallax and identical simulation positions in both eye images");
                }
            }
            Vk(gpu.vkDeviceWaitIdle(gpu.device), "fixture resource lifetime completes");
            ++frame;
            if (serve) Sleep(16);
        }
        Require(serve || frame == 5, "four moving same-frame pairs verified");
        std::puts("PASS: cooperative same-frame geometry, shared GPU pair, motion snapshot consistency, no alternate-frame pairing");
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
