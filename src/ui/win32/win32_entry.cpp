// Windows entry point.
//
// pzmodmgr is linked as a GUI-subsystem program (MinGW: -mwindows -municode,
// MSVC: /SUBSYSTEM:WINDOWS) so that starting it from Explorer or a shortcut
// opens the manager window alone. A console-subsystem binary gets a console
// allocated for it, which showed up as a second, empty black window.
//
// A GUI-subsystem process starts with no console attached, so CLI mode has to
// borrow the caller's console (or create one) before anything is printed, and
// the command line arrives as UTF-16 and must be converted for run_app().
#define _CRT_SECURE_NO_WARNINGS
#include "ui/ui.hpp"
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {

std::string to_u8(const wchar_t* w) {
    const int len = w ? lstrlenW(w) : 0;
    if (len <= 0) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr);
    return s;
}

// Reconnect stdout/stderr/stdin to the console for CLI mode.
//
// When the caller redirected output (cmd, PowerShell, a pipe) the inherited
// handle already works, so leave it alone: no console window, no lost
// redirection. Otherwise attach to the console of the shell that launched us
// and only allocate a fresh one when there is none.
void attach_console() {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const DWORD type = (out && out != INVALID_HANDLE_VALUE) ? GetFileType(out)
                                                            : FILE_TYPE_UNKNOWN;
    if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) return;

    if (!AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole()) return;

    FILE* f = nullptr;
    f = freopen("CONOUT$", "w", stdout);
    (void)f;
    f = freopen("CONOUT$", "w", stderr);
    (void)f;
    f = freopen("CONIN$", "r", stdin);
    (void)f;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> storage;
    if (wargv) {
        storage.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) storage.push_back(to_u8(wargv[i]));
        LocalFree(wargv);
    }
    if (storage.empty()) {
        storage.push_back("pzmodmgr.exe");
        argc = 1;
    }

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& arg : storage) argv.push_back(arg.data());
    argv.push_back(nullptr);

    // Without arguments this is the double-click / shortcut case: stay
    // windowless. The detached update helper must also stay windowless.
    const bool update_helper =
        argc > 1 && std::string(argv[1]) == "--apply-update";
    if (argc > 1 && !update_helper) attach_console();

    return pz::run_app(argc, argv.data());
}
