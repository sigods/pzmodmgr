#include "core/mod_info_c.h"

#include <limits.h>
#include <string.h>

static int key_is(const char* key, size_t length, const char* expected) {
    const size_t expected_length = strlen(expected);
    return length == expected_length && memcmp(key, expected, length) == 0;
}

static void trim_span(const char** data, size_t* length) {
    while (*length != 0) {
        const char first = **data;
        if (first != ' ' && first != '\t' && first != '\r' && first != '\n') break;
        ++*data;
        --*length;
    }
    while (*length != 0) {
        const char last = (*data)[*length - 1];
        if (last != ' ' && last != '\t' && last != '\r' && last != '\n') break;
        --*length;
    }
}

static int add_required_id(pz_mod_info* info, const char* data, size_t length) {
    size_t i;
    pz_str* slot;

    trim_span(&data, &length);
    while (length != 0 && (data[0] == '\\' || data[0] == '/')) {
        ++data;
        --length;
    }
    if (length >= 2 && data[0] == '"' && data[length - 1] == '"') {
        ++data;
        length -= 2;
    }
    if (length == 0) return 1;

    for (i = 0; i < info->required_ids.length; ++i) {
        const pz_str* existing =
            (const pz_str*)pz_vec_at_const(&info->required_ids, i);
        if (pz_str_equals(existing, data, length)) return 1;
    }

    slot = (pz_str*)pz_vec_push(&info->required_ids);
    if (!slot) return 0;
    if (!pz_str_assign(slot, data, length)) {
        --info->required_ids.length;
        return 0;
    }
    return 1;
}

static int parse_required_ids(pz_mod_info* info, const char* value,
                              size_t length) {
    size_t start = 0;

    for (;;) {
        size_t end = start;
        while (end < length && value[end] != ',' && value[end] != ';') ++end;
        if (!add_required_id(info, value + start, end - start)) return 0;
        if (end == length) break;
        start = end + 1;
    }
    return 1;
}

void pz_mod_info_init(pz_mod_info* info) {
    if (!info) return;
    memset(info, 0, sizeof(*info));
    (void)pz_vec_init(&info->required_ids, sizeof(pz_str));
}

void pz_mod_info_free(pz_mod_info* info) {
    size_t i;

    if (!info) return;
    for (i = 0; i < info->required_ids.length; ++i)
        pz_str_free((pz_str*)pz_vec_at(&info->required_ids, i));
    pz_vec_free(&info->required_ids);
    pz_str_free(&info->id);
    pz_str_free(&info->name);
    pz_str_free(&info->author);
    pz_str_free(&info->mod_version);
    pz_str_free(&info->version_min);
    pz_str_free(&info->version_max);
    pz_str_free(&info->icon);
    pz_mod_info_init(info);
}

int pz_mod_info_parse_line(pz_mod_info* info, const char* line,
                           size_t length) {
    const char* equals;
    const char* key;
    const char* value;
    size_t key_length;
    size_t value_length;

    if (!info || (!line && length != 0)) return 0;
    equals = (const char*)memchr(line, '=', length);
    if (!equals) return 1;
    key = line;
    key_length = (size_t)(equals - line);
    value = equals + 1;
    value_length = length - key_length - 1;

    if (key_is(key, key_length, "id"))
        return pz_str_assign(&info->id, value, value_length);
    if (key_is(key, key_length, "name"))
        return pz_str_assign(&info->name, value, value_length);
    if (key_is(key, key_length, "author"))
        return pz_str_assign(&info->author, value, value_length);
    if (key_is(key, key_length, "modversion"))
        return pz_str_assign(&info->mod_version, value, value_length);
    if (key_is(key, key_length, "versionMin"))
        return pz_str_assign(&info->version_min, value, value_length);
    if (key_is(key, key_length, "versionMax"))
        return pz_str_assign(&info->version_max, value, value_length);
    if (key_is(key, key_length, "require") ||
        key_is(key, key_length, "requires"))
        return parse_required_ids(info, value, value_length);
    if (key_is(key, key_length, "icon"))
        return pz_str_assign(&info->icon, value, value_length);
    return 1;
}

int pz_mod_info_parse(pz_mod_info* info, const char* text, size_t length) {
    size_t start = 0;

    if (!info || (!text && length != 0)) return 0;
    while (start < length) {
        size_t end = start;
        size_t line_end;
        while (end < length && text[end] != '\n') ++end;
        line_end = end;
        if (line_end > start && text[line_end - 1] == '\r') --line_end;
        if (!pz_mod_info_parse_line(info, text + start, line_end - start))
            return 0;
        if (end == length) break;
        start = end + 1;
    }
    return 1;
}

size_t pz_mod_info_required_count(const pz_mod_info* info) {
    return info ? info->required_ids.length : 0;
}

const pz_str* pz_mod_info_required_at(const pz_mod_info* info, size_t index) {
    if (!info) return NULL;
    return (const pz_str*)pz_vec_at_const(&info->required_ids, index);
}

int pz_version_rank(const char* name, size_t length) {
    size_t i;
    int value = 0;

    if (!name || length == 0) return -1;
    for (i = 0; i < length; ++i) {
        int digit;
        if (name[i] < '0' || name[i] > '9') return -1;
        digit = name[i] - '0';
        if (value > (INT_MAX - digit) / 10) return -1;
        value = value * 10 + digit;
    }
    return value;
}

static int major_version(const char* value, size_t length) {
    size_t i = 0;
    int major = 0;
    int found = 0;

    while (i < length && (value[i] < '0' || value[i] > '9')) ++i;
    while (i < length && value[i] >= '0' && value[i] <= '9') {
        const int digit = value[i] - '0';
        if (major > (INT_MAX - digit) / 10) return -1;
        major = major * 10 + digit;
        found = 1;
        ++i;
    }
    return found ? major : -1;
}

int pz_supports_game_version(const char* version_min, size_t min_length,
                             const char* version_max, size_t max_length,
                             const char* game_version, size_t game_length) {
    int game;
    int minimum;
    int maximum;

    if (!game_version || game_length == 0) return 1;
    game = major_version(game_version, game_length);
    if (game < 0) return 1;
    minimum = major_version(version_min, min_length);
    maximum = major_version(version_max, max_length);
    if (minimum < 0 && maximum < 0) return 1;

    /* Project Zomboid mods break across major builds (41 -> 42 -> 43), but a
     * 42.22 mod normally still runs on 42.21. Treat the bounds as tested major
     * builds rather than exact patch versions. */
    if (minimum >= 0 && maximum >= 0)
        return game >= minimum && game <= maximum;
    if (minimum >= 0) return game == minimum;
    return game == maximum;
}

int pz_version_range(const char* version_min, size_t min_length,
                     const char* version_max, size_t max_length, pz_str* out) {
    if (!out) return 0;
    pz_str_clear(out);
    if (min_length == 0 && max_length == 0) return 1;

    if (!pz_str_assign_cstr(out, "PZ ")) return 0;
    if (min_length != 0) {
        if (!pz_str_append(out, version_min, min_length)) return 0;
        if (!pz_str_append_cstr(out, max_length != 0 ? "-" : "+")) return 0;
    } else {
        if (!pz_str_append_cstr(out, "up to ")) return 0;
    }
    if (max_length != 0 &&
        !pz_str_append(out, version_max, max_length))
        return 0;
    return 1;
}

int pz_mod_info_supports_game_version(const pz_mod_info* info,
                                      const char* game_version,
                                      size_t game_length) {
    if (!info) return 1;
    return pz_supports_game_version(
        info->version_min.data, info->version_min.length,
        info->version_max.data, info->version_max.length,
        game_version, game_length);
}

int pz_mod_info_version_range(const pz_mod_info* info, pz_str* out) {
    if (!info) return pz_version_range(NULL, 0, NULL, 0, out);
    return pz_version_range(
        info->version_min.data, info->version_min.length,
        info->version_max.data, info->version_max.length, out);
}
