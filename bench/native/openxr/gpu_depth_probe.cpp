#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "depth_fusion.h"
#include "foreground_refinement.h"
#include "gpu_depth.h"
#include "motion_estimator.h"
#include <cstdio>
#include <dxgi1_6.h>
#include <windows.h>
static void Put(gpudepth::Processor &p, ID3D12Resource *out,
                const std::vector<float> &values) {
  auto up = gpudepth::Buffer(p.work.device.Get(), values.size() * 4,
                             D3D12_HEAP_TYPE_UPLOAD);
  void *m = nullptr;
  gpudepth::Check(up->Map(0, nullptr, &m), "upload map");
  memcpy(m, values.data(), values.size() * 4);
  up->Unmap(0, nullptr);
  p.work.Begin();
  p.work.Copy(out, up.Get(), values.size() * 4,
              D3D12_RESOURCE_STATE_GENERIC_READ);
  p.work.Submit();
  p.work.Wait();
}
static bool Compare(const char *name, const std::vector<float> &a,
                    const std::vector<float> &b, float limit) {
  double mean = 0;
  float worst = 0;
  for (size_t i = 0; i < a.size(); i++) {
    float d = fabs(a[i] - b[i]);
    if (!std::isfinite(a[i]))
      return false;
    mean += d;
    worst = std::max(worst, d);
  }
  mean /= a.size();
  printf("%s: mean %.9f max %.9f %s\n", name, mean, worst,
         worst <= limit ? "PASS" : "FAIL");
  return worst <= limit;
}

static bool CheckCrop(gpudepth::Processor &p, const char *name,
                      const std::vector<float> &base,
                      const std::vector<float> &local, const DepthCrop &crop,
                      bool shouldAccept, bool shouldChange = false) {
  auto expected = base;
  const bool cpuAccepted =
      FuseForeground(expected, local, p.width, p.height, crop);
  Put(p, p.raw.Get(), local);
  Put(p, p.nearMap.Get(), base);
  p.work.Begin();
  auto c = p.Size();
  c.count = 4;
  p.Dispatch("Clear", c, {}, {p.range.Get()});
  p.Dispatch("RangeReduce", c, {p.raw.Get()}, {p.reduction.Get()});
  c.count = (p.width * p.height + 255) / 256;
  p.Dispatch("RangeFinish", c, {p.reduction.Get()}, {p.scratch[13].Get()});
  c.p0 = crop.x;
  c.p1 = crop.y;
  c.p2 = crop.size;
  p.Dispatch("CropFit", c, {p.raw.Get(), p.nearMap.Get(), p.scratch[13].Get()},
             {p.scratch[15].Get()});
  p.Dispatch("CropApply", c,
             {p.raw.Get(), p.nearMap.Get(), p.scratch[15].Get()},
             {p.scratch[14].Get()});
  p.work.Copy(p.nearMap.Get(), p.scratch[14].Get(),
              base.size() * sizeof(float));
  p.Finish(0, 0, true);
  p.work.Submit();
  const auto summary = p.ReadSummary();
  const auto actual = p.Download(p.nearMap.Get());
  float changed = 0;
  for (size_t i = 0; i < actual.size(); ++i)
    changed = std::max(changed, std::fabs(actual[i] - base[i]));
  const bool status = cpuAccepted == shouldAccept &&
                      (summary.cropAccepted != 0) == shouldAccept &&
                      (!shouldChange || changed > .001f);
  printf("%s status: CPU %d GPU %u, maximum change %.7f %s\n", name,
         cpuAccepted, summary.cropAccepted, changed, status ? "PASS" : "FAIL");
  // A rejected crop must be exactly unchanged, including outside the crop.
  return Compare(name, actual, expected, shouldAccept ? 2e-4f : 0.f) && status;
}

int main(int argc, char **argv) {
  try {
    gpudepth::ComPtr<IDXGIFactory4> factory;
    gpudepth::Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    gpudepth::ComPtr<IDXGIAdapter1> adapter;
    const int adapterIndex = argc > 1 ? atoi(argv[1]) : 0;
    gpudepth::Check(factory->EnumAdapters1(adapterIndex, &adapter), "adapter");
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    wprintf(L"GPU: %s\n", desc.Description);
    gpudepth::ComPtr<ID3D12Device> dev;
    gpudepth::Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                      IID_PPV_ARGS(&dev)),
                    "device");
    const int w = 686, h = 392;
    gpudepth::Processor p;
    p.Init(dev.Get(), w, h);
    std::vector<float> z(w * h), anchor(w * h), luma(w * h), previous(w * h);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        int i = y * w + x;
        z[i] = .15f + .65f * x / (w - 1) + .07f * sinf(y * .04f);
        anchor[i] =
            std::clamp(.65f * z[i] + .15f + .06f * sinf(x * .027f), 0.f, 1.f);
        luma[i] = float((x * 7 + y * 3) % 256);
        previous[i] = std::clamp(z[i] + .02f * sinf(x * .05f), 0.f, 1.f);
      }
    Put(p, p.raw.Get(), z);
    p.work.Begin();
    p.Normalize(p.raw.Get(), w, h, .03f, .4f, false);
    p.Finish(0, 0);
    p.work.Submit();
    auto sm = p.ReadSummary();
    auto norm = p.Download(p.nearMap.Get());
    std::vector<float> expected = z;
    auto mm = std::minmax_element(z.begin(), z.end());
    for (auto &v : expected)
      v = (v - *mm.first) / (*mm.second - *mm.first);
    bool ok = Compare("normalize", norm, expected, 2e-5f);
    const int pitch = ((w + 7) / 8 * 4 + 255) & ~255;
    auto vectors = gpudepth::Buffer(dev.Get(), pitch * ((h + 7) / 8));
    Put(p, vectors.Get(), std::vector<float>(pitch * ((h + 7) / 8) / 4));
    Put(p, p.nearMap.Get(), z);
    Put(p, p.previous.Get(), previous);
    Put(p, p.luma.Get(), luma);
    Put(p, p.previousLuma.Get(), luma);
    p.work.Begin();
    p.Steady(vectors.Get(), pitch, (w + 7) / 8, (h + 7) / 8);
    p.work.Submit();
    auto stable = p.Download(p.nearMap.Get());
    for (size_t i = 0; i < expected.size(); i++)
      expected[i] = .5f * (z[i] + previous[i]);
    ok = Compare("steady identity", stable, expected, 3e-5f) && ok;
    std::vector<int16_t> cpuVectors((w + 7) / 8 * ((h + 7) / 8) * 2);
    std::vector<float> vectorBytes(pitch * ((h + 7) / 8) / 4);
    for (int y = 0; y < (h + 7) / 8; y++)
      for (int x = 0; x < (w + 7) / 8; x++) {
        int16_t pair[2] = {-24, -12};
        memcpy(&vectorBytes[y * pitch / 4 + x], pair, 4);
        cpuVectors[(y * ((w + 7) / 8) + x) * 2] = -24;
        cpuVectors[(y * ((w + 7) / 8) + x) * 2 + 1] = -12;
      }
    Put(p, vectors.Get(), vectorBytes);
    fusion::Image refLuma(w, h), currentLuma(w, h), previousImage(w, h),
        currentImage(w, h);
    refLuma.v = luma;
    previousImage.v = previous;
    currentImage.v = z;
    const auto mv = fusion::MotionFromVectors(cpuVectors.data(), (w + 7) / 8,
                                              (h + 7) / 8, w, h);
    currentLuma = fusion::Remap(refLuma, mv);
    for (auto &v : currentLuma.v)
      v = roundf(v);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w / 3; x++)
        currentLuma.v[y * w + x] = 255 - currentLuma.v[y * w + x];
    const auto trust = fusion::MotionTrust(currentLuma, refLuma, mv);
    const auto movedPrevious = fusion::Remap(previousImage, mv);
    const auto cpuSteady = fusion::Steady(currentImage, movedPrevious, trust);
    Put(p, p.nearMap.Get(), z);
    Put(p, p.previous.Get(), previous);
    Put(p, p.luma.Get(), currentLuma.v);
    Put(p, p.previousLuma.Get(), luma);
    p.work.Begin();
    p.Steady(vectors.Get(), pitch, (w + 7) / 8, (h + 7) / 8);
    p.work.Submit();
    ok = Compare("steady translated partial trust", p.Download(p.nearMap.Get()),
                 cpuSteady.v, 5e-5f) &&
         ok;
    fusion::Image zi(w, h), ai(w, h);
    zi.v = z;
    ai.v = anchor;
    float ga, gb;
    fusion::GlobalFit(zi, ai, ga, gb);
    auto cpu = fusion::Fuse(zi, ai, {}, ga, gb);
    Put(p, p.nearMap.Get(), z);
    Put(p, p.anchor.Get(), anchor);
    p.work.Begin();
    p.Fit(p.nearMap.Get(), p.anchor.Get(), p.anchorFit.Get());
    p.Fuse(nullptr, 0, 0, 0, true, true);
    p.work.Submit();
    auto fused = p.Download(p.nearMap.Get());
    ok = Compare("fusion same frame", fused, cpu.v, 2e-3f) && ok;
    Put(p, p.nearMap.Get(), z);
    Put(p, p.anchorLuma.Get(), luma);
    const auto movedAnchor = fusion::Remap(ai, mv);
    const auto cpuMovedFusion = fusion::Fuse(zi, movedAnchor, trust, ga, gb);
    p.work.Begin();
    p.Fuse(vectors.Get(), pitch, (w + 7) / 8, (h + 7) / 8, false, true);
    p.work.Submit();
    ok = Compare("fusion translated partial trust", p.Download(p.nearMap.Get()),
                 cpuMovedFusion.v, 2e-3f) &&
         ok;
    // Constant maps reset the temporal normalization range; the following range
    // must take effect immediately rather than fading from a previous frame.
    Put(p, p.raw.Get(), std::vector<float>(w * h, .42f));
    p.work.Begin();
    p.Normalize(p.raw.Get(), w, h, .01f, .4f, true);
    p.Finish(0, 0);
    p.work.Submit();
    auto flat = p.ReadSummary();
    ok = flat.lo == flat.hi &&
         Compare("constant map", p.Download(p.nearMap.Get()),
                 std::vector<float>(w * h), 1e-6f) &&
         ok;
    Put(p, p.raw.Get(), z);
    p.work.Begin();
    p.Normalize(p.raw.Get(), w, h, .0001f, .4f, true);
    p.Finish(0, 0);
    p.work.Submit();
    auto afterFlat = p.ReadSummary();
    bool reset = afterFlat.hi - afterFlat.lo > .5f;
    printf("constant-to-range reset: %s\n", reset ? "PASS" : "FAIL");
    ok = reset && ok;
    auto invalid = z;
    invalid[100] = NAN;
    Put(p, p.raw.Get(), invalid);
    p.work.Begin();
    p.Normalize(p.raw.Get(), w, h, .01f, .4f, true);
    p.Finish(0, 0);
    p.work.Submit();
    auto invalidSummary = p.ReadSummary();
    printf("nonfinite status: %u %s\n", invalidSummary.invalid,
           invalidSummary.invalid ? "PASS" : "FAIL");
    ok = invalidSummary.invalid && ok;
    // Foreground crop alignment uses a different coordinate system from the
    // full map. Test a non-central half-size crop, actual detail changes, and
    // all rejection paths against the executable CPU reference.
    const DepthCrop crop{.23f, .17f, .5f};
    std::vector<float> local(w * h);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        local[y * w + x] =
            (SampleDepth(z, w, h, crop.x + (x + .5f) / w * crop.size,
                         crop.y + (y + .5f) / h * crop.size) -
             .12f) /
            .63f;
    ok = CheckCrop(p, "foreground affine crop", z, local, crop, true) && ok;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        local[y * w + x] += .04f * std::sin(x * .07f) * std::sin(y * .1f);
    ok = CheckCrop(p, "foreground detailed crop", z, local, crop, true, true) &&
         ok;
    ok = CheckCrop(p, "foreground flat rejection", z,
                   std::vector<float>(w * h, .42f), crop, false) &&
         ok;
    auto negative = local;
    for (auto &v : negative)
      v = 1.f - v;
    ok = CheckCrop(p, "foreground negative-correlation rejection", z, negative,
                   crop, false) &&
         ok;
    local[100] = NAN; // deliberately outside the fit's sparse sample points
    ok =
        CheckCrop(p, "foreground nonfinite rejection", z, local, crop, false) &&
        ok;
    // Round-trip a changing GPU buffer through a cross-adapter shared heap.
    gpudepth::ComPtr<IDXGIAdapter1> otherAdapter;
    const HRESULT otherFound =
        factory->EnumAdapters1(adapterIndex == 0 ? 1 : 0, &otherAdapter);
    if (otherFound == DXGI_ERROR_NOT_FOUND) {
      printf("cross GPU transfer SKIP: only one adapter available\n");
      printf("GPU depth probe %s\n", ok ? "PASS" : "FAIL");
      return ok ? 0 : 1;
    }
    gpudepth::Check(otherFound, "other adapter");
    gpudepth::ComPtr<ID3D12Device> other;
    gpudepth::Check(D3D12CreateDevice(otherAdapter.Get(),
                                      D3D_FEATURE_LEVEL_11_0,
                                      IID_PPV_ARGS(&other)),
                    "other device");
    auto otherBuffer = gpudepth::Buffer(other.Get(), w * h * 4);
    gpudepth::Bridge outgoing, incoming;
    outgoing.Init(dev.Get(), other.Get(), w * h * 4);
    incoming.Init(other.Get(), dev.Get(), w * h * 4);
    Put(p, p.raw.Get(), z);
    outgoing.Transfer(p.raw.Get(), otherBuffer.Get(), w * h * 4);
    incoming.Transfer(otherBuffer.Get(), p.nearMap.Get(), w * h * 4);
    ok = Compare("cross GPU return", p.Download(p.nearMap.Get()), z, 0) && ok;
    Put(p, p.raw.Get(), anchor);
    outgoing.Transfer(p.raw.Get(), otherBuffer.Get(), w * h * 4);
    incoming.Transfer(otherBuffer.Get(), p.nearMap.Get(), w * h * 4);
    ok = Compare("cross GPU changing input", p.Download(p.nearMap.Get()),
                 anchor, 0) &&
         ok;
    printf("GPU depth probe %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
  } catch (const std::exception &e) {
    printf("FAIL: %s\n", e.what());
    return 1;
  }
}
