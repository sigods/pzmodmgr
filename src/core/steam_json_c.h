#ifndef PZ_CORE_STEAM_JSON_C_H
#define PZ_CORE_STEAM_JSON_C_H

/* Parsers for the two Steam Web API responses used by the Workshop client. */

#include "core/c_util.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t pz_parse_published_file_time_updated(const char* json, size_t length);

/* Writes the item title to out. Returns 0 when the field is missing or
 * malformed. */
int pz_parse_published_file_title(const char* json, size_t length, pz_str* out);

uint32_t pz_parse_published_file_result(const char* json, size_t length);

/* Initializes out as a uint64_t list. Returns 0 on allocation failure. */
int pz_parse_collection_children(const char* json, size_t length, pz_vec* out);

#ifdef __cplusplus
}
#endif

#endif
