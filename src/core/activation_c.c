#include "core/activation_c.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static void trim_span(const char** data, size_t* length) {
    while (*length != 0) {
        const unsigned char first = (unsigned char)**data;
        if (!isspace(first)) break;
        ++*data;
        --*length;
    }
    while (*length != 0) {
        const unsigned char last = (unsigned char)(*data)[*length - 1];
        if (!isspace(last)) break;
        --*length;
    }
}

static int is_blank(const char* data, size_t length) {
    trim_span(&data, &length);
    return length == 0;
}

static int starts_with_mod(const char* line, size_t length) {
    return length >= 3 && memcmp(line, "mod", 3) == 0 &&
           (length == 3 || isspace((unsigned char)line[3]) || line[3] == '=');
}

static void normalize_mod_id(const char** data, size_t* length) {
    trim_span(data, length);
    while (*length != 0 && ((*data)[0] == '\\' || (*data)[0] == '/')) {
        ++*data;
        --*length;
    }
    trim_span(data, length);
}

static int add_unique_string(pz_vec* list, const char* data, size_t length) {
    size_t i;
    pz_str* slot;

    if (length == 0) return 1;
    for (i = 0; i < list->length; ++i) {
        const pz_str* existing = (const pz_str*)pz_vec_at_const(list, i);
        if (pz_str_equals(existing, data, length)) return 1;
    }
    slot = (pz_str*)pz_vec_push(list);
    if (!slot) return 0;
    if (!pz_str_assign(slot, data, length)) {
        --list->length;
        return 0;
    }
    return 1;
}

static int add_unique_u64(pz_vec* list, uint64_t value) {
    size_t i;
    uint64_t* slot;

    if (value == 0) return 1;
    for (i = 0; i < list->length; ++i) {
        const uint64_t* existing = (const uint64_t*)pz_vec_at_const(list, i);
        if (*existing == value) return 1;
    }
    slot = (uint64_t*)pz_vec_push(list);
    if (!slot) return 0;
    *slot = value;
    return 1;
}

void pz_activation_string_list_free(pz_vec* list) {
    size_t i;
    if (!list) return;
    for (i = 0; i < list->length; ++i)
        pz_str_free((pz_str*)pz_vec_at(list, i));
    pz_vec_free(list);
}

static int line_value(const char* line, size_t length, const char* key,
                      const char** value, size_t* value_length, int* matched) {
    const char* equals = (const char*)memchr(line, '=', length);
    const char* key_end;
    size_t key_length;

    *matched = 0;
    if (!equals) return 1;
    key_end = equals;
    while (key_end > line && isspace((unsigned char)key_end[-1])) --key_end;
    while (line < key_end && isspace((unsigned char)line[0])) ++line;
    key_length = (size_t)(key_end - line);
    if (key_length != strlen(key) || memcmp(line, key, key_length) != 0)
        return 1;
    *matched = 1;
    *value = equals + 1;
    *value_length = length - (size_t)(equals + 1 - line);
    return 1;
}

int pz_activation_block_uses_backslash(const char* text, size_t length) {
    size_t start = 0;

    if (!text && length != 0) return 0;
    while (start < length) {
        size_t end = start;
        size_t line_end;
        const char* line;
        size_t line_length;
        const char* value;
        size_t value_length;
        int matched;

        while (end < length && text[end] != '\n') ++end;
        line_end = end;
        if (line_end > start && text[line_end - 1] == '\r') --line_end;
        line = text + start;
        line_length = line_end - start;
        trim_span(&line, &line_length);
        if (starts_with_mod(line, line_length)) {
            if (!line_value(line, line_length, "mod", &value, &value_length,
                            &matched))
                return 0;
            (void)matched;
            trim_span(&value, &value_length);
            if (value_length != 0 && value[0] == '\\') return 1;
        }
        if (end == length) break;
        start = end + 1;
    }
    return 0;
}

int pz_activation_parse_block_ids(const char* text, size_t length, pz_vec* out) {
    size_t start = 0;
    int ok = 1;

    if (!out || (!text && length != 0)) return 0;
    pz_vec_init(out, sizeof(pz_str));
    while (ok && start < length) {
        size_t end = start;
        size_t line_end;
        const char* line;
        size_t line_length;
        const char* equals;

        while (end < length && text[end] != '\n') ++end;
        line_end = end;
        if (line_end > start && text[line_end - 1] == '\r') --line_end;
        line = text + start;
        line_length = line_end - start;
        trim_span(&line, &line_length);
        if (starts_with_mod(line, line_length)) {
            equals = (const char*)memchr(line, '=', line_length);
            if (equals) {
                const char* value = equals + 1;
                size_t value_length = line_length - (size_t)(equals + 1 - line);
                trim_span(&value, &value_length);
                if (value_length != 0 && value[value_length - 1] == ',')
                    --value_length;
                normalize_mod_id(&value, &value_length);
                ok = add_unique_string(out, value, value_length);
            }
        }
        if (end == length) break;
        start = end + 1;
    }
    if (!ok) pz_activation_string_list_free(out);
    return ok;
}

int pz_activation_parse_ini_ids(const char* text, size_t length, pz_vec* out,
                                int* backslash) {
    size_t start = 0;
    int ok = 1;

    if (!out || (!text && length != 0)) return 0;
    pz_vec_init(out, sizeof(pz_str));
    if (backslash) *backslash = 0;
    while (ok && start < length) {
        size_t end = start;
        size_t line_end;
        const char* line;
        size_t line_length;
        const char* value;
        size_t value_length;
        int matched;
        size_t part = 0;

        while (end < length && text[end] != '\n') ++end;
        line_end = end;
        if (line_end > start && text[line_end - 1] == '\r') --line_end;
        line = text + start;
        line_length = line_end - start;
        if (!line_value(line, line_length, "Mods", &value, &value_length,
                        &matched)) {
            ok = 0;
            break;
        }
        if (!matched) {
            if (end == length) break;
            start = end + 1;
            continue;
        }
        while (ok) {
            size_t separator = part;
            const char* raw;
            size_t raw_length;
            while (separator < value_length && value[separator] != ';')
                ++separator;
            raw = value + part;
            raw_length = separator - part;
            trim_span(&raw, &raw_length);
            if (raw_length != 0 && raw[0] == '\\' && backslash) *backslash = 1;
            normalize_mod_id(&raw, &raw_length);
            ok = add_unique_string(out, raw, raw_length);
            if (separator == value_length) break;
            part = separator + 1;
        }
        if (end == length) break;
        start = end + 1;
    }
    if (!ok) pz_activation_string_list_free(out);
    return ok;
}

static int parse_u64(const char* data, size_t length, uint64_t* out) {
    uint64_t value = 0;
    size_t i;

    if (length == 0) return 0;
    for (i = 0; i < length; ++i) {
        const unsigned digit = (unsigned)(data[i] - '0');
        if (data[i] < '0' || data[i] > '9') return 0;
        if (value > (UINT64_MAX - digit) / 10) return 0;
        value = value * 10 + digit;
    }
    *out = value;
    return 1;
}

int pz_activation_parse_ini_workshop_ids(const char* text, size_t length,
                                         pz_vec* out) {
    size_t start = 0;
    int ok = 1;

    if (!out || (!text && length != 0)) return 0;
    pz_vec_init(out, sizeof(uint64_t));
    while (ok && start < length) {
        size_t end = start;
        size_t line_end;
        const char* line;
        size_t line_length;
        const char* value;
        size_t value_length;
        int matched;
        size_t part = 0;

        while (end < length && text[end] != '\n') ++end;
        line_end = end;
        if (line_end > start && text[line_end - 1] == '\r') --line_end;
        line = text + start;
        line_length = line_end - start;
        if (!line_value(line, line_length, "WorkshopItems", &value,
                        &value_length, &matched)) {
            ok = 0;
            break;
        }
        if (!matched) {
            if (end == length) break;
            start = end + 1;
            continue;
        }
        while (ok) {
            size_t separator = part;
            const char* raw;
            size_t raw_length;
            uint64_t id;
            while (separator < value_length && value[separator] != ';')
                ++separator;
            raw = value + part;
            raw_length = separator - part;
            trim_span(&raw, &raw_length);
            if (parse_u64(raw, raw_length, &id))
                ok = add_unique_u64(out, id);
            if (separator == value_length) break;
            part = separator + 1;
        }
        if (end == length) break;
        start = end + 1;
    }
    if (!ok) pz_vec_free(out);
    return ok;
}

static int string_list_equals(const pz_vec* a, const pz_vec* b) {
    size_t i;
    if (!a || !b || a->length != b->length) return 0;
    for (i = 0; i < a->length; ++i) {
        const pz_str* left = (const pz_str*)pz_vec_at_const(a, i);
        const pz_str* right = (const pz_str*)pz_vec_at_const(b, i);
        if (!pz_str_equals(left, right->data, right->length)) return 0;
    }
    return 1;
}

static int u64_list_equals(const pz_vec* a, const pz_vec* b) {
    size_t i;
    if (!a || !b || a->length != b->length) return 0;
    for (i = 0; i < a->length; ++i) {
        const uint64_t* left = (const uint64_t*)pz_vec_at_const(a, i);
        const uint64_t* right = (const uint64_t*)pz_vec_at_const(b, i);
        if (*left != *right) return 0;
    }
    return 1;
}

static int split_lines(const char* text, size_t length, pz_vec* lines) {
    size_t start = 0;

    pz_vec_init(lines, sizeof(pz_str));
    while (start < length) {
        size_t end = start;
        size_t line_end;
        pz_str* slot;
        while (end < length && text[end] != '\n') ++end;
        line_end = end;
        if (line_end > start && text[line_end - 1] == '\r') --line_end;
        slot = (pz_str*)pz_vec_push(lines);
        if (!slot || !pz_str_assign(slot, text + start, line_end - start)) {
            if (slot) {
                pz_str_free(slot);
                --lines->length;
            }
            pz_activation_string_list_free(lines);
            return 0;
        }
        if (end == length) break;
        start = end + 1;
    }
    return 1;
}

static int push_line(pz_vec* lines, const char* data, size_t length) {
    pz_str* slot = (pz_str*)pz_vec_push(lines);
    if (!slot) return 0;
    if (!pz_str_assign(slot, data, length)) {
        pz_str_free(slot);
        --lines->length;
        return 0;
    }
    return 1;
}

static int push_line_copy(pz_vec* lines, const pz_str* line) {
    return push_line(lines, line->data, line->length);
}

static int join_lines(const pz_vec* lines, int crlf, pz_str* out) {
    const char* eol = crlf ? "\r\n" : "\n";
    const size_t eol_length = crlf ? 2 : 1;
    size_t i;

    pz_str_clear(out);
    for (i = 0; i < lines->length; ++i) {
        const pz_str* line = (const pz_str*)pz_vec_at_const(lines, i);
        if (!pz_str_append(out, line->data, line->length) ||
            !pz_str_append(out, eol, eol_length))
            return 0;
    }
    return 1;
}

static int uses_crlf(const char* text, size_t length) {
    size_t i;
    for (i = 0; i + 1 < length; ++i)
        if (text[i] == '\r' && text[i + 1] == '\n') return 1;
    return 0;
}

static int build_block(const pz_vec* ids, pz_vec* block) {
    size_t i;
    pz_vec_init(block, sizeof(pz_str));
    if (!push_line(block, "mods", 4) || !push_line(block, "{", 1))
        goto fail;
    for (i = 0; i < ids->length; ++i) {
        const pz_str* id = (const pz_str*)pz_vec_at_const(ids, i);
        pz_str* line = (pz_str*)pz_vec_push(block);
        if (!line) goto fail;
        if (!pz_str_assign_cstr(line, "    mod = ") ||
            !pz_str_append(line, id->data, id->length) ||
            !pz_str_append_char(line, ',')) {
            pz_str_free(line);
            --block->length;
            goto fail;
        }
    }
    if (!push_line(block, "}", 1)) goto fail;
    return 1;
fail:
    pz_activation_string_list_free(block);
    return 0;
}

int pz_activation_rewrite_block_ids(const char* text, size_t length,
                                    const pz_vec* ids, pz_str* out,
                                    int* changed) {
    pz_vec parsed;
    int differs;
    int crlf;
    pz_vec lines;
    pz_vec block;
    pz_vec result;
    size_t mods_line = SIZE_MAX;
    size_t open = SIZE_MAX;
    size_t close = SIZE_MAX;
    size_t i;
    int ok = 0;

    if (!out || (!text && length != 0) || !ids) return 0;
    if (!pz_activation_parse_block_ids(text, length, &parsed)) return 0;
    differs = pz_activation_block_uses_backslash(text, length) ||
              !string_list_equals(&parsed, ids);
    pz_activation_string_list_free(&parsed);
    if (changed) *changed = differs;
    if (!differs) return pz_str_assign(out, text, length);

    crlf = uses_crlf(text, length);
    if (!split_lines(text, length, &lines)) return 0;
    if (!build_block(ids, &block)) {
        pz_activation_string_list_free(&lines);
        return 0;
    }

    for (i = 0; i < lines.length; ++i) {
        const pz_str* line = (const pz_str*)pz_vec_at_const(&lines, i);
        const char* data = line->data;
        size_t line_length = line->length;
        trim_span(&data, &line_length);
        if (line_length == 4 && memcmp(data, "mods", 4) == 0) {
            mods_line = i;
            break;
        }
    }
    if (mods_line != SIZE_MAX) {
        for (i = mods_line + 1; i < lines.length; ++i) {
            const pz_str* line = (const pz_str*)pz_vec_at_const(&lines, i);
            const char* data = line->data;
            size_t line_length = line->length;
            trim_span(&data, &line_length);
            if (line_length == 1 && data[0] == '{') {
                open = i;
                break;
            }
        }
    }
    if (open != SIZE_MAX) {
        int depth = 0;
        for (i = open; i < lines.length; ++i) {
            const pz_str* line = (const pz_str*)pz_vec_at_const(&lines, i);
            const char* data = line->data;
            size_t line_length = line->length;
            size_t j;
            trim_span(&data, &line_length);
            for (j = 0; j < line_length; ++j) {
                if (data[j] == '{') ++depth;
                else if (data[j] == '}') --depth;
            }
            if (depth == 0) {
                close = i;
                break;
            }
        }
    }

    pz_vec_init(&result, sizeof(pz_str));
    if (open != SIZE_MAX && close > open) {
        for (i = 0; i < mods_line; ++i) {
            if (!push_line_copy(&result,
                                (const pz_str*)pz_vec_at_const(&lines, i)))
                goto done;
        }
        for (i = 0; i < block.length; ++i) {
            if (!push_line_copy(&result,
                                (const pz_str*)pz_vec_at_const(&block, i)))
                goto done;
        }
        for (i = close + 1; i < lines.length; ++i) {
            if (!push_line_copy(&result,
                                (const pz_str*)pz_vec_at_const(&lines, i)))
                goto done;
        }
    } else if (lines.length == 0) {
        if (!push_line(&result, "version = 1,", 12) ||
            !push_line(&result, "", 0))
            goto done;
        for (i = 0; i < block.length; ++i) {
            if (!push_line_copy(&result,
                                (const pz_str*)pz_vec_at_const(&block, i)))
                goto done;
        }
    } else {
        for (i = 0; i < lines.length; ++i) {
            if (!push_line_copy(&result,
                                (const pz_str*)pz_vec_at_const(&lines, i)))
                goto done;
        }
        if (lines.length != 0) {
            const pz_str* last =
                (const pz_str*)pz_vec_at_const(&lines, lines.length - 1);
            if (!is_blank(last->data, last->length) &&
                !push_line(&result, "", 0))
                goto done;
        }
        for (i = 0; i < block.length; ++i) {
            if (!push_line_copy(&result,
                                (const pz_str*)pz_vec_at_const(&block, i)))
                goto done;
        }
    }
    ok = join_lines(&result, crlf, out);

done:
    pz_activation_string_list_free(&result);
    pz_activation_string_list_free(&block);
    pz_activation_string_list_free(&lines);
    return ok;
}

static int build_ini_mods_value(const pz_vec* ids, int backslash, pz_str* out) {
    size_t i;
    pz_str_clear(out);
    for (i = 0; i < ids->length; ++i) {
        const pz_str* id = (const pz_str*)pz_vec_at_const(ids, i);
        if (i != 0 && !pz_str_append_char(out, ';')) return 0;
        if (backslash && !pz_str_append_char(out, '\\')) return 0;
        if (!pz_str_append(out, id->data, id->length)) return 0;
    }
    return 1;
}

static int build_workshop_value(const pz_vec* ids, pz_str* out) {
    size_t i;
    pz_str_clear(out);
    for (i = 0; i < ids->length; ++i) {
        const uint64_t* id = (const uint64_t*)pz_vec_at_const(ids, i);
        if (i != 0 && !pz_str_append_char(out, ';')) return 0;
        if (!pz_str_append_u64(out, *id)) return 0;
    }
    return 1;
}

static int rewrite_ini(const char* text, size_t length, const char* key,
                       const pz_str* value, int ids_empty, pz_str* out,
                       int* changed, int changed_value) {
    pz_vec lines;
    int replaced = 0;
    int crlf;
    size_t i;
    int ok = 0;

    if (changed) *changed = changed_value;
    crlf = uses_crlf(text, length);
    if (!split_lines(text, length, &lines)) return 0;
    for (i = 0; i < lines.length; ++i) {
        pz_str* line = (pz_str*)pz_vec_at(&lines, i);
        const char* found;
        size_t found_length;
        int matched;
        if (!line_value(line->data, line->length, key, &found, &found_length,
                        &matched))
            goto done;
        if (!matched) continue;
        if (!pz_str_assign_cstr(line, key) ||
            !pz_str_append_char(line, '=') ||
            !pz_str_append(line, value->data, value->length))
            goto done;
        replaced = 1;
        break;
    }
    if (!replaced) {
        const char* trimmed = text;
        size_t trimmed_length = length;
        trim_span(&trimmed, &trimmed_length);
        if (ids_empty && trimmed_length == 0) {
            ok = pz_str_assign(out, text, length);
            goto done;
        }
        {
            pz_str line;
            pz_str_init(&line);
            if (!pz_str_assign_cstr(&line, key) ||
                !pz_str_append_char(&line, '=') ||
                !pz_str_append(&line, value->data, value->length) ||
                !push_line_copy(&lines, &line)) {
                pz_str_free(&line);
                goto done;
            }
            pz_str_free(&line);
        }
    }
    ok = join_lines(&lines, crlf, out);

done:
    pz_activation_string_list_free(&lines);
    return ok;
}

int pz_activation_rewrite_ini_mods(const char* text, size_t length,
                                   const pz_vec* ids, int backslash,
                                   pz_str* out, int* changed) {
    pz_vec parsed;
    pz_str value;
    int same;
    int ok;
    int old_backslash = 0;

    if (!out || (!text && length != 0) || !ids) return 0;
    if (!pz_activation_parse_ini_ids(text, length, &parsed, &old_backslash))
        return 0;
    same = string_list_equals(&parsed, ids);
    pz_activation_string_list_free(&parsed);
    pz_str_init(&value);
    if (!build_ini_mods_value(ids, backslash, &value)) {
        pz_str_free(&value);
        return 0;
    }
    ok = rewrite_ini(text, length, "Mods", &value, ids->length == 0, out,
                     changed, !same);
    pz_str_free(&value);
    return ok;
}

int pz_activation_rewrite_ini_workshop_items(const char* text, size_t length,
                                             const pz_vec* workshop_ids,
                                             pz_str* out, int* changed) {
    pz_vec parsed;
    pz_str value;
    int same;
    int ok;

    if (!out || (!text && length != 0) || !workshop_ids) return 0;
    if (!pz_activation_parse_ini_workshop_ids(text, length, &parsed)) return 0;
    same = u64_list_equals(&parsed, workshop_ids);
    pz_vec_free(&parsed);
    pz_str_init(&value);
    if (!build_workshop_value(workshop_ids, &value)) {
        pz_str_free(&value);
        return 0;
    }
    ok = rewrite_ini(text, length, "WorkshopItems", &value,
                     workshop_ids->length == 0, out, changed, !same);
    pz_str_free(&value);
    return ok;
}
