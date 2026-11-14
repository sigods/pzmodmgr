#include "ui/mod_icon_c.h"

#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <stdlib.h>
#include <string.h>

void pz_mod_icon_init(pz_mod_icon* icon) {
    if (!icon) return;
    icon->size = 0;
    icon->rgba = NULL;
}

void pz_mod_icon_free(pz_mod_icon* icon) {
    if (!icon) return;
    free(icon->rgba);
    icon->rgba = NULL;
    icon->size = 0;
}

static uint8_t average_channel(const uint8_t* source, int stride, int x0,
                               int y0, int x1, int y1, int channel) {
    uint64_t sum = 0;
    uint64_t count = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            sum += source[(size_t)y * (size_t)stride + (size_t)x * 4 +
                          (size_t)channel];
            ++count;
        }
    }
    return count == 0 ? 0 : (uint8_t)(sum / count);
}

int pz_mod_icon_load(const char* path, size_t path_length, int size,
                     pz_mod_icon* out) {
    if (!out) return 0;
    pz_mod_icon_init(out);
    if (!path || path_length == 0 || size <= 0) return 0;

    char* name = (char*)malloc(path_length + 1);
    if (!name) return 0;
    memcpy(name, path, path_length);
    name[path_length] = '\0';

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels =
        stbi_load(name, &width, &height, &channels, 4);
    free(name);
    if (!pixels || width <= 0 || height <= 0) {
        stbi_image_free(pixels);
        return 0;
    }

    const int side = width < height ? width : height;
    const int left = (width - side) / 2;
    const int top = (height - side) / 2;
    const size_t bytes = (size_t)size * (size_t)size * 4;
    uint8_t* rgba = (uint8_t*)malloc(bytes);
    if (!rgba) {
        stbi_image_free(pixels);
        return 0;
    }

    for (int y = 0; y < size; ++y) {
        const int sy0 = top + y * side / size;
        int sy1 = top + (y + 1) * side / size;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < size; ++x) {
            const int sx0 = left + x * side / size;
            int sx1 = left + (x + 1) * side / size;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            const size_t destination =
                ((size_t)y * (size_t)size + (size_t)x) * 4;
            for (int channel = 0; channel < 4; ++channel) {
                rgba[destination + (size_t)channel] = average_channel(
                    pixels, width * 4, sx0, sy0, sx1, sy1, channel);
            }
        }
    }

    stbi_image_free(pixels);
    out->size = size;
    out->rgba = rgba;
    return 1;
}
