#pragma once
#include <onnxruntime_cxx_api.h>
#include <vector>

namespace vrx {
class ModelDepth {
public:
    ModelDepth(const char* modelPath, bool requireCuda);
    std::vector<float> Run(const float* nchw, bool verbose = true);
private:
    Ort::Env environment_;
    Ort::Session session_{nullptr};
    Ort::MemoryInfo memory_;
};
} // namespace vrx
