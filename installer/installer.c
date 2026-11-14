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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef PZ_VERSION
#define PZ_VERSION L"0.3.1"
#endif

#define PZ_PATH_MAX 32768
#define PZ_WARN_MAX 4096
#define PZ_ERR_MAX 1024

#define IDI_SETUP 101
#define IDC_INSTALL 1000
#define IDC_BROWSE 1001
#define IDC_PATH 1002
#define IDC_STARTMENU 1003
#define IDC_DESKTOP 1004
#define IDC_FIREFOX 1005
#define IDC_CHROME 1006
#define IDC_LAUNCH 1007

#define RES_APP 2001
#define RES_HOST 2002
#define RES_README 2003
#define RES_LICENSE 2004
#define RES_FIREFOX_MANIFEST 2005
#define RES_FIREFOX_JS 2006
#define RES_CHROME_MANIFEST 2009
#define RES_CHROME_JS 2010
#define RES_CHROME_ICON16 2012
#define RES_CHROME_ICON32 2013
#define RES_CHROME_ICON48 2014
#define RES_CHROME_ICON128 2015
#define RES_UNINSTALLER 2016

typedef enum PayloadNeed { kAlways, kFirefox, kChromium, kAnyBrowser } PayloadNeed;

typedef struct PayloadFile {
    int id;
    const wchar_t* relative;
    PayloadNeed need;
} PayloadFile;

typedef struct ChromiumBrowser {
    const wchar_t* name;
    const wchar_t* exe;
    const wchar_t* app_path;
    const wchar_t* host_key;
    int installed;
} ChromiumBrowser;

/* Small growable UTF-8 buffer. */
typedef struct CharBuf {
    char* data;
    size_t length;
    size_t capacity;
} CharBuf;

static const wchar_t kAppName[] = L"PZ Mod Manager";
static const wchar_t kVersion[] = PZ_VERSION;
static const wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PZModManager";
static const wchar_t kFirefoxHostKey[] =
    L"Software\\Mozilla\\NativeMessagingHosts\\pzmodmgr";

static const PayloadFile kPayload[] = {
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

static const int kPayloadCount =
    (int)(sizeof(kPayload) / sizeof(kPayload[0]));

static ChromiumBrowser g_chromium_browsers[] = {
    {L"Chrome", L"Google\\Chrome\\Application\\chrome.exe", L"chrome.exe",
     L"Software\\Google\\Chrome\\NativeMessagingHosts\\pzmodmgr", 0},
    {L"Chromium", L"Chromium\\Application\\chrome.exe", L"chromium.exe",
     L"Software\\Chromium\\NativeMessagingHosts\\pzmodmgr", 0},
    {L"Brave", L"BraveSoftware\\Brave-Browser\\Application\\brave.exe",
     L"brave.exe",
     L"Software\\BraveSoftware\\Brave-Browser\\NativeMessagingHosts\\pzmodmgr",
     0},
    {L"Edge", L"Microsoft\\Edge\\Application\\msedge.exe", L"msedge.exe",
     L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\pzmodmgr", 0},
};

static HINSTANCE g_instance = NULL;
static HWND g_hwnd = NULL;
static HWND g_path = NULL;
static HWND g_browse = NULL;
static HWND g_install = NULL;
static HWND g_cancel = NULL;
static HWND g_startmenu = NULL;
static HWND g_desktop = NULL;
static HWND g_firefox = NULL;
static HWND g_chrome = NULL;
static HWND g_launch = NULL;
static HWND g_progress = NULL;
static HWND g_status = NULL;

static HFONT g_title_font = NULL;
static HFONT g_sub_font = NULL;
static HFONT g_heading_font = NULL;
static HFONT g_body_font = NULL;
static HFONT g_small_font = NULL;
static HBRUSH g_white_brush = NULL;
static int g_dpi = 96;

static int g_installing = 0;
static int g_installed = 0;
static int g_upgrade = 0;
static int g_launch_after = 0;
static int g_firefox_installed = 0;
static int g_chromium_installed = 0;
static wchar_t g_chromium_label[512] = L"Chrome";
static wchar_t g_browse_start[PZ_PATH_MAX];

/* ---------------------------------------------------------------- strings */

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

static void wcat(wchar_t* dst, size_t cap, const wchar_t* src) {
    size_t used, n;
    if (!dst || cap == 0 || !src) return;
    used = wcslen(dst);
    if (used >= cap - 1) return;
    n = wcslen(src);
    if (n > cap - used - 1) n = cap - used - 1;
    memcpy(dst + used, src, n * sizeof(wchar_t));
    dst[used + n] = 0;
}

static void wjoin(wchar_t* out, size_t cap, const wchar_t* a,
                  const wchar_t* b) {
    size_t len;
    if (!out || cap == 0) return;
    if (!a || !*a) {
        wcopy(out, cap, b ? b : L"");
        return;
    }
    if (!b || !*b) {
        wcopy(out, cap, a);
        return;
    }
    wcopy(out, cap, a);
    len = wcslen(out);
    if (len && out[len - 1] != L'\\' && out[len - 1] != L'/') wcat(out, cap, L"\\");
    wcat(out, cap, b);
}

static void wdirname(wchar_t* out, size_t cap, const wchar_t* path) {
    const wchar_t* slash;
    const wchar_t* back;
    const wchar_t* split;
    size_t n;
    if (!out || cap == 0) return;
    if (!path || !*path) {
        wcopy(out, cap, L".");
        return;
    }
    slash = wcsrchr(path, L'/');
    back = wcsrchr(path, L'\\');
    split = slash > back ? slash : back;
    if (!split) {
        wcopy(out, cap, L".");
        return;
    }
    n = (size_t)(split - path);
    if (n == 0) n = 1;
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n * sizeof(wchar_t));
    out[n] = 0;
}

static const wchar_t* wbasename(const wchar_t* path) {
    const wchar_t* slash;
    const wchar_t* back;
    const wchar_t* split;
    if (!path) return L"";
    slash = wcsrchr(path, L'/');
    back = wcsrchr(path, L'\\');
    split = slash > back ? slash : back;
    return split ? split + 1 : path;
}

static int wempty(const wchar_t* text) { return !text || !*text; }

static void wtrim(wchar_t* text) {
    wchar_t* first;
    size_t len;
    if (!text) return;
    first = text;
    while (*first == L' ' || *first == L'\t' || *first == L'\r' || *first == L'\n')
        ++first;
    len = wcslen(first);
    while (len && (first[len - 1] == L' ' || first[len - 1] == L'\t' ||
                   first[len - 1] == L'\r' || first[len - 1] == L'\n'))
        --len;
    if (first != text) memmove(text, first, len * sizeof(wchar_t));
    text[len] = 0;
}

/* ---------------------------------------------------------------- paths/io */

static int file_exists(const wchar_t* path) {
    const DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES &&
           !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const wchar_t* path) {
    const DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static int module_path(HMODULE module, wchar_t* out, size_t cap) {
    DWORD n;
    if (!out || cap == 0) return 0;
    n = GetModuleFileNameW(module, out, (DWORD)cap);
    if (n == 0 || n >= cap) return 0;
    return 1;
}

static void full_path(const wchar_t* path, wchar_t* out, size_t cap) {
    DWORD n;
    size_t len;
    if (!out || cap == 0) return;
    n = GetFullPathNameW(path, (DWORD)cap, out, NULL);
    if (n == 0 || n >= cap) {
        wcopy(out, cap, path);
        return;
    }
    len = wcslen(out);
    while (len > 3 && (out[len - 1] == L'\\' || out[len - 1] == L'/')) out[--len] = 0;
}

static int known_folder(const KNOWNFOLDERID* id, wchar_t* out, size_t cap) {
    PWSTR raw = NULL;
    size_t len;
    if (!id || !out || cap == 0) return 0;
    out[0] = 0;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, NULL, &raw)) || !raw)
        return 0;
    wcopy(out, cap, raw);
    CoTaskMemFree(raw);
    len = wcslen(out);
    while (len > 3 && (out[len - 1] == L'\\' || out[len - 1] == L'/')) out[--len] = 0;
    return 1;
}

static void win_error(DWORD error, wchar_t* out, size_t cap) {
    LPWSTR raw = NULL;
    DWORD n;
    if (!out || cap == 0) return;
    n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_ALLOCATE_BUFFER |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, error, 0, (LPWSTR)&raw, 0, NULL);
    if (n == 0 || !raw) {
        swprintf(out, cap, L"Windows error %lu", (unsigned long)error);
        return;
    }
    wcopy(out, cap, raw);
    LocalFree(raw);
    wtrim(out);
}

static int path_is_under(const wchar_t* child, const wchar_t* parent) {
    const size_t parent_len = wcslen(parent);
    if (_wcsicmp(child, parent) == 0) return 1;
    if (_wcsnicmp(child, parent, parent_len) != 0 || wcslen(child) <= parent_len)
        return 0;
    return child[parent_len] == L'\\' || child[parent_len] == L'/';
}

static int ensure_dir(const wchar_t* dir, wchar_t* error, size_t cap) {
    wchar_t message[PZ_ERR_MAX];
    const int rc = SHCreateDirectoryExW(NULL, dir, NULL);
    if (rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS ||
        rc == ERROR_FILE_EXISTS)
        return 1;
    SetLastError((DWORD)rc);
    win_error((DWORD)rc, message, sizeof(message) / sizeof(message[0]));
    swprintf(error, cap, L"Could not create %ls: %ls", dir, message);
    return 0;
}

static char* to_utf8_alloc(const wchar_t* text) {
    int n;
    char* out;
    if (!text || !*text) {
        out = (char*)malloc(1);
        if (out) out[0] = 0;
        return out;
    }
    n = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    out = (char*)malloc((size_t)n);
    if (!out) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, out, n, NULL, NULL)) {
        free(out);
        return NULL;
    }
    return out;
}

static int from_utf8(const char* text, wchar_t* out, size_t cap) {
    int n;
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!text || !*text) return 1;
    n = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (n <= 0) return 0;
    if ((size_t)n > cap) n = (int)cap;
    if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, out, n)) return 0;
    out[cap - 1] = 0;
    return 1;
}

static char* read_utf8_file(const wchar_t* path, size_t* out_length) {
    HANDLE file;
    LARGE_INTEGER size;
    char* out = NULL;
    DWORD total = 0;

    if (out_length) *out_length = 0;
    file = CreateFileW(path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return NULL;
    }
    out = (char*)malloc((size_t)size.QuadPart + 1);
    if (!out) {
        CloseHandle(file);
        return NULL;
    }
    while (total < (DWORD)size.QuadPart) {
        DWORD got = 0;
        if (!ReadFile(file, out + total, (DWORD)size.QuadPart - total, &got,
                      NULL) ||
            got == 0)
            break;
        total += got;
    }
    CloseHandle(file);
    out[total] = 0;
    if (out_length) *out_length = total;
    return out;
}

static char* config_value(const char* text, const char* key) {
    const size_t key_length = strlen(key);
    const char* line = text;
    if (!text) return NULL;
    while (*line) {
        const char* end = strchr(line, '\n');
        const char* stop = end ? end : line + strlen(line);
        size_t length = (size_t)(stop - line);
        if (length && line[length - 1] == '\r') --length;
        if (length >= key_length + 1 && strncmp(line, key, key_length) == 0 &&
            line[key_length] == '=') {
            const char* value = line + key_length + 1;
            const size_t value_length = length - key_length - 1;
            char* out = (char*)malloc(value_length + 1);
            if (!out) return NULL;
            memcpy(out, value, value_length);
            out[value_length] = 0;
            return out;
        }
        if (!end) break;
        line = end + 1;
    }
    return NULL;
}

static int char_buf_reserve(CharBuf* buffer, size_t extra) {
    size_t needed;
    size_t capacity;
    char* next;
    if (!buffer) return 0;
    needed = buffer->length + extra + 1;
    if (needed <= buffer->capacity) return 1;
    capacity = buffer->capacity ? buffer->capacity : 128;
    while (capacity < needed) capacity *= 2;
    next = (char*)realloc(buffer->data, capacity);
    if (!next) return 0;
    buffer->data = next;
    buffer->capacity = capacity;
    return 1;
}

static int char_buf_append(CharBuf* buffer, const char* text, size_t length) {
    if (!char_buf_reserve(buffer, length)) return 0;
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = 0;
    return 1;
}

static int char_buf_cstr(CharBuf* buffer, const char* text) {
    return char_buf_append(buffer, text, strlen(text));
}

static int char_buf_char(CharBuf* buffer, char c) {
    return char_buf_append(buffer, &c, 1);
}

static void char_buf_free(CharBuf* buffer) {
    if (!buffer) return;
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

static int json_escape(CharBuf* out, const char* text) {
    const unsigned char* cursor = (const unsigned char*)text;
    if (!out || !text) return 0;
    while (*cursor) {
        switch (*cursor) {
        case '"':
            if (!char_buf_cstr(out, "\\\"")) return 0;
            break;
        case '\\':
            if (!char_buf_cstr(out, "\\\\")) return 0;
            break;
        case '\b':
            if (!char_buf_cstr(out, "\\b")) return 0;
            break;
        case '\f':
            if (!char_buf_cstr(out, "\\f")) return 0;
            break;
        case '\n':
            if (!char_buf_cstr(out, "\\n")) return 0;
            break;
        case '\r':
            if (!char_buf_cstr(out, "\\r")) return 0;
            break;
        case '\t':
            if (!char_buf_cstr(out, "\\t")) return 0;
            break;
        default:
            if (*cursor < 0x20) {
                char escaped[7];
                snprintf(escaped, sizeof escaped, "\\u%04x", *cursor);
                if (!char_buf_cstr(out, escaped)) return 0;
            } else if (!char_buf_char(out, (char)*cursor)) {
                return 0;
            }
            break;
        }
        ++cursor;
    }
    return 1;
}

static int write_file_bytes(const wchar_t* path, const void* data, DWORD size,
                            wchar_t* error, size_t cap) {
    HANDLE file;
    const unsigned char* bytes = (const unsigned char*)data;
    DWORD written = 0;
    wchar_t message[PZ_ERR_MAX];

    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        win_error(GetLastError(), message, sizeof(message) / sizeof(message[0]));
        swprintf(error, cap, L"Could not create %ls: %ls", path, message);
        return 0;
    }
    while (written < size) {
        DWORD chunk = 0;
        if (!WriteFile(file, bytes + written, size - written, &chunk, NULL) ||
            chunk == 0) {
            win_error(GetLastError(), message,
                      sizeof(message) / sizeof(message[0]));
            swprintf(error, cap, L"Could not write %ls: %ls", path, message);
            CloseHandle(file);
            DeleteFileW(path);
            return 0;
        }
        written += chunk;
    }
    CloseHandle(file);
    return 1;
}

static int write_utf8_file(const wchar_t* path, const char* text,
                           size_t length, wchar_t* error, size_t cap) {
    return write_file_bytes(path, text, (DWORD)length, error, cap);
}

static int extract_resource(int id, const wchar_t* dest, wchar_t* error,
                            size_t cap) {
    HRSRC resource = FindResourceW(g_instance, MAKEINTRESOURCEW(id), RT_RCDATA);
    DWORD size;
    HGLOBAL loaded;
    void* data;
    if (!resource) {
        swprintf(error, cap, L"Installer payload is missing (resource %d).",
                 id);
        return 0;
    }
    size = SizeofResource(g_instance, resource);
    loaded = LoadResource(g_instance, resource);
    data = loaded ? LockResource(loaded) : NULL;
    if (!data) {
        swprintf(error, cap,
                 L"Could not read installer payload (resource %d).", id);
        return 0;
    }
    return write_file_bytes(dest, data, size, error, cap);
}

/* --------------------------------------------------------------- registry */

static int set_reg_sz(HKEY root, const wchar_t* subkey, const wchar_t* name,
                      const wchar_t* value) {
    HKEY key = NULL;
    DWORD bytes;
    LONG rc;
    if (RegCreateKeyExW(root, subkey, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL) !=
        ERROR_SUCCESS)
        return 0;
    bytes = (DWORD)((wcslen(value) + 1) * sizeof(wchar_t));
    rc = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value, bytes);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

static int set_reg_dword(HKEY root, const wchar_t* subkey,
                         const wchar_t* name, DWORD value) {
    HKEY key = NULL;
    LONG rc;
    if (RegCreateKeyExW(root, subkey, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL) !=
        ERROR_SUCCESS)
        return 0;
    rc = RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value,
                        sizeof value);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

static int get_reg_sz(HKEY root, const wchar_t* subkey, const wchar_t* name,
                      wchar_t* out, size_t cap) {
    HKEY key = NULL;
    DWORD bytes = (DWORD)(cap * sizeof(wchar_t));
    DWORD type = 0;
    LONG rc;
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return 0;
    rc = RegQueryValueExW(key, name, NULL, &type, (BYTE*)out, &bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ) return 0;
    out[cap - 1] = 0;
    return 1;
}

/* -------------------------------------------------------------- shortcuts */

static int create_shortcut(const wchar_t* shortcut, const wchar_t* target,
                           const wchar_t* arguments,
                           const wchar_t* description) {
    IShellLinkW* link = NULL;
    IPersistFile* file = NULL;
    wchar_t working[PZ_PATH_MAX];
    HRESULT hr;
    int ok = 0;

    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IShellLinkW, (void**)&link);
    if (FAILED(hr) || !link) return 0;

    link->lpVtbl->SetPath(link, target);
    link->lpVtbl->SetArguments(link, arguments);
    link->lpVtbl->SetDescription(link, description);
    wdirname(working, sizeof(working) / sizeof(working[0]), target);
    link->lpVtbl->SetWorkingDirectory(link, working);
    link->lpVtbl->SetIconLocation(link, target, 0);

    hr = link->lpVtbl->QueryInterface(link, &IID_IPersistFile, (void**)&file);
    if (SUCCEEDED(hr) && file) {
        ok = SUCCEEDED(file->lpVtbl->Save(file, shortcut, TRUE));
        file->lpVtbl->Release(file);
    }
    link->lpVtbl->Release(link);
    return ok;
}

static void programs_shortcut_dir(wchar_t* out, size_t cap) {
    wchar_t programs[PZ_PATH_MAX];
    if (!known_folder(&FOLDERID_Programs, programs,
                      sizeof(programs) / sizeof(programs[0]))) {
        out[0] = 0;
        return;
    }
    wjoin(out, cap, programs, L"PZ Mod Manager");
}

static void remove_shortcuts(void) {
    wchar_t programs[PZ_PATH_MAX];
    wchar_t path[PZ_PATH_MAX];
    wchar_t desktop[PZ_PATH_MAX];

    programs_shortcut_dir(programs, sizeof(programs) / sizeof(programs[0]));
    if (programs[0]) {
        wjoin(path, sizeof(path) / sizeof(path[0]), programs,
              L"PZ Mod Manager.lnk");
        DeleteFileW(path);
        wjoin(path, sizeof(path) / sizeof(path[0]), programs,
              L"Uninstall PZ Mod Manager.lnk");
        DeleteFileW(path);
        RemoveDirectoryW(programs);
    }
    if (known_folder(&FOLDERID_Desktop, desktop,
                     sizeof(desktop) / sizeof(desktop[0]))) {
        wjoin(path, sizeof(path) / sizeof(path[0]), desktop,
              L"PZ Mod Manager.lnk");
        DeleteFileW(path);
    }
}

static void create_shortcuts(const wchar_t* dir, int start_menu, int desktop,
                             wchar_t* warning, size_t cap) {
    wchar_t exe[PZ_PATH_MAX];
    wchar_t uninstaller[PZ_PATH_MAX];
    wchar_t link[PZ_PATH_MAX];
    wchar_t error[PZ_ERR_MAX];

    wjoin(exe, sizeof(exe) / sizeof(exe[0]), dir, L"pzmodmgr.exe");
    wjoin(uninstaller, sizeof(uninstaller) / sizeof(uninstaller[0]), dir,
          L"uninstall.exe");

    if (start_menu) {
        wchar_t menu_dir[PZ_PATH_MAX];
        programs_shortcut_dir(menu_dir, sizeof(menu_dir) / sizeof(menu_dir[0]));
        error[0] = 0;
        wjoin(link, sizeof(link) / sizeof(link[0]), menu_dir,
              L"PZ Mod Manager.lnk");
        if (!ensure_dir(menu_dir, error, sizeof(error) / sizeof(error[0])) ||
            !create_shortcut(link, exe, L"",
                             L"Project Zomboid Workshop mod manager")) {
            wcat(warning, cap, L"Could not create the Start Menu shortcut.\r\n");
        }
        wjoin(link, sizeof(link) / sizeof(link[0]), menu_dir,
              L"Uninstall PZ Mod Manager.lnk");
        if (!create_shortcut(link, uninstaller, L"--uninstall",
                             L"Uninstall PZ Mod Manager")) {
            wcat(warning, cap, L"Could not create the uninstall shortcut.\r\n");
        }
    }

    if (desktop) {
        wchar_t desktop_dir[PZ_PATH_MAX];
        if (!known_folder(&FOLDERID_Desktop, desktop_dir,
                          sizeof(desktop_dir) / sizeof(desktop_dir[0]))) {
            wcat(warning, cap, L"Could not create the desktop shortcut.\r\n");
        } else {
            wjoin(link, sizeof(link) / sizeof(link[0]), desktop_dir,
                  L"PZ Mod Manager.lnk");
            if (!create_shortcut(link, exe, L"",
                                 L"Project Zomboid Workshop mod manager"))
                wcat(warning, cap,
                     L"Could not create the desktop shortcut.\r\n");
        }
    }
}

/* ------------------------------------------------------------ native host */

static int native_host_json(const wchar_t* host, int chrome, CharBuf* out) {
    char* path;
    int ok;

    if (!out) return 0;
    path = to_utf8_alloc(host);
    if (!path) return 0;
    ok = char_buf_cstr(out, "{\n  \"name\": \"pzmodmgr\",\n"
                            "  \"description\": \"Project Zomboid Mod Manager "
                            "native host\",\n  \"path\": \"") &&
         json_escape(out, path) &&
         char_buf_cstr(out, "\",\n  \"type\": \"stdio\",\n");
    free(path);
    if (!ok) return 0;
    if (chrome)
        return char_buf_cstr(out,
                             "  \"allowed_origins\": "
                             "[\"chrome-extension://medndgoffhfpiabahjpdkfihnhdcmhoo/"
                             "\"]\n}\n");
    return char_buf_cstr(out,
                         "  \"allowed_extensions\": [\"pzmodmgr@local\"]\n}\n");
}

static int register_browser_host(const wchar_t* dir, int chrome,
                                 wchar_t* warning, size_t cap) {
    wchar_t folder[64];
    wchar_t manifest_dir[PZ_PATH_MAX];
    wchar_t host[PZ_PATH_MAX];
    wchar_t manifest_path[PZ_PATH_MAX];
    wchar_t error[PZ_ERR_MAX];
    CharBuf manifest;
    size_t i;
    int ok;

    wcopy(folder, sizeof(folder) / sizeof(folder[0]),
          chrome ? L"chrome-native-host" : L"firefox-native-host");
    wjoin(manifest_dir, sizeof(manifest_dir) / sizeof(manifest_dir[0]), dir,
          folder);
    error[0] = 0;
    if (!ensure_dir(manifest_dir, error, sizeof(error) / sizeof(error[0]))) {
        wcat(warning, cap, error);
        wcat(warning, cap, L"\r\n");
        return 0;
    }

    wjoin(host, sizeof(host) / sizeof(host[0]), dir,
          L"pzmodmgr_native_host.exe");
    memset(&manifest, 0, sizeof manifest);
    if (!native_host_json(host, chrome, &manifest)) {
        char_buf_free(&manifest);
        return 0;
    }
    wjoin(manifest_path, sizeof(manifest_path) / sizeof(manifest_path[0]),
          manifest_dir, L"pzmodmgr.json");
    ok = write_utf8_file(manifest_path, manifest.data, manifest.length, error,
                         sizeof(error) / sizeof(error[0]));
    char_buf_free(&manifest);
    if (!ok) {
        wcat(warning, cap, error);
        wcat(warning, cap, L"\r\n");
        return 0;
    }

    if (chrome) {
        int any = 0;
        for (i = 0; i < sizeof(g_chromium_browsers) / sizeof(g_chromium_browsers[0]);
             ++i) {
            ChromiumBrowser* browser = &g_chromium_browsers[i];
            if (!browser->installed) continue;
            any = 1;
            if (!set_reg_sz(HKEY_CURRENT_USER, browser->host_key, NULL,
                            manifest_path)) {
                wcat(warning, cap, L"Could not register the ");
                wcat(warning, cap, browser->name);
                wcat(warning, cap, L" native host.\r\n");
            }
        }
        return any;
    }
    if (!set_reg_sz(HKEY_CURRENT_USER, kFirefoxHostKey, NULL, manifest_path)) {
        wcat(warning, cap,
             L"Could not register the Firefox native host in the registry.\r\n");
        return 0;
    }
    return 1;
}

/* -------------------------------------------------------------- uninstall */

static int write_uninstall_registry(const wchar_t* dir,
                                    const wchar_t* uninstaller,
                                    DWORD estimated_kb, wchar_t* warning,
                                    size_t cap) {
    wchar_t exe[PZ_PATH_MAX];
    wchar_t display_icon[PZ_PATH_MAX + 8];
    wchar_t uninstall_cmd[PZ_PATH_MAX + 32];
    wchar_t quiet_cmd[PZ_PATH_MAX + 40];
    int ok = 1;

    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    wjoin(exe, sizeof(exe) / sizeof(exe[0]), dir, L"pzmodmgr.exe");
    swprintf(uninstall_cmd, sizeof(uninstall_cmd) / sizeof(uninstall_cmd[0]),
             L"\"%ls\" --uninstall", uninstaller);
    swprintf(quiet_cmd, sizeof(quiet_cmd) / sizeof(quiet_cmd[0]), L"%ls /S",
             uninstall_cmd);
    swprintf(display_icon, sizeof(display_icon) / sizeof(display_icon[0]),
             L"%ls,0", exe);

    ok = set_reg_sz(HKEY_CURRENT_USER, kUninstallKey, L"DisplayName", kAppName) && ok;
    ok = set_reg_sz(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion",
                    kVersion) &&
         ok;
    ok = set_reg_sz(HKEY_CURRENT_USER, kUninstallKey, L"Publisher",
                    L"pzmodmgr contributors") &&
         ok;
    ok = set_reg_sz(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", dir) &&
         ok;
    ok = set_reg_sz(HKEY_CURRENT_USER, kUninstallKey, L"DisplayIcon",
                    display_icon) &&
         ok;
    ok = set_reg_sz(HKEY_CURRENT_USER, kUninstallKey, L"UninstallString",
                    uninstall_cmd) &&
         ok;
    ok = set_reg_sz(HKEY_CURRENT_USER, kUninstallKey, L"QuietUninstallString",
                    quiet_cmd) &&
         ok;
    ok = set_reg_dword(HKEY_CURRENT_USER, kUninstallKey, L"NoModify", 1) && ok;
    ok = set_reg_dword(HKEY_CURRENT_USER, kUninstallKey, L"NoRepair", 1) && ok;
    ok = set_reg_dword(HKEY_CURRENT_USER, kUninstallKey, L"EstimatedSize",
                       estimated_kb) &&
         ok;
    if (!ok)
        wcat(warning, cap,
             L"Could not write the uninstall entry in Apps & Features.\r\n");
    return ok;
}

static int safe_install_dir(const wchar_t* dir, wchar_t* reason, size_t cap) {
    static const KNOWNFOLDERID* const protected_ids[] = {
        &FOLDERID_Profile,       &FOLDERID_Desktop,
        &FOLDERID_Documents,     &FOLDERID_Downloads,
        &FOLDERID_LocalAppData,  &FOLDERID_RoamingAppData,
        &FOLDERID_ProgramFiles,  &FOLDERID_ProgramFilesX86,
        &FOLDERID_Windows,
    };
    wchar_t profile[PZ_PATH_MAX];
    wchar_t zomboid[PZ_PATH_MAX];
    wchar_t protected_dir[PZ_PATH_MAX];
    size_t i;

    if (wcslen(dir) <= 3 || PathIsRootW(dir)) {
        wcopy(reason, cap, L"Choose a folder, not a drive root.");
        return 0;
    }

    profile[0] = 0;
    if (known_folder(&FOLDERID_Profile, profile,
                     sizeof(profile) / sizeof(profile[0]))) {
        wjoin(zomboid, sizeof(zomboid) / sizeof(zomboid[0]), profile,
              L"Zomboid");
        if (path_is_under(zomboid, dir) || path_is_under(dir, zomboid)) {
            wcopy(reason, cap,
                  L"Do not install into your Zomboid data folder. Mods and "
                  L"settings live there.");
            return 0;
        }
    }

    for (i = 0; i < sizeof(protected_ids) / sizeof(protected_ids[0]); ++i) {
        if (!known_folder(protected_ids[i], protected_dir,
                          sizeof(protected_dir) / sizeof(protected_dir[0])))
            continue;
        if (path_is_under(protected_dir, dir)) {
            wcopy(reason, cap,
                  L"Choose a dedicated application folder, not a system or "
                  L"personal folder.");
            return 0;
        }
    }

    if (known_folder(&FOLDERID_Windows, protected_dir,
                     sizeof(protected_dir) / sizeof(protected_dir[0])) &&
        path_is_under(dir, protected_dir)) {
        wcopy(reason, cap, L"Do not install into the Windows folder.");
        return 0;
    }
    return 1;
}

static int safe_uninstall_dir(const wchar_t* dir) {
    wchar_t profile[PZ_PATH_MAX];
    wchar_t zomboid[PZ_PATH_MAX];
    wchar_t exe[PZ_PATH_MAX];
    wchar_t uninstaller[PZ_PATH_MAX];

    if (wcslen(dir) <= 3 || PathIsRootW(dir)) return 0;
    if (known_folder(&FOLDERID_Profile, profile,
                     sizeof(profile) / sizeof(profile[0]))) {
        wjoin(zomboid, sizeof(zomboid) / sizeof(zomboid[0]), profile,
              L"Zomboid");
        if (path_is_under(dir, zomboid) || path_is_under(zomboid, dir))
            return 0;
    }
    wjoin(exe, sizeof(exe) / sizeof(exe[0]), dir, L"pzmodmgr.exe");
    wjoin(uninstaller, sizeof(uninstaller) / sizeof(uninstaller[0]), dir,
          L"uninstall.exe");
    return file_exists(exe) && file_exists(uninstaller);
}

static int app_path_exists(const wchar_t* exe) {
    const HKEY roots[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};
    wchar_t sub[PZ_PATH_MAX];
    size_t i;

    for (i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i) {
        HKEY key = NULL;
        wchar_t value[PZ_PATH_MAX];
        DWORD bytes = sizeof(value);
        DWORD type = 0;
        LONG rc;
        swprintf(sub, sizeof(sub) / sizeof(sub[0]),
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%ls",
                 exe);
        if (RegOpenKeyExW(roots[i], sub, 0, KEY_READ, &key) != ERROR_SUCCESS)
            continue;
        rc = RegQueryValueExW(key, NULL, NULL, &type, (BYTE*)value, &bytes);
        RegCloseKey(key);
        value[sizeof(value) / sizeof(value[0]) - 1] = 0;
        if (rc == ERROR_SUCCESS && type == REG_SZ && file_exists(value))
            return 1;
    }
    return 0;
}

static int installed_in_program_dirs(const wchar_t* relative) {
    static const int folders[] = {CSIDL_PROGRAM_FILES, CSIDL_PROGRAM_FILESX86,
                                  CSIDL_LOCAL_APPDATA};
    wchar_t root[PZ_PATH_MAX];
    wchar_t candidate[PZ_PATH_MAX];
    size_t i;

    for (i = 0; i < sizeof(folders) / sizeof(folders[0]); ++i) {
        if (FAILED(SHGetFolderPathW(NULL, folders[i], NULL, 0, root))) continue;
        wjoin(candidate, sizeof(candidate) / sizeof(candidate[0]), root,
              relative);
        if (file_exists(candidate)) return 1;
    }
    return 0;
}

static void detect_browsers(void) {
    size_t i;

    g_firefox_installed =
        app_path_exists(L"firefox.exe") ||
        installed_in_program_dirs(L"Mozilla Firefox\\firefox.exe");

    g_chromium_installed = 0;
    g_chromium_label[0] = 0;
    for (i = 0; i < sizeof(g_chromium_browsers) / sizeof(g_chromium_browsers[0]);
         ++i) {
        ChromiumBrowser* browser = &g_chromium_browsers[i];
        browser->installed = app_path_exists(browser->app_path) ||
                             installed_in_program_dirs(browser->exe);
        if (!browser->installed) continue;
        g_chromium_installed = 1;
        if (g_chromium_label[0]) wcat(g_chromium_label, 512, L", ");
        wcat(g_chromium_label, 512, browser->name);
    }
}

static int remove_tree(const wchar_t* dir) {
    WIN32_FIND_DATAW data;
    wchar_t pattern[PZ_PATH_MAX];
    wchar_t child[PZ_PATH_MAX];
    HANDLE found;

    if (wcslen(dir) <= 3 || PathIsRootW(dir)) return 0;
    wjoin(pattern, sizeof(pattern) / sizeof(pattern[0]), dir, L"*");
    memset(&data, 0, sizeof data);
    found = FindFirstFileW(pattern, &data);
    if (found != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(data.cFileName, L".") == 0 ||
                wcscmp(data.cFileName, L"..") == 0)
                continue;
            wjoin(child, sizeof(child) / sizeof(child[0]), dir, data.cFileName);
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

static int app_is_running(void) {
    return FindWindowW(L"PZModMgrWnd", NULL) != NULL;
}

static void default_zomboid_dir(wchar_t* out, size_t cap) {
    wchar_t profile[PZ_PATH_MAX];
    out[0] = 0;
    if (known_folder(&FOLDERID_Profile, profile,
                     sizeof(profile) / sizeof(profile[0])))
        wjoin(out, cap, profile, L"Zomboid");
}

static void seed_install_config(const wchar_t* install_dir,
                                const wchar_t* requested, wchar_t* warning,
                                size_t cap) {
    wchar_t config[PZ_PATH_MAX];
    wchar_t profile[PZ_PATH_MAX];
    wchar_t legacy[PZ_PATH_MAX];
    wchar_t zomboid[PZ_PATH_MAX];
    wchar_t error[PZ_ERR_MAX];
    wchar_t old_zomboid_w[PZ_PATH_MAX];
    char* text;
    char* steamcmd;
    char* user;
    char* old_zomboid;
    char* dark_mode;
    char* zomboid_utf8;
    CharBuf body;
    int ok;

    wjoin(config, sizeof(config) / sizeof(config[0]), install_dir,
          L"config.ini");
    profile[0] = 0;
    known_folder(&FOLDERID_Profile, profile,
                 sizeof(profile) / sizeof(profile[0]));
    legacy[0] = 0;
    if (profile[0]) {
        wchar_t zomboid_dir[PZ_PATH_MAX];
        wjoin(zomboid_dir, sizeof(zomboid_dir) / sizeof(zomboid_dir[0]),
              profile, L"Zomboid");
        wjoin(legacy, sizeof(legacy) / sizeof(legacy[0]), zomboid_dir,
              L"pzmodmgr\\config.ini");
    }

    text = NULL;
    if (file_exists(config))
        text = read_utf8_file(config, NULL);
    else if (legacy[0] && file_exists(legacy))
        text = read_utf8_file(legacy, NULL);

    steamcmd = config_value(text, "steamcmd_exe");
    user = config_value(text, "steam_user");
    old_zomboid = config_value(text, "zomboid_dir");
    dark_mode = config_value(text, "dark_mode");
    free(text);

    if (!dark_mode || !(strcmp(dark_mode, "1") == 0 ||
                        strcmp(dark_mode, "on") == 0 ||
                        strcmp(dark_mode, "true") == 0)) {
        free(dark_mode);
        dark_mode = (char*)malloc(2);
        if (dark_mode) {
            dark_mode[0] = '0';
            dark_mode[1] = 0;
        }
    }

    zomboid[0] = 0;
    if (requested && *requested)
        wcopy(zomboid, sizeof(zomboid) / sizeof(zomboid[0]), requested);
    else if (old_zomboid && *old_zomboid)
        from_utf8(old_zomboid, zomboid, sizeof(zomboid) / sizeof(zomboid[0]));
    if (!zomboid[0])
        default_zomboid_dir(zomboid, sizeof(zomboid) / sizeof(zomboid[0]));
    wtrim(zomboid);
    full_path(zomboid, old_zomboid_w, sizeof(old_zomboid_w) / sizeof(old_zomboid_w[0]));
    wcopy(zomboid, sizeof(zomboid) / sizeof(zomboid[0]), old_zomboid_w);
    if (!zomboid[0] || PathIsRootW(zomboid)) {
        wcat(warning, cap,
             L"Could not determine a usable Zomboid cache folder.\r\n");
        free(steamcmd);
        free(user);
        free(old_zomboid);
        free(dark_mode);
        return;
    }

    if (!user || !*user) {
        free(user);
        user = (char*)malloc(10);
        if (user) memcpy(user, "anonymous", 10);
    }

    zomboid_utf8 = to_utf8_alloc(zomboid);
    memset(&body, 0, sizeof body);
    ok = zomboid_utf8 && char_buf_cstr(&body, "steamcmd_exe=") &&
         char_buf_cstr(&body, steamcmd ? steamcmd : "") &&
         char_buf_cstr(&body, "\nsteam_user=") &&
         char_buf_cstr(&body, user ? user : "") &&
         char_buf_cstr(&body, "\nzomboid_dir=") && char_buf_cstr(&body, zomboid_utf8) &&
         char_buf_cstr(&body, "\ndark_mode=") &&
         char_buf_cstr(&body, dark_mode ? dark_mode : "0") &&
         char_buf_char(&body, '\n');
    if (!ok) {
        wcat(warning, cap, L"Could not build the installer config.\r\n");
    } else {
        error[0] = 0;
        if (!write_utf8_file(config, body.data, body.length, error,
                             sizeof(error) / sizeof(error[0]))) {
            wcat(warning, cap, error);
            wcat(warning, cap, L"\r\n");
        }
    }
    char_buf_free(&body);
    free(zomboid_utf8);
    free(steamcmd);
    free(user);
    free(old_zomboid);
    free(dark_mode);
}

static int existing_install_dir(wchar_t* out, size_t cap) {
    wchar_t dir[PZ_PATH_MAX];
    wchar_t exe[PZ_PATH_MAX];
    wchar_t profile[PZ_PATH_MAX];
    wchar_t zomboid[PZ_PATH_MAX];

    out[0] = 0;
    if (!get_reg_sz(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", dir,
                    sizeof(dir) / sizeof(dir[0])) ||
        !dir[0] || wcslen(dir) <= 3 || PathIsRootW(dir))
        return 0;
    wjoin(exe, sizeof(exe) / sizeof(exe[0]), dir, L"pzmodmgr.exe");
    if (!file_exists(exe)) return 0;
    if (known_folder(&FOLDERID_Profile, profile,
                     sizeof(profile) / sizeof(profile[0]))) {
        wjoin(zomboid, sizeof(zomboid) / sizeof(zomboid[0]), profile,
              L"Zomboid");
        if (path_is_under(dir, zomboid)) return 0;
    }
    wcopy(out, cap, dir);
    return 1;
}

static void default_install_dir(wchar_t* out, size_t cap) {
    wchar_t local[PZ_PATH_MAX];
    wchar_t programs[PZ_PATH_MAX];
    if (existing_install_dir(out, cap)) return;
    out[0] = 0;
    if (!known_folder(&FOLDERID_LocalAppData, local,
                      sizeof(local) / sizeof(local[0])))
        return;
    wjoin(programs, sizeof(programs) / sizeof(programs[0]), local, L"Programs");
    wjoin(out, cap, programs, L"PZModManager");
}

/* ------------------------------------------------------------------- UI */

static int S(int value) { return MulDiv(value, g_dpi, 96); }

static int checked(HWND box) {
    return SendMessageW(box, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void set_checked(HWND box, int value) {
    SendMessageW(box, BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
}

static HFONT make_font(int points, int weight) {
    return CreateFontW(-MulDiv(points, g_dpi, 72), 0, 0, 0, weight, FALSE, FALSE,
                       FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static void set_font(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
}

static void pump_messages(void) {
    MSG message;
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        if (!IsDialogMessageW(g_hwnd, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

static void set_status(const wchar_t* text) {
    if (g_status) SetWindowTextW(g_status, text);
}

static void show_error(const wchar_t* text) {
    MessageBoxW(g_hwnd, text, L"PZ Mod Manager Setup", MB_OK | MB_ICONERROR);
}

static int payload_wanted(PayloadNeed need, int firefox, int chromium) {
    if (need == kAlways) return 1;
    if (need == kFirefox) return firefox;
    if (need == kChromium) return chromium;
    return firefox || chromium;
}

static int extract_payload(const wchar_t* dir, int firefox, int chromium,
                           int silent, wchar_t* error, size_t cap) {
    DWORD completed = 0;
    size_t i;
    for (i = 0; i < sizeof(kPayload) / sizeof(kPayload[0]); ++i) {
        const PayloadFile* item = &kPayload[i];
        wchar_t dest[PZ_PATH_MAX];
        wchar_t parent[PZ_PATH_MAX];
        if (!payload_wanted(item->need, firefox, chromium)) continue;
        wjoin(dest, sizeof(dest) / sizeof(dest[0]), dir, item->relative);
        wdirname(parent, sizeof(parent) / sizeof(parent[0]), dest);
        if (!ensure_dir(parent, error, cap)) return 0;
        if (!extract_resource(item->id, dest, error, cap)) return 0;
        ++completed;
        if (!silent) {
            wchar_t line[PZ_PATH_MAX + 32];
            swprintf(line, sizeof(line) / sizeof(line[0]), L"Installing %ls...",
                     item->relative);
            set_status(line);
            SendMessageW(g_progress, PBM_SETPOS, completed, 0);
            UpdateWindow(g_hwnd);
            pump_messages();
        }
    }
    return 1;
}

static int perform_install(const wchar_t* raw_dir, int start_menu, int desktop,
                           int firefox, int chromium,
                           const wchar_t* zomboid_dir, int silent,
                           wchar_t* warning, size_t cap) {
    wchar_t dir[PZ_PATH_MAX];
    wchar_t trimmed[PZ_PATH_MAX];
    wchar_t reason[PZ_ERR_MAX];
    wchar_t error[PZ_ERR_MAX];
    wchar_t uninstaller[PZ_PATH_MAX];
    DWORD needed = 0;
    DWORD estimated_kb = 0;
    size_t i;

    wcopy(trimmed, sizeof(trimmed) / sizeof(trimmed[0]), raw_dir);
    wtrim(trimmed);
    full_path(trimmed, dir, sizeof(dir) / sizeof(dir[0]));
    if (!safe_install_dir(dir, reason, sizeof(reason) / sizeof(reason[0]))) {
        wcopy(warning, cap, reason);
        return 0;
    }
    if (app_is_running()) {
        wcopy(warning, cap,
              L"PZ Mod Manager is running. Close it, then run setup again.");
        return 0;
    }

    error[0] = 0;
    if (!ensure_dir(dir, error, sizeof(error) / sizeof(error[0]))) {
        wcopy(warning, cap, error);
        return 0;
    }

    for (i = 0; i < sizeof(kPayload) / sizeof(kPayload[0]); ++i)
        if (payload_wanted(kPayload[i].need, firefox, chromium)) ++needed;

    if (!silent) {
        SendMessageW(g_progress, PBM_SETRANGE32, 0, needed + 1);
        SendMessageW(g_progress, PBM_SETPOS, 0, 0);
    }
    if (!extract_payload(dir, firefox, chromium, silent, warning, cap)) return 0;

    wjoin(uninstaller, sizeof(uninstaller) / sizeof(uninstaller[0]), dir,
          L"uninstall.exe");
    if (firefox)
        register_browser_host(dir, 0, warning, cap);
    else
        RegDeleteTreeW(HKEY_CURRENT_USER, kFirefoxHostKey);
    if (chromium)
        register_browser_host(dir, 1, warning, cap);
    else {
        for (i = 0; i < sizeof(g_chromium_browsers) / sizeof(g_chromium_browsers[0]);
             ++i)
            RegDeleteTreeW(HKEY_CURRENT_USER, g_chromium_browsers[i].host_key);
    }

    create_shortcuts(dir, start_menu, desktop, warning, cap);

    for (i = 0; i < sizeof(kPayload) / sizeof(kPayload[0]); ++i) {
        HRSRC resource;
        if (!payload_wanted(kPayload[i].need, firefox, chromium)) continue;
        resource = FindResourceW(g_instance, MAKEINTRESOURCEW(kPayload[i].id),
                                 RT_RCDATA);
        if (resource)
            estimated_kb += (SizeofResource(g_instance, resource) + 1023) / 1024;
    }
    write_uninstall_registry(dir, uninstaller, estimated_kb, warning, cap);
    seed_install_config(dir, zomboid_dir, warning, cap);

    if (!silent) {
        SendMessageW(g_progress, PBM_SETPOS, needed + 1, 0);
        set_status(g_upgrade ? L"Updated successfully." : L"Installed successfully.");
        UpdateWindow(g_hwnd);
    }
    return 1;
}

static void launch_app(const wchar_t* dir) {
    wchar_t exe[PZ_PATH_MAX];
    wjoin(exe, sizeof(exe) / sizeof(exe[0]), dir, L"pzmodmgr.exe");
    ShellExecuteW(NULL, L"open", exe, NULL, dir, SW_SHOWNORMAL);
}

static int CALLBACK browse_callback(HWND hwnd, UINT message, LPARAM lparam,
                                    LPARAM data) {
    (void)lparam;
    if (message == BFFM_INITIALIZED && data)
        SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

static int browse_for_folder(HWND owner, const wchar_t* start,
                             wchar_t* selected, size_t cap) {
    BROWSEINFOW info;
    PIDLIST_ABSOLUTE item;
    wchar_t path[PZ_PATH_MAX];
    int ok;

    wcopy(g_browse_start, sizeof(g_browse_start) / sizeof(g_browse_start[0]),
          start);
    memset(&info, 0, sizeof info);
    info.hwndOwner = owner;
    info.lpszTitle = L"Select the folder where PZ Mod Manager should be installed";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    info.lpfn = browse_callback;
    info.lParam = (LPARAM)g_browse_start;
    item = SHBrowseForFolderW(&info);
    if (!item) return 0;
    ok = SHGetPathFromIDListW(item, path) != FALSE;
    CoTaskMemFree(item);
    if (ok) wcopy(selected, cap, path);
    return ok;
}

static void enable_controls(int enable) {
    EnableWindow(g_path, enable);
    EnableWindow(g_browse, enable);
    EnableWindow(g_startmenu, enable);
    EnableWindow(g_desktop, enable);
    EnableWindow(g_firefox, enable);
    EnableWindow(g_chrome, enable);
    EnableWindow(g_launch, enable);
    EnableWindow(g_install, enable);
    EnableWindow(g_cancel, enable);
}

static void begin_install(void) {
    wchar_t raw[PZ_PATH_MAX];
    wchar_t trimmed[PZ_PATH_MAX];
    wchar_t dir[PZ_PATH_MAX];
    wchar_t reason[PZ_ERR_MAX];
    wchar_t warning[PZ_WARN_MAX];
    int ok;

    if (g_installing) return;
    GetWindowTextW(g_path, raw, PZ_PATH_MAX);
    wcopy(trimmed, sizeof(trimmed) / sizeof(trimmed[0]), raw);
    wtrim(trimmed);
    full_path(trimmed, dir, sizeof(dir) / sizeof(dir[0]));
    if (!safe_install_dir(dir, reason, sizeof(reason) / sizeof(reason[0]))) {
        show_error(reason);
        SetFocus(g_path);
        return;
    }
    if (app_is_running()) {
        show_error(L"PZ Mod Manager is running. Close it, then run setup again.");
        return;
    }

    g_installing = 1;
    g_launch_after = checked(g_launch);
    enable_controls(0);
    SetWindowTextW(g_install, L"Installing...");
    set_status(L"Preparing...");
    SetCursor(LoadCursorW(NULL, IDC_WAIT));

    warning[0] = 0;
    ok = perform_install(dir, checked(g_startmenu), checked(g_desktop),
                         checked(g_firefox), checked(g_chrome), L"", 0, warning,
                         sizeof(warning) / sizeof(warning[0]));

    SetCursor(LoadCursorW(NULL, IDC_ARROW));
    g_installing = 0;

    if (!ok) {
        enable_controls(1);
        SetWindowTextW(g_install, g_upgrade ? L"Update" : L"Install");
        set_status(L"Installation failed.");
        show_error(warning);
        return;
    }

    g_installed = 1;
    SetWindowTextW(g_install, L"Finish");
    EnableWindow(g_install, TRUE);
    SetFocus(g_install);
    ShowWindow(g_cancel, SW_HIDE);
    if (warning[0]) {
        MessageBoxW(g_hwnd, warning, L"Installed with warnings",
                    MB_OK | MB_ICONWARNING);
    }
    set_status(g_upgrade ? L"Updated successfully. Select Finish to close."
                         : L"Installed successfully. Select Finish to close.");
}

static void paint_window(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    int header;
    TRIVERTEX vertices[2];
    GRADIENT_RECT gradient;
    RECT accent;
    HBRUSH orange;
    HFONT old;
    RECT title;
    RECT subtitle;
    RECT version;
    HPEN line;
    HPEN old_pen;

    GetClientRect(hwnd, &client);
    FillRect(dc, &client, g_white_brush);

    header = S(104);
    vertices[0].x = 0;
    vertices[0].y = 0;
    vertices[0].Red = 20 << 8;
    vertices[0].Green = 48 << 8;
    vertices[0].Blue = 35 << 8;
    vertices[0].Alpha = 0;
    vertices[1].x = (SHORT)client.right;
    vertices[1].y = (SHORT)header;
    vertices[1].Red = 52 << 8;
    vertices[1].Green = 116 << 8;
    vertices[1].Blue = 80 << 8;
    vertices[1].Alpha = 0;
    gradient.UpperLeft = 0;
    gradient.LowerRight = 1;
    GradientFill(dc, vertices, 2, &gradient, 1, GRADIENT_FILL_RECT_V);

    accent.left = 0;
    accent.top = header - S(4);
    accent.right = client.right;
    accent.bottom = header;
    orange = CreateSolidBrush(RGB(226, 132, 61));
    FillRect(dc, &accent, orange);
    DeleteObject(orange);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    old = (HFONT)SelectObject(dc, g_title_font);
    title.left = S(28);
    title.top = S(18);
    title.right = client.right - S(120);
    title.bottom = S(58);
    DrawTextW(dc, kAppName, -1, &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SelectObject(dc, g_sub_font);
    SetTextColor(dc, RGB(198, 224, 208));
    subtitle.left = S(30);
    subtitle.top = S(56);
    subtitle.right = client.right - S(120);
    subtitle.bottom = S(84);
    DrawTextW(dc, L"Project Zomboid Workshop installer", -1, &subtitle,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SetTextColor(dc, RGB(198, 224, 208));
    version.left = client.right - S(148);
    version.top = S(20);
    version.right = client.right - S(28);
    version.bottom = S(48);
    DrawTextW(dc, L"v" PZ_VERSION, -1, &version,
              DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, old);

    line = CreatePen(PS_SOLID, 1, RGB(226, 232, 228));
    old_pen = (HPEN)SelectObject(dc, line);
    MoveToEx(dc, S(24), S(424), NULL);
    LineTo(dc, client.right - S(24), S(424));
    SelectObject(dc, old_pen);
    DeleteObject(line);
    EndPaint(hwnd, &ps);
}

static void create_controls(HWND hwnd) {
    HDC dc;
    int dpi;
    HWND heading;
    HWND user_label;
    HWND dest_label;
    wchar_t user[256];
    DWORD user_len = 256;
    wchar_t user_line[320];
    wchar_t label[1024];
    HWND footer;
    wchar_t dest[PZ_PATH_MAX];

    g_dpi = 96;
    dc = GetDC(NULL);
    if (dc) {
        dpi = GetDeviceCaps(dc, LOGPIXELSX);
        if (dpi > 0) g_dpi = dpi;
        ReleaseDC(NULL, dc);
    }

    g_white_brush = CreateSolidBrush(RGB(255, 255, 255));
    g_title_font = make_font(22, FW_BOLD);
    g_sub_font = make_font(10, FW_NORMAL);
    g_heading_font = make_font(15, FW_SEMIBOLD);
    g_body_font = make_font(9, FW_NORMAL);
    g_small_font = make_font(8, FW_NORMAL);

    g_upgrade = existing_install_dir(dest, sizeof(dest) / sizeof(dest[0]));
    heading = CreateWindowW(L"STATIC",
                            g_upgrade ? L"Update PZ Mod Manager"
                                      : L"Install PZ Mod Manager",
                            WS_CHILD | WS_VISIBLE | SS_LEFT, S(24), S(116),
                            S(600), S(30), hwnd, NULL, g_instance, NULL);
    set_font(heading, g_heading_font);

    if (!GetUserNameW(user, &user_len)) wcopy(user, 256, L"this user");
    wcopy(user_line, sizeof(user_line) / sizeof(user_line[0]), L"for ");
    wcat(user_line, sizeof(user_line) / sizeof(user_line[0]), user);
    user_label = CreateWindowW(L"STATIC", user_line, WS_CHILD | WS_VISIBLE,
                               S(24), S(146), S(600), S(20), hwnd, NULL,
                               g_instance, NULL);
    set_font(user_label, g_body_font);

    dest_label = CreateWindowW(L"STATIC", L"Destination folder",
                               WS_CHILD | WS_VISIBLE, S(24), S(178), S(300),
                               S(18), hwnd, NULL, g_instance, NULL);
    set_font(dest_label, g_body_font);

    g_path = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                             S(24), S(198), S(464), S(28), hwnd,
                             (HMENU)(INT_PTR)IDC_PATH, g_instance, NULL);
    set_font(g_path, g_body_font);
    SendMessageW(g_path, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(S(6), S(6)));

    g_browse = CreateWindowW(L"BUTTON", L"Browse...",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(498),
                             S(198), S(138), S(28), hwnd,
                             (HMENU)(INT_PTR)IDC_BROWSE, g_instance, NULL);
    set_font(g_browse, g_body_font);

    g_startmenu = CreateWindowW(L"BUTTON", L"Create a Start Menu shortcut",
                                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX |
                                    WS_TABSTOP,
                                S(26), S(242), S(600), S(22), hwnd,
                                (HMENU)(INT_PTR)IDC_STARTMENU, g_instance, NULL);
    g_desktop = CreateWindowW(L"BUTTON", L"Create a desktop shortcut",
                              WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX |
                                  WS_TABSTOP,
                              S(26), S(268), S(600), S(22), hwnd,
                              (HMENU)(INT_PTR)IDC_DESKTOP, g_instance, NULL);
    g_firefox = CreateWindowW(L"BUTTON",
                              L"Register the Firefox right-click add-on host",
                              WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX |
                                  WS_TABSTOP,
                              S(26), S(294), S(600), S(22), hwnd,
                              (HMENU)(INT_PTR)IDC_FIREFOX, g_instance, NULL);
    g_chrome = CreateWindowW(L"BUTTON",
                             L"Register the Chrome right-click add-on host",
                             WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX |
                                 WS_TABSTOP,
                             S(26), S(320), S(600), S(22), hwnd,
                             (HMENU)(INT_PTR)IDC_CHROME, g_instance, NULL);
    detect_browsers();
    if (!g_firefox_installed) ShowWindow(g_firefox, SW_HIDE);
    if (!g_chromium_installed) ShowWindow(g_chrome, SW_HIDE);
    if (g_chromium_installed) {
        wcopy(label, sizeof(label) / sizeof(label[0]), L"Register the ");
        wcat(label, sizeof(label) / sizeof(label[0]), g_chromium_label);
        wcat(label, sizeof(label) / sizeof(label[0]),
             L" right-click add-on host");
        SetWindowTextW(g_chrome, label);
    }
    g_launch = CreateWindowW(L"BUTTON", L"Launch PZ Mod Manager",
                             WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX |
                                 WS_TABSTOP,
                             S(26), S(346), S(600), S(22), hwnd,
                             (HMENU)(INT_PTR)IDC_LAUNCH, g_instance, NULL);
    set_font(g_startmenu, g_body_font);
    set_font(g_desktop, g_body_font);
    set_font(g_firefox, g_body_font);
    set_font(g_chrome, g_body_font);
    set_font(g_launch, g_body_font);

    g_progress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
                                 WS_CHILD | WS_VISIBLE, S(24), S(380), S(612),
                                 S(10), hwnd, NULL, g_instance, NULL);
    SendMessageW(g_progress, PBM_SETRANGE32, 0, kPayloadCount + 1);

    g_status = CreateWindowW(L"STATIC",
                             g_upgrade
                                 ? L"Ready to update. Existing mods and settings are kept."
                                 : L"Ready to install.",
                             WS_CHILD | WS_VISIBLE, S(24), S(398), S(612),
                             S(20), hwnd, NULL, g_instance, NULL);
    set_font(g_status, g_small_font);

    g_install = CreateWindowW(L"BUTTON", g_upgrade ? L"Update" : L"Install",
                              WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON |
                                  WS_TABSTOP,
                              S(450), S(432), S(90), S(30), hwnd,
                              (HMENU)(INT_PTR)IDC_INSTALL, g_instance, NULL);
    g_cancel = CreateWindowW(L"BUTTON", L"Cancel",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(548),
                             S(432), S(90), S(30), hwnd,
                             (HMENU)(INT_PTR)IDCANCEL, g_instance, NULL);
    set_font(g_install, g_body_font);
    set_font(g_cancel, g_body_font);

    footer = CreateWindowW(
        L"STATIC",
        L"Installs for the current user. Your mods stay in %USERPROFILE%\\Zomboid\\mods.",
        WS_CHILD | WS_VISIBLE, S(24), S(470), S(612), S(18), hwnd, NULL,
        g_instance, NULL);
    set_font(footer, g_small_font);

    default_install_dir(dest, sizeof(dest) / sizeof(dest[0]));
    SetWindowTextW(g_path, dest);
    set_checked(g_startmenu, 1);
    if (g_firefox_installed) set_checked(g_firefox, 1);
    if (g_chromium_installed) set_checked(g_chrome, 1);
    set_checked(g_launch, 1);
    SetFocus(g_install);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam) {
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
        HDC dc = (HDC)wparam;
        SetBkColor(dc, RGB(255, 255, 255));
        SetTextColor(dc, RGB(52, 58, 54));
        return (LRESULT)g_white_brush;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_INSTALL:
            if (g_installed) {
                if (g_launch_after) {
                    wchar_t raw[PZ_PATH_MAX];
                    wchar_t dir[PZ_PATH_MAX];
                    GetWindowTextW(g_path, raw, PZ_PATH_MAX);
                    wtrim(raw);
                    full_path(raw, dir, sizeof(dir) / sizeof(dir[0]));
                    launch_app(dir);
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
            wchar_t raw[PZ_PATH_MAX];
            wchar_t selected[PZ_PATH_MAX];
            if (g_installing) return 0;
            GetWindowTextW(g_path, raw, PZ_PATH_MAX);
            if (browse_for_folder(hwnd, raw, selected,
                                  sizeof(selected) / sizeof(selected[0])))
                SetWindowTextW(g_path, selected);
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

static int run_uninstall(int silent) {
    wchar_t self[PZ_PATH_MAX];
    wchar_t dir[PZ_PATH_MAX];
    wchar_t temp_dir[MAX_PATH];
    wchar_t temp[MAX_PATH];
    DWORD temp_len;
    int moved = 0;
    int removed;
    int answer;

    if (!module_path(NULL, self, sizeof(self) / sizeof(self[0]))) return 1;
    wdirname(dir, sizeof(dir) / sizeof(dir[0]), self);
    if (!safe_uninstall_dir(dir)) {
        if (!silent)
            MessageBoxW(NULL,
                        L"The uninstaller is not running from a valid PZ Mod Manager folder.",
                        L"Uninstall PZ Mod Manager", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (app_is_running()) {
        if (!silent)
            MessageBoxW(NULL, L"Close PZ Mod Manager before uninstalling it.",
                        L"Uninstall PZ Mod Manager", MB_OK | MB_ICONWARNING);
        return 1;
    }
    if (!silent) {
        answer = MessageBoxW(
            NULL,
            L"Remove PZ Mod Manager from this user account?\r\n\r\n"
            L"Your mods and settings in %USERPROFILE%\\Zomboid are kept.",
            L"Uninstall PZ Mod Manager", MB_YESNO | MB_ICONQUESTION);
        if (answer != IDYES) return 0;
    }

    remove_shortcuts();
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, kFirefoxHostKey);
    {
        size_t i;
        for (i = 0; i < sizeof(g_chromium_browsers) / sizeof(g_chromium_browsers[0]);
             ++i)
            RegDeleteTreeW(HKEY_CURRENT_USER, g_chromium_browsers[i].host_key);
    }

    temp_dir[0] = 0;
    temp[0] = 0;
    temp_len = GetTempPathW(MAX_PATH, temp_dir);
    if (temp_len > 0 && temp_len < MAX_PATH) {
        swprintf(temp, MAX_PATH, L"%lspzmodmgr-uninstall-%lu.exe", temp_dir,
                 (unsigned long)GetCurrentProcessId());
        moved = MoveFileExW(self, temp, MOVEFILE_REPLACE_EXISTING) != 0;
    }

    removed = remove_tree(dir);
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

static int run_gui(void) {
    HICON icon;
    WNDCLASSEXW wc;
    RECT work;
    DWORD style;
    int width;
    int height;
    int x;
    int y;
    HWND hwnd;
    MSG message;

    icon = (HICON)LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_SETUP),
                             IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = g_instance;
    wc.hIcon = icon;
    wc.hIconSm = icon;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"PZModMgrSetupWnd";
    RegisterClassExW(&wc);

    work.left = 0;
    work.top = 0;
    work.right = S(660);
    work.bottom = S(496);
    style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&work, style, FALSE, 0);
    width = work.right - work.left;
    height = work.bottom - work.top;
    x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    hwnd = CreateWindowExW(0, wc.lpszClassName, L"PZ Mod Manager Setup", style,
                           x, y, width, height, NULL, NULL, g_instance, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return (int)message.wParam;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line,
                    int show) {
    INITCOMMONCONTROLSEX controls;
    HRESULT com;
    int argc = 0;
    LPWSTR* argv;
    int uninstall = 0;
    int silent = 0;
    wchar_t destination[PZ_PATH_MAX];
    wchar_t zomboid[PZ_PATH_MAX];
    wchar_t self[PZ_PATH_MAX];
    int result = 0;
    int i;

    (void)previous;
    (void)command_line;
    (void)show;
    g_instance = instance;
    SetProcessDPIAware();

    memset(&controls, 0, sizeof controls);
    controls.dwSize = sizeof controls;
    controls.dwICC =
        ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&controls);

    com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    destination[0] = 0;
    zomboid[0] = 0;
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    module_path(NULL, self, sizeof(self) / sizeof(self[0]));
    if (_wcsicmp(wbasename(self), L"uninstall.exe") == 0) uninstall = 1;

    if (argv) {
        for (i = 1; i < argc; ++i) {
            const wchar_t* arg = argv[i];
            if (_wcsicmp(arg, L"--uninstall") == 0 ||
                _wcsicmp(arg, L"/uninstall") == 0) {
                uninstall = 1;
            } else if (_wcsicmp(arg, L"/S") == 0 ||
                       _wcsicmp(arg, L"--silent") == 0) {
                silent = 1;
            } else if (_wcsnicmp(arg, L"/D=", 3) == 0) {
                wcopy(destination, sizeof(destination) / sizeof(destination[0]),
                      arg + 3);
                wtrim(destination);
            } else if (_wcsnicmp(arg, L"/ZOMBOIDDIR=", 12) == 0) {
                wcopy(zomboid, sizeof(zomboid) / sizeof(zomboid[0]), arg + 12);
                wtrim(zomboid);
            }
        }
        LocalFree(argv);
    }

    if (uninstall) {
        result = run_uninstall(silent);
    } else if (silent) {
        wchar_t dir[PZ_PATH_MAX];
        wchar_t warning[PZ_WARN_MAX];
        if (!destination[0])
            default_install_dir(dir, sizeof(dir) / sizeof(dir[0]));
        else
            wcopy(dir, sizeof(dir) / sizeof(dir[0]), destination);
        warning[0] = 0;
        result = perform_install(dir, 1, 0, 1, 1, zomboid, 1, warning,
                                 sizeof(warning) / sizeof(warning[0]))
                     ? 0
                     : 1;
    } else {
        result = run_gui();
    }

    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
