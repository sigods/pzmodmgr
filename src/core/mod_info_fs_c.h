#ifndef PZ_CORE_MOD_INFO_FS_C_H
#define PZ_CORE_MOD_INFO_FS_C_H

/* C99 filesystem traversal around the mod.info parser.
 *
 * Finds the directory that holds mod.info (including B42 version folders),
 * chooses the newest folder compatible with the game build, parses the file,
 * and resolves the icon path. */

#include "core/c_util.h"
#include "core/fs_c.h"
#include "core/mod_info_c.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parses dir/mod.info or the best version subfolder. out must be initialized
 * with pz_mod_info_init(); on success its icon is a resolved path. Returns 0
 * when no mod.info exists or parsing/allocation fails. */
int pz_mod_info_read(const pz_path* dir, const char* game_version,
                     size_t game_length, pz_mod_info* out);

/* Same, without game-version filtering (newest version folder wins). */
int pz_mod_info_read_any(const pz_path* dir, pz_mod_info* out);

/* Fills out with pz_str paths for every mod root inside a Workshop item:
 *   <item>/mod.info, <item>/mods/<X>/mod.info,
 *   <item>/Contents/mods/<X>/mod.info
 * out must be initialized and is replaced. Release with
 * pz_activation_string_list_free() or by freeing each pz_str then the vector. */
int pz_discover_mod_roots(const pz_path* content, pz_vec* out);

#ifdef __cplusplus
}
#endif

#endif
