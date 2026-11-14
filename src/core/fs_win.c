#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "core/fs_c_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

struct pz_fs_file {
    FILE* stream;
    pz_fs_code error;
};

static pz_fs_code code_for_win32(DWORD value) {
    switch (value) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
        return PZ_FS_NOT_FOUND;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return PZ_FS_EXISTS;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return PZ_FS_ACCESS;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return PZ_FS_BUSY;
    case ERROR_NOT_SAME_DEVICE:
        return PZ_FS_CROSS_DEVICE;
    case ERROR_DIR_NOT_EMPTY:
        return PZ_FS_NOT_EMPTY;
    case ERROR_INVALID_NAME:
    case ERROR_CANT_ACCESS_FILE:
    case ERROR_INVALID_PARAMETER:
        return PZ_FS_INVALID;
    case ERROR_FILENAME_EXCED_RANGE:
        return PZ_FS_TOO_LONG;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return PZ_FS_NO_SPACE;
    case ERROR_CRC:
    case ERROR_READ_FAULT:
    case ERROR_WRITE_FAULT:
        return PZ_FS_IO;
    default:
        return PZ_FS_SYSTEM;
    }
}

static void fail_win32(pz_fs_error* error, DWORD value) {
    pz_fs_error_set(error, code_for_win32(value), (int)value);
}

static pz_fs_code code_for_errno(int value) {
    switch (value) {
    case ENOENT:
    case ENOTDIR:
        return PZ_FS_NOT_FOUND;
    case EEXIST:
        return PZ_FS_EXISTS;
    case EACCES:
    case EPERM:
        return PZ_FS_ACCESS;
    case EINVAL:
        return PZ_FS_INVALID;
    case ENAMETOOLONG:
        return PZ_FS_TOO_LONG;
    case ENOSPC:
        return PZ_FS_NO_SPACE;
    case EIO:
        return PZ_FS_IO;
    default:
        return PZ_FS_SYSTEM;
    }
}

static int convert_utf8(const char* text, size_t length, wchar_t** out,
                        pz_fs_error* error) {
    wchar_t* buffer;
    int needed;

    if (length > (size_t)INT_MAX) {
        pz_fs_error_set(error, PZ_FS_TOO_LONG, 0);
        return 0;
    }
    needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)length,
                                 NULL, 0);
    if (needed <= 0) {
        pz_fs_error_set(error, PZ_FS_INVALID, (int)GetLastError());
        return 0;
    }
    buffer = (wchar_t*)malloc(((size_t)needed + 1) * sizeof(wchar_t));
    if (!buffer) {
        pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
        return 0;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)length,
                            buffer, needed) != needed) {
        free(buffer);
        pz_fs_error_set(error, PZ_FS_INVALID, (int)GetLastError());
        return 0;
    }
    buffer[needed] = L'\0';
    *out = buffer;
    return 1;
}

static wchar_t* copy_wide(const wchar_t* text) {
    const size_t length = wcslen(text);
    wchar_t* copy = (wchar_t*)malloc((length + 1) * sizeof(wchar_t));

    if (!copy) return NULL;
    memcpy(copy, text, (length + 1) * sizeof(wchar_t));
    return copy;
}

/* Absolute paths long enough for the legacy limit, and every UNC path, go
 * through the extended-length form. The API keeps the normal form. */
static wchar_t* extended_copy(const wchar_t* text) {
    const size_t length = wcslen(text);
    const int unc = length >= 2 && text[0] == L'\\' && text[1] == L'\\';
    const int drive = length >= 3 && text[1] == L':' &&
                      (text[2] == L'\\' || text[2] == L'/');

    if (!unc && !drive) return copy_wide(text);
    if (!unc && length < 248) return copy_wide(text);

    if (unc) {
        static const wchar_t prefix[] = L"\\\\?\\UNC\\";
        const size_t prefix_length = (sizeof prefix / sizeof prefix[0]) - 1;
        wchar_t* result = (wchar_t*)malloc((prefix_length + length) * sizeof(wchar_t));

        if (!result) return NULL;
        memcpy(result, prefix, prefix_length * sizeof(wchar_t));
        memcpy(result + prefix_length, text + 2, (length - 1) * sizeof(wchar_t));
        return result;
    }

    {
        static const wchar_t prefix[] = L"\\\\?\\";
        const size_t prefix_length = (sizeof prefix / sizeof prefix[0]) - 1;
        wchar_t* result = (wchar_t*)malloc((prefix_length + length + 1) * sizeof(wchar_t));

        if (!result) return NULL;
        memcpy(result, prefix, prefix_length * sizeof(wchar_t));
        memcpy(result + prefix_length, text, (length + 1) * sizeof(wchar_t));
        return result;
    }
}

static int make_wide(const pz_path* path, wchar_t** out, pz_fs_error* error) {
    wchar_t* plain = NULL;
    wchar_t* wide;

    if (!path || !path->text.data || path->text.length == 0) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (!convert_utf8(path->text.data, path->text.length, &plain, error)) return 0;
    wide = extended_copy(plain);
    free(plain);
    if (!wide) {
        pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
        return 0;
    }
    *out = wide;
    return 1;
}

int pz_fs_exists(const pz_path* path, int* exists, pz_fs_error* error) {
    wchar_t* wide = NULL;
    DWORD attributes;

    if (!exists) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (!make_wide(path, &wide, error)) return 0;
    attributes = GetFileAttributesW(wide);
    free(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD value = GetLastError();
        if (value == ERROR_FILE_NOT_FOUND || value == ERROR_PATH_NOT_FOUND ||
            value == ERROR_INVALID_NAME) {
            *exists = 0;
            pz_fs_error_clear(error);
            return 1;
        }
        fail_win32(error, value);
        return 0;
    }
    *exists = 1;
    return 1;
}

int pz_fs_kind_of(const pz_path* path, pz_fs_kind* kind, pz_fs_error* error) {
    wchar_t* wide = NULL;
    DWORD attributes;

    if (!kind) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (!make_wide(path, &wide, error)) return 0;
    attributes = GetFileAttributesW(wide);
    free(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        fail_win32(error, GetLastError());
        return 0;
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        *kind = PZ_FS_LINK;
    else if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        *kind = PZ_FS_DIR;
    else
        *kind = PZ_FS_FILE;
    return 1;
}

int pz_fs_file_size(const pz_path* path, uint64_t* size, pz_fs_error* error) {
    wchar_t* wide = NULL;
    WIN32_FILE_ATTRIBUTE_DATA data;

    if (!size) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (!make_wide(path, &wide, error)) return 0;
    if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &data)) {
        const DWORD value = GetLastError();
        free(wide);
        fail_win32(error, value);
        return 0;
    }
    free(wide);
    *size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    return 1;
}

int pz_fs_is_empty(const pz_path* path, int* empty, pz_fs_error* error) {
    wchar_t* wide = NULL;
    DWORD attributes;

    if (!empty) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (!make_wide(path, &wide, error)) return 0;
    attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD value = GetLastError();
        free(wide);
        fail_win32(error, value);
        return 0;
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        const size_t length = wcslen(wide);
        wchar_t* pattern = (wchar_t*)malloc((length + 3) * sizeof(wchar_t));
        WIN32_FIND_DATAW found;
        HANDLE handle;
        int any = 0;

        if (!pattern) {
            free(wide);
            pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
            return 0;
        }
        memcpy(pattern, wide, length * sizeof(wchar_t));
        pattern[length] = L'\\';
        pattern[length + 1] = L'*';
        pattern[length + 2] = L'\0';
        free(wide);
        handle = FindFirstFileW(pattern, &found);
        free(pattern);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD value = GetLastError();
            if (value == ERROR_FILE_NOT_FOUND) {
                *empty = 1;
                return 1;
            }
            fail_win32(error, value);
            return 0;
        }
        do {
            if (wcscmp(found.cFileName, L".") == 0 ||
                wcscmp(found.cFileName, L"..") == 0)
                continue;
            any = 1;
            break;
        } while (FindNextFileW(handle, &found));
        FindClose(handle);
        *empty = !any;
        return 1;
    }
    {
        WIN32_FILE_ATTRIBUTE_DATA data;

        if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &data)) {
            const DWORD value = GetLastError();
            free(wide);
            fail_win32(error, value);
            return 0;
        }
        free(wide);
        *empty = data.nFileSizeHigh == 0 && data.nFileSizeLow == 0;
        return 1;
    }
}

int pz_fs_is_executable(const pz_path* path, int* executable,
                        pz_fs_error* error) {
    wchar_t* wide = NULL;
    DWORD attributes;

    if (!executable) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (!make_wide(path, &wide, error)) return 0;
    attributes = GetFileAttributesW(wide);
    free(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        fail_win32(error, GetLastError());
        return 0;
    }
    *executable = (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    return 1;
}

int pz_fs_create_directories(const pz_path* path, pz_fs_error* error) {
    wchar_t* wide = NULL;
    size_t length;
    size_t i;
    size_t first = 0;
    int ok = 1;

    if (!path || !path->text.data || path->text.length == 0) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (!convert_utf8(path->text.data, path->text.length, &wide, error)) return 0;
    length = wcslen(wide);

    if (length >= 3 && wide[1] == L':' &&
        (wide[2] == L'\\' || wide[2] == L'/')) {
        first = 3;
    } else if (length >= 2 && wide[0] == L'\\' && wide[1] == L'\\') {
        /* A UNC path starts with two separators and already has its server and
         * share components; creating the partial prefixes would fail. */
        size_t k = 2;
        while (k < length && wide[k] != L'\\' && wide[k] != L'/') ++k;
        if (k < length) {
            ++k;
            while (k < length && wide[k] != L'\\' && wide[k] != L'/') ++k;
        }
        first = k;
    } else if (length >= 1 && (wide[0] == L'\\' || wide[0] == L'/')) {
        first = 1;
    }

    for (i = first; i <= length && ok; ++i) {
        const wchar_t saved = wide[i];
        DWORD attributes;

        if (i != length && wide[i] != L'\\' && wide[i] != L'/') continue;
        if (i == 2 && wide[1] == L':') continue;
        wide[i] = L'\0';
        if (!CreateDirectoryW(wide, NULL)) {
            const DWORD value = GetLastError();
            attributes = GetFileAttributesW(wide);
            if (value != ERROR_ALREADY_EXISTS ||
                attributes == INVALID_FILE_ATTRIBUTES ||
                !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                fail_win32(error, value);
                ok = 0;
            }
        }
        wide[i] = saved;
    }
    free(wide);
    return ok;
}

int pz_fs_remove(const pz_path* path, pz_fs_error* error) {
    wchar_t* wide = NULL;
    DWORD attributes;
    int ok;

    if (!make_wide(path, &wide, error)) return 0;
    attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD value = GetLastError();
        free(wide);
        fail_win32(error, value);
        return 0;
    }
    if (attributes & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(wide, attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY);

    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        ok = RemoveDirectoryW(wide) != 0;
    else
        ok = DeleteFileW(wide) != 0;

    if (!ok) {
        const DWORD value = GetLastError();
        free(wide);
        fail_win32(error, value);
        return 0;
    }
    free(wide);
    return 1;
}

int pz_fs_rename(const pz_path* from, const pz_path* to, pz_fs_error* error) {
    wchar_t* wide_from = NULL;
    wchar_t* wide_to = NULL;
    int ok;

    if (!make_wide(from, &wide_from, error)) return 0;
    if (!make_wide(to, &wide_to, error)) {
        free(wide_from);
        return 0;
    }
    ok = MoveFileW(wide_from, wide_to) != 0;
    if (!ok) {
        const DWORD value = GetLastError();
        free(wide_from);
        free(wide_to);
        fail_win32(error, value);
        return 0;
    }
    free(wide_from);
    free(wide_to);
    return 1;
}

int pz_fs_copy_file(const pz_path* from, const pz_path* to, unsigned flags,
                    pz_fs_error* error) {
    wchar_t* wide_from = NULL;
    wchar_t* wide_to = NULL;
    DWORD attributes;
    int ok;

    if (!make_wide(from, &wide_from, error)) return 0;
    if (!make_wide(to, &wide_to, error)) {
        free(wide_from);
        return 0;
    }
    attributes = GetFileAttributesW(wide_from);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD value = GetLastError();
        free(wide_from);
        free(wide_to);
        fail_win32(error, value);
        return 0;
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        free(wide_from);
        free(wide_to);
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    attributes = GetFileAttributesW(wide_to);
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        free(wide_from);
        free(wide_to);
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }

    ok = CopyFileW(wide_from, wide_to,
                   (flags & PZ_FS_COPY_OVERWRITE) ? FALSE : TRUE) != 0;
    if (!ok) {
        const DWORD value = GetLastError();
        free(wide_from);
        free(wide_to);
        fail_win32(error, value);
        return 0;
    }
    free(wide_from);
    free(wide_to);
    return 1;
}

int pz_fs_copy_link(const pz_path* from, const pz_path* to, unsigned flags,
                    pz_fs_error* error) {
    (void)from;
    (void)to;
    (void)flags;
    pz_fs_error_set(error, PZ_FS_INVALID, 0);
    return 0;
}

int pz_fs_replace_file(const pz_path* from, const pz_path* to,
                       pz_fs_error* error) {
    wchar_t* wide_from = NULL;
    wchar_t* wide_to = NULL;
    int ok;

    if (!make_wide(from, &wide_from, error)) return 0;
    if (!make_wide(to, &wide_to, error)) {
        free(wide_from);
        return 0;
    }
    ok = MoveFileExW(wide_from, wide_to,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) {
        const DWORD value = GetLastError();
        free(wide_from);
        free(wide_to);
        fail_win32(error, value);
        return 0;
    }
    free(wide_from);
    free(wide_to);
    return 1;
}

int pz_fs_set_executable(const pz_path* path, int enabled, pz_fs_error* error) {
    (void)path;
    (void)enabled;
    pz_fs_error_clear(error);
    return 1;
}

int pz_fs_list_dir(const pz_path* dir, pz_vec* out, pz_fs_error* error) {
    wchar_t* wide = NULL;
    size_t length;
    wchar_t* pattern;
    WIN32_FIND_DATAW found;
    HANDLE handle;
    pz_vec list;

    if (!out) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (!make_wide(dir, &wide, error)) return 0;
    length = wcslen(wide);
    pattern = (wchar_t*)malloc((length + 4) * sizeof(wchar_t));
    if (!pattern) {
        free(wide);
        pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
        return 0;
    }
    memcpy(pattern, wide, length * sizeof(wchar_t));
    if (length != 0 && (wide[length - 1] == L'\\' || wide[length - 1] == L'/')) {
        pattern[length] = L'*';
        pattern[length + 1] = L'\0';
    } else {
        pattern[length] = L'\\';
        pattern[length + 1] = L'*';
        pattern[length + 2] = L'\0';
    }
    free(wide);

    handle = FindFirstFileW(pattern, &found);
    free(pattern);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD value = GetLastError();
        if (value == ERROR_FILE_NOT_FOUND) {
            pz_vec_init(out, sizeof(pz_fs_entry));
            return 1;
        }
        fail_win32(error, value);
        return 0;
    }

    pz_vec_init(&list, sizeof(pz_fs_entry));
    do {
        pz_fs_kind kind;

        if (wcscmp(found.cFileName, L".") == 0 ||
            wcscmp(found.cFileName, L"..") == 0)
            continue;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            kind = PZ_FS_LINK;
        else if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            kind = PZ_FS_DIR;
        else
            kind = PZ_FS_FILE;

        {
            char* utf8 = NULL;
            int needed = WideCharToMultiByte(CP_UTF8, 0, found.cFileName, -1, NULL,
                                             0, NULL, NULL);
            int added = 0;

            if (needed > 0) {
                utf8 = (char*)malloc((size_t)needed);
                if (utf8 &&
                    WideCharToMultiByte(CP_UTF8, 0, found.cFileName, -1, utf8,
                                        needed, NULL, NULL) == needed)
                    added = pz_fs_entries_add(&list, utf8, strlen(utf8), kind);
                free(utf8);
            }
            if (!added) {
                FindClose(handle);
                pz_fs_entries_free(&list);
                pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
                return 0;
            }
        }
    } while (FindNextFileW(handle, &found));
    FindClose(handle);

    pz_fs_entries_sort(&list);
    pz_fs_entries_free(out);
    *out = list;
    return 1;
}

static int64_t filetime_to_seconds(const FILETIME* value) {
    const uint64_t ticks = ((uint64_t)value->dwHighDateTime << 32) |
                           value->dwLowDateTime;
    return (int64_t)(ticks / 10000000ULL) - 11644473600LL;
}

static void seconds_to_filetime(int64_t seconds, FILETIME* value) {
    const uint64_t ticks = ((uint64_t)(seconds + 11644473600LL)) * 10000000ULL;
    value->dwLowDateTime = (DWORD)(ticks & 0xffffffffu);
    value->dwHighDateTime = (DWORD)(ticks >> 32);
}

int pz_fs_last_write_time(const pz_path* path, int64_t* seconds,
                          pz_fs_error* error) {
    wchar_t* wide = NULL;
    WIN32_FILE_ATTRIBUTE_DATA data;

    if (!seconds) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (!make_wide(path, &wide, error)) return 0;
    if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &data)) {
        const DWORD value = GetLastError();
        free(wide);
        fail_win32(error, value);
        return 0;
    }
    free(wide);
    *seconds = filetime_to_seconds(&data.ftLastWriteTime);
    return 1;
}

int pz_fs_set_last_write_time(const pz_path* path, int64_t seconds,
                              pz_fs_error* error) {
    wchar_t* wide = NULL;
    HANDLE handle;
    FILETIME time;

    if (!make_wide(path, &wide, error)) return 0;
    handle = CreateFileW(wide, FILE_WRITE_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        fail_win32(error, GetLastError());
        return 0;
    }
    seconds_to_filetime(seconds, &time);
    if (!SetFileTime(handle, NULL, NULL, &time)) {
        const DWORD value = GetLastError();
        CloseHandle(handle);
        fail_win32(error, value);
        return 0;
    }
    CloseHandle(handle);
    return 1;
}

int pz_fs_executable_path(pz_path* out, pz_fs_error* error) {
    DWORD capacity = 512;

    if (!out) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    for (;;) {
        wchar_t* buffer = (wchar_t*)malloc((size_t)capacity * sizeof(wchar_t));
        DWORD length;

        if (!buffer) {
            pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
            return 0;
        }
        length = GetModuleFileNameW(NULL, buffer, capacity);
        if (length == 0) {
            const DWORD value = GetLastError();
            free(buffer);
            fail_win32(error, value);
            return 0;
        }
        if (length < capacity - 1) {
            char* utf8 = NULL;
            int needed = WideCharToMultiByte(CP_UTF8, 0, buffer, (int)length, NULL,
                                             0, NULL, NULL);
            int ok = 0;

            if (needed > 0) {
                utf8 = (char*)malloc((size_t)needed + 1);
                if (utf8 &&
                    WideCharToMultiByte(CP_UTF8, 0, buffer, (int)length, utf8,
                                        needed, NULL, NULL) == needed) {
                    utf8[needed] = '\0';
                    ok = pz_path_assign(out, utf8, (size_t)needed);
                }
            }
            free(utf8);
            free(buffer);
            if (!ok) pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
            return ok;
        }
        free(buffer);
        capacity *= 2;
        if (capacity > 65536) {
            pz_fs_error_set(error, PZ_FS_TOO_LONG, 0);
            return 0;
        }
    }
}

int pz_fs_current_dir(pz_path* out, pz_fs_error* error) {
    DWORD capacity = 512;

    if (!out) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    for (;;) {
        wchar_t* buffer = (wchar_t*)malloc((size_t)capacity * sizeof(wchar_t));
        DWORD length;

        if (!buffer) {
            pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
            return 0;
        }
        length = GetCurrentDirectoryW(capacity, buffer);
        if (length == 0) {
            const DWORD value = GetLastError();
            free(buffer);
            fail_win32(error, value);
            return 0;
        }
        if (length < capacity) {
            char* utf8 = NULL;
            int needed = WideCharToMultiByte(CP_UTF8, 0, buffer, (int)length, NULL,
                                             0, NULL, NULL);
            int ok = 0;

            if (needed > 0) {
                utf8 = (char*)malloc((size_t)needed + 1);
                if (utf8 &&
                    WideCharToMultiByte(CP_UTF8, 0, buffer, (int)length, utf8,
                                        needed, NULL, NULL) == needed) {
                    utf8[needed] = '\0';
                    ok = pz_path_assign(out, utf8, (size_t)needed);
                }
            }
            free(utf8);
            free(buffer);
            if (!ok) pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
            return ok;
        }
        free(buffer);
        capacity *= 2;
        if (capacity > 65536) {
            pz_fs_error_set(error, PZ_FS_TOO_LONG, 0);
            return 0;
        }
    }
}

int pz_fs_temp_dir(pz_path* out, pz_fs_error* error) {
    DWORD capacity = 512;

    if (!out) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    for (;;) {
        wchar_t* buffer = (wchar_t*)malloc((size_t)capacity * sizeof(wchar_t));
        DWORD length;

        if (!buffer) {
            pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
            return 0;
        }
        length = GetTempPathW(capacity, buffer);
        if (length == 0) {
            const DWORD value = GetLastError();
            free(buffer);
            fail_win32(error, value);
            return 0;
        }
        if (length < capacity) {
            char* utf8 = NULL;
            int needed = WideCharToMultiByte(CP_UTF8, 0, buffer, (int)length, NULL,
                                             0, NULL, NULL);
            int ok = 0;

            if (needed > 0) {
                utf8 = (char*)malloc((size_t)needed + 1);
                if (utf8 &&
                    WideCharToMultiByte(CP_UTF8, 0, buffer, (int)length, utf8,
                                        needed, NULL, NULL) == needed) {
                    utf8[needed] = '\0';
                    ok = pz_path_assign(out, utf8, (size_t)needed);
                }
            }
            free(utf8);
            free(buffer);
            if (!ok) pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
            return ok;
        }
        free(buffer);
        capacity *= 2;
        if (capacity > 65536) {
            pz_fs_error_set(error, PZ_FS_TOO_LONG, 0);
            return 0;
        }
    }
}

static int open_file(const pz_path* path, const wchar_t* mode, pz_fs_file** file,
                     pz_fs_error* error) {
    wchar_t* wide = NULL;
    pz_fs_file* handle;
    FILE* stream;

    if (!file) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (!make_wide(path, &wide, error)) return 0;
    stream = _wfopen(wide, mode);
    free(wide);
    if (!stream) {
        pz_fs_error_set(error, code_for_errno(errno), errno);
        return 0;
    }
    handle = (pz_fs_file*)malloc(sizeof *handle);
    if (!handle) {
        fclose(stream);
        pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
        return 0;
    }
    handle->stream = stream;
    handle->error = PZ_FS_OK;
    *file = handle;
    return 1;
}

int pz_fs_open_read(const pz_path* path, pz_fs_file** file, pz_fs_error* error) {
    return open_file(path, L"rb", file, error);
}

int pz_fs_open_write(const pz_path* path, pz_fs_file** file,
                     pz_fs_error* error) {
    return open_file(path, L"wb", file, error);
}

int pz_fs_open_append(const pz_path* path, pz_fs_file** file,
                      pz_fs_error* error) {
    return open_file(path, L"ab", file, error);
}

size_t pz_fs_read(pz_fs_file* file, void* buffer, size_t size) {
    size_t got;

    if (!file || !buffer || size == 0) return 0;
    got = fread(buffer, 1, size, file->stream);
    if (got < size && ferror(file->stream)) file->error = PZ_FS_IO;
    return got;
}

size_t pz_fs_write(pz_fs_file* file, const void* buffer, size_t size) {
    size_t put;

    if (!file || (!buffer && size != 0)) return 0;
    put = fwrite(buffer, 1, size, file->stream);
    if (put < size) {
        file->error = errno != 0 ? code_for_errno(errno) : PZ_FS_IO;
        if (file->error == PZ_FS_OK) file->error = PZ_FS_IO;
    }
    return put;
}

pz_fs_code pz_fs_file_error(const pz_fs_file* file) {
    return file ? file->error : PZ_FS_INVALID;
}

int pz_fs_close(pz_fs_file* file, pz_fs_error* error) {
    int result = 1;

    if (!file) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (file->error != PZ_FS_OK) {
        pz_fs_error_set(error, file->error, 0);
        result = 0;
    }
    if (fclose(file->stream) != 0) {
        if (result) pz_fs_error_set(error, PZ_FS_IO, 0);
        result = 0;
    }
    free(file);
    return result;
}
