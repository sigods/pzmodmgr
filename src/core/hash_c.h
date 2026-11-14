#ifndef PZ_CORE_HASH_C_H
#define PZ_CORE_HASH_C_H

/* One-shot SHA-256 for release verification. */

#include "core/c_util.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writes the lowercase 64-character hex digest to out. Returns 0 on
 * allocation failure or when length cannot be represented in the SHA-256
 * padding. */
int pz_sha256_hex(const char* data, size_t length, pz_str* out);

#ifdef __cplusplus
}
#endif

#endif
