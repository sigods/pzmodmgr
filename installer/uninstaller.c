// SPDX-License-Identifier: GPL-3.0-only
//
// Small standalone uninstaller. It deliberately does not contain the setup
// payload, so it stays small in the installed program folder.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>

#include <string.h>
#include <wchar.h>

#define PZ_PATH_MAX 32768

static const wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PZModManager";
static const wchar_t* const kHostKeys[] = {
    L"Software\\Mozilla\\NativeMessagingHosts\\pzmodmgr",
    L"Software\\Google\\Chrome\\NativeMessagingHosts\\pzmodmgr",
    L"Software\\Chromium\\NativeMessagingHosts\\pzmodmgr",
    L"Software\\BraveSoftware\\Brave-Browser\\NativeMessagingHosts\\pzmodmgr",
    L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\pzmodmgr",
};

static void wcopy(wchar_t* dst, size_t cap, const wchar_t* src) {
    size_t n;
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    n = wcslen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n * sizeof(wchar_t));
    dst[n] = 0;
}

static void append(wchar_t* dst, size_t cap, const wchar_t* base,
                   const wchar_t* leaf) {
    size_t used;
    wcopy(dst, cap, base);
    used = wcslen(dst);
    if (used && dst[used - 1] != L'\\') {
        if (used + 1 < cap) {
            dst[used++] = L'\\';
            dst[used] = 0;
        }
    }
    used = wcslen(dst);
    if (used < cap - 1) wcopy(dst + used, cap - used, leaf);
}

static int file_exists(const wchar_t* path) {
    const DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES &&
           !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const wchar_t* path) {
    const DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static int remove_tree(const wchar_t* dir) {
    wchar_t pattern[PZ_PATH_MAX];
    WIN32_FIND_DATAW data;
    HANDLE found;

    if (!dir || !*dir || PathIsRootW(dir)) return 0;
    append(pattern, sizeof(pattern) / sizeof(pattern[0]), dir, L"*");
    memset(&data, 0, sizeof data);
    found = FindFirstFileW(pattern, &data);
    if (found != INVALID_HANDLE_VALUE) {
        do {
            wchar_t child[PZ_PATH_MAX];
            if (wcscmp(data.cFileName, L".") == 0 ||
                wcscmp(data.cFileName, L"..") == 0)
                continue;
            append(child, sizeof(child) / sizeof(child[0]), dir, data.cFileName);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    RemoveDirectoryW(child);
                else
                    remove_tree(child);
            } else {
                SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child);
            }
        } while (FindNextFileW(found, &data));
        FindClose(found);
    }
    return RemoveDirectoryW(dir) != 0;
}

static void delete_shortcuts(void) {
    wchar_t path[PZ_PATH_MAX];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, path))) {
        wchar_t menu[PZ_PATH_MAX];
        wchar_t link[PZ_PATH_MAX];
        append(menu, sizeof(menu) / sizeof(menu[0]), path, L"PZ Mod Manager");
        append(link, sizeof(link) / sizeof(link[0]), menu,
               L"PZ Mod Manager.lnk");
        DeleteFileW(link);
        append(link, sizeof(link) / sizeof(link[0]), menu,
               L"Uninstall PZ Mod Manager.lnk");
        DeleteFileW(link);
        RemoveDirectoryW(menu);
    }
    if (SUCCEEDED(
            SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, path))) {
        wchar_t link[PZ_PATH_MAX];
        append(link, sizeof(link) / sizeof(link[0]), path,
               L"PZ Mod Manager.lnk");
        DeleteFileW(link);
    }
}

static int safe_dir(const wchar_t* dir) {
    wchar_t exe[PZ_PATH_MAX];
    wchar_t uninstaller[PZ_PATH_MAX];
    if (!dir || !*dir || PathIsRootW(dir)) return 0;
    if (wcsstr(dir, L"\\Zomboid") || wcsstr(dir, L"\\mods")) return 0;
    append(exe, sizeof(exe) / sizeof(exe[0]), dir, L"pzmodmgr.exe");
    append(uninstaller, sizeof(uninstaller) / sizeof(uninstaller[0]), dir,
           L"uninstall.exe");
    return file_exists(exe) && file_exists(uninstaller);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line,
                    int show) {
    int argc = 0;
    LPWSTR* argv;
    int silent = 0;
    wchar_t self[PZ_PATH_MAX];
    wchar_t dir[PZ_PATH_MAX];
    wchar_t* slash;
    DWORD self_len;
    DWORD temp_len;
    wchar_t temp_path[MAX_PATH];
    wchar_t temp[MAX_PATH];
    int moved = 0;
    int removed;
    int i;

    (void)instance;
    (void)previous;
    (void)command_line;
    (void)show;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"/S") == 0 ||
            _wcsicmp(argv[i], L"--silent") == 0)
            silent = 1;
    }
    if (argv) LocalFree(argv);

    self[0] = 0;
    self_len = GetModuleFileNameW(NULL, self, PZ_PATH_MAX);
    if (self_len == 0 || self_len >= PZ_PATH_MAX) return 1;

    wcopy(dir, sizeof(dir) / sizeof(dir[0]), self);
    slash = wcsrchr(dir, L'\\');
    if (!slash) return 1;
    *slash = 0;

    if (!safe_dir(dir)) {
        if (!silent)
            MessageBoxW(NULL,
                        L"The uninstaller is not running from a valid PZ Mod Manager folder.",
                        L"Uninstall PZ Mod Manager", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (FindWindowW(L"PZModMgrWnd", NULL)) {
        if (!silent)
            MessageBoxW(NULL, L"Close PZ Mod Manager before uninstalling it.",
                        L"Uninstall PZ Mod Manager", MB_OK | MB_ICONWARNING);
        return 1;
    }

    if (!silent) {
        if (MessageBoxW(NULL,
                        L"Remove PZ Mod Manager from this user account?\r\n\r\n"
                        L"Your mods and settings in %USERPROFILE%\\Zomboid are kept.",
                        L"Uninstall PZ Mod Manager",
                        MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
    }

    delete_shortcuts();
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    for (i = 0; i < (int)(sizeof(kHostKeys) / sizeof(kHostKeys[0])); ++i)
        RegDeleteTreeW(HKEY_CURRENT_USER, kHostKeys[i]);

    // Rename the running executable out of the program folder, then delete the
    // folder. The temporary copy is removed after the next reboot.
    temp_path[0] = 0;
    temp[0] = 0;
    temp_len = GetTempPathW(MAX_PATH, temp_path);
    if (temp_len > 0 && temp_len < MAX_PATH &&
        GetTempFileNameW(temp_path, L"pzm", 0, temp) != 0) {
        moved = MoveFileExW(self, temp, MOVEFILE_REPLACE_EXISTING) != 0;
    }

    {
        wchar_t from[PZ_PATH_MAX + 2];
        SHFILEOPSTRUCTW op;
        size_t len;
        memset(from, 0, sizeof from);
        wcopy(from, PZ_PATH_MAX, dir);
        len = wcslen(from);
        from[len] = 0;
        from[len + 1] = 0;
        memset(&op, 0, sizeof op);
        op.wFunc = FO_DELETE;
        op.pFrom = from;
        op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_SILENT;
        removed = SHFileOperationW(&op) == 0 && !dir_exists(dir);
    }
    if (!removed) removed = remove_tree(dir);
    if (moved)
        MoveFileExW(temp, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    else
        MoveFileExW(self, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);

    if (!silent) {
        MessageBoxW(NULL,
                    removed ? L"PZ Mod Manager was uninstalled."
                            : L"PZ Mod Manager was partially removed. Some files may remain.",
                    L"Uninstall PZ Mod Manager",
                    MB_OK | (removed ? MB_ICONINFORMATION : MB_ICONWARNING));
    }
    return removed ? 0 : 1;
}
