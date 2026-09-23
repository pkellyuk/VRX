#include "live_depth.h"
#include "model_depth.h"
#include "block_motion.h"
#include "synthetic_scene.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <exception>
#include <utility>

namespace vrx {
LiveDepth::LiveDepth(const char* modelPath, bool cuda)
    : model_(std::make_unique<ModelDepth>(modelPath, cuda)) {
    worker_ = std::thread([this] { Work(); });
}
LiveDepth::~LiveDepth() {
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        stop_ = true;
    }
    input_ready_.notify_all();
    if (worker_.joinable()) worker_.join();
}
void LiveDepth::Submit(std::vector<float> input, uint64_t sequence, uint64_t layout, double arrival,
                       std::vector<float> grid) {
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        pending_.tensor = std::move(input);
        pending_.grid = std::move(grid);
        pending_.sequence = sequence;
        pending_.layout = layout;
        pending_.arrival = arrival;
        pending_.submitted = std::chrono::steady_clock::now();
        has_pending_ = true;
    }
    input_ready_.notify_one();
}
std::shared_ptr<const LiveDepth::Result> LiveDepth::Latest() const {
    return std::atomic_load_explicit(&latest_, std::memory_order_acquire);
}
std::string LiveDepth::Error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return error_;
}
LiveDepth::Timing LiveDepth::Timings() const {
    std::vector<std::array<double, 3>> samples;
    {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        samples = timing_samples_;
    }
    Timing result;
    result.samples = samples.size();
    if (samples.empty()) return result;
    auto percentile = [&](size_t channel, double fraction) {
        std::vector<double> values;
        values.reserve(samples.size());
        for (const auto& sample : samples) values.push_back(sample[channel]);
        std::sort(values.begin(), values.end());
        return values[size_t((values.size() - 1) * fraction)];
    };
    result.waitMedianMs = percentile(0, 0.5);
    result.waitP95Ms = percentile(0, 0.95);
    result.modelMedianMs = percentile(1, 0.5);
    result.modelP95Ms = percentile(1, 0.95);
    result.arrivalToDepthMedianMs = percentile(2, 0.5);
    result.arrivalToDepthP95Ms = percentile(2, 0.95);
    return result;
}
namespace {
// xrapp5's steadying state: the previous output (undilated) and its frame's luma.
struct SteadyState {
    bool have = false;
    uint64_t sequence = 0, layout = 0;
    fusion::Image output, luma;
};

// Steady depth, as xrapp5's PostProcessDepth: the previous output, moved by the
// motion from this frame to its frame, blended in where the moved picture matches.
// `grid` is the frame on the depth grid; the luma is taken from its RGBA bytes as
// xrapp5 takes it from its grid image.
bool SteadyDepth(SteadyState& state, std::vector<float>& near, const std::vector<float>& grid,
                 uint64_t sequence, uint64_t layout, float& trustMean) {
    constexpr int w = kSyntheticWidth, h = kSyntheticHeight;
    constexpr size_t plane = size_t(w) * h;
    if (near.size() != plane || grid.size() != 3 * plane) { state.have = false; return false; }
    std::vector<uint32_t> rgba(plane);
    auto byte = [](float value) { return uint32_t(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)); };
    for (size_t i = 0; i < plane; ++i)
        rgba[i] = byte(grid[i]) | (byte(grid[plane + i]) << 8) | (byte(grid[2 * plane + i]) << 16) | 0xff000000u;
    fusion::Image luma = fusion::LumaFromRgba(rgba.data(), w, h);
    fusion::Image out(w, h);
    out.v = std::move(near);
    if (state.layout != layout) state.have = false;   // a resized source: nothing lines up
    bool steadied = false;
    if (state.have && state.sequence != sequence) {
        const std::vector<int16_t> vectors = EstimateBlockMotion(luma, state.luma);
        const fusion::Motion motion = fusion::MotionFromVectors(vectors.data(), (w + fusion::MV_BLOCK - 1) / fusion::MV_BLOCK,
                                                                (h + fusion::MV_BLOCK - 1) / fusion::MV_BLOCK, w, h);
        const fusion::Image trust = fusion::MotionTrust(luma, state.luma, motion, &trustMean);
        out = fusion::Steady(out, fusion::Remap(state.output, motion), trust);
        steadied = true;
    }
    state.output = out;
    state.luma = std::move(luma);
    state.sequence = sequence;
    state.layout = layout;
    state.have = true;
    near = std::move(out.v);
    return steadied;
}
}

void LiveDepth::Work() {
    RangeSmoother smoother;
    SteadyState steadyState;
    Input input;
    try {
        while (true) {
            {
                std::unique_lock<std::mutex> lock(input_mutex_);
                input_ready_.wait(lock, [this] { return stop_ || has_pending_; });
                if (stop_) break;
                std::swap(input, pending_);
                has_pending_ = false;
            }
            const auto modelStart = std::chrono::steady_clock::now();
            auto result = std::make_shared<Result>();
            const bool steady = steady_ && !input.grid.empty();
            result->near = model_->Run(input.tensor.data(), false, &smoother, !steady);
            const auto modelEnd = std::chrono::steady_clock::now();
            if (steady) {
                const auto steadyStart = modelEnd;
                result->steadied = SteadyDepth(steadyState, result->near, input.grid, input.sequence,
                                               input.layout, result->motionTrust);
                ModelDepth::Dilate(result->near);
                result->steadyMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - steadyStart).count();
            } else {
                steadyState.have = false;
            }
            const auto depthEnd = std::chrono::steady_clock::now();
            result->modelMs = std::chrono::duration<double, std::milli>(modelEnd - modelStart).count();
            result->completedAt = std::chrono::duration<double>(depthEnd.time_since_epoch()).count();
            result->sourceSequence = input.sequence;
            result->sourceLayout = input.layout;
            result->captureArrival = input.arrival;
            std::atomic_store_explicit(&latest_, std::shared_ptr<const Result>(result),
                                       std::memory_order_release);
            const double waitMs = std::chrono::duration<double, std::milli>(modelStart - input.submitted).count();
            const double modelMs = std::chrono::duration<double, std::milli>(modelEnd - modelStart).count();
            const double arrivalToDepthMs =
                std::chrono::duration<double, std::milli>(depthEnd.time_since_epoch()).count() -
                input.arrival * 1000.0;
            {
                std::lock_guard<std::mutex> lock(timing_mutex_);
                timing_samples_.push_back({waitMs, modelMs, arrivalToDepthMs});
            }
            ++completed_;
        }
    } catch (const std::exception& error) {
        {
            std::lock_guard<std::mutex> lock(error_mutex_);
            error_ = error.what();
        }
        failed_ = true;
        std::fprintf(stderr, "Live ZipDepth stopped: %s\n", error.what());
    }
}
} // namespace vrx
