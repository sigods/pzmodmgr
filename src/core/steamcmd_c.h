#ifndef PZ_CORE_STEAMCMD_C_H
#define PZ_CORE_STEAMCMD_C_H

/* C99 steamcmd and Steam Web API boundary.
 *
 * The configured steamcmd path and the Steam username live in the caller; this
 * module never stores a password and never adds +login user pass.
 */

#include "core/c_util.h"
#include "core/fs_c.h"
#include "core/mod_manager_c.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pz_download_outcome {
    int ok;
    pz_path content_dir;
    pz_str detail;
} pz_download_outcome;

void pz_download_outcome_init(pz_download_outcome* outcome);
void pz_download_outcome_free(pz_download_outcome* outcome);

/* Resolves a usable steamcmd. Uses the configured path, then the built-in
 * copy, then downloads Valve's build into data_dir, then falls back to a
 * system install. Replaces exe and sets changed when the caller should
 * persist it. Returns 1 when a path is available. */
int pz_steamcmd_ensure(const pz_path* data_dir, pz_str* exe, int* changed,
                       const pz_log* log);

/* Finds an existing system steamcmd (PZMODMGR_STEAMCMD, STEAMCMD, PATH, then
 * common install directories) without downloading anything. out must be
 * initialized. Returns 1 when a path was found. */
int pz_steamcmd_find(pz_str* out);

/* Runs one Workshop download. `exe` is updated when ensure() had to switch
 * paths. Returns 1 only when the item landed in content_dir. */
int pz_steamcmd_download_workshop(const pz_path* data_dir, pz_str* exe,
                                  const pz_str* steam_user, uint64_t item_id,
                                  const pz_path* install_dir, const pz_log* log,
                                  pz_download_outcome* outcome);

/* 1 = OK, 9 = not found/private, 15 = access denied, 0 = unreachable. */
uint32_t pz_steamcmd_fetch_result(uint64_t item_id);
uint64_t pz_steamcmd_fetch_time_updated(uint64_t item_id);
int pz_steamcmd_fetch_details(uint64_t item_id, pz_remote_details* details);

/* Fills out with uint64_t child ids from a collection. Returns 1 when the
 * request succeeded; a non-collection item yields an empty vector. */
int pz_steamcmd_fetch_collection(uint64_t collection_id, pz_vec* out);

#ifdef __cplusplus
}
#endif

#endif
