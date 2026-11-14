#define _POSIX_C_SOURCE 200809L

#include "core/process_c.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static int str_equals(const pz_str* value, const char* expected) {
    return value && value->length == strlen(expected) &&
           (value->length == 0 || memcmp(value->data, expected, value->length) == 0);
}

static void count_line(void* context, const char* line) {
    int* count = (int*)context;
    (void)line;
    ++*count;
}

static void test_quote(void) {
    pz_str out;

    pz_str_init(&out);
    check(pz_process_quote(&out, "simple", 6), "quote succeeds");
#ifdef _WIN32
    check(str_equals(&out, "\"simple\""), "quote wraps in double quotes");
#else
    check(str_equals(&out, "'simple'"), "quote wraps in single quotes");
#endif

    pz_str_clear(&out);
    check(pz_process_quote(&out, "with space", 10), "quote handles a space");
#ifdef _WIN32
    check(str_equals(&out, "\"with space\""), "space stays inside the quotes");
#else
    check(str_equals(&out, "'with space'"), "space stays inside the quotes");
#endif

#ifndef _WIN32
    pz_str_clear(&out);
    check(pz_process_quote(&out, "it's", 4), "quote handles an apostrophe");
    check(str_equals(&out, "'it'\\''s'"), "apostrophe is escaped for the shell");
#endif

    pz_str_clear(&out);
    check(pz_process_quote(&out, "", 0), "quote handles an empty argument");
#ifdef _WIN32
    check(str_equals(&out, "\"\""), "empty argument is quoted");
#else
    check(str_equals(&out, "''"), "empty argument is quoted");
#endif

    pz_str_free(&out);
}

static void test_capture(void) {
    pz_str output;
    pz_log log;
    int exit_code = -1;
    int lines = 0;

    pz_str_init(&output);
    pz_log_set(&log, count_line, &lines);

    {
        static const char command[] = "printf 'one\\ntwo\\n'";
        check(pz_process_capture(command, sizeof(command) - 1, &output, &log,
                                 &exit_code),
              "capture runs the command");
        check(exit_code == 0, "capture reports a zero exit status");
        check(str_equals(&output, "one\ntwo\n"), "capture keeps combined output");
        check(lines == 2, "capture reports each complete line once");
    }

    pz_str_clear(&output);
    lines = 0;
    {
        static const char command[] = "printf 'tail'";
        check(pz_process_capture(command, sizeof(command) - 1, &output, &log,
                                 &exit_code),
              "capture runs a command without a trailing newline");
        check(str_equals(&output, "tail"), "capture keeps the unterminated tail");
        check(lines == 1, "capture reports the unterminated last line");
    }

    pz_str_clear(&output);
    lines = 0;
    {
        static const char command[] = "printf 'a\\r\\nb\\r\\n' | cat";
        check(pz_process_capture(command, sizeof(command) - 1, &output, NULL,
                                 &exit_code),
              "capture runs without a log sink");
        check(exit_code == 0, "capture without a log still reports the status");
        check(str_equals(&output, "a\r\nb\r\n"), "capture is byte exact");
    }

    pz_str_clear(&output);
    {
        static const char command[] = "exit 3";
        check(pz_process_capture(command, sizeof(command) - 1, &output, NULL,
                                 &exit_code),
              "capture runs a failing command");
        check(exit_code != 0, "capture reports a failing exit status");
    }

    pz_str_free(&output);
}

static void test_processes(void) {
    pz_path exe;
    pz_str args[2];
    unsigned long pid = 0;
    unsigned long self = pz_process_current_id();

    check(self != 0, "current process id is available");
    check(pz_process_is_running(self), "the current process is running");

    pz_path_init(&exe);
    pz_str_init(&args[0]);
    pz_str_init(&args[1]);

#ifdef _WIN32
    pz_path_assign_cstr(&exe, "cmd.exe");
    pz_str_assign_cstr(&args[0], "/c");
    pz_str_assign_cstr(&args[1], "exit 0");
#else
    pz_path_assign_cstr(&exe, "/bin/sh");
    pz_str_assign_cstr(&args[0], "-c");
    pz_str_assign_cstr(&args[1], "exit 0");
#endif

    check(pz_process_start_detached(&exe, args, 2, &pid), "detached child starts");
    check(pid != 0, "detached child has a pid");
    check(pz_process_wait(pid), "wait sees the child exit");
    check(!pz_process_is_running(pid), "an exited child is not running");

    check(pz_process_wait(0), "waiting for pid 0 succeeds immediately");

    pz_str_free(&args[1]);
    pz_str_free(&args[0]);
    pz_path_free(&exe);
}

int main(void) {
    test_quote();
    test_capture();
    test_processes();

    if (failures) {
        fprintf(stderr, "%d process test(s) failed\n", failures);
        return 1;
    }
    printf("all process tests passed\n");
    return 0;
}
