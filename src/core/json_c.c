#include "core/json_c.h"

#include <string.h>

static int is_json_space(char value) {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

static void skip_space(const char* json, size_t length, size_t* index) {
    while (*index < length && is_json_space(json[*index])) ++*index;
}

static int read_hex4(const char* json, size_t length, size_t* index,
                     uint32_t* value) {
    uint32_t parsed = 0;
    int n;

    if (!index || *index > length || length - *index < 4) return 0;
    for (n = 0; n < 4; ++n) {
        const char c = json[(*index)++];
        parsed <<= 4;
        if (c >= '0' && c <= '9')
            parsed |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            parsed |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            parsed |= (uint32_t)(c - 'A' + 10);
        else
            return 0;
    }
    *value = parsed;
    return 1;
}

static int append_utf8(pz_str* out, uint32_t codepoint) {
    char bytes[4];
    size_t count;

    if (codepoint <= 0x7f) {
        bytes[0] = (char)codepoint;
        count = 1;
    } else if (codepoint <= 0x7ff) {
        bytes[0] = (char)(0xc0 | (codepoint >> 6));
        bytes[1] = (char)(0x80 | (codepoint & 0x3f));
        count = 2;
    } else if (codepoint <= 0xffff) {
        if (codepoint >= 0xd800 && codepoint <= 0xdfff) return 0;
        bytes[0] = (char)(0xe0 | (codepoint >> 12));
        bytes[1] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[2] = (char)(0x80 | (codepoint & 0x3f));
        count = 3;
    } else if (codepoint <= 0x10ffff) {
        bytes[0] = (char)(0xf0 | (codepoint >> 18));
        bytes[1] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
        bytes[2] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[3] = (char)(0x80 | (codepoint & 0x3f));
        count = 4;
    } else {
        return 0;
    }
    return pz_str_append(out, bytes, count);
}

int pz_json_find_key(const char* json, size_t length, const char* key,
                     size_t from, size_t* pos) {
    const size_t key_length = key ? strlen(key) : 0;
    size_t i;

    if ((!json && length != 0) || !key || !pos || from > length) return 0;
    i = from;
    while (i < length) {
        size_t start;
        size_t found_length;

        if (json[i] != '"') {
            ++i;
            continue;
        }
        start = ++i;
        while (i < length && json[i] != '"') {
            if (json[i] == '\\' && i + 1 < length)
                i += 2;
            else
                ++i;
        }
        if (i >= length) return 0;
        found_length = i - start;
        ++i;
        if (found_length == key_length &&
            (key_length == 0 || memcmp(json + start, key, key_length) == 0)) {
            *pos = i;
            return 1;
        }
    }
    return 0;
}

int pz_json_skip_string(const char* json, size_t length, size_t* index) {
    size_t i;

    if (!json || !index || *index >= length || json[*index] != '"') return 0;
    i = *index + 1;
    while (i < length) {
        if (json[i] == '\\' && i + 1 < length) {
            i += 2;
        } else if (json[i] == '"') {
            *index = i + 1;
            return 1;
        } else {
            ++i;
        }
    }
    *index = i;
    return 0;
}

int pz_json_read_string_after(const char* json, size_t length, size_t pos,
                              pz_str* out, size_t* after) {
    size_t i = pos;

    if ((!json && length != 0) || !out) return 0;
    pz_str_clear(out);

    skip_space(json, length, &i);
    if (i >= length || json[i] != ':') goto fail;
    ++i;
    skip_space(json, length, &i);
    if (i >= length || json[i] != '"') goto fail;
    ++i;

    while (i < length) {
        const char c = json[i++];

        if (c == '"') {
            if (after) *after = i;
            return 1;
        }
        if (c != '\\') {
            if (!pz_str_append_char(out, c)) goto fail;
            continue;
        }
        if (i >= length) goto fail;

        {
            const char escaped = json[i++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                if (!pz_str_append_char(out, escaped)) goto fail;
                break;
            case 'b':
                if (!pz_str_append_char(out, '\b')) goto fail;
                break;
            case 'f':
                if (!pz_str_append_char(out, '\f')) goto fail;
                break;
            case 'n':
                if (!pz_str_append_char(out, '\n')) goto fail;
                break;
            case 'r':
                if (!pz_str_append_char(out, '\r')) goto fail;
                break;
            case 't':
                if (!pz_str_append_char(out, '\t')) goto fail;
                break;
            case 'u': {
                uint32_t codepoint = 0;
                if (!read_hex4(json, length, &i, &codepoint)) goto fail;
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    uint32_t low = 0;
                    if (i + 6 > length || json[i] != '\\' ||
                        json[i + 1] != 'u')
                        goto fail;
                    i += 2;
                    if (!read_hex4(json, length, &i, &low) ||
                        low < 0xdc00 || low > 0xdfff)
                        goto fail;
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) +
                                (low - 0xdc00);
                }
                if (!append_utf8(out, codepoint)) goto fail;
                break;
            }
            default:
                goto fail;
            }
        }
    }

fail:
    pz_str_clear(out);
    return 0;
}

int pz_json_parse_uint64(const char* data, size_t length, uint64_t* value) {
    uint64_t parsed = 0;
    size_t i;

    if (!value || (!data && length != 0) || length == 0) return 0;
    for (i = 0; i < length; ++i) {
        const uint64_t digit = (uint64_t)(data[i] - '0');
        if (data[i] < '0' || data[i] > '9') return 0;
        if (parsed > (UINT64_MAX - digit) / 10) return 0;
        parsed = parsed * 10 + digit;
    }
    *value = parsed;
    return 1;
}

int pz_json_read_uint_after(const char* json, size_t length, size_t pos,
                            uint64_t* value) {
    size_t i = pos;
    size_t first;

    if ((!json && length != 0) || !value) return 0;

    skip_space(json, length, &i);
    if (i >= length || json[i] != ':') return 0;
    ++i;
    skip_space(json, length, &i);
    first = i;
    while (i < length && json[i] >= '0' && json[i] <= '9') ++i;
    return pz_json_parse_uint64(json + first, i - first, value);
}

int pz_json_string_field(const char* json, size_t length, const char* key,
                         size_t from, pz_str* out, size_t* after) {
    size_t pos;

    if (!pz_json_find_key(json, length, key, from, &pos)) return 0;
    return pz_json_read_string_after(json, length, pos, out, after);
}

int pz_json_uint_field(const char* json, size_t length, const char* key,
                       size_t from, uint64_t* value) {
    size_t pos;

    if (!pz_json_find_key(json, length, key, from, &pos)) return 0;
    return pz_json_read_uint_after(json, length, pos, value);
}
