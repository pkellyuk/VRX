#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vrx {
class ModelDepth;
// Runs ZipDepth on a worker thread over model inputs the renderer prepares on
// the GPU, and publishes the newest completed depth map without blocking.
class LiveDepth {
public:
    struct Result {
        std::vector<float> near;
        uint64_t sourceSequence = 0;
        uint64_t sourceLayout = 0;
        double captureArrival = 0;
        double modelMs = 0;          // ZipDepth and its postprocessing for this map
        double completedAt = 0;      // steady-clock seconds, as captureArrival
    };
    struct Timing {
        uint64_t samples = 0;
        double waitMedianMs = 0, waitP95Ms = 0;   // hand-over to model start
        double modelMedianMs = 0, modelP95Ms = 0;
        double arrivalToDepthMedianMs = 0, arrivalToDepthP95Ms = 0;
    };
    LiveDepth(const char* modelPath, bool cuda);
    ~LiveDepth();
    LiveDepth(const LiveDepth&) = delete;
    LiveDepth& operator=(const LiveDepth&) = delete;
    // Hand over a prepared 3 x 384 x 672 input for a source frame. An input
    // the worker has not started yet is replaced.
    void Submit(std::vector<float> input, uint64_t sequence, uint64_t layout, double arrival);
    std::shared_ptr<const Result> Latest() const;
    bool Healthy() const { return !failed_; }
    std::string Error() const;
    uint64_t Completed() const { return completed_; }
    Timing Timings() const;
private:
    void Work();
    struct Input {
        std::vector<float> tensor;
        uint64_t sequence = 0, layout = 0;
        double arrival = 0;
        std::chrono::steady_clock::time_point submitted;
    };
    std::unique_ptr<ModelDepth> model_;
    std::mutex input_mutex_;
    std::condition_variable input_ready_;
    Input pending_;
    bool has_pending_ = false;
    std::thread worker_;
    std::atomic<bool> stop_{false}, failed_{false};
    std::atomic<uint64_t> completed_{0};
    mutable std::mutex error_mutex_;
    std::string error_;
    std::shared_ptr<const Result> latest_;
    mutable std::mutex timing_mutex_;
    std::vector<std::array<double, 3>> timing_samples_;
};
} // namespace vrx
