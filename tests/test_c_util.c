/* C99 tests for the migration utility layer. */
#include "core/c_util.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void check(int ok, const char* what) {
    if (!ok) {
        ++g_failures;
        printf("FAIL: %s\n", what);
    }
}

static void test_strings(void) {
    pz_str text;
    pz_str_init(&text);
    check(text.data == NULL && text.length == 0 && text.capacity == 0,
          "string init");

    check(pz_str_assign_cstr(&text, "hello"), "string assign");
    check(text.length == 5 && strcmp(pz_str_cstr(&text), "hello") == 0,
          "string assign contents");
    check(pz_str_append_char(&text, ' '), "append char");
    check(pz_str_append_cstr(&text, "world"), "append cstr");
    check(pz_str_equals(&text, "hello world", 11), "string equals");
    check(!pz_str_equals(&text, "hello", 5), "string length mismatch");

    /* Appending a slice of the same buffer must survive reallocation. */
    check(pz_str_append(&text, text.data, text.length),
          "self append");
    check(pz_str_equals(&text, "hello worldhello world", 22),
          "self append contents");

    {
        char long_text[65];
        memset(long_text, 'x', 64);
        long_text[64] = '\0';
        pz_str_free(&text);
        pz_str_init(&text);
        check(pz_str_assign_cstr(&text, long_text), "long string assign");
        check(pz_str_append(&text, text.data, text.length),
              "self append across reallocation");
        check(text.length == 128 && text.data[0] == 'x' &&
                  text.data[127] == 'x',
              "self append across reallocation contents");
    }

    check(pz_str_assign(&text, "a\0b", 3), "embedded NUL assign");
    check(text.length == 3 && text.data[1] == '\0' && text.data[2] == 'b',
          "embedded NUL preserved");
    check(pz_str_equals(&text, "a\0b", 3), "embedded NUL equals");

    pz_str_clear(&text);
    check(text.length == 0 && text.data && text.data[0] == '\0',
          "string clear keeps allocation");

    check(pz_str_append_u64(&text, 0), "u64 zero");
    check(pz_str_equals(&text, "0", 1), "u64 zero contents");
    pz_str_clear(&text);
    check(pz_str_append_u64(&text, UINT64_MAX), "u64 max");
    check(pz_str_equals(&text, "18446744073709551615", 20),
          "u64 max contents");

    pz_str_clear(&text);
    check(pz_str_append_i64(&text, INT64_MIN), "i64 min");
    check(pz_str_equals(&text, "-9223372036854775808", 20),
          "i64 min contents");
    pz_str_clear(&text);
    check(pz_str_append_i64(&text, -42), "i64 negative");
    check(pz_str_equals(&text, "-42", 3), "i64 negative contents");

    check(!pz_str_assign(NULL, "x", 1), "null string assign rejected");
    check(!pz_str_assign(&text, NULL, 1), "null source rejected");
    check(pz_str_assign(&text, NULL, 0), "null empty source accepted");
    check(text.length == 0, "null empty source clears");

    check(!pz_str_reserve(&text, SIZE_MAX), "impossible reserve fails");
    check(pz_str_equals(&text, "", 0), "failed reserve leaves string intact");

    pz_str_free(&text);
    check(text.data == NULL && text.length == 0 && text.capacity == 0,
          "string free");
}

static void test_vector(void) {
    pz_vec values;
    uint64_t* slot;
    uint64_t extra = 33;
    size_t i;

    check(pz_vec_init(&values, sizeof(uint64_t)), "vector init");
    for (i = 0; i < 20; ++i) {
        slot = (uint64_t*)pz_vec_push(&values);
        check(slot != NULL, "vector push");
        if (slot) *slot = (uint64_t)(i + 1);
    }
    check(values.length == 20 && values.capacity >= 20, "vector grows");
    check(*(const uint64_t*)pz_vec_at_const(&values, 0) == 1,
          "vector first item");
    check(*(const uint64_t*)pz_vec_at_const(&values, 19) == 20,
          "vector last item");
    check(pz_vec_at(&values, 20) == NULL, "vector bounds check");
    check(pz_vec_push_copy(&values, &extra), "vector push copy");
    check(*(const uint64_t*)pz_vec_at_const(&values, 20) == extra,
          "vector copy contents");

    pz_vec_clear(&values);
    check(values.length == 0 && values.capacity >= 21, "vector clear");
    check(pz_vec_at(&values, 0) == NULL, "cleared vector has no items");
    check(!pz_vec_reserve(&values, SIZE_MAX), "impossible vector reserve fails");
    pz_vec_free(&values);
    check(values.data == NULL && values.item_size == 0, "vector free");

    check(!pz_vec_init(&values, 0), "zero-size vector rejected");
}

static void test_errors(void) {
    pz_error error;
    pz_str text;

    pz_error_clear(&error);
    pz_str_init(&text);
    check(!pz_error_is_set(&error), "cleared error is unset");
    check(pz_error_format(&error, &text), "success error formats");
    check(pz_str_equals(&text, "Success", 7), "success message");

    pz_error_set(&error, 42);
    check(pz_error_is_set(&error) && error.kind == PZ_ERROR_GENERIC,
          "generic error set");
    check(pz_error_format(&error, &text), "generic error formats");
    check(text.length > 0 && !pz_str_equals(&text, "Success", 7),
          "generic error message");

#ifndef _WIN32
    check(pz_str_equals(&text, strerror(42), strlen(strerror(42))),
          "generic error preserves strerror behavior");
    pz_error_set_system(&error, ENOENT);
    check(error.kind == PZ_ERROR_SYSTEM, "system error kind");
    check(pz_error_format(&error, &text), "system error formats");
    check(text.length > 0 && !pz_str_equals(&text, "error 2", 7),
          "system error uses strerror");
#endif

    pz_str_free(&text);
}

typedef struct log_context {
    int count;
    char first[64];
    char second[64];
} log_context;

static void collect_log(void* context, const char* line) {
    log_context* log = (log_context*)context;
    if (log->count == 0)
        snprintf(log->first, sizeof(log->first), "%s", line);
    else if (log->count == 1)
        snprintf(log->second, sizeof(log->second), "%s", line);
    ++log->count;
}

static void test_log(void) {
    log_context context;
    pz_log log;
    pz_str text;

    context.count = 0;
    context.first[0] = '\0';
    context.second[0] = '\0';
    pz_log_init(&log);
    check(!pz_log_enabled(&log), "log starts disabled");
    pz_log_emit(&log, "ignored");
    pz_log_set(&log, collect_log, &context);
    check(pz_log_enabled(&log), "log enabled");

    pz_str_init(&text);
    pz_str_assign_cstr(&text, "first line");
    pz_log_emit(&log, "second line");
    pz_log_emit_str(&log, &text);
    check(context.count == 2, "log callback count");
    check(strcmp(context.first, "second line") == 0, "log first line");
    check(strcmp(context.second, "first line") == 0, "log string line");
    pz_log_emit(NULL, "ignored");
    pz_str_free(&text);
}

int main(void) {
    test_strings();
    test_vector();
    test_errors();
    test_log();
    if (g_failures != 0) {
        printf("%d C utility test(s) failed\n", g_failures);
        return 1;
    }
    printf("C utility tests passed\n");
    return 0;
}
