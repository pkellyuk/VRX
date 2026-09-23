#include "model_depth.h"
#include "synthetic_scene.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace vrx {
namespace {
constexpr int modelW = 672, modelH = 384;
constexpr size_t modelPixels = size_t(modelW) * modelH;

std::vector<float> ResampleDepth(const float* raw) {
    std::vector<float> grid(size_t(kSyntheticWidth) * kSyntheticHeight);
    for (int y = 0; y < kSyntheticHeight; ++y) {
        const float fy = std::clamp((y + 0.5f) / kSyntheticHeight * modelH - 0.5f,
                                    0.0f, float(modelH - 1));
        const int y0 = int(fy), y1 = std::min(y0 + 1, modelH - 1);
        const float ty = fy - y0;
        for (int x = 0; x < kSyntheticWidth; ++x) {
            const float fx = std::clamp((x + 0.5f) / kSyntheticWidth * modelW - 0.5f,
                                        0.0f, float(modelW - 1));
            const int x0 = int(fx), x1 = std::min(x0 + 1, modelW - 1);
            const float tx = fx - x0;
            const float top = raw[y0 * modelW + x0] + (raw[y0 * modelW + x1] - raw[y0 * modelW + x0]) * tx;
            const float bottom = raw[y1 * modelW + x0] + (raw[y1 * modelW + x1] - raw[y1 * modelW + x0]) * tx;
            grid[size_t(y) * kSyntheticWidth + x] = top + (bottom - top) * ty;
        }
    }
    return grid;
}

void NormalizeNear(std::vector<float>& values, bool verbose, RangeSmoother* smoother) {
    const auto [minIt, maxIt] = std::minmax_element(values.begin(), values.end());
    const float minimum = *minIt, maximum = *maxIt;
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || maximum - minimum < 1e-6f)
        throw std::runtime_error("Invalid ZipDepth output range");
    float low = 0.0f, high = 1.0f;
    const double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    DepthRange(values, smoother, now, low, high);
    const float inverse = 1.0f / (high - low);
    for (float& value : values) value = std::clamp((value - low) * inverse, 0.0f, 1.0f);
    if (verbose) std::printf("ZipDepth normalization: %.6f..%.6f -> 0..1\n", low, high);
}

void DilateNear(std::vector<float>& values) {
    constexpr int w = kSyntheticWidth, h = kSyntheticHeight, radius = 3;
    std::vector<float> source = values;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float maximum = 0;
            for (int dx = -radius; dx <= radius; ++dx)
                maximum = std::max(maximum, source[size_t(y) * w + std::clamp(x + dx, 0, w - 1)]);
            values[size_t(y) * w + x] = maximum;
        }
    source = values;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float maximum = 0;
            for (int dy = -radius; dy <= radius; ++dy)
                maximum = std::max(maximum, source[size_t(std::clamp(y + dy, 0, h - 1)) * w + x]);
            values[size_t(y) * w + x] = maximum;
        }
}
}

ModelDepth::ModelDepth(const char* modelPath, bool requireCuda)
    : environment_(ORT_LOGGING_LEVEL_WARNING, "vrx-linux"), memory_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)) {
    const auto providers = Ort::GetAvailableProviders();
    const bool cuda = std::find(providers.begin(), providers.end(), "CUDAExecutionProvider") != providers.end();
    if (requireCuda && !cuda) throw std::runtime_error("ONNX Runtime CUDA provider unavailable");
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(4);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    if (requireCuda) {
        OrtCUDAProviderOptions cudaOptions{};
        cudaOptions.device_id = 0;
        options.AppendExecutionProvider_CUDA(cudaOptions);
        options.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
    }
    session_ = Ort::Session(environment_, modelPath, options);
    if (session_.GetInputCount() != 1 || session_.GetOutputCount() != 1)
        throw std::runtime_error("ZipDepth must have one input and one output");
    Ort::AllocatorWithDefaultOptions allocator;
    auto inputName = session_.GetInputNameAllocated(0, allocator);
    auto outputName = session_.GetOutputNameAllocated(0, allocator);
    auto inputType = session_.GetInputTypeInfo(0);
    auto outputType = session_.GetOutputTypeInfo(0);
    if (std::string(inputName.get()) != "image" ||
        std::string(outputName.get()) != "depth" ||
        inputType.GetTensorTypeAndShapeInfo().GetShape() != std::vector<int64_t>{1, 3, modelH, modelW} ||
        inputType.GetTensorTypeAndShapeInfo().GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
        outputType.GetTensorTypeAndShapeInfo().GetElementCount() != modelPixels ||
        outputType.GetTensorTypeAndShapeInfo().GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        throw std::runtime_error("ZipDepth shape/names do not match VRX");
    std::printf("ZipDepth model loaded with %s provider\n", requireCuda ? "CUDA" : "CPU");
}

void ModelDepth::Dilate(std::vector<float>& near) { DilateNear(near); }

std::vector<float> ModelDepth::Run(const float* nchw, bool verbose, RangeSmoother* smoother, bool dilate) {
    if (!nchw) throw std::runtime_error("Null ZipDepth input");
    const int64_t shape[4] = {1, 3, modelH, modelW};
    auto input = Ort::Value::CreateTensor<float>(memory_, const_cast<float*>(nchw), 3 * modelPixels, shape, 4);
    const char* inputs[1] = {"image"}, *outputs[1] = {"depth"};
    const auto start = std::chrono::steady_clock::now();
    auto result = session_.Run(Ort::RunOptions{nullptr}, inputs, &input, 1, outputs, 1);
    const double milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    if (result.size() != 1 || !result[0].IsTensor() ||
        result[0].GetTensorTypeAndShapeInfo().GetElementCount() != modelPixels ||
        result[0].GetTensorTypeAndShapeInfo().GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        throw std::runtime_error("ZipDepth returned an invalid tensor");
    std::vector<float> near = ResampleDepth(result[0].GetTensorData<float>());
    NormalizeNear(near, verbose, smoother);
    if (dilate) DilateNear(near);
    if (verbose) std::printf("ZipDepth inference: %.1f ms, normalized depth %dx%d\n",
                             milliseconds, kSyntheticWidth, kSyntheticHeight);
    return near;
}
} // namespace vrx
