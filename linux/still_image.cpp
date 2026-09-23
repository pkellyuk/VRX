#include "still_image.h"
#include "synthetic_scene.h"
#include <png.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace vrx {
std::vector<unsigned char> LoadStillPng(const char* path) {
    if (!path || !*path) throw std::runtime_error("No still PNG path");
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path))
        throw std::runtime_error(std::string("Cannot open PNG: ") + image.message);
    if (image.width < 1 || image.height < 1 || image.width > 16384 || image.height > 16384) {
        png_image_free(&image);
        throw std::runtime_error("Still PNG dimensions outside supported range");
    }
    image.format = PNG_FORMAT_RGB;
    std::vector<unsigned char> source(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, source.data(), 0, nullptr)) {
        const std::string error = image.message;
        png_image_free(&image);
        throw std::runtime_error("Cannot decode PNG: " + error);
    }
    const int sourceWidth = int(image.width), sourceHeight = int(image.height);
    png_image_free(&image);
    std::vector<unsigned char> output(size_t(kSyntheticWidth) * kSyntheticHeight * 3);
    for (int y = 0; y < kSyntheticHeight; ++y) {
        const float py = std::clamp((y + 0.5f) / kSyntheticHeight * sourceHeight - 0.5f,
                                    0.0f, float(sourceHeight - 1));
        const int y0 = int(py), y1 = std::min(y0 + 1, sourceHeight - 1);
        const float fy = py - y0;
        for (int x = 0; x < kSyntheticWidth; ++x) {
            const float px = std::clamp((x + 0.5f) / kSyntheticWidth * sourceWidth - 0.5f,
                                        0.0f, float(sourceWidth - 1));
            const int x0 = int(px), x1 = std::min(x0 + 1, sourceWidth - 1);
            const float fx = px - x0;
            for (int c = 0; c < 3; ++c) {
                auto channel = [&](int ix, int iy) {
                    return float(source[(size_t(iy) * sourceWidth + ix) * 3 + c]);
                };
                const float top = channel(x0, y0) + (channel(x1, y0) - channel(x0, y0)) * fx;
                const float bottom = channel(x0, y1) + (channel(x1, y1) - channel(x0, y1)) * fx;
                output[(size_t(y) * kSyntheticWidth + x) * 3 + c] =
                    static_cast<unsigned char>(std::lround(top + (bottom - top) * fy));
            }
        }
    }
    std::printf("Still PNG: %s (%dx%d -> %dx%d)\n",
                path, sourceWidth, sourceHeight, kSyntheticWidth, kSyntheticHeight);
    return output;
}
} // namespace vrx
