#define _POSIX_C_SOURCE 200809L

#include "core/process_c.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wchar.h>
#else
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char** environ;
#endif

typedef int (*pz_process_sink)(void* context, const char* data, size_t length);

/* ------------------------------------------------------------ Windows text */

#ifdef _WIN32

static wchar_t* utf8_to_wide(const char* text, size_t length) {
    int count;
    wchar_t* out;

    if (!text || length == 0 || length > (size_t)INT_MAX) return NULL;
    count = MultiByteToWideChar(CP_UTF8, 0, text, (int)length, NULL, 0);
    if (count <= 0) return NULL;
    out = (wchar_t*)malloc(((size_t)count + 1) * sizeof(wchar_t));
    if (!out) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, text, (int)length, out, count)) {
        free(out);
        return NULL;
    }
    out[count] = L'\0';
    return out;
}

/* Appends a wide run as UTF-8. WideCharToMultiByte handles surrogate pairs, so
 * paths and arguments containing non-BMP characters survive unchanged. */
static int append_wide(pz_str* out, const wchar_t* text, size_t length) {
    int count;
    char* buffer;
    int ok;

    if (length == 0) return 1;
    if (length > (size_t)INT_MAX) return 0;
    count = WideCharToMultiByte(CP_UTF8, 0, text, (int)length, NULL, 0, NULL, NULL);
    if (count <= 0) return 0;
    buffer = (char*)malloc((size_t)count);
    if (!buffer) return 0;
    if (!WideCharToMultiByte(CP_UTF8, 0, text, (int)length, buffer, count, NULL, NULL)) {
        free(buffer);
        return 0;
    }
    ok = pz_str_append(out, buffer, (size_t)count);
    free(buffer);
    return ok;
}

/* MSVC/MinGW argument quoting for the CreateProcessW command line. */
static int quote_windows_arg(const wchar_t* arg, pz_str* out) {
    const wchar_t* cursor;
    size_t i;

    if (!arg || !*arg) return pz_str_append_cstr(out, "\"\"");
    if (!wcspbrk(arg, L" \t\n\v\"")) return append_wide(out, arg, wcslen(arg));
    if (!pz_str_append_char(out, '"')) return 0;

    cursor = arg;
    for (;;) {
        size_t slashes = 0;
        const wchar_t* run;

        while (*cursor == L'\\') {
            ++cursor;
            ++slashes;
        }
        if (!*cursor) {
            /* Backslashes before the closing quote must be doubled. */
            for (i = 0; i < slashes * 2; ++i)
                if (!pz_str_append_char(out, '\\')) return 0;
            break;
        }
        if (*cursor == L'"') {
            for (i = 0; i < slashes * 2 + 1; ++i)
                if (!pz_str_append_char(out, '\\')) return 0;
            if (!pz_str_append_char(out, '"')) return 0;
            ++cursor;
            continue;
        }
        for (i = 0; i < slashes; ++i)
            if (!pz_str_append_char(out, '\\')) return 0;
        run = cursor;
        while (*cursor && *cursor != L'\\' && *cursor != L'"') ++cursor;
        if (!append_wide(out, run, (size_t)(cursor - run))) return 0;
    }
    return pz_str_append_char(out, '"');
}

static int append_quoted(pz_str* out, const pz_str* value) {
    wchar_t* wide;
    int ok;

    if (!value || value->length == 0) return quote_windows_arg(L"", out);
    wide = utf8_to_wide(value->data, value->length);
    if (!wide) return 0;
    ok = quote_windows_arg(wide, out);
    free(wide);
    return ok;
}

#endif /* _WIN32 */

/* --------------------------------------------------------------- spawning */

#ifdef _WIN32

static int spawn_capture(const char* command, size_t length,
                         pz_process_sink sink, void* context, int* exit_code) {
    static const wchar_t prefix[] = L"cmd.exe /c \"";
    const size_t prefix_length = (sizeof(prefix) / sizeof(prefix[0])) - 1;
    SECURITY_ATTRIBUTES attributes;
    HANDLE read_pipe = NULL;
    HANDLE write_pipe = NULL;
    HANDLE null_input = INVALID_HANDLE_VALUE;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    wchar_t* wide_command = NULL;
    wchar_t* line;
    size_t command_length = 0;
    char buffer[4096];
    DWORD got = 0;
    int ok = 1;

    *exit_code = -1;
    if (command && length) {
        wide_command = utf8_to_wide(command, length);
        if (!wide_command) return 0;
        command_length = wcslen(wide_command);
    }
    line = (wchar_t*)malloc((prefix_length + command_length + 2) * sizeof(wchar_t));
    if (!line) {
        free(wide_command);
        return 0;
    }
    memcpy(line, prefix, prefix_length * sizeof(wchar_t));
    if (wide_command)
        memcpy(line + prefix_length, wide_command, command_length * sizeof(wchar_t));
    free(wide_command);
    line[prefix_length + command_length] = L'"';
    line[prefix_length + command_length + 1] = L'\0';

    memset(&attributes, 0, sizeof(attributes));
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    if (!CreatePipe(&read_pipe, &write_pipe, &attributes, 0)) {
        free(line);
        return 0;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    null_input = CreateFileW(L"NUL", GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                             OPEN_EXISTING, 0, NULL);
    if (null_input == INVALID_HANDLE_VALUE) {
        ok = 0;
        goto done;
    }

    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_input;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    memset(&process, 0, sizeof(process));
    /* CREATE_NO_WINDOW keeps every curl.exe/steamcmd.exe child from flashing a
     * console window: pzmodmgr is a GUI-subsystem program and owns none. */
    if (!CreateProcessW(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL,
                        &startup, &process)) {
        ok = 0;
        goto done;
    }

    CloseHandle(write_pipe);
    write_pipe = NULL;
    CloseHandle(null_input);
    null_input = INVALID_HANDLE_VALUE;

    while (ReadFile(read_pipe, buffer, (DWORD)sizeof(buffer), &got, NULL) && got > 0) {
        if (!sink(context, buffer, (size_t)got)) ok = 0;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    {
        DWORD code = 0;
        if (GetExitCodeProcess(process.hProcess, &code)) *exit_code = (int)code;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

done:
    if (write_pipe) CloseHandle(write_pipe);
    if (null_input != INVALID_HANDLE_VALUE) CloseHandle(null_input);
    if (read_pipe) CloseHandle(read_pipe);
    free(line);
    return ok;
}

#else

static int spawn_capture(const char* command, size_t length,
                         pz_process_sink sink, void* context, int* exit_code) {
    FILE* pipe;
    char* text;
    char buffer[4096];
    size_t got;
    int ok = 1;

    *exit_code = -1;
    text = (char*)malloc(length + 1);
    if (!text) return 0;
    if (length) memcpy(text, command, length);
    text[length] = '\0';
    pipe = popen(text, "r");
    free(text);
    if (!pipe) return 0;
    while ((got = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
        if (!sink(context, buffer, got)) ok = 0;
    if (ferror(pipe)) ok = 0;
    *exit_code = pclose(pipe);
    return ok;
}

#endif

/* ---------------------------------------------------------------- capture */

typedef struct capture_sink {
    pz_str* output;
    pz_str pending;
    const pz_log* log;
    int ok;
} capture_sink;

/* Reports every complete line in pending and drops it from the buffer. The
 * buffer stays NUL-terminated, so lines can be handed out without a copy. */
static void capture_flush_lines(capture_sink* sink) {
    size_t start = 0;

    for (;;) {
        char* base = sink->pending.data;
        char* newline;
        size_t line_length;

        if (!base || start >= sink->pending.length) break;
        newline = (char*)memchr(base + start, '\n', sink->pending.length - start);
        if (!newline) break;
        line_length = (size_t)(newline - (base + start));
        *newline = '\0';
        if (line_length && base[start + line_length - 1] == '\r') {
            base[start + line_length - 1] = '\0';
            --line_length;
        }
        if (line_length) pz_log_emit(sink->log, base + start);
        start = (size_t)(newline - base) + 1;
    }
    if (start) {
        memmove(sink->pending.data, sink->pending.data + start,
                sink->pending.length - start);
        sink->pending.length -= start;
        sink->pending.data[sink->pending.length] = '\0';
    }
}

/* Reports the last line when the child exits without a trailing newline. */
static void capture_finish(capture_sink* sink) {
    size_t length;

    capture_flush_lines(sink);
    if (!sink->log || !sink->pending.data) return;
    length = sink->pending.length;
    if (!length) return;
    if (sink->pending.data[length - 1] == '\r') --length;
    sink->pending.data[length] = '\0';
    if (length) pz_log_emit(sink->log, sink->pending.data);
    sink->pending.length = 0;
}

static int capture_sink_cb(void* context, const char* data, size_t length) {
    capture_sink* sink = (capture_sink*)context;

    if (!pz_str_append(sink->output, data, length)) {
        sink->ok = 0;
        return 0;
    }
    if (!sink->log) return 1;
    if (!pz_str_append(&sink->pending, data, length)) {
        sink->ok = 0;
        return 0;
    }
    capture_flush_lines(sink);
    return 1;
}

int pz_process_quote(pz_str* out, const char* value, size_t length) {
    size_t i;

    if (!out) return 0;
    if (!value && length) return 0;
#ifdef _WIN32
    if (!pz_str_append_char(out, '"')) return 0;
    if (!pz_str_append(out, value, length)) return 0;
    return pz_str_append_char(out, '"');
#else
    if (!pz_str_append_char(out, '\'')) return 0;
    for (i = 0; i < length; ++i) {
        if (value[i] == '\'') {
            if (!pz_str_append_cstr(out, "'\\''")) return 0;
        } else if (!pz_str_append_char(out, value[i])) {
            return 0;
        }
    }
    return pz_str_append_char(out, '\'');
#endif
}

int pz_process_capture(const char* command, size_t length, pz_str* output,
                       const pz_log* log, int* exit_code) {
    capture_sink sink;
    int started;

    if (exit_code) *exit_code = -1;
    if (!output || (!command && length)) return 0;
    pz_str_clear(output);
    pz_str_init(&sink.pending);
    sink.output = output;
    sink.log = pz_log_enabled(log) ? log : NULL;
    sink.ok = 1;

    started = spawn_capture(command, length, capture_sink_cb, &sink, exit_code);
    capture_finish(&sink);
    pz_str_free(&sink.pending);
    return started && sink.ok;
}

int pz_process_start_detached(const pz_path* exe, const pz_str* args,
                              size_t arg_count, unsigned long* pid) {
    if (pid) *pid = 0;
    if (!exe || exe->text.length == 0) return 0;
    if (!args) arg_count = 0;
#ifdef _WIN32
    {
        wchar_t* wide_exe;
        wchar_t* wide_line;
        wchar_t* wide_dir = NULL;
        pz_path parent;
        pz_str command;
        STARTUPINFOW startup;
        PROCESS_INFORMATION process;
        size_t i;
        BOOL started;

        wide_exe = utf8_to_wide(exe->text.data, exe->text.length);
        if (!wide_exe) return 0;
        pz_str_init(&command);
        if (!append_quoted(&command, &exe->text)) {
            free(wide_exe);
            pz_str_free(&command);
            return 0;
        }
        for (i = 0; i < arg_count; ++i) {
            if (!pz_str_append_char(&command, ' ') ||
                !append_quoted(&command, &args[i])) {
                free(wide_exe);
                pz_str_free(&command);
                return 0;
            }
        }
        /* CreateProcessW receives a UTF-16 line it may modify in place, so the
         * assembled UTF-8 command is converted into its own buffer. */
        wide_line = utf8_to_wide(command.data, command.length);
        pz_str_free(&command);
        if (!wide_line) {
            free(wide_exe);
            return 0;
        }
        pz_path_init(&parent);
        if (pz_path_parent(exe, &parent) && parent.text.length)
            wide_dir = utf8_to_wide(parent.text.data, parent.text.length);
        pz_path_free(&parent);

        memset(&startup, 0, sizeof(startup));
        startup.cb = sizeof(startup);
        memset(&process, 0, sizeof(process));
        started = CreateProcessW(wide_exe, wide_line, NULL, NULL, FALSE,
                                 CREATE_NO_WINDOW, NULL, wide_dir, &startup,
                                 &process);
        free(wide_exe);
        free(wide_line);
        free(wide_dir);
        if (!started) return 0;
        if (pid) *pid = (unsigned long)process.dwProcessId;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 1;
    }
#else
    {
        posix_spawn_file_actions_t actions;
        posix_spawnattr_t attributes;
        char** argv = NULL;
        size_t i;
        pid_t child = 0;
        int ok = 1;

        if (posix_spawn_file_actions_init(&actions) != 0) return 0;
        if (posix_spawnattr_init(&attributes) != 0) {
            posix_spawn_file_actions_destroy(&actions);
            return 0;
        }
        for (i = 0; i < 3 && ok; ++i)
            if (posix_spawn_file_actions_addopen(&actions, (int)i, "/dev/null",
                                                 O_RDWR, 0) != 0)
                ok = 0;
#ifdef POSIX_SPAWN_SETSID
        /* Detach from the terminal's session so closing the UI does not take
         * the helper down with it. */
        if (ok) posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
#endif
        if (ok) {
            argv = (char**)calloc(arg_count + 2, sizeof(char*));
            if (!argv) ok = 0;
        }
        if (ok) {
            argv[0] = exe->text.data;
            for (i = 0; i < arg_count; ++i)
                argv[i + 1] = args[i].data ? args[i].data : (char*)"";
            argv[arg_count + 1] = NULL;
            if (posix_spawn(&child, exe->text.data, &actions, &attributes, argv,
                            environ) != 0)
                ok = 0;
        }
        free(argv);
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        if (!ok) return 0;
        if (pid) *pid = (unsigned long)child;
        return 1;
    }
#endif
}

/* ------------------------------------------------------------------- wait */

int pz_process_wait(unsigned long pid) {
    if (pid == 0) return 1;
#ifdef _WIN32
    {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
        if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
        WaitForSingleObject(process, INFINITE);
        CloseHandle(process);
        return 1;
    }
#else
    {
        struct timespec delay;
        int status;

        delay.tv_sec = 0;
        delay.tv_nsec = 200 * 1000 * 1000;
        for (;;) {
            const pid_t result = waitpid((pid_t)pid, &status, WNOHANG);
            if (result == (pid_t)pid) return 1;
            if (result < 0) {
                if (errno == EINTR) continue;
                if (errno != ECHILD) return 0;
                /* Not our child: poll with signal 0 instead. */
                for (;;) {
                    if (kill((pid_t)pid, 0) != 0 && errno != EPERM) return 1;
                    nanosleep(&delay, NULL);
                }
            }
            nanosleep(&delay, NULL);
        }
    }
#endif
}

int pz_process_is_running(unsigned long pid) {
    if (pid == 0) return 0;
#ifdef _WIN32
    {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
        DWORD result;
        if (!process) return 0;
        result = WaitForSingleObject(process, 0);
        CloseHandle(process);
        return result == WAIT_TIMEOUT;
    }
#else
    {
        int status;
        const pid_t result = waitpid((pid_t)pid, &status, WNOHANG);

        /* A child that exited but was never reaped is a zombie, not a running
         * process; reaping it here keeps callers from polling forever. */
        if (result == (pid_t)pid) return 0;
        if (result == 0) return 1;
        if (errno == EINTR) return 1;
        if (kill((pid_t)pid, 0) == 0) return 1;
        return errno == EPERM;
    }
#endif
}

unsigned long pz_process_current_id(void) {
#ifdef _WIN32
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}
