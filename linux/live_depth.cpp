#include "live_depth.h"
#include "model_depth.h"
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
void LiveDepth::Submit(std::vector<float> input, uint64_t sequence, uint64_t layout, double arrival) {
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        pending_.tensor = std::move(input);
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
void LiveDepth::Work() {
    RangeSmoother smoother;
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
            result->near = model_->Run(input.tensor.data(), false, &smoother);
            const auto modelEnd = std::chrono::steady_clock::now();
            result->modelMs = std::chrono::duration<double, std::milli>(modelEnd - modelStart).count();
            result->sourceSequence = input.sequence;
            result->sourceLayout = input.layout;
            result->captureArrival = input.arrival;
            std::atomic_store_explicit(&latest_, std::shared_ptr<const Result>(result),
                                       std::memory_order_release);
            const double waitMs = std::chrono::duration<double, std::milli>(modelStart - input.submitted).count();
            const double modelMs = std::chrono::duration<double, std::milli>(modelEnd - modelStart).count();
            const double arrivalToDepthMs =
                std::chrono::duration<double, std::milli>(modelEnd.time_since_epoch()).count() -
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
