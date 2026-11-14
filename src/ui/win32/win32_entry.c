/* Windows entry point.
 *
 * pzmodmgr is linked as a GUI-subsystem program (MinGW: -mwindows -municode,
 * MSVC: /SUBSYSTEM:WINDOWS) so that starting it from Explorer or a shortcut
 * opens the manager window alone. A console-subsystem binary gets a console
 * allocated for it, which showed up as a second, empty black window.
 *
 * A GUI-subsystem process starts with no console attached, so CLI mode has to
 * borrow the caller's console (or create one) before anything is printed, and
 * the command line arrives as UTF-16 and must be converted for pz_app_run().
 */
#define _CRT_SECURE_NO_WARNINGS
#include "app/app_c.h"

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* to_u8(const wchar_t* wide) {
    char* text;
    int length;
    int written;

    if (!wide || wide[0] == L'\0') {
        text = (char*)malloc(1);
        if (text) text[0] = '\0';
        return text;
    }
    length = lstrlenW(wide);
    written = WideCharToMultiByte(CP_UTF8, 0, wide, length, NULL, 0, NULL, NULL);
    if (written <= 0) return NULL;
    text = (char*)malloc((size_t)written + 1);
    if (!text) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, 0, wide, length, text, written, NULL,
                             NULL)) {
        free(text);
        return NULL;
    }
    text[written] = '\0';
    return text;
}

/* Reconnect stdout/stderr/stdin to the console for CLI mode.
 *
 * When the caller redirected output (cmd, PowerShell, a pipe) the inherited
 * handle already works, so leave it alone: no console window, no lost
 * redirection. Otherwise attach to the console of the shell that launched us
 * and only allocate a fresh one when there is none. */
static void attach_console(void) {
    HANDLE out;
    DWORD type;
    FILE* file;

    out = GetStdHandle(STD_OUTPUT_HANDLE);
    type = (out && out != INVALID_HANDLE_VALUE) ? GetFileType(out)
                                                : FILE_TYPE_UNKNOWN;
    if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole()) return;

    file = freopen("CONOUT$", "w", stdout);
    (void)file;
    file = freopen("CONOUT$", "w", stderr);
    (void)file;
    file = freopen("CONIN$", "r", stdin);
    (void)file;
}

static void free_argv(int argc, char** argv) {
    int i;
    if (!argv) return;
    for (i = 0; i < argc; ++i) free(argv[i]);
    free(argv);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line,
                    int show) {
    int wide_count = 0;
    LPWSTR* wide_argv;
    char** argv;
    int argc;
    int i;
    int code;

    (void)instance;
    (void)previous;
    (void)command_line;
    (void)show;

    wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_count);
    if (!wide_argv || wide_count <= 0) {
        if (wide_argv) LocalFree(wide_argv);
        wide_argv = NULL;
        wide_count = 1;
    }
    argv = (char**)calloc((size_t)wide_count + 1, sizeof(char*));
    if (!argv) {
        if (wide_argv) LocalFree(wide_argv);
        return 1;
    }
    argc = 0;
    for (i = 0; i < wide_count; ++i) {
        const wchar_t* source =
            wide_argv && i < wide_count ? wide_argv[i] : L"pzmodmgr.exe";
        char* converted = to_u8(source);
        if (!converted) {
            free_argv(argc, argv);
            if (wide_argv) LocalFree(wide_argv);
            return 1;
        }
        argv[argc++] = converted;
    }
    if (argc == 0) {
        argv[argc] = to_u8(L"pzmodmgr.exe");
        if (!argv[argc]) {
            free_argv(argc, argv);
            if (wide_argv) LocalFree(wide_argv);
            return 1;
        }
        argc = 1;
    }
    argv[argc] = NULL;
    if (wide_argv) LocalFree(wide_argv);

    /* Without arguments this is the double-click / shortcut case: stay
     * windowless. The detached update helper must also stay windowless. */
    if (argc > 1 && strcmp(argv[1], "--apply-update") != 0) attach_console();

    code = pz_app_run(argc, argv);
    free_argv(argc, argv);
    return code;
}
