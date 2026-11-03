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
#include <cwchar>

namespace {

const wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PZModManager";
const wchar_t* const kHostKeys[] = {
    L"Software\\Mozilla\\NativeMessagingHosts\\pzmodmgr",
    L"Software\\Google\\Chrome\\NativeMessagingHosts\\pzmodmgr",
    L"Software\\Chromium\\NativeMessagingHosts\\pzmodmgr",
    L"Software\\BraveSoftware\\Brave-Browser\\NativeMessagingHosts\\pzmodmgr",
    L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\pzmodmgr",
};

bool file_exists(const wchar_t* path) {
    const DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const wchar_t* path) {
    const DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

void append(wchar_t* dst, const wchar_t* base, const wchar_t* leaf) {
    wcscpy(dst, base);
    if (dst[0] && dst[wcslen(dst) - 1] != L'\\') wcscat(dst, L"\\");
    wcscat(dst, leaf);
}

bool remove_tree(const wchar_t* dir) {
    if (!dir || !*dir || PathIsRootW(dir)) return false;

    wchar_t pattern[32768]{};
    swprintf(pattern, 32768, L"%s\\*", dir);
    WIN32_FIND_DATAW data{};
    HANDLE found = FindFirstFileW(pattern, &data);
    if (found != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
                continue;
            wchar_t child[32768]{};
            swprintf(child, 32768, L"%s\\%s", dir, data.cFileName);
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

void delete_shortcuts() {
    wchar_t path[32768]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, path))) {
        wchar_t menu[32768]{};
        append(menu, path, L"PZ Mod Manager");
        wchar_t link[32768]{};
        append(link, menu, L"PZ Mod Manager.lnk");
        DeleteFileW(link);
        append(link, menu, L"Uninstall PZ Mod Manager.lnk");
        DeleteFileW(link);
        RemoveDirectoryW(menu);
    }
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, path))) {
        wchar_t link[32768]{};
        append(link, path, L"PZ Mod Manager.lnk");
        DeleteFileW(link);
    }
}

bool safe_dir(const wchar_t* dir) {
    if (!dir || !*dir || PathIsRootW(dir)) return false;
    if (wcsstr(dir, L"\\Zomboid") || wcsstr(dir, L"\\mods")) return false;
    wchar_t exe[32768]{};
    append(exe, dir, L"pzmodmgr.exe");
    wchar_t uninstaller[32768]{};
    append(uninstaller, dir, L"uninstall.exe");
    return file_exists(exe) && file_exists(uninstaller);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool silent = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"/S") == 0 || _wcsicmp(argv[i], L"--silent") == 0)
            silent = true;
    }
    if (argv) LocalFree(argv);

    wchar_t self[32768]{};
    const DWORD self_len = GetModuleFileNameW(nullptr, self, 32768);
    if (self_len == 0 || self_len >= 32768) return 1;

    wchar_t dir[32768]{};
    wcscpy(dir, self);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (!slash) return 1;
    *slash = 0;

    if (!safe_dir(dir)) {
        if (!silent)
            MessageBoxW(nullptr,
                        L"The uninstaller is not running from a valid PZ Mod Manager folder.",
                        L"Uninstall PZ Mod Manager", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (FindWindowW(L"PZModMgrWnd", nullptr)) {
        if (!silent)
            MessageBoxW(nullptr, L"Close PZ Mod Manager before uninstalling it.",
                        L"Uninstall PZ Mod Manager", MB_OK | MB_ICONWARNING);
        return 1;
    }

    if (!silent) {
        if (MessageBoxW(nullptr,
                        L"Remove PZ Mod Manager from this user account?\r\n\r\n"
                        L"Your mods and settings in %USERPROFILE%\\Zomboid are kept.",
                        L"Uninstall PZ Mod Manager",
                        MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
    }

    delete_shortcuts();
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    for (const auto* key : kHostKeys) RegDeleteTreeW(HKEY_CURRENT_USER, key);

    // Rename the running executable out of the program folder, then delete the
    // folder. The temporary copy is removed after the next reboot.
    wchar_t temp_path[MAX_PATH]{};
    wchar_t temp[MAX_PATH]{};
    bool moved = false;
    if (GetTempPathW(MAX_PATH, temp_path) > 0 &&
        GetTempFileNameW(temp_path, L"pzm", 0, temp) != 0) {
        moved = MoveFileExW(self, temp, MOVEFILE_REPLACE_EXISTING) != 0;
    }

    wchar_t from[32768]{};
    wcscpy(from, dir);
    const size_t from_len = wcslen(from);
    from[from_len + 1] = 0;
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_SILENT;
    bool removed = SHFileOperationW(&op) == 0 && !dir_exists(dir);
    if (!removed) removed = remove_tree(dir);
    if (moved) MoveFileExW(temp, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    else MoveFileExW(self, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);

    if (!silent) {
        MessageBoxW(nullptr,
                    removed ? L"PZ Mod Manager was uninstalled."
                            : L"PZ Mod Manager was partially removed. Some files may remain.",
                    L"Uninstall PZ Mod Manager",
                    MB_OK | (removed ? MB_ICONINFORMATION : MB_ICONWARNING));
    }
    return removed ? 0 : 1;
}
