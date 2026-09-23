// Checks the packaged ZipDepth ONNX contract and runs one CPU inference pass.
// CUDA provider validation is reported separately; no silent GPU fallback.
#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::puts("vrx-model-probe [--cuda] [zipdepth_faithful_fp16_672x384.onnx]");
        return 0;
    }
    bool requireCuda = false;
    const char* model = "bench/models/zipdepth_faithful_fp16_672x384.onnx";
    bool modelProvided = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--cuda") requireCuda = true;
        else if (argv[i][0] != '-' && !modelProvided) { model = argv[i]; modelProvided = true; }
        else {
            std::fputs("usage: vrx-model-probe [--cuda] [model.onnx]\n", stderr);
            return 2;
        }
    }
    try {
        std::printf("ONNX Runtime: %s\n", OrtGetApiBase()->GetVersionString());
        const auto providers = Ort::GetAvailableProviders();
        for (const auto& provider : providers) std::printf("Provider: %s\n", provider.c_str());
        const bool cuda = std::find(providers.begin(), providers.end(), "CUDAExecutionProvider") != providers.end();
        std::printf("CUDA provider: %s\n", cuda ? "available" : "unavailable");
        if (requireCuda && !cuda) throw std::runtime_error("CUDA provider unavailable");
        Ort::Env environment(ORT_LOGGING_LEVEL_WARNING, "vrx-model-probe");
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(4);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        if (requireCuda) {
            OrtCUDAProviderOptions cudaOptions{};
            cudaOptions.device_id = 0;
            options.AppendExecutionProvider_CUDA(cudaOptions);
            options.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
        }
        Ort::Session session(environment, model, options);
        Ort::AllocatorWithDefaultOptions allocator;
        if (session.GetInputCount() != 1 || session.GetOutputCount() != 1)
            throw std::runtime_error("ZipDepth must have exactly one input and one output");
        auto inputName = session.GetInputNameAllocated(0, allocator);
        auto outputName = session.GetOutputNameAllocated(0, allocator);
        auto inputType = session.GetInputTypeInfo(0);
        auto outputType = session.GetOutputTypeInfo(0);
        auto inputInfo = inputType.GetTensorTypeAndShapeInfo();
        auto outputInfo = outputType.GetTensorTypeAndShapeInfo();
        auto inputShape = inputInfo.GetShape();
        auto outputShape = outputInfo.GetShape();
        if (std::string(inputName.get()) != "image" || std::string(outputName.get()) != "depth" ||
            inputShape != std::vector<int64_t>{1, 3, 384, 672} ||
            inputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            throw std::runtime_error("ZipDepth input/output contract does not match VRX");
        std::printf("Model: %s\n", model);
        std::printf("Input: %s [%lld,%lld,%lld,%lld] float32\n", inputName.get(),
                    static_cast<long long>(inputShape[0]), static_cast<long long>(inputShape[1]),
                    static_cast<long long>(inputShape[2]), static_cast<long long>(inputShape[3]));
        std::printf("Output: %s shape", outputName.get());
        for (int64_t dimension : outputShape) std::printf(" %lld", static_cast<long long>(dimension));
        std::putchar('\n');
        constexpr size_t plane = 672u * 384u;
        std::vector<float> input(3 * plane);
        for (int y = 0; y < 384; ++y)
            for (int x = 0; x < 672; ++x) {
                const size_t index = size_t(y) * 672 + x;
                input[index] = float(x) / 671.0f;
                input[plane + index] = float(y) / 383.0f;
                input[2 * plane + index] = 0.5f;
            }
        Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(),
                                                            inputShape.data(), inputShape.size());
        const char* inputs[] = {inputName.get()};
        const char* outputs[] = {outputName.get()};
        std::vector<Ort::Value> result;
        double elapsed = 0.0;
        const int passes = requireCuda ? 3 : 1;
        for (int pass = 0; pass < passes; ++pass) {
            const auto start = std::chrono::steady_clock::now();
            result = session.Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
            elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            std::printf("%s inference pass %d: %.1f ms\n",
                        requireCuda ? "CUDA" : "CPU", pass + 1, elapsed);
        }
        if (result.size() != 1 || !result[0].IsTensor()) throw std::runtime_error("ZipDepth returned no tensor");
        auto resultInfo = result[0].GetTensorTypeAndShapeInfo();
        if (resultInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            resultInfo.GetElementCount() != plane)
            throw std::runtime_error("ZipDepth returned an unexpected depth tensor");
        const float* depth = result[0].GetTensorData<float>();
        size_t nonfinite = 0;
        float lowest = INFINITY, highest = -INFINITY;
        for (size_t i = 0; i < plane; ++i) {
            if (!std::isfinite(depth[i])) { ++nonfinite; continue; }
            lowest = std::min(lowest, depth[i]);
            highest = std::max(highest, depth[i]);
        }
        std::printf("Depth range %.6f..%.6f, nonfinite %zu\n", lowest, highest, nonfinite);
        if (requireCuda) {
            Ort::SessionOptions cpuOptions;
            cpuOptions.SetIntraOpNumThreads(4);
            Ort::Session cpuSession(environment, model, cpuOptions);
            auto cpuResult = cpuSession.Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
            const float* cpuDepth = cpuResult[0].GetTensorData<float>();
            double meanDifference = 0.0;
            float maximumDifference = 0.0f;
            for (size_t i = 0; i < plane; ++i) {
                const float difference = std::fabs(depth[i] - cpuDepth[i]);
                meanDifference += difference;
                maximumDifference = std::max(maximumDifference, difference);
            }
            meanDifference /= plane;
            std::printf("CUDA/CPU depth difference: mean %.8f, max %.8f (output range %.8f)\n",
                        meanDifference, maximumDifference, highest - lowest);
        }
        return nonfinite == 0 && highest > lowest ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ZipDepth probe: %s\n", error.what());
        return 1;
    }
}
