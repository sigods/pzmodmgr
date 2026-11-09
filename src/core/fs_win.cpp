#include "core/fs_native.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cwchar>
#include <utility>

namespace pz {
namespace native_fs {
namespace {

bool is_sep(wchar_t c) { return c == L'\\' || c == L'/'; }

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                   static_cast<int>(text.size()), nullptr, 0);
    UINT codepage = CP_UTF8;
    if (size <= 0) {
        codepage = CP_ACP;
        size = MultiByteToWideChar(codepage, 0, text.data(),
                                   static_cast<int>(text.size()), nullptr, 0);
    }
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(codepage, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), size);
    return result;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::string normalized(std::string text) {
    for (char& c : text)
        if (c == '/') c = '\\';
    return text;
}

void set_error(pz::error_code& ec, DWORD code) {
    ec.assign(static_cast<int>(code), pz::error_kind::system);
}

void clear_or_set(pz::error_code& ec) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
        ec.clear();
    else
        set_error(ec, code);
}

DWORD attributes(const std::wstring& value) {
    return GetFileAttributesW(value.c_str());
}

bool delete_file(const std::wstring& value) {
    if (DeleteFileW(value.c_str())) return true;
    if (GetLastError() != ERROR_ACCESS_DENIED) return false;
    SetFileAttributesW(value.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(value.c_str()) != 0;
}

bool delete_tree(const std::wstring& value, std::uintmax_t& removed,
                 pz::error_code& ec) {
    const DWORD attr = attributes(value);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        clear_or_set(ec);
        return !ec;
    }

    if ((attr & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (attr & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        const std::wstring pattern = value + L"\\*";
        WIN32_FIND_DATAW data{};
        HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
        if (handle != INVALID_HANDLE_VALUE) {
            do {
                if (std::wcscmp(data.cFileName, L".") == 0 ||
                    std::wcscmp(data.cFileName, L"..") == 0)
                    continue;
                if (!delete_tree(value + L"\\" + data.cFileName, removed, ec)) {
                    FindClose(handle);
                    return false;
                }
            } while (FindNextFileW(handle, &data));
            FindClose(handle);
        }
        if (!RemoveDirectoryW(value.c_str())) {
            set_error(ec, GetLastError());
            return false;
        }
    } else if (!delete_file(value)) {
        set_error(ec, GetLastError());
        return false;
    }
    ++removed;
    ec.clear();
    return true;
}

bool copy_tree(const path& from, const path& to, copy_options options,
               pz::error_code& ec) {
    pz::error_code type_ec;
    if (is_directory(from, type_ec)) {
        create_directories(to, ec);
        if (ec) return false;
        for (const auto& entry : directory_iterator(from, ec)) {
            if (ec) return false;
            if (!copy_tree(entry.path(), to / entry.path().filename(), options, ec))
                return false;
        }
        return true;
    }
    if (!is_regular_file(from, type_ec)) {
        ec = type_ec;
        return false;
    }
    if (!to.parent_path().empty()) {
        create_directories(to.parent_path(), ec);
        if (ec) return false;
    }
    return copy_file(from, to, options, ec);
}

}  // namespace

path::path(const char* value) : value_(normalized(value ? value : "")) {}
path::path(const std::string& value) : value_(normalized(value)) {}
path::path(const std::wstring& value) : value_(normalized(narrow(value))) {}

path& path::operator=(const char* value) {
    value_ = normalized(value ? value : "");
    reset();
    return *this;
}

path& path::operator=(const std::string& value) {
    value_ = normalized(value);
    reset();
    return *this;
}

path& path::operator=(const std::wstring& value) {
    value_ = normalized(narrow(value));
    reset();
    return *this;
}

const std::wstring& path::wstring() const {
    if (!wide_ready_) {
        wide_ = widen(value_);
        wide_ready_ = true;
    }
    return wide_;
}

path path::parent_path() const {
    if (value_.empty()) return {};
    std::size_t end = value_.size();
    while (end != 0 && is_separator(value_[end - 1])) --end;
    if (end == 0) return path("\\");

    const path root = root_path();
    if (end <= root.value_.size()) return root;
    const std::size_t slash = value_.find_last_of(separator_chars(), end - 1);
    if (slash == std::string::npos) {
        if (value_.size() >= 2 && value_[1] == ':') return path(value_.substr(0, 2));
        return {};
    }
    if (slash < root.value_.size()) return root;
    return path(value_.substr(0, slash));
}

bool path::is_absolute() const {
    if (value_.size() >= 3 &&
        std::isalpha(static_cast<unsigned char>(value_[0])) &&
        value_[1] == ':' && is_separator(value_[2]))
        return true;
    if (value_.size() >= 2 && is_separator(value_[0]) && is_separator(value_[1])) {
        // A UNC path is absolute once it has a server and a root separator.
        // The share name is part of the relative path, so \\server\share is
        // already an absolute directory (and a valid Zomboid folder).
        const std::size_t server_end = value_.find_first_of(separator_chars(), 2);
        return server_end != std::string::npos && server_end > 2;
    }
    return false;
}

path path::root_path() const {
    if (value_.size() >= 3 &&
        std::isalpha(static_cast<unsigned char>(value_[0])) &&
        value_[1] == ':' && is_separator(value_[2]))
        return path(value_.substr(0, 3));
    if (value_.size() >= 2 && is_separator(value_[0]) && is_separator(value_[1])) {
        const std::size_t server_end = value_.find_first_of(separator_chars(), 2);
        if (server_end == std::string::npos) return {};
        return path(value_.substr(0, server_end + 1));
    }
    if (!value_.empty() && is_separator(value_[0])) return path("\\");
    return {};
}

path path::lexically_normal() const {
    if (value_.empty()) return {};
    const path root = root_path();
    const std::size_t root_size = root.value_.size();
    const std::string rest = value_.substr(std::min(root_size, value_.size()));

    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos < rest.size()) {
        while (pos < rest.size() && is_separator(rest[pos])) ++pos;
        const std::size_t start = pos;
        while (pos < rest.size() && !is_separator(rest[pos])) ++pos;
        if (start == pos) continue;
        std::string part = rest.substr(start, pos - start);
        if (part == ".") continue;
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..")
                parts.pop_back();
            else if (root.empty())
                parts.push_back(std::move(part));
            continue;
        }
        parts.push_back(std::move(part));
    }

    std::string out = root.value_;
    for (const auto& part : parts) {
        if (!out.empty() && !is_separator(out.back()))
            out.push_back(preferred_separator);
        out += part;
    }
    if (out.empty()) return path(".");
    return path(out);
}

directory_iterator::directory_iterator(const path& dir) {
    pz::error_code ignored;
    *this = directory_iterator(dir, ignored);
}

directory_iterator::directory_iterator(const path& dir, pz::error_code& ec) {
    ec.clear();
    if (dir.empty()) return;
    const std::wstring pattern = (dir / "*").wstring();
    WIN32_FIND_DATAW data{};
    HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        clear_or_set(ec);
        return;
    }
    do {
        if (std::wcscmp(data.cFileName, L".") == 0 ||
            std::wcscmp(data.cFileName, L"..") == 0)
            continue;
        const bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entries_.emplace_back(dir / narrow(data.cFileName), directory, !directory);
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
}

path current_path() {
    std::vector<wchar_t> buffer(260);
    for (;;) {
        const DWORD size = GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()),
                                                buffer.data());
        if (size == 0) return {};
        if (size < buffer.size()) return path(std::wstring(buffer.data(), size));
        buffer.resize(size);
    }
}

path temp_directory_path() {
    std::vector<wchar_t> buffer(MAX_PATH + 1);
    for (;;) {
        const DWORD size = GetTempPathW(static_cast<DWORD>(buffer.size()),
                                        buffer.data());
        if (size == 0) return {};
        if (size < buffer.size()) return path(std::wstring(buffer.data(), size));
        buffer.resize(size);
    }
}

bool exists(const path& p, pz::error_code& ec) {
    const DWORD attr = attributes(p.wstring());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        clear_or_set(ec);
        return false;
    }
    ec.clear();
    return true;
}

bool exists(const path& p) {
    pz::error_code ec;
    return exists(p, ec);
}

bool is_regular_file(const path& p, pz::error_code& ec) {
    const DWORD attr = attributes(p.wstring());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        clear_or_set(ec);
        return false;
    }
    ec.clear();
    return (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool is_regular_file(const path& p) {
    pz::error_code ec;
    return is_regular_file(p, ec);
}

bool is_directory(const path& p, pz::error_code& ec) {
    const DWORD attr = attributes(p.wstring());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        clear_or_set(ec);
        return false;
    }
    ec.clear();
    return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool is_directory(const path& p) {
    pz::error_code ec;
    return is_directory(p, ec);
}

bool is_empty(const path& p, pz::error_code& ec) {
    const DWORD attr = attributes(p.wstring());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        clear_or_set(ec);
        return false;
    }
    ec.clear();
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        std::uintmax_t size = 0;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &data)) {
            set_error(ec, GetLastError());
            return false;
        }
        size = (static_cast<std::uintmax_t>(data.nFileSizeHigh) << 32) |
               data.nFileSizeLow;
        return size == 0;
    }

    const std::wstring pattern = (p / "*").wstring();
    WIN32_FIND_DATAW data{};
    HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        clear_or_set(ec);
        return ec == pz::error_code{};
    }
    bool empty = true;
    do {
        if (std::wcscmp(data.cFileName, L".") != 0 &&
            std::wcscmp(data.cFileName, L"..") != 0) {
            empty = false;
            break;
        }
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
    return empty;
}

bool is_empty(const path& p) {
    pz::error_code ec;
    return is_empty(p, ec);
}

std::uintmax_t file_size(const path& p, pz::error_code& ec) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &data)) {
        set_error(ec, GetLastError());
        return 0;
    }
    ec.clear();
    return (static_cast<std::uintmax_t>(data.nFileSizeHigh) << 32) |
           data.nFileSizeLow;
}

file_time_type last_write_time(const path& p, pz::error_code& ec) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &data)) {
        set_error(ec, GetLastError());
        return {};
    }
    ec.clear();
    const std::uint64_t ticks =
        (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
        data.ftLastWriteTime.dwLowDateTime;
    constexpr std::uint64_t kWindowsToUnixEpoch = 116444736000000000ULL;
    const std::uint64_t unix_ticks =
        ticks > kWindowsToUnixEpoch ? ticks - kWindowsToUnixEpoch : 0;
    return static_cast<file_time_type>(unix_ticks / 10000000ULL);
}

bool last_write_time(const path& p, file_time_type value, pz::error_code& ec) {
    constexpr std::uint64_t kWindowsToUnixEpoch = 116444736000000000ULL;
    const std::uint64_t unix_ticks =
        value > 0 ? static_cast<std::uint64_t>(value) * 10000000ULL : 0;
    const std::uint64_t ticks = unix_ticks + kWindowsToUnixEpoch;
    FILETIME ft{};
    ft.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFu);
    ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);

    HANDLE handle = CreateFileW(p.wstring().c_str(), FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        set_error(ec, GetLastError());
        return false;
    }
    const BOOL ok = SetFileTime(handle, nullptr, nullptr, &ft);
    const DWORD code = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!ok) {
        set_error(ec, code);
        return false;
    }
    ec.clear();
    return true;
}

bool create_directories(const path& p, pz::error_code& ec) {
    ec.clear();
    const std::wstring value = p.wstring();
    if (value.empty()) return false;

    std::size_t start = 0;
    if (value.size() >= 3 && value[1] == L':' && is_sep(value[2]))
        start = 3;
    else if (value.size() >= 2 && is_sep(value[0]) && is_sep(value[1])) {
        const std::size_t server_end = value.find_first_of(L"\\/", 2);
        if (server_end != std::wstring::npos) {
            const std::size_t share_end = value.find_first_of(L"\\/", server_end + 1);
            start = share_end == std::wstring::npos ? value.size() : share_end + 1;
        }
    }

    bool created = false;
    for (std::size_t i = start; i <= value.size(); ++i) {
        if (i != value.size() && !is_sep(value[i])) continue;
        if (i == 0) continue;
        const std::wstring prefix = value.substr(0, i);
        if (prefix.empty() || prefix.back() == L':') continue;
        if (CreateDirectoryW(prefix.c_str(), nullptr)) {
            created = true;
            continue;
        }
        const DWORD code = GetLastError();
        if (code == ERROR_ALREADY_EXISTS) {
            const DWORD attr = attributes(prefix);
            if (attr != INVALID_FILE_ATTRIBUTES &&
                (attr & FILE_ATTRIBUTE_DIRECTORY) != 0)
                continue;
        }
        set_error(ec, code);
        return false;
    }
    return created;
}

bool create_directories(const path& p) {
    pz::error_code ec;
    return create_directories(p, ec);
}

std::uintmax_t remove_all(const path& p, pz::error_code& ec) {
    ec.clear();
    std::uintmax_t removed = 0;
    delete_tree(p.wstring(), removed, ec);
    return removed;
}

bool remove(const path& p, pz::error_code& ec) {
    ec.clear();
    const std::wstring value = p.wstring();
    const DWORD attr = attributes(value);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        clear_or_set(ec);
        return false;
    }
    const bool ok = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0
                        ? RemoveDirectoryW(value.c_str()) != 0
                        : delete_file(value);
    if (!ok) set_error(ec, GetLastError());
    return ok;
}

bool rename(const path& from, const path& to, pz::error_code& ec) {
    ec.clear();
    if (MoveFileW(from.wstring().c_str(), to.wstring().c_str())) return true;
    set_error(ec, GetLastError());
    return false;
}

bool copy_file(const path& from, const path& to, pz::error_code& ec) {
    return copy_file(from, to, copy_options::none, ec);
}

bool copy_file(const path& from, const path& to, copy_options options,
               pz::error_code& ec) {
    ec.clear();
    const bool overwrite =
        (static_cast<unsigned>(options) &
         static_cast<unsigned>(copy_options::overwrite_existing)) != 0;
    if (CopyFileW(from.wstring().c_str(), to.wstring().c_str(),
                  overwrite ? FALSE : TRUE))
        return true;
    set_error(ec, GetLastError());
    return false;
}

bool copy(const path& from, const path& to, copy_options options,
          pz::error_code& ec) {
    ec.clear();
    return copy_tree(from, to, options, ec);
}

}  // namespace native_fs
}  // namespace pz

#endif  // _WIN32
