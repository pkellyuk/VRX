// Real Vulkan offscreen tests and the static reference producer.
#include "render_support.h"


using namespace xg::render_test;

int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string(argv[1]).rfind("--serve=", 0) == 0)
        {
            const std::string name = std::string(argv[1]).substr(8);
            const std::wstring session(name.begin(), name.end());
            xg::PairWriter writer;
            Require(writer.Create(session), "create demo session (name must be valid and unused)");
            const auto left = Render("left", true), right = Render("right", true);
            std::vector<uint8_t> pair(size_t(Width) * Height * 8);
            for (uint32_t y = 0; y < Height; ++y)
            {
                std::memcpy(pair.data() + size_t(y) * Width * 8, left.data() + size_t(y) * Width * 4, Width * 4);
                std::memcpy(pair.data() + size_t(y) * Width * 8 + Width * 4, right.data() + size_t(y) * Width * 4, Width * 4);
            }
            std::printf("Serving static geometry test scene '%s' for 120 s; VRX: --geometry-session=%s\n", name.c_str(), name.c_str());
            std::fflush(stdout);
            const auto deadline = GetTickCount64() + 120000;
            while (GetTickCount64() < deadline)
            {
                // These are the same immutable simulation frame, rendered twice
                // in this process through the fixed-eye layer. No game copies.
                writer.Publish(Width, Height, pair, 1, 1);
                Sleep(16);
            }
            return 0;
        }
        Require(argc == 1, "usage: render_test.exe [--serve=session]");
        for (const bool block : {false, true})
        {
            const auto mono = Render(nullptr, block), left = Render("left", block), right = Render("right", block);
            const double expected[3] = {4, 2, 0};
            for (uint32_t post = 0; post < 3; ++post)
            {
                const double center = Centroid(mono, post), l = Centroid(left, post), r = Centroid(right, post);
                std::printf("%s post %u: mono %.2f left %.2f right %.2f (expected shifts +/-%.2f px)\n",
                    block ? "block" : "direct", post, center, l, r, expected[post]);
                Require(std::abs(center - 63.5) < .01, "baseline geometry differs");
                Require(std::abs(l - center - expected[post]) < .01 && std::abs(center - r - expected[post]) < .01,
                    "eye parallax or HUD isolation differs from geometry");
            }
            SavePair(block ? "xgstereo-block-sbs.ppm" : "xgstereo-direct-sbs.ppm", left, right);
        }
        std::puts("PASS: explicit Windows Vulkan layer produces expected near/far parallax; HUD stays fixed");
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
