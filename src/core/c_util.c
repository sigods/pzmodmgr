#include "core/c_util.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

void pz_str_init(pz_str* string) {
    if (!string) return;
    string->data = NULL;
    string->length = 0;
    string->capacity = 0;
}

void pz_str_free(pz_str* string) {
    if (!string) return;
    free(string->data);
    pz_str_init(string);
}

void pz_str_clear(pz_str* string) {
    if (!string) return;
    string->length = 0;
    if (string->data) string->data[0] = '\0';
}

int pz_str_reserve(pz_str* string, size_t capacity) {
    size_t next;
    char* data;

    if (!string) return 0;
    if (capacity <= string->capacity) return 1;

    next = string->capacity ? string->capacity : 32;
    while (next < capacity) {
        if (next > SIZE_MAX / 2) {
            next = capacity;
            break;
        }
        next *= 2;
    }

    data = (char*)realloc(string->data, next);
    if (!data) return 0;
    string->data = data;
    string->capacity = next;
    if (string->length == 0) string->data[0] = '\0';
    return 1;
}

int pz_str_assign(pz_str* string, const char* data, size_t length) {
    size_t offset = 0;
    int aliases = 0;

    if (!string) return 0;
    if (!data && length != 0) return 0;
    if (length == SIZE_MAX) return 0;

    if (data && string->data) {
        const uintptr_t base = (uintptr_t)string->data;
        const uintptr_t source = (uintptr_t)data;
        if (source >= base && source < base + string->capacity) {
            aliases = 1;
            offset = source - base;
        }
    }
    if (!pz_str_reserve(string, length + 1)) return 0;
    if (aliases) data = string->data + offset;
    if (length) memmove(string->data, data, length);
    string->data[length] = '\0';
    string->length = length;
    return 1;
}

int pz_str_assign_cstr(pz_str* string, const char* text) {
    return pz_str_assign(string, text, text ? strlen(text) : 0);
}

int pz_str_append(pz_str* string, const char* data, size_t length) {
    const size_t old_length = string ? string->length : 0;
    size_t offset = 0;
    int aliases = 0;

    if (!string) return 0;
    if (!data && length != 0) return 0;
    if (length == 0) return 1;
    if (length > SIZE_MAX - old_length - 1) return 0;

    if (string->data) {
        const uintptr_t base = (uintptr_t)string->data;
        const uintptr_t source = (uintptr_t)data;
        if (source >= base && source < base + string->capacity) {
            aliases = 1;
            offset = source - base;
        }
    }
    if (!pz_str_reserve(string, old_length + length + 1)) return 0;
    if (aliases) data = string->data + offset;
    memmove(string->data + old_length, data, length);
    string->length = old_length + length;
    string->data[string->length] = '\0';
    return 1;
}

int pz_str_append_cstr(pz_str* string, const char* text) {
    return pz_str_append(string, text, text ? strlen(text) : 0);
}

int pz_str_append_char(pz_str* string, char value) {
    return pz_str_append(string, &value, 1);
}

int pz_str_append_u64(pz_str* string, uint64_t value) {
    char reversed[20];
    char digits[20];
    size_t count = 0;
    size_t i;

    do {
        reversed[count++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0);

    for (i = 0; i < count; ++i) digits[i] = reversed[count - 1 - i];
    return pz_str_append(string, digits, count);
}

int pz_str_append_i64(pz_str* string, int64_t value) {
    uint64_t magnitude;

    if (value < 0) {
        /* Avoid negating INT64_MIN in signed arithmetic. */
        magnitude = (uint64_t)(-(value + 1)) + 1;
        if (!pz_str_append_char(string, '-')) return 0;
    } else {
        magnitude = (uint64_t)value;
    }
    return pz_str_append_u64(string, magnitude);
}

const char* pz_str_cstr(const pz_str* string) {
    return string && string->data ? string->data : "";
}

int pz_str_equals(const pz_str* string, const char* data, size_t length) {
    if (!string) return 0;
    if (string->length != length) return 0;
    if (length == 0) return 1;
    return data && memcmp(string->data, data, length) == 0;
}

int pz_vec_init(pz_vec* vector, size_t item_size) {
    if (!vector || item_size == 0) return 0;
    vector->data = NULL;
    vector->length = 0;
    vector->capacity = 0;
    vector->item_size = item_size;
    return 1;
}

void pz_vec_free(pz_vec* vector) {
    if (!vector) return;
    free(vector->data);
    vector->data = NULL;
    vector->length = 0;
    vector->capacity = 0;
    vector->item_size = 0;
}

void pz_vec_clear(pz_vec* vector) {
    if (vector) vector->length = 0;
}

int pz_vec_reserve(pz_vec* vector, size_t count) {
    size_t next;
    size_t bytes;
    unsigned char* data;

    if (!vector || vector->item_size == 0) return 0;
    if (count <= vector->capacity) return 1;
    if (count > SIZE_MAX / vector->item_size) return 0;

    next = vector->capacity ? vector->capacity : 8;
    while (next < count) {
        if (next > SIZE_MAX / 2) {
            next = count;
            break;
        }
        next *= 2;
    }
    if (next > SIZE_MAX / vector->item_size) next = count;
    bytes = next * vector->item_size;

    data = (unsigned char*)realloc(vector->data, bytes);
    if (!data) return 0;
    vector->data = data;
    vector->capacity = next;
    return 1;
}

void* pz_vec_push(pz_vec* vector) {
    void* slot;

    if (!vector || vector->item_size == 0) return NULL;
    if (vector->length == SIZE_MAX) return NULL;
    if (!pz_vec_reserve(vector, vector->length + 1)) return NULL;
    slot = vector->data + vector->length * vector->item_size;
    memset(slot, 0, vector->item_size);
    ++vector->length;
    return slot;
}

int pz_vec_push_copy(pz_vec* vector, const void* item) {
    void* slot;

    if (!item) return 0;
    slot = pz_vec_push(vector);
    if (!slot) return 0;
    memcpy(slot, item, vector->item_size);
    return 1;
}

void* pz_vec_at(pz_vec* vector, size_t index) {
    if (!vector || index >= vector->length) return NULL;
    return vector->data + index * vector->item_size;
}

const void* pz_vec_at_const(const pz_vec* vector, size_t index) {
    if (!vector || index >= vector->length) return NULL;
    return vector->data + index * vector->item_size;
}

void pz_error_clear(pz_error* error) {
    if (!error) return;
    error->code = 0;
    error->kind = PZ_ERROR_GENERIC;
}

void pz_error_set(pz_error* error, int code) {
    if (!error) return;
    error->code = code;
    error->kind = PZ_ERROR_GENERIC;
}

void pz_error_set_system(pz_error* error, int code) {
    if (!error) return;
    error->code = code;
    error->kind = PZ_ERROR_SYSTEM;
}

int pz_error_is_set(const pz_error* error) {
    return error && error->code != 0;
}

int pz_error_value(const pz_error* error) {
    return error ? error->code : 0;
}

int pz_error_format(const pz_error* error, pz_str* out) {
    if (!error || !out) return 0;
    pz_str_clear(out);
    if (error->code == 0) return pz_str_assign_cstr(out, "Success");

#ifdef _WIN32
    {
        char* buffer = NULL;
        const DWORD length = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, (DWORD)error->code, 0, (LPSTR)&buffer, 0, NULL);
        if (length && buffer) {
            int ok = pz_str_assign(out, buffer, (size_t)length);
            LocalFree(buffer);
            if (ok) {
                while (out->length > 0) {
                    const char last = out->data[out->length - 1];
                    if (last != '\r' && last != '\n' && last != ' ' && last != '.')
                        break;
                    --out->length;
                }
                if (out->data) out->data[out->length] = '\0';
                if (out->length != 0) return 1;
            }
        }
    }
#else
    {
        const char* text = strerror(error->code);
        if (text && *text) return pz_str_assign_cstr(out, text);
    }
#endif

    if (!pz_str_assign_cstr(out, "error ")) return 0;
    return pz_str_append_i64(out, (int64_t)error->code);
}

void pz_log_init(pz_log* log) {
    if (!log) return;
    log->callback = NULL;
    log->context = NULL;
}

void pz_log_set(pz_log* log, void (*callback)(void* context, const char* line),
                void* context) {
    if (!log) return;
    log->callback = callback;
    log->context = context;
}

int pz_log_enabled(const pz_log* log) {
    return log && log->callback != NULL;
}

void pz_log_emit(const pz_log* log, const char* line) {
    if (!log || !log->callback) return;
    log->callback(log->context, line ? line : "");
}

void pz_log_emit_str(const pz_log* log, const pz_str* line) {
    if (!line) {
        pz_log_emit(log, "");
        return;
    }
    pz_log_emit(log, pz_str_cstr(line));
}
