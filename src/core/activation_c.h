#ifndef PZ_CORE_ACTIVATION_C_H
#define PZ_CORE_ACTIVATION_C_H

/* C99 parsers/writers for mods/default.txt, save mods.txt, and server INI
 * lists. Target discovery and file I/O live in activation_targets_c. */

#include "core/c_util.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* String-list out parameters are initialized by the parse functions and hold
 * pz_str elements. Release them with pz_activation_string_list_free(). */
int pz_activation_parse_block_ids(const char* text, size_t length, pz_vec* out);
int pz_activation_parse_ini_ids(const char* text, size_t length, pz_vec* out,
                                int* backslash);
int pz_activation_parse_ini_workshop_ids(const char* text, size_t length,
                                         pz_vec* out);

void pz_activation_string_list_free(pz_vec* list);
int pz_activation_block_uses_backslash(const char* text, size_t length);

/* ids is a pz_vec of pz_str; workshop_ids is a pz_vec of uint64_t. */
int pz_activation_rewrite_block_ids(const char* text, size_t length,
                                    const pz_vec* ids, pz_str* out,
                                    int* changed);
int pz_activation_rewrite_ini_mods(const char* text, size_t length,
                                   const pz_vec* ids, int backslash,
                                   pz_str* out, int* changed);
int pz_activation_rewrite_ini_workshop_items(const char* text, size_t length,
                                             const pz_vec* workshop_ids,
                                             pz_str* out, int* changed);

#ifdef __cplusplus
}
#endif

#endif
