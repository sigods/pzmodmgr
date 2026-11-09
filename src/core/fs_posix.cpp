#include "core/fs_native.hpp"

#ifndef _WIN32

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

namespace pz {
namespace native_fs {
namespace {

void set_error(pz::error_code& ec, int code) {
    ec.assign(code, pz::error_kind::generic);
}

void clear_or_set(pz::error_code& ec) {
    const int code = errno;
    if (code == ENOENT || code == ENOTDIR)
        ec.clear();
    else
        set_error(ec, code);
}

bool stat_path(const path& p, struct stat& st, pz::error_code& ec) {
    if (::stat(p.c_str(), &st) != 0) {
        clear_or_set(ec);
        return false;
    }
    ec.clear();
    return true;
}

bool delete_tree(const std::string& value, std::uintmax_t& removed,
                 pz::error_code& ec) {
    struct stat st {};
    if (::lstat(value.c_str(), &st) != 0) {
        clear_or_set(ec);
        return !ec;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR* dir = ::opendir(value.c_str());
        if (!dir) {
            set_error(ec, errno);
            return false;
        }
        bool ok = true;
        while (const dirent* entry = ::readdir(dir)) {
            if (std::strcmp(entry->d_name, ".") == 0 ||
                std::strcmp(entry->d_name, "..") == 0)
                continue;
            if (!delete_tree(value + "/" + entry->d_name, removed, ec)) {
                ok = false;
                break;
            }
        }
        ::closedir(dir);
        if (!ok) return false;
        if (::rmdir(value.c_str()) != 0) {
            set_error(ec, errno);
            return false;
        }
    } else if (::unlink(value.c_str()) != 0) {
        set_error(ec, errno);
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

path::path(const char* value) : value_(value ? value : "") {}
path::path(const std::string& value) : value_(value) {}

path& path::operator=(const char* value) {
    value_ = value ? value : "";
    return *this;
}

path& path::operator=(const std::string& value) {
    value_ = value;
    return *this;
}

path path::parent_path() const {
    if (value_.empty()) return {};
    std::size_t end = value_.size();
    while (end != 0 && is_separator(value_[end - 1])) --end;
    if (end == 0) return path("/");
    const std::size_t slash = value_.find_last_of(separator_chars(), end - 1);
    if (slash == std::string::npos) return {};
    if (slash == 0) return path("/");
    return path(value_.substr(0, slash));
}

bool path::is_absolute() const {
    return !value_.empty() && value_[0] == '/';
}

path path::root_path() const {
    return is_absolute() ? path("/") : path{};
}

path path::lexically_normal() const {
    if (value_.empty()) return {};
    const bool absolute = value_[0] == '/';

    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos < value_.size()) {
        while (pos < value_.size() && is_separator(value_[pos])) ++pos;
        const std::size_t start = pos;
        while (pos < value_.size() && !is_separator(value_[pos])) ++pos;
        if (start == pos) continue;

        std::string part = value_.substr(start, pos - start);
        if (part == ".") continue;
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..")
                parts.pop_back();
            else if (!absolute)
                parts.push_back(std::move(part));
            continue;
        }
        parts.push_back(std::move(part));
    }

    std::string out = absolute ? "/" : "";
    for (const auto& part : parts) {
        if (!out.empty() && !is_separator(out.back())) out.push_back('/');
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

    DIR* handle = ::opendir(dir.c_str());
    if (!handle) {
        clear_or_set(ec);
        return;
    }
    while (const dirent* entry = ::readdir(handle)) {
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0)
            continue;

        struct stat st {};
        const bool have_status = ::stat((dir / entry->d_name).c_str(), &st) == 0;
        const bool directory = have_status && S_ISDIR(st.st_mode);
        const bool regular = have_status && S_ISREG(st.st_mode);
        entries_.emplace_back(dir / entry->d_name, directory, regular);
    }
    ::closedir(handle);
}

path current_path() {
    std::vector<char> buffer(256);
    for (;;) {
        if (::getcwd(buffer.data(), buffer.size())) return path(buffer.data());
        if (errno != ERANGE) return {};
        buffer.resize(buffer.size() * 2);
    }
}

path temp_directory_path() {
    const char* value = std::getenv("TMPDIR");
    if (value && *value) return path(value);
    return path("/tmp");
}

bool exists(const path& p, pz::error_code& ec) {
    struct stat st {};
    return stat_path(p, st, ec);
}

bool exists(const path& p) {
    pz::error_code ec;
    return exists(p, ec);
}

bool is_regular_file(const path& p, pz::error_code& ec) {
    struct stat st {};
    return stat_path(p, st, ec) && S_ISREG(st.st_mode);
}

bool is_regular_file(const path& p) {
    pz::error_code ec;
    return is_regular_file(p, ec);
}

bool is_directory(const path& p, pz::error_code& ec) {
    struct stat st {};
    return stat_path(p, st, ec) && S_ISDIR(st.st_mode);
}

bool is_directory(const path& p) {
    pz::error_code ec;
    return is_directory(p, ec);
}

bool is_empty(const path& p, pz::error_code& ec) {
    struct stat st {};
    if (!stat_path(p, st, ec)) return false;
    if (!S_ISDIR(st.st_mode)) return st.st_size == 0;

    DIR* handle = ::opendir(p.c_str());
    if (!handle) {
        set_error(ec, errno);
        return false;
    }
    bool empty = true;
    while (const dirent* entry = ::readdir(handle)) {
        if (std::strcmp(entry->d_name, ".") != 0 &&
            std::strcmp(entry->d_name, "..") != 0) {
            empty = false;
            break;
        }
    }
    ::closedir(handle);
    return empty;
}

bool is_empty(const path& p) {
    pz::error_code ec;
    return is_empty(p, ec);
}

std::uintmax_t file_size(const path& p, pz::error_code& ec) {
    struct stat st {};
    if (!stat_path(p, st, ec)) return 0;
    return static_cast<std::uintmax_t>(st.st_size);
}

file_time_type last_write_time(const path& p, pz::error_code& ec) {
    struct stat st {};
    if (!stat_path(p, st, ec)) return {};
    return static_cast<file_time_type>(st.st_mtim.tv_sec);
}

bool last_write_time(const path& p, file_time_type value, pz::error_code& ec) {
    if (value < 0) value = 0;

    timespec times[2]{};
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = static_cast<time_t>(value);
    times[1].tv_nsec = 0;
    if (::utimensat(AT_FDCWD, p.c_str(), times, 0) != 0) {
        set_error(ec, errno);
        return false;
    }
    ec.clear();
    return true;
}

bool create_directories(const path& p, pz::error_code& ec) {
    ec.clear();
    const std::string& value = p.string();
    if (value.empty()) return false;

    bool created = false;
    for (std::size_t i = 0; i <= value.size(); ++i) {
        if (i != value.size() && value[i] != '/') continue;
        if (i == 0) continue;
        const std::string prefix = value.substr(0, i);
        if (prefix.find_first_not_of('/') == std::string::npos) continue;

        if (::mkdir(prefix.c_str(), 0777) == 0) {
            created = true;
            continue;
        }
        const int code = errno;
        if (code == EEXIST) {
            struct stat st {};
            if (::stat(prefix.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) continue;
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
    delete_tree(p.string(), removed, ec);
    return removed;
}

bool remove(const path& p, pz::error_code& ec) {
    ec.clear();
    struct stat st {};
    if (::lstat(p.c_str(), &st) != 0) {
        clear_or_set(ec);
        return false;
    }
    const int rc = S_ISDIR(st.st_mode) ? ::rmdir(p.c_str()) : ::unlink(p.c_str());
    if (rc != 0) {
        set_error(ec, errno);
        return false;
    }
    return true;
}

bool rename(const path& from, const path& to, pz::error_code& ec) {
    ec.clear();
    if (::rename(from.c_str(), to.c_str()) == 0) return true;
    set_error(ec, errno);
    return false;
}

bool copy_file(const path& from, const path& to, pz::error_code& ec) {
    return copy_file(from, to, copy_options::none, ec);
}

bool copy_file(const path& from, const path& to, copy_options options,
               pz::error_code& ec) {
    ec.clear();
    struct stat source {};
    if (::stat(from.c_str(), &source) != 0) {
        set_error(ec, errno);
        return false;
    }
    if (!S_ISREG(source.st_mode)) {
        set_error(ec, EINVAL);
        return false;
    }

    const bool overwrite =
        (static_cast<unsigned>(options) &
         static_cast<unsigned>(copy_options::overwrite_existing)) != 0;
    const int in = ::open(from.c_str(), O_RDONLY);
    if (in < 0) {
        set_error(ec, errno);
        return false;
    }
    const int out = ::open(to.c_str(),
                           O_WRONLY | O_CREAT |
                               (overwrite ? O_TRUNC : O_EXCL),
                           0600);
    if (out < 0) {
        set_error(ec, errno);
        ::close(in);
        return false;
    }

    std::vector<char> buffer(64 * 1024);
    bool ok = true;
    for (;;) {
        const ssize_t got = ::read(in, buffer.data(), buffer.size());
        if (got == 0) break;
        if (got < 0) {
            if (errno == EINTR) continue;
            set_error(ec, errno);
            ok = false;
            break;
        }
        ssize_t written = 0;
        while (written < got) {
            const ssize_t part =
                ::write(out, buffer.data() + written, static_cast<std::size_t>(got - written));
            if (part < 0) {
                if (errno == EINTR) continue;
                set_error(ec, errno);
                ok = false;
                break;
            }
            written += part;
        }
        if (!ok) break;
    }
    if (ok && ::fchmod(out, source.st_mode & 07777) != 0) {
        set_error(ec, errno);
        ok = false;
    }
    if (::close(out) != 0 && ok) {
        set_error(ec, errno);
        ok = false;
    }
    ::close(in);
    if (!ok && !overwrite) ::unlink(to.c_str());
    return ok;
}

bool copy(const path& from, const path& to, copy_options options,
          pz::error_code& ec) {
    ec.clear();
    return copy_tree(from, to, options, ec);
}

bool permissions(const path& p, perms value, perm_options options,
                 pz::error_code& ec) {
    ec.clear();
    struct stat st {};
    if (::stat(p.c_str(), &st) != 0) {
        set_error(ec, errno);
        return false;
    }

    mode_t mode = static_cast<mode_t>(static_cast<unsigned>(value));
    if (options != perm_options::replace) {
        const mode_t current = st.st_mode & 07777;
        if (options == perm_options::add)
            mode = current | mode;
        else
            mode = current & ~mode;
    }
    if (::chmod(p.c_str(), mode) != 0) {
        set_error(ec, errno);
        return false;
    }
    return true;
}

bool permissions(const path& p, perms value, pz::error_code& ec) {
    return permissions(p, value, perm_options::replace, ec);
}

}  // namespace native_fs
}  // namespace pz

#endif  // _WIN32
