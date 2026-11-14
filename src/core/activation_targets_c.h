#ifndef PZ_CORE_ACTIVATION_TARGETS_C_H
#define PZ_CORE_ACTIVATION_TARGETS_C_H

/* C99 discovery and writing of default.txt, save mods.txt, and server INI
 * activation targets. The parser and rewrite primitives live in
 * activation_c.{h,c}. */

#include "core/activation_c.h"
#include "core/mod_manager_c.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fills out (sizeof pz_activation_target_data, initialized by this function)
 * with the default target followed by sorted saves and servers. Returns 0 only
 * when the result vector cannot be allocated. */
int pz_activation_targets_discover(const char* zomboid_dir,
                                   size_t zomboid_length,
                                   const char* game_version,
                                   size_t game_version_length, pz_vec* out);

/* Adds or removes mod_ids, and adds workshop ids for servers. target must have
 * been produced by pz_activation_targets_discover and keeps its lists in sync
 * with the file. Returns 0 only when a needed write fails. */
/* `mod_ids` and `workshop_ids` may be NULL when the caller has no IDs; an
 * empty vector is equivalent. */
int pz_activation_targets_write(pz_activation_target_data* target,
                                const pz_vec* mod_ids,
                                const pz_vec* workshop_ids, int enabled);

#ifdef __cplusplus
}
#endif

#endif
