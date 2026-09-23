#pragma once
#include <onnxruntime_cxx_api.h>
#include "depth_range.h"
#include <vector>

namespace vrx {
class ModelDepth {
public:
    ModelDepth(const char* modelPath, bool requireCuda);
    // Near values 0..1 on the depth grid, dilated unless `dilate` is false (steady
    // depth works on the undilated map, as xrapp5 does, and dilates afterwards).
    std::vector<float> Run(const float* nchw, bool verbose = true, RangeSmoother* smoother = nullptr,
                           bool dilate = true);
    static void Dilate(std::vector<float>& near);
private:
    Ort::Env environment_;
    Ort::Session session_{nullptr};
    Ort::MemoryInfo memory_;
};
} // namespace vrx
