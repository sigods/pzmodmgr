#ifndef PZ_CORE_JSON_C_H
#define PZ_CORE_JSON_C_H

/* Small JSON scanners for the flat API responses this program reads. These
 * are not a general JSON parser: they locate a key and read the scalar after
 * it, with full string-escape decoding for titles. */

#include "core/c_util.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Finds the next quoted key in json. On success *pos is the index just past
 * the closing quote. */
int pz_json_find_key(const char* json, size_t length, const char* key,
                     size_t from, size_t* pos);

/* index points at an opening quote; on success it is advanced past the
 * closing quote. */
int pz_json_skip_string(const char* json, size_t length, size_t* index);

/* Reads the string value after a key (skipping whitespace and ':'). The
 * decoded UTF-8 result replaces out. after, when non-NULL, receives the index
 * just past the closing quote. */
int pz_json_read_string_after(const char* json, size_t length, size_t pos,
                              pz_str* out, size_t* after);

/* Reads the unsigned integer value after a key. */
int pz_json_read_uint_after(const char* json, size_t length, size_t pos,
                            uint64_t* value);

int pz_json_string_field(const char* json, size_t length, const char* key,
                         size_t from, pz_str* out, size_t* after);
int pz_json_uint_field(const char* json, size_t length, const char* key,
                       size_t from, uint64_t* value);

/* Strict decimal parser used by the collection id reader. */
int pz_json_parse_uint64(const char* data, size_t length, uint64_t* value);

#ifdef __cplusplus
}
#endif

#endif
