#include "ui/mod_icon.hpp"

#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace pz {
namespace {
std::uint8_t average_channel(const std::uint8_t* src, int stride,
                             int x0, int y0, int x1, int y1, int channel) {
    std::uint64_t sum = 0;
    std::uint64_t count = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            sum += src[static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) +
                       static_cast<std::size_t>(x) * 4 + static_cast<std::size_t>(channel)];
            ++count;
        }
    }
    return count == 0 ? 0 : static_cast<std::uint8_t>(sum / count);
}
}  // namespace

bool load_mod_icon(const fs::path& path, int size, ModIcon& out) {
    out = {};
    if (size <= 0 || path.empty()) return false;

    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
    if (!pixels || width <= 0 || height <= 0) {
        stbi_image_free(pixels);
        return false;
    }

    const int side = std::min(width, height);
    const int left = (width - side) / 2;
    const int top = (height - side) / 2;
    out.size = size;
    out.rgba.resize(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4);

    for (int y = 0; y < size; ++y) {
        const int sy0 = top + y * side / size;
        const int sy1 = top + std::max(sy0 + 1, (y + 1) * side / size);
        for (int x = 0; x < size; ++x) {
            const int sx0 = left + x * side / size;
            const int sx1 = left + std::max(sx0 + 1, (x + 1) * side / size);
            std::size_t dst = (static_cast<std::size_t>(y) * static_cast<std::size_t>(size) +
                               static_cast<std::size_t>(x)) * 4;
            for (int c = 0; c < 4; ++c)
                out.rgba[dst + static_cast<std::size_t>(c)] =
                    average_channel(pixels, width * 4, sx0, sy0, sx1, sy1, c);
        }
    }

    stbi_image_free(pixels);
    return true;
}
}  // namespace pz
