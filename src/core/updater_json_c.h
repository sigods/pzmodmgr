#ifndef PZ_CORE_UPDATER_JSON_C_H
#define PZ_CORE_UPDATER_JSON_C_H

/* Parses the GitHub latest-release response and compares dotted versions. */

#include "core/c_util.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pz_app_update {
    int ok;
    int available;
    pz_str current_version;
    pz_str latest_version;
    pz_str asset_name;
    pz_str asset_url;
    pz_str asset_digest;
    uint64_t asset_size;
    pz_str message;
} pz_app_update;

void pz_app_update_init(pz_app_update* update);
void pz_app_update_free(pz_app_update* update);

int pz_app_version_newer(const char* latest, size_t latest_length,
                         const char* current, size_t current_length);

/* Returns 0 only on allocation failure; malformed JSON produces an update
 * with ok=0 and a user-facing message. */
int pz_parse_latest_app_release(const char* json, size_t length,
                                const char* current_version,
                                size_t current_version_length,
                                const char* platform, size_t platform_length,
                                pz_app_update* out);

#ifdef __cplusplus
}
#endif

#endif
