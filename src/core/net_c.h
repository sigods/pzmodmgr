#ifndef PZ_CORE_NET_C_H
#define PZ_CORE_NET_C_H

/* C99 curl/tar boundary used by the update code and by steamcmd's own
 * bootstrap. Every command line is built here with pz_process_quote() and run
 * through pz_process_capture(); no other core file assembles a curl/tar
 * command. Failures fill `detail` with one user-facing line.
 */

#include "core/c_util.h"
#include "core/fs_c.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GETs a URL with the GitHub API headers. Returns 1 on success. */
int pz_net_get(const char* url, size_t length, pz_str* response, pz_str* detail);

/* POSTs a form body to a Steam Web API endpoint. Returns 1 on success. */
int pz_net_post(const char* url, size_t url_length, const char* body,
                size_t body_length, pz_str* response);

/* Downloads url to dest through a sibling ".part" file and atomically replaces
 * dest. Creates dest's parent directory. Returns 1 on success. */
int pz_net_download(const char* url, size_t url_length, const pz_path* dest,
                    pz_str* detail);

/* Extracts a .zip or .tar.gz archive into dest, creating dest first. Returns 1
 * on success and 0 for an unsupported archive or a failed extraction. */
int pz_net_extract(const pz_path* archive, const pz_path* dest, pz_str* detail);

#ifdef __cplusplus
}
#endif

#endif
