#ifndef PZ_UI_MOD_ICON_C_H
#define PZ_UI_MOD_ICON_C_H

/* C99 PNG thumbnail decoder shared by the native UIs. The caller receives a
 * size*size RGBA buffer and releases it with pz_mod_icon_free(). */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pz_mod_icon {
    int size;
    uint8_t* rgba;
} pz_mod_icon;

void pz_mod_icon_init(pz_mod_icon* icon);
void pz_mod_icon_free(pz_mod_icon* icon);

/* Decodes path (UTF-8, path_length bytes), center-crops it to a square, and
 * scales it to size. Returns 1 on success. */
int pz_mod_icon_load(const char* path, size_t path_length, int size,
                     pz_mod_icon* out);

#ifdef __cplusplus
}
#endif

#endif
