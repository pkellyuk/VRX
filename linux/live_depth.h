#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vrx {
class PortalCapture;
class ModelDepth;
class LiveDepth {
public:
    struct Result {
        std::vector<float> near;
        uint64_t sourceSequence = 0;
        uint64_t sourceLayout = 0;
        double captureArrival = 0;
    };
    struct Timing {
        uint64_t samples = 0;
        double prepMedianMs = 0, prepP95Ms = 0;
        double modelMedianMs = 0, modelP95Ms = 0;
        double arrivalToDepthMedianMs = 0, arrivalToDepthP95Ms = 0;
    };
    LiveDepth(PortalCapture& capture, const char* modelPath, bool cuda);
    ~LiveDepth();
    LiveDepth(const LiveDepth&) = delete;
    LiveDepth& operator=(const LiveDepth&) = delete;
    std::shared_ptr<const Result> Latest() const;
    bool Healthy() const { return !failed_; }
    std::string Error() const;
    uint64_t Completed() const { return completed_; }
    Timing Timings() const;
private:
    void Work();
    PortalCapture& capture_;
    std::unique_ptr<ModelDepth> model_;
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
