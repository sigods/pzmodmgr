// SPDX-License-Identifier: GPL-3.0-only
//
// PZ Mod Manager setup / uninstaller.
//
// The payload (application, native messaging host, docs and Firefox add-on)
// is embedded as RCDATA by the packaging script. This executable installs it
// for the current user, so no administrator rights or UAC prompt are needed.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#ifndef PZ_VERSION
#define PZ_VERSION L"0.2.0"
#endif

namespace {

constexpr wchar_t kAppName[] = L"PZ Mod Manager";
constexpr wchar_t kVersion[] = L"0.2.0";
constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PZModManager";
constexpr wchar_t kFirefoxHostKey[] = L"Software\\Mozilla\\NativeMessagingHosts\\pzmodmgr";

constexpr int IDI_SETUP = 101;
constexpr int IDC_INSTALL = 1000;
constexpr int IDC_BROWSE = 1001;
constexpr int IDC_PATH = 1002;
constexpr int IDC_STARTMENU = 1003;
constexpr int IDC_DESKTOP = 1004;
constexpr int IDC_FIREFOX = 1005;
constexpr int IDC_CHROME = 1006;
constexpr int IDC_LAUNCH = 1007;

constexpr int RES_APP = 2001;
constexpr int RES_HOST = 2002;
constexpr int RES_README = 2003;
constexpr int RES_LICENSE = 2004;
constexpr int RES_FIREFOX_MANIFEST = 2005;
constexpr int RES_FIREFOX_JS = 2006;
constexpr int RES_CHROME_MANIFEST = 2009;
constexpr int RES_CHROME_JS = 2010;
constexpr int RES_CHROME_ICON16 = 2012;
constexpr int RES_CHROME_ICON32 = 2013;
constexpr int RES_CHROME_ICON48 = 2014;
constexpr int RES_CHROME_ICON128 = 2015;
constexpr int RES_UNINSTALLER = 2016;

enum PayloadNeed { kAlways, kFirefox, kChromium, kAnyBrowser };

struct PayloadFile {
    int id;
    const wchar_t* relative;
    PayloadNeed need;
};

const PayloadFile kPayload[] = {
    {RES_APP, L"pzmodmgr.exe", kAlways},
    {RES_UNINSTALLER, L"uninstall.exe", kAlways},
    {RES_README, L"README.txt", kAlways},
    {RES_LICENSE, L"LICENSE.txt", kAlways},
    {RES_HOST, L"pzmodmgr_native_host.exe", kAnyBrowser},
    {RES_FIREFOX_MANIFEST, L"firefox\\manifest.json", kFirefox},
    {RES_FIREFOX_JS, L"firefox\\background.js", kFirefox},
    {RES_CHROME_MANIFEST, L"chrome-extension\\manifest.json", kChromium},
    {RES_CHROME_JS, L"chrome-extension\\background.js", kChromium},
    {RES_CHROME_ICON16, L"chrome-extension\\icons\\icon16.png", kChromium},
    {RES_CHROME_ICON32, L"chrome-extension\\icons\\icon32.png", kChromium},
    {RES_CHROME_ICON48, L"chrome-extension\\icons\\icon48.png", kChromium},
    {RES_CHROME_ICON128, L"chrome-extension\\icons\\icon128.png", kChromium},
};

constexpr int kPayloadCount = static_cast<int>(sizeof(kPayload) / sizeof(kPayload[0]));

HINSTANCE g_instance = nullptr;
HWND g_hwnd = nullptr;
HWND g_path = nullptr;
HWND g_browse = nullptr;
HWND g_install = nullptr;
HWND g_cancel = nullptr;
HWND g_startmenu = nullptr;
HWND g_desktop = nullptr;
HWND g_firefox = nullptr;
HWND g_chrome = nullptr;
HWND g_launch = nullptr;
HWND g_progress = nullptr;
HWND g_status = nullptr;

HFONT g_title_font = nullptr;
HFONT g_sub_font = nullptr;
HFONT g_heading_font = nullptr;
HFONT g_body_font = nullptr;
HFONT g_small_font = nullptr;
HBRUSH g_white_brush = nullptr;
int g_dpi = 96;

bool g_installing = false;
bool g_installed = false;
bool g_launch_after = false;
bool g_firefox_installed = false;
bool g_chromium_installed = false;
std::wstring g_chromium_label = L"Chrome";
std::wstring g_browse_start;

struct ChromiumBrowser {
    const wchar_t* name;
    const wchar_t* exe;
    const wchar_t* app_path;
    const wchar_t* host_key;
    bool installed;
};

ChromiumBrowser g_chromium_browsers[] = {
    {L"Chrome", L"Google\\Chrome\\Application\\chrome.exe", L"chrome.exe",
     L"Software\\Google\\Chrome\\NativeMessagingHosts\\pzmodmgr", false},
    {L"Chromium", L"Chromium\\Application\\chrome.exe", L"chromium.exe",
     L"Software\\Chromium\\NativeMessagingHosts\\pzmodmgr", false},
    {L"Brave", L"BraveSoftware\\Brave-Browser\\Application\\brave.exe", L"brave.exe",
     L"Software\\BraveSoftware\\Brave-Browser\\NativeMessagingHosts\\pzmodmgr", false},
    {L"Edge", L"Microsoft\\Edge\\Application\\msedge.exe", L"msedge.exe",
     L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\pzmodmgr", false},
};

int S(int value) {
    return MulDiv(value, g_dpi, 96);
}

std::wstring trim(const std::wstring& in) {
    const auto first = in.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const auto last = in.find_last_not_of(L" \t\r\n");
    return in.substr(first, last - first + 1);
}

std::wstring dir_of(const std::wstring& path) {
    const auto pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    return path.substr(0, pos);
}

std::wstring file_of(const std::wstring& path) {
    const auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

bool file_exists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring module_path(HMODULE module = nullptr) {
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD n = GetModuleFileNameW(module, buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
        if (n == 0) return {};
        if (n < buffer.size() - 1) return std::wstring(buffer.data(), n);
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring full_path(const std::wstring& path) {
    wchar_t buffer[32768]{};
    const DWORD n = GetFullPathNameW(path.c_str(), 32768, buffer, nullptr);
    if (n == 0 || n >= 32768) return path;
    std::wstring out(buffer, n);
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return out;
}

std::wstring known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) || !raw) return {};
    std::wstring path(raw);
    CoTaskMemFree(raw);
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
}

std::wstring win_error(DWORD error = GetLastError()) {
    LPWSTR raw = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
    if (n == 0 || !raw) return L"Windows error " + std::to_wstring(error);
    std::wstring message(raw, n);
    LocalFree(raw);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' ||
                                message.back() == L' '))
        message.pop_back();
    return message;
}

bool iequals_prefix(const std::wstring& text, const std::wstring& prefix) {
    return text.size() >= prefix.size() &&
           _wcsnicmp(text.c_str(), prefix.c_str(), prefix.size()) == 0;
}

bool path_is_under(const std::wstring& child, const std::wstring& parent) {
    if (_wcsicmp(child.c_str(), parent.c_str()) == 0) return true;
    if (!iequals_prefix(child, parent) || child.size() <= parent.size()) return false;
    return child[parent.size()] == L'\\' || child[parent.size()] == L'/';
}

bool ensure_dir(const std::wstring& dir, std::wstring& error) {
    const int rc = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS || rc == ERROR_FILE_EXISTS)
        return true;
    SetLastError(static_cast<DWORD>(rc));
    error = L"Could not create " + dir + L": " + win_error();
    return false;
}

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}

std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (unsigned char c : in) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[7]{};
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

bool write_file_bytes(const std::wstring& path, const void* data, DWORD size,
                      std::wstring& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"Could not create " + path + L": " + win_error();
        return false;
    }
    const auto* bytes = static_cast<const unsigned char*>(data);
    DWORD written = 0;
    while (written < size) {
        DWORD chunk = 0;
        if (!WriteFile(file, bytes + written, size - written, &chunk, nullptr) || chunk == 0) {
            error = L"Could not write " + path + L": " + win_error();
            CloseHandle(file);
            DeleteFileW(path.c_str());
            return false;
        }
        written += chunk;
    }
    CloseHandle(file);
    return true;
}

bool write_utf8_file(const std::wstring& path, const std::string& text, std::wstring& error) {
    const std::string with_bom = text;  // Firefox accepts UTF-8 without a BOM.
    return write_file_bytes(path, with_bom.data(), static_cast<DWORD>(with_bom.size()), error);
}

bool extract_resource(int id, const std::wstring& dest, std::wstring& error) {
    HRSRC resource = FindResourceW(g_instance, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) {
        error = L"Installer payload is missing (resource " + std::to_wstring(id) + L").";
        return false;
    }
    const DWORD size = SizeofResource(g_instance, resource);
    HGLOBAL loaded = LoadResource(g_instance, resource);
    void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data) {
        error = L"Could not read installer payload (resource " + std::to_wstring(id) + L").";
        return false;
    }
    return write_file_bytes(dest, data, size, error);
}

bool set_reg_sz(HKEY root, const wchar_t* subkey, const wchar_t* name,
                const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LONG rc = RegSetValueExW(key, name, 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

bool set_reg_dword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    const LONG rc = RegSetValueExW(key, name, 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&value), sizeof value);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

bool create_shortcut(const std::wstring& shortcut, const std::wstring& target,
                     const std::wstring& arguments, const std::wstring& description) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, reinterpret_cast<void**>(&link))) ||
        !link)
        return false;

    link->SetPath(target.c_str());
    link->SetArguments(arguments.c_str());
    link->SetDescription(description.c_str());
    link->SetWorkingDirectory(dir_of(target).c_str());
    link->SetIconLocation(target.c_str(), 0);

    IPersistFile* file = nullptr;
    const HRESULT hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file));
    bool ok = false;
    if (SUCCEEDED(hr) && file) {
        ok = SUCCEEDED(file->Save(shortcut.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

std::wstring programs_shortcut_dir() {
    return join(known_folder(FOLDERID_Programs), L"PZ Mod Manager");
}

void remove_shortcuts() {
    const std::wstring programs = programs_shortcut_dir();
    DeleteFileW(join(programs, L"PZ Mod Manager.lnk").c_str());
    DeleteFileW(join(programs, L"Uninstall PZ Mod Manager.lnk").c_str());
    RemoveDirectoryW(programs.c_str());
    const std::wstring desktop = known_folder(FOLDERID_Desktop);
    if (!desktop.empty()) DeleteFileW(join(desktop, L"PZ Mod Manager.lnk").c_str());
}

bool create_shortcuts(const std::wstring& dir, bool start_menu, bool desktop,
                      std::wstring& warning) {
    const std::wstring exe = join(dir, L"pzmodmgr.exe");
    const std::wstring uninstaller = join(dir, L"uninstall.exe");

    if (start_menu) {
        const std::wstring menu_dir = programs_shortcut_dir();
        std::wstring error;
        if (!ensure_dir(menu_dir, error) ||
            !create_shortcut(join(menu_dir, L"PZ Mod Manager.lnk"), exe, L"",
                             L"Project Zomboid Workshop mod manager")) {
            warning += L"Could not create the Start Menu shortcut.\r\n";
        }
        if (!create_shortcut(join(menu_dir, L"Uninstall PZ Mod Manager.lnk"), uninstaller,
                             L"--uninstall", L"Uninstall PZ Mod Manager")) {
            warning += L"Could not create the uninstall shortcut.\r\n";
        }
    }

    if (desktop) {
        const std::wstring desktop_dir = known_folder(FOLDERID_Desktop);
        if (desktop_dir.empty() ||
            !create_shortcut(join(desktop_dir, L"PZ Mod Manager.lnk"), exe, L"",
                             L"Project Zomboid Workshop mod manager")) {
            warning += L"Could not create the desktop shortcut.\r\n";
        }
    }
    return true;
}

std::string native_host_json(const std::wstring& host, bool chrome) {
    const std::string path = json_escape(to_utf8(host));
    std::string manifest =
        "{\n"
        "  \"name\": \"pzmodmgr\",\n"
        "  \"description\": \"Project Zomboid Mod Manager native host\",\n"
        "  \"path\": \"" + path + "\",\n"
        "  \"type\": \"stdio\",\n";
    if (chrome) {
        manifest += "  \"allowed_origins\": [\"chrome-extension://medndgoffhfpiabahjpdkfihnhdcmhoo/\"]\n";
    } else {
        manifest += "  \"allowed_extensions\": [\"pzmodmgr@local\"]\n";
    }
    manifest += "}\n";
    return manifest;
}

bool register_browser_host(const std::wstring& dir, bool chrome, std::wstring& warning) {
    const std::wstring folder = chrome ? L"chrome-native-host" : L"firefox-native-host";
    const std::wstring manifest_dir = join(dir, folder);
    std::wstring error;
    if (!ensure_dir(manifest_dir, error)) {
        warning += error + L"\r\n";
        return false;
    }

    const std::wstring host = join(dir, L"pzmodmgr_native_host.exe");
    const std::string manifest = native_host_json(host, chrome);
    const std::wstring manifest_path = join(manifest_dir, L"pzmodmgr.json");
    if (!write_utf8_file(manifest_path, manifest, error)) {
        warning += error + L"\r\n";
        return false;
    }
    if (chrome) {
        bool any = false;
        for (const auto& browser : g_chromium_browsers) {
            if (!browser.installed) continue;
            any = true;
            if (!set_reg_sz(HKEY_CURRENT_USER, browser.host_key, nullptr, manifest_path))
                warning += std::wstring(L"Could not register the ") + browser.name +
                           L" native host.\r\n";
        }
        return any;
    }
    if (!set_reg_sz(HKEY_CURRENT_USER, kFirefoxHostKey, nullptr, manifest_path)) {
        warning += L"Could not register the Firefox native host in the registry.\r\n";
        return false;
    }
    return true;
}

bool write_uninstall_registry(const std::wstring& dir, const std::wstring& uninstaller,
                              DWORD estimated_kb, std::wstring& warning) {
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);

    const std::wstring exe = join(dir, L"pzmodmgr.exe");
    const std::wstring uninstall_cmd = L"\"" + uninstaller + L"\" --uninstall";
    const std::wstring quiet_cmd = uninstall_cmd + L" /S";

    bool ok = true;
    ok = set_reg_sz(HKEY_CURRENT_USER, kUninstallKey, L"DisplayName", kAppName) && ok;
    ok = set_reg_sz(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion", kVersion) && ok;
    ok = set_reg_sz(HKEY_CURRENT_USER, kUninstallKey, L"Publisher", L"pzmodmgr contributors") && ok;
    ok = set_reg_sz(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", dir) && ok;
    ok = set_reg_sz(HKEY_CURRENT_USER, kUninstallKey, L"DisplayIcon", exe + L",0") && ok;
    ok = set_reg_sz(HKEY_CURRENT_USER, kUninstallKey, L"UninstallString", uninstall_cmd) && ok;
    ok = set_reg_sz(HKEY_CURRENT_USER, kUninstallKey, L"QuietUninstallString", quiet_cmd) && ok;
    ok = set_reg_dword(HKEY_CURRENT_USER, kUninstallKey, L"NoModify", 1) && ok;
    ok = set_reg_dword(HKEY_CURRENT_USER, kUninstallKey, L"NoRepair", 1) && ok;
    ok = set_reg_dword(HKEY_CURRENT_USER, kUninstallKey, L"EstimatedSize", estimated_kb) && ok;
    if (!ok) warning += L"Could not write the uninstall entry in Apps & Features.\r\n";
    return ok;
}

bool safe_install_dir(const std::wstring& dir, std::wstring& reason) {
    if (dir.size() <= 3 || PathIsRootW(dir.c_str())) {
        reason = L"Choose a folder, not a drive root.";
        return false;
    }

    const std::wstring profile = known_folder(FOLDERID_Profile);
    const std::wstring zomboid = profile.empty() ? L"" : join(profile, L"Zomboid");
    if (!zomboid.empty() && (path_is_under(zomboid, dir) || path_is_under(dir, zomboid))) {
        reason = L"Do not install into your Zomboid data folder. Mods and settings live there.";
        return false;
    }

    const KNOWNFOLDERID protected_ids[] = {
        FOLDERID_Profile, FOLDERID_Desktop, FOLDERID_Documents, FOLDERID_Downloads,
        FOLDERID_LocalAppData, FOLDERID_RoamingAppData, FOLDERID_ProgramFiles,
        FOLDERID_ProgramFilesX86, FOLDERID_Windows,
    };
    for (const auto& id : protected_ids) {
        const std::wstring protected_dir = known_folder(id);
        if (!protected_dir.empty() && path_is_under(protected_dir, dir)) {
            reason = L"Choose a dedicated application folder, not a system or personal folder.";
            return false;
        }
    }

    const std::wstring windows = known_folder(FOLDERID_Windows);
    if (!windows.empty() && path_is_under(dir, windows)) {
        reason = L"Do not install into the Windows folder.";
        return false;
    }
    return true;
}

bool safe_uninstall_dir(const std::wstring& dir) {
    if (dir.size() <= 3 || PathIsRootW(dir.c_str())) return false;
    const std::wstring profile = known_folder(FOLDERID_Profile);
    const std::wstring zomboid = profile.empty() ? L"" : join(profile, L"Zomboid");
    if (!zomboid.empty() && (path_is_under(dir, zomboid) || path_is_under(zomboid, dir)))
        return false;
    if (!file_exists(join(dir, L"pzmodmgr.exe")) || !file_exists(join(dir, L"uninstall.exe")))
        return false;
    return true;
}

bool app_path_exists(const wchar_t* exe) {
    const HKEY roots[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};
    for (HKEY root : roots) {
        std::wstring sub = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\";
        sub += exe;
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        wchar_t value[32768]{};
        DWORD bytes = sizeof(value);
        DWORD type = 0;
        const LONG rc = RegQueryValueExW(key, nullptr, nullptr, &type,
                                         reinterpret_cast<BYTE*>(value), &bytes);
        RegCloseKey(key);
        if (rc == ERROR_SUCCESS && type == REG_SZ && file_exists(value)) return true;
    }
    return false;
}

bool installed_in_program_dirs(const wchar_t* relative) {
    const int folders[] = {CSIDL_PROGRAM_FILES, CSIDL_PROGRAM_FILESX86, CSIDL_LOCAL_APPDATA};
    for (int folder : folders) {
        wchar_t root[32768]{};
        if (FAILED(SHGetFolderPathW(nullptr, folder, nullptr, 0, root))) continue;
        wchar_t candidate[32768]{};
        swprintf(candidate, 32768, L"%s\\%s", root, relative);
        if (file_exists(candidate)) return true;
    }
    return false;
}

void detect_browsers() {
    g_firefox_installed =
        app_path_exists(L"firefox.exe") ||
        installed_in_program_dirs(L"Mozilla Firefox\\firefox.exe");

    g_chromium_installed = false;
    std::wstring names;
    for (auto& browser : g_chromium_browsers) {
        browser.installed = app_path_exists(browser.app_path) ||
                            installed_in_program_dirs(browser.exe);
        if (!browser.installed) continue;
        g_chromium_installed = true;
        if (!names.empty()) names += L", ";
        names += browser.name;
    }
    if (!names.empty()) g_chromium_label = names;
}

bool remove_tree(const std::wstring& dir) {
    if (dir.size() <= 3 || PathIsRootW(dir.c_str())) return false;
    WIN32_FIND_DATAW data{};
    HANDLE found = FindFirstFileW(join(dir, L"*").c_str(), &data);
    if (found != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
                continue;
            const std::wstring child = join(dir, data.cFileName);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    RemoveDirectoryW(child.c_str());
                else
                    remove_tree(child);
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(found, &data));
        FindClose(found);
    }
    return RemoveDirectoryW(dir.c_str()) != 0;
}

bool app_is_running() {
    return FindWindowW(L"PZModMgrWnd", nullptr) != nullptr;
}

// A fresh install can use public Workshop items immediately. Existing config
// is never overwritten, and users can change the account in Settings.
void seed_default_config(std::wstring& warning) {
    const std::wstring profile = known_folder(FOLDERID_Profile);
    if (profile.empty()) return;
    const std::wstring data_dir = join(join(profile, L"Zomboid"), L"pzmodmgr");
    const std::wstring config = join(data_dir, L"config.ini");
    if (file_exists(config)) return;

    std::wstring error;
    if (!ensure_dir(data_dir, error)) {
        warning += error + L"\r\n";
        return;
    }
    if (!write_utf8_file(config, "steamcmd_exe=\nsteam_user=anonymous\n", error))
        warning += error + L"\r\n";
}

std::wstring default_install_dir() {
    const std::wstring local = known_folder(FOLDERID_LocalAppData);
    return local.empty() ? L"" : join(join(local, L"Programs"), L"PZModManager");
}

bool checked(HWND box) {
    return SendMessageW(box, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void set_checked(HWND box, bool value) {
    SendMessageW(box, BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
}

HFONT make_font(int points, int weight) {
    return CreateFontW(-MulDiv(points, g_dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void set_font(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void pump_messages() {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (!IsDialogMessageW(g_hwnd, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

void set_status(const std::wstring& text) {
    if (g_status) SetWindowTextW(g_status, text.c_str());
}

void show_error(const std::wstring& text) {
    MessageBoxW(g_hwnd, text.c_str(), L"PZ Mod Manager Setup", MB_OK | MB_ICONERROR);
}

bool payload_wanted(PayloadNeed need, bool firefox, bool chromium) {
    if (need == kAlways) return true;
    if (need == kFirefox) return firefox;
    if (need == kChromium) return chromium;
    return firefox || chromium;
}

bool extract_payload(const std::wstring& dir, bool firefox, bool chromium, bool silent,
                     std::wstring& error) {
    DWORD completed = 0;
    for (const auto& item : kPayload) {
        if (!payload_wanted(item.need, firefox, chromium)) continue;
        const std::wstring dest = join(dir, item.relative);
        std::wstring parent = dir_of(dest);
        if (!ensure_dir(parent, error)) return false;
        if (!extract_resource(item.id, dest, error)) return false;
        ++completed;
        if (!silent) {
            set_status(std::wstring(L"Installing ") + item.relative + L"...");
            SendMessageW(g_progress, PBM_SETPOS, completed, 0);
            UpdateWindow(g_hwnd);
            pump_messages();
        }
    }
    return true;
}

bool perform_install(const std::wstring& raw_dir, bool start_menu, bool desktop,
                     bool firefox, bool chromium, bool silent, std::wstring& warning) {
    const std::wstring dir = full_path(trim(raw_dir));
    std::wstring reason;
    if (!safe_install_dir(dir, reason)) {
        warning = reason;
        return false;
    }

    if (app_is_running()) {
        warning = L"PZ Mod Manager is running. Close it, then run setup again.";
        return false;
    }

    std::wstring error;
    if (!ensure_dir(dir, error)) {
        warning = error;
        return false;
    }

    DWORD needed = 0;
    for (const auto& item : kPayload)
        if (payload_wanted(item.need, firefox, chromium)) ++needed;

    if (!silent) {
        SendMessageW(g_progress, PBM_SETRANGE32, 0, needed + 1);
        SendMessageW(g_progress, PBM_SETPOS, 0, 0);
    }
    if (!extract_payload(dir, firefox, chromium, silent, warning)) return false;

    const std::wstring uninstaller = join(dir, L"uninstall.exe");
    if (firefox) register_browser_host(dir, false, warning);
    else RegDeleteTreeW(HKEY_CURRENT_USER, kFirefoxHostKey);
    if (chromium) register_browser_host(dir, true, warning);
    else {
        for (const auto& browser : g_chromium_browsers)
            RegDeleteTreeW(HKEY_CURRENT_USER, browser.host_key);
    }

    create_shortcuts(dir, start_menu, desktop, warning);

    DWORD estimated_kb = 0;
    for (const auto& item : kPayload) {
        if (!payload_wanted(item.need, firefox, chromium)) continue;
        HRSRC resource = FindResourceW(g_instance, MAKEINTRESOURCEW(item.id), RT_RCDATA);
        if (resource) estimated_kb += (SizeofResource(g_instance, resource) + 1023) / 1024;
    }
    write_uninstall_registry(dir, uninstaller, estimated_kb, warning);
    seed_default_config(warning);

    if (!silent) {
        SendMessageW(g_progress, PBM_SETPOS, needed + 1, 0);
        set_status(L"Installed successfully.");
        UpdateWindow(g_hwnd);
    }
    return true;
}

void launch_app(const std::wstring& dir) {
    const std::wstring exe = join(dir, L"pzmodmgr.exe");
    ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
}

bool browse_for_folder(HWND owner, const std::wstring& start, std::wstring& selected) {
    g_browse_start = start;
    BROWSEINFOW info{};
    info.hwndOwner = owner;
    info.lpszTitle = L"Select the folder where PZ Mod Manager should be installed";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    info.lpfn = [](HWND hwnd, UINT message, LPARAM, LPARAM data) -> int {
        if (message == BFFM_INITIALIZED && data)
            SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
        return 0;
    };
    info.lParam = reinterpret_cast<LPARAM>(g_browse_start.c_str());
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&info);
    if (!item) return false;
    wchar_t path[32768]{};
    const bool ok = SHGetPathFromIDListW(item, path) != FALSE;
    CoTaskMemFree(item);
    if (ok) selected = path;
    return ok;
}

void begin_install() {
    if (g_installing) return;

    wchar_t raw[32768]{};
    GetWindowTextW(g_path, raw, 32768);
    const std::wstring dir = full_path(trim(raw));
    std::wstring reason;
    if (!safe_install_dir(dir, reason)) {
        show_error(reason);
        SetFocus(g_path);
        return;
    }
    if (app_is_running()) {
        show_error(L"PZ Mod Manager is running. Close it, then run setup again.");
        return;
    }

    g_installing = true;
    g_launch_after = checked(g_launch);
    EnableWindow(g_path, FALSE);
    EnableWindow(g_browse, FALSE);
    EnableWindow(g_startmenu, FALSE);
    EnableWindow(g_desktop, FALSE);
    EnableWindow(g_firefox, FALSE);
    EnableWindow(g_chrome, FALSE);
    EnableWindow(g_launch, FALSE);
    EnableWindow(g_install, FALSE);
    EnableWindow(g_cancel, FALSE);
    SetWindowTextW(g_install, L"Installing...");
    set_status(L"Preparing...");
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    std::wstring warning;
    const bool ok = perform_install(dir, checked(g_startmenu), checked(g_desktop),
                                    checked(g_firefox), checked(g_chrome), false, warning);

    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    g_installing = false;

    if (!ok) {
        EnableWindow(g_path, TRUE);
        EnableWindow(g_browse, TRUE);
        EnableWindow(g_startmenu, TRUE);
        EnableWindow(g_desktop, TRUE);
        EnableWindow(g_firefox, TRUE);
        EnableWindow(g_chrome, TRUE);
        EnableWindow(g_launch, TRUE);
        EnableWindow(g_install, TRUE);
        EnableWindow(g_cancel, TRUE);
        SetWindowTextW(g_install, L"Install");
        set_status(L"Installation failed.");
        show_error(warning);
        return;
    }

    g_installed = true;
    SetWindowTextW(g_install, L"Finish");
    EnableWindow(g_install, TRUE);
    SetFocus(g_install);
    ShowWindow(g_cancel, SW_HIDE);
    if (!warning.empty()) {
        MessageBoxW(g_hwnd, warning.c_str(), L"Installed with warnings",
                    MB_OK | MB_ICONWARNING);
    }
    set_status(L"Installed successfully. Select Finish to close.");
}

void paint_window(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);

    FillRect(dc, &client, g_white_brush);

    const int header = S(104);
    TRIVERTEX vertices[2] = {
        {0, 0, 20 << 8, 48 << 8, 35 << 8, 0},
        {static_cast<SHORT>(client.right), static_cast<SHORT>(header), 52 << 8, 116 << 8,
         80 << 8, 0},
    };
    GRADIENT_RECT gradient{0, 1};
    GradientFill(dc, vertices, 2, &gradient, 1, GRADIENT_FILL_RECT_V);

    RECT accent{0, header - S(4), client.right, header};
    HBRUSH orange = CreateSolidBrush(RGB(226, 132, 61));
    FillRect(dc, &accent, orange);
    DeleteObject(orange);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    HFONT old = static_cast<HFONT>(SelectObject(dc, g_title_font));
    RECT title{S(28), S(18), client.right - S(120), S(58)};
    DrawTextW(dc, kAppName, -1, &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SelectObject(dc, g_sub_font);
    SetTextColor(dc, RGB(198, 224, 208));
    RECT subtitle{S(30), S(56), client.right - S(120), S(84)};
    DrawTextW(dc, L"Project Zomboid Workshop installer", -1, &subtitle,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SetTextColor(dc, RGB(198, 224, 208));
    RECT version{client.right - S(148), S(20), client.right - S(28), S(48)};
    DrawTextW(dc, L"v" PZ_VERSION, -1, &version, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, old);

    HPEN line = CreatePen(PS_SOLID, 1, RGB(226, 232, 228));
    HPEN old_pen = static_cast<HPEN>(SelectObject(dc, line));
    MoveToEx(dc, S(24), S(424), nullptr);
    LineTo(dc, client.right - S(24), S(424));
    SelectObject(dc, old_pen);
    DeleteObject(line);

    EndPaint(hwnd, &ps);
}

void create_controls(HWND hwnd) {
    g_dpi = 96;
    HDC dc = GetDC(nullptr);
    if (dc) {
        const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
        if (dpi > 0) g_dpi = dpi;
        ReleaseDC(nullptr, dc);
    }

    g_white_brush = CreateSolidBrush(RGB(255, 255, 255));
    g_title_font = make_font(22, FW_BOLD);
    g_sub_font = make_font(10, FW_NORMAL);
    g_heading_font = make_font(15, FW_SEMIBOLD);
    g_body_font = make_font(9, FW_NORMAL);
    g_small_font = make_font(8, FW_NORMAL);

    HWND heading = CreateWindowW(L"STATIC", L"Install PZ Mod Manager",
                                 WS_CHILD | WS_VISIBLE | SS_LEFT, S(24), S(116), S(600),
                                 S(30), hwnd, nullptr, g_instance, nullptr);
    set_font(heading, g_heading_font);

    wchar_t user[256]{};
    DWORD user_len = 256;
    if (!GetUserNameW(user, &user_len)) wcscpy(user, L"this user");
    const std::wstring user_line = std::wstring(L"for ") + user;
    HWND user_label = CreateWindowW(L"STATIC", user_line.c_str(), WS_CHILD | WS_VISIBLE,
                                    S(24), S(146), S(600), S(20), hwnd, nullptr, g_instance,
                                    nullptr);
    set_font(user_label, g_body_font);

    HWND dest_label = CreateWindowW(L"STATIC", L"Destination folder", WS_CHILD | WS_VISIBLE,
                                    S(24), S(178), S(300), S(18), hwnd, nullptr, g_instance,
                                    nullptr);
    set_font(dest_label, g_body_font);

    g_path = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP, S(24),
                             S(198), S(464), S(28), hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PATH)),
                             g_instance, nullptr);
    set_font(g_path, g_body_font);
    SendMessageW(g_path, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(S(6), S(6)));

    g_browse = CreateWindowW(L"BUTTON", L"Browse...",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(498), S(198), S(138),
                             S(28), hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BROWSE)),
                             g_instance, nullptr);
    set_font(g_browse, g_body_font);

    g_startmenu = CreateWindowW(L"BUTTON", L"Create a Start Menu shortcut",
                                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP, S(26),
                                S(242), S(600), S(22), hwnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STARTMENU)),
                                g_instance, nullptr);
    g_desktop = CreateWindowW(L"BUTTON", L"Create a desktop shortcut",
                              WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP, S(26),
                              S(268), S(600), S(22), hwnd,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DESKTOP)),
                              g_instance, nullptr);
    g_firefox = CreateWindowW(L"BUTTON", L"Register the Firefox right-click add-on host",
                              WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP, S(26),
                              S(294), S(600), S(22), hwnd,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FIREFOX)),
                              g_instance, nullptr);
    g_chrome = CreateWindowW(L"BUTTON", L"Register the Chrome right-click add-on host",
                             WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP, S(26),
                             S(320), S(600), S(22), hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHROME)),
                             g_instance, nullptr);
    detect_browsers();
    if (!g_firefox_installed) ShowWindow(g_firefox, SW_HIDE);
    if (!g_chromium_installed) ShowWindow(g_chrome, SW_HIDE);
    if (g_chromium_installed) {
        const std::wstring label = std::wstring(L"Register the ") + g_chromium_label +
                                   L" right-click add-on host";
        SetWindowTextW(g_chrome, label.c_str());
    }
    g_launch = CreateWindowW(L"BUTTON", L"Launch PZ Mod Manager",
                             WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP, S(26),
                             S(346), S(600), S(22), hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LAUNCH)),
                             g_instance, nullptr);
    set_font(g_startmenu, g_body_font);
    set_font(g_desktop, g_body_font);
    set_font(g_firefox, g_body_font);
    set_font(g_chrome, g_body_font);
    set_font(g_launch, g_body_font);

    g_progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE,
                                 S(24), S(380), S(612), S(10), hwnd, nullptr, g_instance,
                                 nullptr);
    SendMessageW(g_progress, PBM_SETRANGE32, 0, kPayloadCount + 1);

    g_status = CreateWindowW(L"STATIC", L"Ready to install.", WS_CHILD | WS_VISIBLE,
                             S(24), S(398), S(612), S(20), hwnd, nullptr, g_instance,
                             nullptr);
    set_font(g_status, g_small_font);

    g_install = CreateWindowW(L"BUTTON", L"Install",
                              WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                              S(450), S(432), S(90), S(30), hwnd,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_INSTALL)),
                              g_instance, nullptr);
    g_cancel = CreateWindowW(L"BUTTON", L"Cancel",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(548), S(432), S(90),
                             S(30), hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)),
                             g_instance, nullptr);
    set_font(g_install, g_body_font);
    set_font(g_cancel, g_body_font);

    HWND footer = CreateWindowW(
        L"STATIC",
        L"Installs for the current user. Your mods stay in %USERPROFILE%\\Zomboid\\mods.",
        WS_CHILD | WS_VISIBLE, S(24), S(470), S(612), S(18), hwnd, nullptr, g_instance,
        nullptr);
    set_font(footer, g_small_font);

    const std::wstring dest = default_install_dir();
    SetWindowTextW(g_path, dest.c_str());
    set_checked(g_startmenu, true);
    if (g_firefox_installed) set_checked(g_firefox, true);
    if (g_chromium_installed) set_checked(g_chrome, true);
    set_checked(g_launch, true);
    SetFocus(g_install);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        g_hwnd = hwnd;
        create_controls(hwnd);
        return 0;
    case WM_PAINT:
        paint_window(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkColor(dc, RGB(255, 255, 255));
        SetTextColor(dc, RGB(52, 58, 54));
        return reinterpret_cast<LRESULT>(g_white_brush);
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_INSTALL:
            if (g_installed) {
                if (g_launch_after) {
                    wchar_t raw[32768]{};
                    GetWindowTextW(g_path, raw, 32768);
                    launch_app(full_path(trim(raw)));
                }
                DestroyWindow(hwnd);
            } else {
                begin_install();
            }
            return 0;
        case IDCANCEL:
            if (!g_installing) DestroyWindow(hwnd);
            return 0;
        case IDC_BROWSE: {
            if (g_installing) return 0;
            wchar_t raw[32768]{};
            GetWindowTextW(g_path, raw, 32768);
            std::wstring selected;
            if (browse_for_folder(hwnd, raw, selected))
                SetWindowTextW(g_path, selected.c_str());
            return 0;
        }
        default:
            return 0;
        }
    case WM_CLOSE:
        if (!g_installing) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_title_font) DeleteObject(g_title_font);
        if (g_sub_font) DeleteObject(g_sub_font);
        if (g_heading_font) DeleteObject(g_heading_font);
        if (g_body_font) DeleteObject(g_body_font);
        if (g_small_font) DeleteObject(g_small_font);
        if (g_white_brush) DeleteObject(g_white_brush);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

int run_uninstall(bool silent) {
    const std::wstring self = module_path();
    const std::wstring dir = dir_of(self);
    if (!safe_uninstall_dir(dir)) {
        if (!silent)
            MessageBoxW(nullptr,
                        L"The uninstaller is not running from a valid PZ Mod Manager folder.",
                        L"Uninstall PZ Mod Manager", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (app_is_running()) {
        if (!silent)
            MessageBoxW(nullptr, L"Close PZ Mod Manager before uninstalling it.",
                        L"Uninstall PZ Mod Manager", MB_OK | MB_ICONWARNING);
        return 1;
    }
    if (!silent) {
        const int answer = MessageBoxW(
            nullptr,
            L"Remove PZ Mod Manager from this user account?\r\n\r\n"
            L"Your mods and settings in %USERPROFILE%\\Zomboid are kept.",
            L"Uninstall PZ Mod Manager", MB_YESNO | MB_ICONQUESTION);
        if (answer != IDYES) return 0;
    }

    remove_shortcuts();
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, kFirefoxHostKey);
    for (const auto& browser : g_chromium_browsers)
        RegDeleteTreeW(HKEY_CURRENT_USER, browser.host_key);

    wchar_t temp_dir[MAX_PATH]{};
    const DWORD temp_len = GetTempPathW(MAX_PATH, temp_dir);
    bool moved = false;
    std::wstring temp;
    if (temp_len > 0 && temp_len < MAX_PATH) {
        temp = std::wstring(temp_dir) + L"pzmodmgr-uninstall-" +
               std::to_wstring(GetCurrentProcessId()) + L".exe";
        moved = MoveFileExW(self.c_str(), temp.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }

    const bool removed = remove_tree(dir);
    if (moved) MoveFileExW(temp.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    else MoveFileExW(self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);

    if (!silent) {
        MessageBoxW(nullptr,
                    removed ? L"PZ Mod Manager was uninstalled."
                            : L"PZ Mod Manager was partially removed. Some files may remain.",
                    L"Uninstall PZ Mod Manager",
                    MB_OK | (removed ? MB_ICONINFORMATION : MB_ICONWARNING));
    }
    return removed ? 0 : 1;
}

int run_gui() {
    const std::wstring self = module_path();
    const HICON icon = static_cast<HICON>(
        LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_SETUP), IMAGE_ICON, 0, 0,
                   LR_DEFAULTSIZE | LR_SHARED));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = g_instance;
    wc.hIcon = icon;
    wc.hIconSm = icon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"PZModMgrSetupWnd";
    RegisterClassExW(&wc);

    RECT work{0, 0, S(660), S(496)};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&work, style, FALSE, 0);
    const int width = work.right - work.left;
    const int height = work.bottom - work.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"PZ Mod Manager Setup", style, x, y,
                                width, height, nullptr, nullptr, g_instance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return static_cast<int>(message.wParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;
    SetProcessDPIAware();

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof controls;
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&controls);

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool uninstall = false;
    bool silent = false;
    std::wstring destination;

    const std::wstring self = module_path();
    if (_wcsicmp(file_of(self).c_str(), L"uninstall.exe") == 0) uninstall = true;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (_wcsicmp(arg.c_str(), L"--uninstall") == 0 ||
            _wcsicmp(arg.c_str(), L"/uninstall") == 0) {
            uninstall = true;
        } else if (_wcsicmp(arg.c_str(), L"/S") == 0 ||
                   _wcsicmp(arg.c_str(), L"--silent") == 0) {
            silent = true;
        } else if (_wcsnicmp(arg.c_str(), L"/D=", 3) == 0) {
            destination = trim(arg.substr(3));
        }
    }
    if (argv) LocalFree(argv);

    int result = 0;
    if (uninstall) {
        result = run_uninstall(silent);
    } else if (silent) {
        const std::wstring dir = destination.empty() ? default_install_dir() : destination;
        std::wstring warning;
        result = perform_install(dir, true, false, true, true, true, warning) ? 0 : 1;
    } else {
        result = run_gui();
    }

    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
