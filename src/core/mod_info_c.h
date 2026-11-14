#ifndef PZ_CORE_MOD_INFO_C_H
#define PZ_CORE_MOD_INFO_C_H

/* C99 parser for mod.info and its version rules. Filesystem discovery and
 * icon resolution live in mod_info_fs_c. */

#include "core/c_util.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pz_mod_info {
    pz_str id;
    pz_str name;
    pz_str author;
    pz_str mod_version;
    pz_str version_min;
    pz_str version_max;
    pz_vec required_ids; /* pz_str elements */
    pz_str icon;         /* raw icon= value before path resolution */
} pz_mod_info;

void pz_mod_info_init(pz_mod_info* info);
void pz_mod_info_free(pz_mod_info* info);

/* Parses one logical line without its CR/LF. Returns 0 on allocation failure. */
int pz_mod_info_parse_line(pz_mod_info* info, const char* line, size_t length);

/* Parses a whole mod.info buffer. Returns 0 on allocation failure. */
int pz_mod_info_parse(pz_mod_info* info, const char* text, size_t length);

size_t pz_mod_info_required_count(const pz_mod_info* info);
const pz_str* pz_mod_info_required_at(const pz_mod_info* info, size_t index);

/* Returns the numeric B42 version-folder rank, or -1 for non-numeric names
 * and integer overflow. */
int pz_version_rank(const char* name, size_t length);

/* Shared version rules used by the parser and the manager. */
int pz_supports_game_version(const char* version_min, size_t min_length,
                             const char* version_max, size_t max_length,
                             const char* game_version, size_t game_length);
int pz_version_range(const char* version_min, size_t min_length,
                     const char* version_max, size_t max_length, pz_str* out);

int pz_mod_info_supports_game_version(const pz_mod_info* info,
                                      const char* game_version,
                                      size_t game_length);
int pz_mod_info_version_range(const pz_mod_info* info, pz_str* out);

#ifdef __cplusplus
}
#endif

#endif
