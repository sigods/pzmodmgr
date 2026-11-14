#include "core/steam_json_c.h"

#include "core/json_c.h"

#include <string.h>

uint64_t pz_parse_published_file_time_updated(const char* json, size_t length) {
    size_t from = 0;

    for (;;) {
        size_t pos;
        uint64_t value = 0;

        if (!pz_json_find_key(json, length, "time_updated", from, &pos))
            return 0;
        if (pz_json_read_uint_after(json, length, pos, &value)) return value;
        from = pos; /* A string value can itself equal "time_updated". */
    }
}

int pz_parse_published_file_title(const char* json, size_t length, pz_str* out) {
    size_t items;
    size_t pos;

    if (!out) return 0;
    if (!pz_json_find_key(json, length, "publishedfiledetails", 0, &items))
        return 0;
    if (!pz_json_find_key(json, length, "title", items, &pos)) return 0;
    return pz_json_read_string_after(json, length, pos, out, NULL);
}

uint32_t pz_parse_published_file_result(const char* json, size_t length) {
    size_t items;
    size_t pos;
    uint64_t value = 0;

    /* GetPublishedFileDetails has an outer "result" (call status) and a
     * per-item "result" inside publishedfiledetails. Only the item's result
     * says whether the item is reachable. */
    if (!pz_json_find_key(json, length, "publishedfiledetails", 0, &items))
        return 0;
    if (!pz_json_find_key(json, length, "result", items, &pos)) return 0;
    if (!pz_json_read_uint_after(json, length, pos, &value)) return 0;
    return (uint32_t)value;
}

static int add_unique_id(pz_vec* ids, uint64_t value) {
    size_t i;
    uint64_t* slot;

    if (value == 0) return 1;
    for (i = 0; i < ids->length; ++i) {
        const uint64_t* existing = (const uint64_t*)pz_vec_at_const(ids, i);
        if (*existing == value) return 1;
    }
    slot = (uint64_t*)pz_vec_push(ids);
    if (!slot) return 0;
    *slot = value;
    return 1;
}

int pz_parse_collection_children(const char* json, size_t length, pz_vec* out) {
    size_t i = 0;

    if (!out || (!json && length != 0)) return 0;
    pz_vec_init(out, sizeof(uint64_t));

    while (i < length) {
        size_t key_start;
        size_t key_length;
        int depth;

        if (json[i] != '"') {
            ++i;
            continue;
        }

        key_start = ++i;
        while (i < length && json[i] != '"') {
            if (json[i] == '\\' && i + 1 < length)
                i += 2;
            else
                ++i;
        }
        if (i >= length) break;
        key_length = i - key_start;
        ++i;
        if (key_length != 8 || memcmp(json + key_start, "children", 8) != 0)
            continue;

        while (i < length && (json[i] == ' ' || json[i] == '\t' ||
                              json[i] == '\n' || json[i] == '\r'))
            ++i;
        if (i >= length || json[i] != ':') continue;
        ++i;
        while (i < length && (json[i] == ' ' || json[i] == '\t' ||
                              json[i] == '\n' || json[i] == '\r'))
            ++i;
        if (i >= length || json[i] != '[') break;
        ++i;

        depth = 1;
        while (i < length && depth > 0) {
            const char c = json[i];

            if (c == '[') {
                ++depth;
                ++i;
            } else if (c == ']') {
                --depth;
                ++i;
            } else if (c == '"') {
                size_t token_start;
                size_t token_length;
                size_t j;

                token_start = ++i;
                while (i < length && json[i] != '"') {
                    if (json[i] == '\\' && i + 1 < length)
                        i += 2;
                    else
                        ++i;
                }
                token_length = i - token_start;
                if (i < length) ++i;

                j = i;
                while (j < length && (json[j] == ' ' || json[j] == '\t' ||
                                      json[j] == '\n' || json[j] == '\r'))
                    ++j;
                if (token_length == 15 &&
                    memcmp(json + token_start, "publishedfileid", 15) == 0 &&
                    j < length && json[j] == ':') {
                    ++j;
                    while (j < length && (json[j] == ' ' || json[j] == '\t' ||
                                          json[j] == '\n' || json[j] == '\r'))
                        ++j;
                    if (j < length && json[j] == '"') {
                        size_t value_start = ++j;
                        uint64_t value = 0;

                        while (j < length && json[j] != '"') ++j;
                        if (pz_json_parse_uint64(json + value_start,
                                                 j - value_start, &value) &&
                            !add_unique_id(out, value)) {
                            pz_vec_free(out);
                            return 0;
                        }
                        if (j < length) ++j;
                        i = j;
                    }
                }
            } else {
                ++i;
            }
        }
        break;
    }
    return 1;
}
