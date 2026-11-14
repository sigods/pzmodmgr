#ifndef PZ_CORE_UPDATER_C_H
#define PZ_CORE_UPDATER_C_H

/* C99 self-updater.
 *
 * check/cleanup/stage mirror the old pz::updater API; the helper entry points
 * are what the re-launched executable runs with --apply-update. The version and
 * platform strings are parameters so this module never reads a build constant.
 */

#include "core/c_util.h"
#include "core/fs_c.h"
#include "core/updater_json_c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "win" or "linux", matching the release asset platform suffix. */
const char* pz_updater_platform_name(void);

/* Fetches the latest GitHub release. Always returns 1 unless allocation fails;
 * the outcome is in out (ok=0 when the API could not be reached). out must
 * first be initialized with pz_app_update_init(), and the caller frees it with
 * pz_app_update_free(). */
int pz_updater_check(const char* current_version, size_t current_version_length,
                     const char* platform, size_t platform_length,
                     pz_app_update* out);

/* Removes the leftover helper and every staged update except one whose
 * pending.txt names a running helper. */
void pz_updater_cleanup_cache(const pz_path* data_dir);

/* Checks, downloads, verifies, extracts, and stages a release, then starts a
 * detached helper that applies it after this process exits. Returns 1 when the
 * update is staged (or none is available), 0 on failure; message always holds a
 * user-facing line. */
int pz_updater_stage(const pz_path* data_dir,
                     const char* current_version, size_t current_version_length,
                     const char* platform, size_t platform_length,
                     const pz_log* log, pz_str* message);

int pz_updater_is_helper_invocation(int argc, char** argv);

/* Runs the helper command line and returns its process exit code. */
int pz_updater_run_helper(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif
