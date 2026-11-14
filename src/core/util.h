#ifndef PZ_CORE_C_UTIL_H
#define PZ_CORE_C_UTIL_H

/* Small C99 building blocks for the C migration.
 *
 * pz_str owns a NUL-terminated byte buffer. length excludes the terminator;
 * capacity includes it. A successful assign/append leaves data != NULL unless
 * the string is empty. Allocation failure returns 0 and leaves the previous
 * value intact. pz_vec is a typed dynamic array with no element destructor;
 * callers release owned elements before pz_vec_free().
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pz_str {
    char* data;
    size_t length;
    size_t capacity;
} pz_str;

typedef struct pz_vec {
    unsigned char* data;
    size_t length;
    size_t capacity;
    size_t item_size;
} pz_vec;

typedef enum pz_error_kind {
    PZ_ERROR_GENERIC = 0,
    PZ_ERROR_SYSTEM = 1
} pz_error_kind;

typedef struct pz_error {
    int code;
    pz_error_kind kind;
} pz_error;

typedef struct pz_log {
    void (*callback)(void* context, const char* line);
    void* context;
} pz_log;

void pz_str_init(pz_str* string);
void pz_str_free(pz_str* string);
void pz_str_clear(pz_str* string);
int pz_str_reserve(pz_str* string, size_t capacity);
int pz_str_assign(pz_str* string, const char* data, size_t length);
int pz_str_assign_cstr(pz_str* string, const char* text);
int pz_str_append(pz_str* string, const char* data, size_t length);
int pz_str_append_cstr(pz_str* string, const char* text);
int pz_str_append_char(pz_str* string, char value);
int pz_str_append_u64(pz_str* string, uint64_t value);
int pz_str_append_i64(pz_str* string, int64_t value);
const char* pz_str_cstr(const pz_str* string);
int pz_str_equals(const pz_str* string, const char* data, size_t length);

int pz_vec_init(pz_vec* vector, size_t item_size);
void pz_vec_free(pz_vec* vector);
void pz_vec_clear(pz_vec* vector);
int pz_vec_reserve(pz_vec* vector, size_t count);
void* pz_vec_push(pz_vec* vector);
int pz_vec_push_copy(pz_vec* vector, const void* item);
void* pz_vec_at(pz_vec* vector, size_t index);
const void* pz_vec_at_const(const pz_vec* vector, size_t index);

void pz_error_clear(pz_error* error);
void pz_error_set(pz_error* error, int code);
void pz_error_set_system(pz_error* error, int code);
int pz_error_is_set(const pz_error* error);
int pz_error_value(const pz_error* error);
/* Replaces out with "Success", the OS message, or "error N". */
int pz_error_format(const pz_error* error, pz_str* out);

void pz_log_init(pz_log* log);
void pz_log_set(pz_log* log, void (*callback)(void* context, const char* line),
                void* context);
int pz_log_enabled(const pz_log* log);
void pz_log_emit(const pz_log* log, const char* line);
void pz_log_emit_str(const pz_log* log, const pz_str* line);

#ifdef __cplusplus
}
#endif

#endif
