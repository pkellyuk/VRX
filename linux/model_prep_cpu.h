#pragma once
#include <vector>

namespace vrx {
// Reference ZipDepth input: NCHW float32, 3 x 384 x 672, 2x2 bilinear taps.
std::vector<float> PrepareModelInput(const std::vector<unsigned char>& rgb);
}
