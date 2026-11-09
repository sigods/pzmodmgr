#pragma once
#include "core/error_code.hpp"
#include <cstdint>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace pz {
namespace native_fs {

#ifdef _WIN32
constexpr char preferred_separator = '\\';
inline bool is_separator(char c) { return c == '\\' || c == '/'; }
inline const char* separator_chars() { return "\\/"; }
#else
constexpr char preferred_separator = '/';
inline bool is_separator(char c) { return c == '/'; }
inline const char* separator_chars() { return "/"; }
#endif

// A deliberately small replacement for std::filesystem. Windows paths are kept
// as UTF-8 strings and converted to UTF-16 at the Win32 boundary; POSIX paths
// are byte strings, as the OS expects. Keeping path conversion out of
// libstdc++ avoids pulling its locale/codecvt/iostream machinery into static
// Windows builds and gives both platforms one implementation to test.
class path {
public:
    path() = default;
    path(const char* value);
    path(const std::string& value);
#ifdef _WIN32
    path(const std::wstring& value);
#endif

    path& operator=(const char* value);
    path& operator=(const std::string& value);
#ifdef _WIN32
    path& operator=(const std::wstring& value);
#endif

    const std::string& string() const { return value_; }
#ifdef _WIN32
    const std::wstring& wstring() const;
    const wchar_t* c_str() const { return wstring().c_str(); }
#else
    const char* c_str() const { return value_.c_str(); }
#endif

    bool empty() const { return value_.empty(); }
    void clear() {
        value_.clear();
        reset();
    }

    path filename() const {
        if (value_.empty()) return {};
        std::size_t end = value_.size();
        while (end != 0 && is_separator(value_[end - 1])) --end;
        if (end == 0) return {};
#ifdef _WIN32
        if (end == 2 && value_[1] == ':') return {};
#endif
        const std::size_t slash = value_.find_last_of(separator_chars(), end - 1);
        if (slash == std::string::npos) return path(value_.substr(0, end));
        if (slash + 1 >= end) return {};
        return path(value_.substr(slash + 1, end - slash - 1));
    }

    path parent_path() const;
    path stem() const {
        const path base = filename();
        const std::string& name = base.value_;
        const std::size_t dot = name.find_last_of('.');
        if (dot == std::string::npos || dot == 0) return base;
        return path(name.substr(0, dot));
    }
    path extension() const {
        const path base = filename();
        const std::string& name = base.value_;
        const std::size_t dot = name.find_last_of('.');
        if (dot == std::string::npos || dot == 0) return {};
        return path(name.substr(dot));
    }
    bool is_absolute() const;
    path root_path() const;
    path lexically_normal() const;

    path& operator/=(const path& other) {
        if (other.empty()) return *this;
        if (other.is_absolute() || value_.empty()) {
            value_ = other.value_;
            reset();
            return *this;
        }
        if (!is_separator(value_.back())) value_.push_back(preferred_separator);
        value_ += other.value_;
        reset();
        return *this;
    }

    path& operator+=(const path& other) {
        value_ += other.value_;
        reset();
        return *this;
    }
    path& operator+=(const char* other) { return *this += path(other); }
    path& operator+=(const std::string& other) { return *this += path(other); }

    friend bool operator==(const path& a, const path& b) { return a.value_ == b.value_; }
    friend bool operator!=(const path& a, const path& b) { return !(a == b); }
    friend bool operator<(const path& a, const path& b) { return a.value_ < b.value_; }
    friend bool operator>(const path& a, const path& b) { return b < a; }

private:
    void reset() const {
#ifdef _WIN32
        wide_ready_ = false;
        wide_.clear();
#endif
    }

    std::string value_;
#ifdef _WIN32
    mutable std::wstring wide_;
    mutable bool wide_ready_ = false;
#endif
};

inline path operator/(path lhs, const path& rhs) { return lhs /= rhs; }

struct directory_entry {
    directory_entry() = default;
    directory_entry(pz::native_fs::path p, bool directory, bool regular)
        : entry_path_(std::move(p)), directory_(directory), regular_(regular) {}

    const pz::native_fs::path& path() const { return entry_path_; }
    bool is_directory(pz::error_code& ec) const {
        ec.clear();
        return directory_;
    }
    bool is_directory() const { return directory_; }
    bool is_regular_file(pz::error_code& ec) const {
        ec.clear();
        return regular_;
    }
    bool is_regular_file() const { return regular_; }

private:
    pz::native_fs::path entry_path_;
    bool directory_ = false;
    bool regular_ = false;
};

class directory_iterator {
public:
    directory_iterator() = default;
    explicit directory_iterator(const path& dir);
    directory_iterator(const path& dir, pz::error_code& ec);

    std::vector<directory_entry>::const_iterator begin() const { return entries_.begin(); }
    std::vector<directory_entry>::const_iterator end() const { return entries_.end(); }

private:
    std::vector<directory_entry> entries_;
};

// Seconds since the Unix epoch. A plain integer avoids std::chrono and its
// libstdc++ chrono.o member; the manager only compares file ages and sets
// timestamps for its own cache/backup cleanup.
using file_time_type = std::int64_t;

inline file_time_type file_time_now() {
    return static_cast<file_time_type>(std::time(nullptr));
}

enum class copy_options : unsigned {
    none = 0,
    recursive = 1u << 0,
    overwrite_existing = 1u << 1,
    copy_symlinks = 1u << 2,
};

inline copy_options operator|(copy_options a, copy_options b) {
    return static_cast<copy_options>(static_cast<unsigned>(a) |
                                     static_cast<unsigned>(b));
}
inline copy_options& operator|=(copy_options& a, copy_options b) {
    a = a | b;
    return a;
}

path current_path();
path temp_directory_path();
bool exists(const path& p);
bool exists(const path& p, pz::error_code& ec);
bool is_regular_file(const path& p);
bool is_regular_file(const path& p, pz::error_code& ec);
bool is_directory(const path& p);
bool is_directory(const path& p, pz::error_code& ec);
bool is_empty(const path& p);
bool is_empty(const path& p, pz::error_code& ec);
std::uintmax_t file_size(const path& p, pz::error_code& ec);
file_time_type last_write_time(const path& p, pz::error_code& ec);
bool last_write_time(const path& p, file_time_type value, pz::error_code& ec);
inline bool last_write_time(const path& p, file_time_type value) {
    pz::error_code ec;
    return last_write_time(p, value, ec);
}
bool create_directories(const path& p);
bool create_directories(const path& p, pz::error_code& ec);
std::uintmax_t remove_all(const path& p, pz::error_code& ec);
inline std::uintmax_t remove_all(const path& p) {
    pz::error_code ec;
    return remove_all(p, ec);
}
bool remove(const path& p, pz::error_code& ec);
bool rename(const path& from, const path& to, pz::error_code& ec);
bool copy_file(const path& from, const path& to, copy_options options,
               pz::error_code& ec);
bool copy_file(const path& from, const path& to, pz::error_code& ec);
bool copy(const path& from, const path& to, copy_options options,
          pz::error_code& ec);

#ifndef _WIN32
enum class perms : unsigned {
    none = 0,
    owner_read = 0400,
    owner_write = 0200,
    owner_exec = 0100,
    group_read = 0040,
    group_write = 0020,
    group_exec = 0010,
    others_read = 0004,
    others_write = 0002,
    others_exec = 0001,
};

inline perms operator|(perms a, perms b) {
    return static_cast<perms>(static_cast<unsigned>(a) |
                              static_cast<unsigned>(b));
}
inline perms& operator|=(perms& a, perms b) {
    a = a | b;
    return a;
}

enum class perm_options { replace, add, remove };

bool permissions(const path& p, perms value, perm_options options,
                 pz::error_code& ec);
bool permissions(const path& p, perms value, pz::error_code& ec);
#endif

}  // namespace native_fs
}  // namespace pz
