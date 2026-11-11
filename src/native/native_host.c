/* Firefox native messaging host: one framed JSON request per process.
 * Supported request: {"url":"<workshop item or collection url/id>"}
 *
 * The host is deliberately C99 and does not link the C++ core. It drives the
 * sibling pzmodmgr executable through the same CLI that users see, captures its
 * output, and returns the framed JSON reply the extension expects.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "core/url_c.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <wchar.h>
#else
#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#include <unistd.h>
#endif

#define MAX_MESSAGE (1024u * 1024u)
#define MAX_LOG_LINES 40
#define MAX_LOG_CHARS 500
#define MAX_STATUS_CHARS 2048

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} Buffer;

static void buffer_free(Buffer* b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static int buffer_reserve(Buffer* b, size_t extra) {
    size_t need = b->len + extra + 1;
    char* grown;
    if (need <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) cap *= 2;
    grown = (char*)realloc(b->data, cap);
    if (!grown) return 0;
    b->data = grown;
    b->cap = cap;
    return 1;
}

static int buffer_append(Buffer* b, const char* text, size_t n) {
    if (!buffer_reserve(b, n)) return 0;
    memcpy(b->data + b->len, text, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

static int buffer_char(Buffer* b, char c) { return buffer_append(b, &c, 1); }

static void buffer_append_u64(Buffer* b, uint64_t value) {
    char digits[32];
    size_t at = sizeof digits;
    do {
        digits[--at] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    buffer_append(b, digits + at, sizeof digits - at);
}

static void buffer_append_hex(Buffer* b, unsigned value, int digits) {
    static const char kHex[] = "0123456789abcdef";
    int shift;
    for (shift = (digits - 1) * 4; shift >= 0; shift -= 4)
        buffer_char(b, kHex[(value >> shift) & 0x0fu]);
}

typedef struct {
    int ok;
    int have_status;
    char message[MAX_STATUS_CHARS];
    char logs[MAX_LOG_LINES][MAX_LOG_CHARS + 1];
    int log_count;
} CommandResult;

static void result_add_log(CommandResult* result, const char* line, size_t n) {
    if (n > MAX_LOG_CHARS) n = MAX_LOG_CHARS;
    if (result->log_count == MAX_LOG_LINES) {
        memmove(result->logs[0], result->logs[1],
                (MAX_LOG_LINES - 1) * sizeof result->logs[0]);
        --result->log_count;
    }
    memcpy(result->logs[result->log_count], line, n);
    result->logs[result->log_count][n] = '\0';
    ++result->log_count;
}

static void result_parse(CommandResult* result, const char* output, size_t length,
                         int exit_code) {
    size_t start = 0;
    while (start < length) {
        size_t end = start;
        size_t n;
        while (end < length && output[end] != '\n') ++end;
        n = end - start;
        if (n != 0 && output[start + n - 1] == '\r') --n;

        if (n >= 4 && memcmp(output + start, "OK: ", 4) == 0) {
            size_t copy = n - 4;
            if (copy >= sizeof result->message) copy = sizeof result->message - 1;
            memcpy(result->message, output + start + 4, copy);
            result->message[copy] = '\0';
            result->ok = 1;
            result->have_status = 1;
        } else if (n >= 7 && memcmp(output + start, "ERROR: ", 7) == 0) {
            size_t copy = n - 7;
            if (copy >= sizeof result->message) copy = sizeof result->message - 1;
            memcpy(result->message, output + start + 7, copy);
            result->message[copy] = '\0';
            result->ok = 0;
            result->have_status = 1;
        } else if (n != 0) {
            result_add_log(result, output + start, n);
        }

        start = end < length ? end + 1 : end;
    }

    if (!result->have_status) {
        result->ok = exit_code == 0;
        strcpy(result->message, result->ok ? "Command completed"
                                           : "Command failed");
    }
}

#ifdef _WIN32

static int widen(const char* utf8, wchar_t* out, size_t cap) {
    int n;
    if (!utf8 || !cap) return 0;
    n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, (int)cap);
    return n > 0;
}

static int executable_dir_w(wchar_t* out, size_t cap) {
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)cap);
    wchar_t* slash;
    if (n == 0 || n >= cap) return 0;
    slash = wcsrchr(out, L'\\');
    if (!slash) slash = wcsrchr(out, L'/');
    if (!slash) return 0;
    *slash = L'\0';
    return 1;
}

static void append_quoted_w(wchar_t* out, size_t cap, size_t* at,
                            const wchar_t* arg) {
    /* Command-line quoting follows the CommandLineToArgvW rules. */
    size_t i;
    int need_quotes = arg[0] == L'\0' || wcspbrk(arg, L" \t\n\v\"") != NULL;
    if (*at + 2 >= cap) return;
    if (!need_quotes) {
        for (i = 0; arg[i] && *at + 1 < cap; ++i) out[(*at)++] = arg[i];
        out[*at] = L'\0';
        return;
    }
    out[(*at)++] = L'"';
    for (i = 0; arg[i] && *at + 2 < cap; ++i) {
        size_t slashes = 0;
        while (arg[i] == L'\\') {
            ++i;
            ++slashes;
        }
        if (arg[i] == L'\0') {
            while (slashes-- && *at + 2 < cap) out[(*at)++] = L'\\';
            break;
        }
        if (arg[i] == L'"') {
            while (slashes-- && *at + 3 < cap) out[(*at)++] = L'\\';
            if (*at + 2 < cap) {
                out[(*at)++] = L'\\';
                out[(*at)++] = L'"';
            }
        } else {
            while (slashes-- && *at + 1 < cap) out[(*at)++] = L'\\';
            if (*at + 1 < cap) out[(*at)++] = arg[i];
        }
    }
    if (*at + 1 < cap) out[(*at)++] = L'"';
    out[*at] = L'\0';
}

static int run_cli_w(const wchar_t* exe, const wchar_t* verb,
                     const wchar_t* arg, CommandResult* result) {
    wchar_t command[65536];
    wchar_t dir[32768];
    size_t at = 0;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE read_pipe = NULL;
    HANDLE write_pipe = NULL;
    HANDLE nul = INVALID_HANDLE_VALUE;
    DWORD code = 1;
    char chunk[4096];
    Buffer output = {0};

    if (!arg) arg = L"";

    command[0] = L'\0';
    append_quoted_w(command, sizeof command / sizeof command[0], &at, exe);
    if (at + 1 < sizeof command / sizeof command[0]) command[at++] = L' ';
    append_quoted_w(command, sizeof command / sizeof command[0], &at, verb);
    if (at + 1 < sizeof command / sizeof command[0]) command[at++] = L' ';
    append_quoted_w(command, sizeof command / sizeof command[0], &at, arg);

    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return 0;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    nul = CreateFileW(L"NUL", GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      OPEN_EXISTING, 0, NULL);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;

    memset(&pi, 0, sizeof pi);
    if (!executable_dir_w(dir, sizeof dir / sizeof dir[0]) ||
        !CreateProcessW(exe, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL,
                        dir, &si, &pi)) {
        CloseHandle(write_pipe);
        CloseHandle(read_pipe);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        return 0;
    }

    CloseHandle(write_pipe);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);

    for (;;) {
        DWORD got = 0;
        if (!ReadFile(read_pipe, chunk, sizeof chunk, &got, NULL) || got == 0)
            break;
        if (!buffer_append(&output, chunk, got)) break;
    }
    CloseHandle(read_pipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    result_parse(result, output.data ? output.data : "", output.len, (int)code);
    buffer_free(&output);
    return 1;
}

static int join_exe_path(wchar_t* out, size_t cap, const wchar_t* dir) {
    static const wchar_t kName[] = L"\\pzmodmgr.exe";
    const size_t dir_len = wcslen(dir);
    const size_t name_len = sizeof kName / sizeof kName[0] - 1;
    if (dir_len + name_len + 1 > cap) return 0;
    memcpy(out, dir, dir_len * sizeof *out);
    memcpy(out + dir_len, kName, (name_len + 1) * sizeof *out);
    return 1;
}

static int run_cli_arg(const char* verb, const char* arg, CommandResult* result) {
    wchar_t exe_dir[32768];
    wchar_t exe[32768];
    wchar_t verb_w[32];
    wchar_t arg_w[8192];
    if (!executable_dir_w(exe_dir, sizeof exe_dir / sizeof exe_dir[0]) ||
        !widen(verb, verb_w, sizeof verb_w / sizeof verb_w[0]) ||
        !widen(arg, arg_w, sizeof arg_w / sizeof arg_w[0]) ||
        !join_exe_path(exe, sizeof exe / sizeof exe[0], exe_dir))
        return 0;
    return run_cli_w(exe, verb_w, arg_w, result);
}

#else

static int executable_path(char* out, size_t cap) {
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    char* slash;
    static const char kName[] = "pzmodmgr";
    if (n <= 0 || (size_t)n >= cap - 1) return 0;
    out[n] = '\0';
    slash = strrchr(out, '/');
    if (!slash) return 0;
    if ((size_t)(slash - out) + 1 + sizeof kName > cap) return 0;
    memcpy(slash + 1, kName, sizeof kName);
    return 1;
}

static void shell_quote(Buffer* command, const char* value) {
    const char* p;
    buffer_char(command, '\'');
    for (p = value; *p; ++p) {
        if (*p == '\'')
            buffer_append(command, "'\\''", 4);
        else
            buffer_char(command, *p);
    }
    buffer_char(command, '\'');
}

static int run_cli_arg(const char* verb, const char* arg, CommandResult* result) {
    char exe[PATH_MAX];
    Buffer command = {0};
    FILE* child;
    char chunk[4096];
    size_t got;
    int rc;
    Buffer output = {0};

    if (!executable_path(exe, sizeof exe)) return 0;
    shell_quote(&command, exe);
    if (!buffer_char(&command, ' ') ||
        !buffer_append(&command, verb, strlen(verb)) ||
        !buffer_char(&command, ' ')) {
        buffer_free(&command);
        return 0;
    }
    shell_quote(&command, arg);
    buffer_append(&command, " 2>&1", 5);

    child = popen(command.data, "r");
    buffer_free(&command);
    if (!child) return 0;
    while ((got = fread(chunk, 1, sizeof chunk, child)) > 0) {
        if (!buffer_append(&output, chunk, got)) break;
    }
    rc = pclose(child);
    result_parse(result, output.data ? output.data : "", output.len, rc);
    buffer_free(&output);
    return 1;
}

#endif

static int read_exact(void* dst, size_t n) {
    return fread(dst, 1, n, stdin) == n;
}

static int read_message(char** message, uint32_t* length) {
    uint32_t len = 0;
    char* text;
    if (!read_exact(&len, sizeof len) || len == 0 || len > MAX_MESSAGE)
        return 0;
    text = (char*)malloc((size_t)len + 1);
    if (!text) return 0;
    if (!read_exact(text, len)) {
        free(text);
        return 0;
    }
    text[len] = '\0';
    *message = text;
    *length = len;
    return 1;
}

static int json_url(const char* json, size_t length, char* out, size_t cap) {
    static const char kKey[] = "\"url\"";
    size_t i;
    size_t at = 0;
    for (i = 0; i + sizeof kKey - 1 <= length; ++i) {
        if (memcmp(json + i, kKey, sizeof kKey - 1) == 0) break;
    }
    if (i + sizeof kKey - 1 > length) return 0;
    i += sizeof kKey - 1;
    while (i < length && isspace((unsigned char)json[i])) ++i;
    if (i >= length || json[i] != ':') return 0;
    ++i;
    while (i < length && isspace((unsigned char)json[i])) ++i;
    if (i >= length || json[i] != '"') return 0;
    ++i;

    while (i < length && json[i] != '"') {
        char c = json[i++];
        if (c == '\\' && i < length) {
            const char escaped = json[i++];
            switch (escaped) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u':
                if (i + 4 <= length) i += 4;
                c = '?';
                break;
            default: c = escaped; break;
            }
        }
        if (at + 1 < cap) out[at++] = c;
    }
    out[at] = '\0';
    return at != 0;
}

static void json_escape(Buffer* out, const char* text) {
    const unsigned char* p;
    for (p = (const unsigned char*)text; *p; ++p) {
        switch (*p) {
        case '"': buffer_append(out, "\\\"", 2); break;
        case '\\': buffer_append(out, "\\\\", 2); break;
        case '\n': buffer_append(out, "\\n", 2); break;
        case '\r': buffer_append(out, "\\r", 2); break;
        case '\t': buffer_append(out, "\\t", 2); break;
        default:
            if (*p < 0x20) {
                buffer_append(out, "\\u00", 4);
                buffer_append_hex(out, *p, 2);
            } else {
                buffer_char(out, (char)*p);
            }
            break;
        }
    }
}

static int write_message(const Buffer* message) {
    uint32_t len = (uint32_t)message->len;
    return fwrite(&len, 1, sizeof len, stdout) == sizeof len &&
           fwrite(message->data, 1, message->len, stdout) == message->len &&
           fflush(stdout) == 0;
}

int main(void) {
    char* request = NULL;
    uint32_t request_length = 0;
    char url[8192];
    CommandResult result;
    uint64_t workshop_id = 0;
    Buffer response = {0};
    int wrote;

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    memset(&result, 0, sizeof result);
    if (!read_message(&request, &request_length)) return 1;

    if (!json_url(request, request_length, url, sizeof url)) {
        strcpy(result.message, "Missing Workshop URL or ID.");
    } else if (!run_cli_arg("add", url, &result)) {
        strcpy(result.message, "Could not start pzmodmgr.");
    } else if (!result.ok &&
               strncmp(result.message, "Already installed", 17) == 0 &&
               pz_url_parse_workshop_id(url, strlen(url), &workshop_id)) {
        char id[32];
        size_t at = sizeof id;
        uint64_t value = workshop_id;
        id[--at] = '\0';
        do {
            id[--at] = (char)('0' + value % 10);
            value /= 10;
        } while (value != 0);
        result.log_count = 0;
        if (!run_cli_arg("update", id + at, &result))
            strcpy(result.message, "Could not start pzmodmgr.");
    }
    free(request);

    buffer_append(&response, "{\"ok\":", 6);
    buffer_append(&response, result.ok ? "true" : "false", result.ok ? 4 : 5);
    buffer_append(&response, ",\"message\":\"", 12);
    json_escape(&response, result.message);
    buffer_char(&response, '"');
    if (result.log_count != 0) {
        int i;
        buffer_append(&response, ",\"log\":[", 8);
        for (i = 0; i < result.log_count; ++i) {
            if (i != 0) buffer_char(&response, ',');
            buffer_char(&response, '"');
            json_escape(&response, result.logs[i]);
            buffer_char(&response, '"');
        }
        buffer_char(&response, ']');
    }
    buffer_char(&response, '}');

    wrote = write_message(&response);
    buffer_free(&response);
    return wrote ? 0 : 1;
}
