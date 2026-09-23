#include "live_depth.h"
#include "model_depth.h"
#include "model_prep_cpu.h"
#include "portal_capture.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <exception>
#include <utility>

namespace vrx {
LiveDepth::LiveDepth(PortalCapture& capture, const char* modelPath, bool cuda)
    : capture_(capture), model_(std::make_unique<ModelDepth>(modelPath, cuda)) {
    worker_ = std::thread([this] { Work(); });
}
LiveDepth::~LiveDepth() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
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
    result.prepMedianMs = percentile(0, 0.5);
    result.prepP95Ms = percentile(0, 0.95);
    result.modelMedianMs = percentile(1, 0.5);
    result.modelP95Ms = percentile(1, 0.95);
    result.arrivalToDepthMedianMs = percentile(2, 0.5);
    result.arrivalToDepthP95Ms = percentile(2, 0.95);
    return result;
}
void LiveDepth::Work() {
    uint64_t lastSequence = 0;
    RangeSmoother smoother;
    try {
        while (!stop_) {
            if (capture_.Captured() <= lastSequence) {
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
                continue;
            }
            PortalCapture::Frame source;
            if (!capture_.Latest(source) || source.sequence <= lastSequence) continue;
            lastSequence = source.sequence;
            const auto prepStart = std::chrono::steady_clock::now();
            auto input = PrepareModelInput(source.rgb);
            const auto modelStart = std::chrono::steady_clock::now();
            auto result = std::make_shared<Result>();
            result->near = model_->Run(input.data(), false, &smoother);
            const auto modelEnd = std::chrono::steady_clock::now();
            result->sourceSequence = source.sequence;
            result->sourceLayout = source.layout;
            result->captureArrival = source.arrival;
            std::atomic_store_explicit(&latest_, std::shared_ptr<const Result>(result),
                                       std::memory_order_release);
            const double prepMs = std::chrono::duration<double, std::milli>(modelStart - prepStart).count();
            const double modelMs = std::chrono::duration<double, std::milli>(modelEnd - modelStart).count();
            const double arrivalToDepthMs =
                std::chrono::duration<double, std::milli>(modelEnd.time_since_epoch()).count() -
                source.arrival * 1000.0;
            {
                std::lock_guard<std::mutex> lock(timing_mutex_);
                timing_samples_.push_back({prepMs, modelMs, arrivalToDepthMs});
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
