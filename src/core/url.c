#include "core/url_c.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int is_trim_char(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int parse_digits(const char* input, size_t begin, size_t limit,
                        uint64_t* out) {
    uint64_t value = 0;
    size_t end = begin;
    size_t i;

    while (end < limit && input[end] >= '0' && input[end] <= '9') ++end;
    if (end == begin || end - begin > 20) return 0;
    for (i = begin; i < end; ++i) {
        const unsigned digit = (unsigned)(input[i] - '0');
        if (value > (UINT64_MAX - digit) / 10) return 0;
        value = value * 10 + digit;
    }
    *out = value;
    return 1;
}

int pz_url_parse_workshop_id(const char* input, size_t length, uint64_t* out) {
    size_t begin = 0;
    size_t end = length;
    size_t i;

    if (!input || !out) return 0;

    while (begin < end && is_trim_char(input[begin])) ++begin;
    while (end > begin && is_trim_char(input[end - 1])) --end;
    if (begin == end) return 0;

    for (i = begin; i < end; ++i)
        if (input[i] < '0' || input[i] > '9') break;
    if (i == end) return parse_digits(input, begin, end, out);

    for (i = begin; end - i >= 3; ++i) {
        if (memcmp(input + i, "id=", 3) == 0)
            return parse_digits(input, i + 3, end, out);
    }
    return 0;
}

static int append_unique(uint64_t** items, size_t* count, size_t* capacity,
                         uint64_t id) {
    size_t i;
    for (i = 0; i < *count; ++i)
        if ((*items)[i] == id) return 1;

    if (*count == *capacity) {
        const size_t next = *capacity ? *capacity * 2 : 8;
        uint64_t* grown = (uint64_t*)realloc(*items, next * sizeof(uint64_t));
        if (!grown) return 0;
        *items = grown;
        *capacity = next;
    }
    (*items)[(*count)++] = id;
    return 1;
}

int pz_url_parse_workshop_ids(const char* input, size_t length, pz_url_ids* out) {
    size_t i = 0;
    size_t capacity = 0;

    if (!out) return 0;
    out->items = NULL;
    out->count = 0;
    if (!input) return length == 0;

    while (i < length) {
        size_t start;
        uint64_t id;

        while (i < length && isspace((unsigned char)input[i])) ++i;
        if (i == length) break;
        start = i;
        while (i < length && !isspace((unsigned char)input[i])) ++i;

        if (!pz_url_parse_workshop_id(input + start, i - start, &id)) {
            pz_url_ids_free(out);
            return 0;
        }
        if (!append_unique(&out->items, &out->count, &capacity, id)) {
            pz_url_ids_free(out);
            return 0;
        }
    }
    return 1;
}

void pz_url_ids_free(pz_url_ids* ids) {
    if (!ids) return;
    free(ids->items);
    ids->items = NULL;
    ids->count = 0;
}
