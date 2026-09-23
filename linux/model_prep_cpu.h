#pragma once
#include <cstdint>
#include <vector>

namespace vrx {
// Box taps per axis for a source of this width (xrapp5: ceil(width / 672), 1..4).
int ModelPrepTaps(int sourceWidth);
// Reference ZipDepth input: NCHW float32, 3 x 384 x 672, from packed RGBA (red
// in the low byte) of any size, with model_prep.comp's bilinear taps.
std::vector<float> PrepareModelInput(const uint32_t* rgba, int width, int height);
// The same from a depth-grid (686 x 392) RGB image.
std::vector<float> PrepareModelInput(const std::vector<unsigned char>& rgb);
}
