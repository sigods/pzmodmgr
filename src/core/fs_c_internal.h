#ifndef PZ_CORE_FS_C_INTERNAL_H
#define PZ_CORE_FS_C_INTERNAL_H

/* Helpers shared by fs_c.c and the platform backends. Not part of the public
 * contract. */

#include "core/fs_c.h"

#ifdef __cplusplus
extern "C" {
#endif

void pz_fs_error_set(pz_fs_error* error, pz_fs_code code, int system_code);

/* Appends one entry with an owned copy of name. Returns 0 on allocation
 * failure. */
int pz_fs_entries_add(pz_vec* entries, const char* name, size_t length,
                      pz_fs_kind kind);
void pz_fs_entries_sort(pz_vec* entries);

int pz_fs_separator_is(char value);

/* Copies a link as a link. POSIX creates a symlink; Windows reports
 * PZ_FS_INVALID because creating links needs a privilege the manager must not
 * assume. */
int pz_fs_copy_link(const pz_path* from, const pz_path* to, unsigned flags,
                    pz_fs_error* error);

#ifdef __cplusplus
}
#endif

#endif
