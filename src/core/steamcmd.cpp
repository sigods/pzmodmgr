#include "core/steamcmd.hpp"
#include "core/paths.hpp"
#include "core/thread.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <string_view>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
extern char** environ;
#endif

namespace pz {
namespace {

std::string quote(const std::string& s) {
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else o += c;
    }
    return o + "'";
#endif
}

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
#endif

// Runs `cmd` through the shell, handing every chunk of combined output to sink.
// Returns the child's exit status, or -1 when it could not be started.
//
// On Windows this is CreateProcessW with CREATE_NO_WINDOW instead of _popen():
// pzmodmgr is a GUI-subsystem program, so it owns no console, and Windows would
// allocate one for every curl.exe and steamcmd.exe child. That showed up as a
// blank console window flashing on each Install/Check (one per child). The
// handles are still pipes, so the output is captured exactly as before.
template <typename Sink>
int spawn_capture(const std::string& cmd, Sink&& sink) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr;
    HANDLE wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;

    // Same shape as the CRT's _popen(): cmd.exe /c <command>. The callers add
    // the outer quotes and the 2>&1 / <nul redirections themselves.
    std::wstring line = L"cmd.exe /c " + widen(cmd);
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) {
        CloseHandle(rd);
        return -1;
    }

    std::array<char, 4096> buf{};
    DWORD got = 0;
    while (ReadFile(rd, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0)
        sink(buf.data(), got);
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
#else
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return -1;

    std::array<char, 4096> buf{};
    std::size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0) sink(buf.data(), n);
    return pclose(p);
#endif
}

// Runs a shell command and appends its combined output to out. When log is not
// null every complete line is also reported to it as it arrives, so long
// steamcmd runs keep streaming into the UI.
int run_capture(const std::string& cmd, const LogFn* log, std::string& out) {
    std::string pending;
    const int rc = spawn_capture(cmd, [&](const char* data, std::size_t n) {
        out.append(data, n);
        if (!log) return;
        pending.append(data, n);
        std::size_t pos = 0;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) (*log)(line);
        }
    });
    if (log && !pending.empty()) {
        if (pending.back() == '\r') pending.pop_back();
        if (!pending.empty()) (*log)(pending);
    }
    return rc;
}

// Runs a shell command and captures the output verbatim (no line splitting).
int run_capture_raw(const std::string& cmd, std::string& out) {
    return run_capture(cmd, nullptr, out);
}

std::string one_line(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    const auto nl = s.find('\n');
    if (nl != std::string::npos) s.resize(nl);
    if (s.size() > 300) s.resize(300);
    return s;
}

// POSTs a form body to a Steam Web API endpoint. No API key is needed.
bool steam_api_post(const std::string& url, const std::string& body, std::string& response) {
    // No HTTP library dependency: use the system curl (bundled with Windows
    // 10+ and standard on Linux). Process spawning stays in this file.
    std::string cmd = "curl -sS --fail --connect-timeout 5 --max-time 20 -X POST --data "
                    + quote(body) + " " + quote(url) + " 2>&1";
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";   // cmd /c quoting rule
#endif
    return run_capture_raw(cmd, response) == 0;
}

bool ends_with(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

#ifdef _WIN32
std::wstring quote_windows_arg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (std::size_t i = 0; i < arg.size();) {
        std::size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++slashes;
        }
        if (i == arg.size()) {
            out.append(slashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(slashes, L'\\');
            out += arg[i];
        }
        ++i;
    }
    out += L'"';
    return out;
}
#endif
}  // namespace

bool http_get(const std::string& url, std::string& response, std::string& detail) {
    response.clear();
    detail.clear();
    std::string cmd = "curl -sS --fail --location --connect-timeout 10 --max-time 30 "
                      "-H \"Accept: application/vnd.github+json\" "
                      "-H \"User-Agent: pzmodmgr\" " + quote(url) + " 2>&1";
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";
#endif
    if (run_capture_raw(cmd, response) == 0) return true;
    detail = one_line(response);
    if (detail.empty()) detail = "could not reach the update server";
    return false;
}

bool download_url(const std::string& url, const fs::path& dest, std::string& detail) {
    detail.clear();
    pz::error_code ec;
    if (!dest.parent_path().empty()) fs::create_directories(dest.parent_path(), ec);
    if (ec) {
        detail = ec.message();
        return false;
    }
    fs::path temp = dest;
    temp += ".part";
    fs::remove(temp, ec);

    std::string output;
    std::string cmd = "curl -sS --fail --location --connect-timeout 10 --max-time 600 -o "
                    + quote(temp.string()) + " " + quote(url) + " 2>&1";
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";
#endif
    const int rc = run_capture_raw(cmd, output);
    if (rc != 0) {
        fs::remove(temp, ec);
        detail = one_line(output);
        if (detail.empty()) detail = "download failed";
        return false;
    }
    const auto size = fs::file_size(temp, ec);
    if (ec || size == 0) {
        fs::remove(temp, ec);
        detail = "downloaded file is empty";
        return false;
    }
    fs::remove(dest, ec);
    fs::rename(temp, dest, ec);
    if (ec) {
        fs::remove(temp, ec);
        detail = ec.message();
        return false;
    }
    return true;
}

bool extract_archive(const fs::path& archive, const fs::path& dest,
                     std::string& detail) {
    detail.clear();
    pz::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) {
        detail = ec.message();
        return false;
    }
    const std::string name = archive.filename().string();
    std::string cmd;
#ifdef _WIN32
    if (ends_with(name, ".zip"))
        cmd = "tar.exe -xf " + quote(archive.string()) + " -C " + quote(dest.string());
    else if (ends_with(name, ".tar.gz") || ends_with(name, ".tgz"))
        cmd = "tar.exe -xzf " + quote(archive.string()) + " -C " + quote(dest.string());
    else {
        detail = "unsupported update archive";
        return false;
    }
    cmd = "\"" + cmd + "\"";
#else
    if (ends_with(name, ".zip"))
        cmd = "unzip -q " + quote(archive.string()) + " -d " + quote(dest.string());
    else if (ends_with(name, ".tar.gz") || ends_with(name, ".tgz"))
        cmd = "tar -xzf " + quote(archive.string()) + " -C " + quote(dest.string());
    else {
        detail = "unsupported update archive";
        return false;
    }
#endif
    std::string output;
    cmd += " 2>&1";
    if (run_capture_raw(cmd, output) == 0) return true;
    detail = one_line(output);
    if (detail.empty()) detail = "could not extract the update archive";
    return false;
}

bool start_detached_process(const fs::path& exe,
                            const std::vector<std::string>& args,
                            unsigned long* pid) {
    if (pid) *pid = 0;
#ifdef _WIN32
    std::wstring command = quote_windows_arg(widen(exe.string()));
    for (const auto& arg : args)
        command += L" " + quote_windows_arg(widen(arg));
    const std::wstring directory = exe.parent_path().wstring();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    const BOOL started = CreateProcessW(
        exe.wstring().c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, directory.empty() ? nullptr : directory.c_str(),
        &si, &pi);
    if (!started) return false;
    if (pid) *pid = static_cast<unsigned long>(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (posix_spawn_file_actions_init(&actions) != 0) return false;
    posix_spawnattr_init(&attributes);
    for (int fd = 0; fd <= 2; ++fd)
        posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", O_RDWR, 0);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
#endif
    std::vector<std::string> storage;
    storage.reserve(args.size() + 1);
    storage.push_back(exe.string());
    storage.insert(storage.end(), args.begin(), args.end());
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& value : storage) argv.push_back(value.data());
    argv.push_back(nullptr);
    pid_t child = 0;
    const int rc = posix_spawn(&child, exe.c_str(), &actions, &attributes,
                               argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (rc != 0) return false;
    if (pid) *pid = static_cast<unsigned long>(child);
    return true;
#endif
}

bool wait_for_process(unsigned long pid) {
    if (pid == 0) return true;
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
    WaitForSingleObject(process, INFINITE);
    CloseHandle(process);
    return true;
#else
    for (;;) {
        if (kill(static_cast<pid_t>(pid), 0) != 0) {
            if (errno == ESRCH) return true;
            if (errno != EPERM) return true;
        }
        sleep_milliseconds(1000);
    }
#endif
}

bool process_is_running(unsigned long pid) {
    if (pid == 0) return false;
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process) return false;
    const DWORD result = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return result == WAIT_TIMEOUT;
#else
    if (kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno == EPERM;
#endif
}

unsigned long current_process_id() {
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

fs::path ensure_steamcmd(Config& cfg, const LogFn& log) {
    pz::error_code ec;
    if (!cfg.steamcmd_exe.empty() && fs::is_regular_file(cfg.steamcmd_exe, ec))
        return cfg.steamcmd_exe;

    const fs::path builtin = builtin_steamcmd_path(cfg);
    if (fs::is_regular_file(builtin, ec)) {
        cfg.steamcmd_exe = builtin;
        return builtin;
    }

    log("steamcmd not found; downloading the built-in copy (one time)...");
    fs::create_directories(builtin.parent_path(), ec);
#ifdef _WIN32
    const char* url = "https://steamcdn-a.akamaihd.net/client/installer/steamcmd.zip";
    const fs::path archive = builtin.parent_path() / "steamcmd-download.zip";
    const std::string extract = "tar.exe -xf ";
#else
    const char* url = "https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz";
    const fs::path archive = builtin.parent_path() / "steamcmd-download.tar.gz";
    const std::string extract = "tar -xzf ";
#endif

    std::string output;
    const std::string download = "curl -sS --fail --location --connect-timeout 10 --max-time 300 -o "
        + quote(archive.string()) + " " + quote(url) + " 2>&1";
    int rc = run_capture_raw(download, output);
    if (rc != 0) {
        log("built-in steamcmd download failed: " + one_line(output));
    } else {
        output.clear();
        const std::string unpack = extract + quote(archive.string()) + " -C " +
                                   quote(builtin.parent_path().string()) + " 2>&1";
        rc = run_capture_raw(unpack, output);
        if (rc != 0) log("built-in steamcmd extract failed: " + one_line(output));
    }
    fs::remove(archive, ec);

    if (fs::is_regular_file(builtin, ec)) {
#ifndef _WIN32
        fs::permissions(builtin,
            fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
            fs::perm_options::add, ec);
#endif
        cfg.steamcmd_exe = builtin;
        log("using built-in steamcmd: " + builtin.string());
        return builtin;
    }

    const fs::path system = find_steamcmd();
    if (!system.empty()) {
        cfg.steamcmd_exe = system;
        log("using system steamcmd: " + system.string());
        return system;
    }

    cfg.steamcmd_exe.clear();
    log("steamcmd is unavailable. Install it or check your network.");
    return {};
}

DownloadOutcome steamcmd_download_workshop_item(Config& cfg,
                                                std::uint64_t item_id,
                                                const fs::path& install_dir,
                                                const LogFn& log) {
    DownloadOutcome out;
    if (cfg.steam_user.empty()) {
        out.detail = "Set the Steam username in Settings first (no password is stored).";
        return out;
    }
    const fs::path steamcmd = ensure_steamcmd(cfg, log);
    if (steamcmd.empty()) {
        out.detail = "steamcmd is unavailable. Check your network or set its path in Settings.";
        return out;
    }
    fs::create_directories(install_dir);

    // Order matters: force_install_dir must come before +login.
    // No password is passed; steamcmd uses its cached login token.
#ifdef _WIN32
    const std::string redir = " <nul 2>&1";
#else
    const std::string redir = " </dev/null 2>&1";
#endif
    std::string cmd = quote(steamcmd.string())
        + " +force_install_dir " + quote(install_dir.string())
        + " +login " + cfg.steam_user
        + " +workshop_download_item " + std::to_string(kProjectZomboidAppId)
        + " " + std::to_string(item_id)
        + " +quit" + redir;
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";   // cmd /c quoting rule
#endif

    std::string tail;
    int rc = run_capture(cmd, &log, tail);

    out.content_dir = install_dir / "steamapps" / "workshop" / "content"
                    / std::to_string(kProjectZomboidAppId) / std::to_string(item_id);

    pz::error_code ec;
    bool has_content = fs::is_directory(out.content_dir, ec) && !fs::is_empty(out.content_dir, ec);
    out.ok = (rc == 0) && has_content;
    if (!out.ok) {
        // steamcmd exits 0 even when Steam refuses the item, so the exit code
        // alone says nothing. Access Denied means Steam cannot see the item at
        // all (private, removed, or not a public Workshop item), which no login
        // or Steam Guard fix can solve.
        if (tail.find("Access Denied") != std::string::npos) {
            out.detail = "Steam refused item " + std::to_string(item_id) +
                         " (Access Denied): it is private or removed, so it cannot be downloaded";
        } else if (rc == 0) {
            out.detail = "steamcmd finished without downloading item " +
                         std::to_string(item_id) + " (item unavailable?)";
        } else {
            out.detail = "steamcmd failed (rc=" + std::to_string(rc) +
                         "). Check: logged in once manually? Steam Guard? account owns PZ?";
        }
    }
    return out;
}

namespace {

// Walks JSON while skipping string bodies and returns the position just after
// the closing quote of the first `key`. npos when the key is absent.
std::size_t find_key(const std::string& json, std::string_view key,
                     std::size_t from = 0) {
    std::size_t i = from;
    while (i < json.size()) {
        if (json[i] != '"') { ++i; continue; }
        const std::size_t start = ++i;
        while (i < json.size() && json[i] != '"') {
            if (json[i] == '\\' && i + 1 < json.size()) ++i;
            ++i;
        }
        if (i >= json.size()) return std::string::npos;
        const std::string_view found(json.data() + start, i - start);
        ++i;   // closing quote
        if (found == key) return i;
    }
    return std::string::npos;
}

// Reads the unsigned number that follows the key found at pos (": 123").
bool read_uint_after(const std::string& json, std::size_t pos, std::uint64_t& value) {
    std::size_t i = pos;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i >= json.size() || json[i] != ':') return false;
    ++i;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;

    const char* first = json.data() + i;
    const char* last = first;
    while (last != json.data() + json.size() &&
           std::isdigit(static_cast<unsigned char>(*last))) ++last;
    if (first == last) return false;

    std::uint64_t parsed = 0;
    const auto r = std::from_chars(first, last, parsed);
    if (r.ec != std::errc{}) return false;
    value = parsed;
    return true;
}

}  // namespace

std::uint64_t parse_published_file_time_updated(const std::string& json) {
    std::size_t from = 0;
    for (;;) {
        const std::size_t pos = find_key(json, "time_updated", from);
        if (pos == std::string::npos) return 0;
        std::uint64_t value = 0;
        if (read_uint_after(json, pos, value)) return value;
        from = pos;   // A string value can itself equal "time_updated".
    }
}

std::uint32_t parse_published_file_result(const std::string& json) {
    // GetPublishedFileDetails answers with an outer "result" (the call status,
    // normally 1) and a per-item "result" inside publishedfiledetails. Only the
    // item's own result says whether the item is reachable.
    const std::size_t items = find_key(json, "publishedfiledetails");
    if (items == std::string::npos) return 0;
    const std::size_t pos = find_key(json, "result", items);
    if (pos == std::string::npos) return 0;

    std::uint64_t value = 0;
    if (!read_uint_after(json, pos, value)) return 0;
    return static_cast<std::uint32_t>(value);
}

namespace {

constexpr const char* kPublishedFileDetailsUrl =
    "https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/";

std::string published_file_details_body(std::uint64_t item_id) {
    return "itemcount=1&publishedfileids[0]=" + std::to_string(item_id);
}

bool fetch_published_file_details(std::uint64_t item_id, std::string& response) {
    return steam_api_post(kPublishedFileDetailsUrl,
                          published_file_details_body(item_id), response);
}

}  // namespace

std::uint32_t fetch_published_file_result(std::uint64_t item_id) {
    // 1 = OK, 9 = k_EResultFileNotFound (removed or private), 15 = access
    // denied. 0 means the API could not be reached, which callers must not read
    // as a verdict about the item.
    std::string response;
    if (!fetch_published_file_details(item_id, response)) return 0;
    return parse_published_file_result(response);
}

std::uint64_t fetch_remote_time_updated(std::uint64_t item_id) {
    std::string response;
    if (!fetch_published_file_details(item_id, response)) return 0;
    return parse_published_file_time_updated(response);
}

std::vector<std::uint64_t> parse_collection_children(const std::string& json) {
    std::vector<std::uint64_t> ids;

    // Find the "children" key, then walk only that array. The collection's own
    // id appears outside it and must not be installed as a child.
    std::size_t i = 0;
    while (i < json.size()) {
        if (json[i] != '"') { ++i; continue; }

        const std::size_t key_start = ++i;
        while (i < json.size() && json[i] != '"') {
            if (json[i] == '\\' && i + 1 < json.size()) ++i;
            ++i;
        }
        std::string_view key(json.data() + key_start, i - key_start);
        if (i < json.size()) ++i;
        if (key != "children") continue;

        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
        if (i >= json.size() || json[i] != ':') continue;
        ++i;
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
        if (i >= json.size() || json[i] != '[') return ids;
        ++i;

        int depth = 1;
        while (i < json.size() && depth > 0) {
            const char c = json[i];
            if (c == '[') {
                ++depth;
                ++i;
            } else if (c == ']') {
                --depth;
                ++i;
            } else if (c == '"') {
                const std::size_t tok_start = ++i;
                while (i < json.size() && json[i] != '"') {
                    if (json[i] == '\\' && i + 1 < json.size()) ++i;
                    ++i;
                }
                std::string_view tok(json.data() + tok_start, i - tok_start);
                if (i < json.size()) ++i;

                std::size_t j = i;
                while (j < json.size() && std::isspace(static_cast<unsigned char>(json[j]))) ++j;
                if (tok == "publishedfileid" && j < json.size() && json[j] == ':') {
                    ++j;
                    while (j < json.size() && std::isspace(static_cast<unsigned char>(json[j]))) ++j;
                    if (j < json.size() && json[j] == '"') {
                        const std::size_t val_start = ++j;
                        while (j < json.size() && json[j] != '"') ++j;
                        std::string_view val(json.data() + val_start, j - val_start);
                        if (j < json.size()) ++j;

                        std::uint64_t value = 0;
                        auto r = std::from_chars(val.data(), val.data() + val.size(), value);
                        if (r.ec == std::errc{} && r.ptr == val.data() + val.size() && value != 0)
                            ids.push_back(value);
                        i = j;   // skip the value string on the next iteration
                    }
                }
            } else {
                ++i;
            }
        }
        break;
    }

    std::vector<std::uint64_t> unique;
    for (auto id : ids)
        if (std::find(unique.begin(), unique.end(), id) == unique.end()) unique.push_back(id);
    return unique;
}

std::vector<std::uint64_t> fetch_collection_items(std::uint64_t collection_id) {
    const std::string url =
        "https://api.steampowered.com/ISteamRemoteStorage/GetCollectionDetails/v1/";
    const std::string body =
        "collectioncount=1&publishedfileids[0]=" + std::to_string(collection_id);

    std::string response;
    if (!steam_api_post(url, body, response)) return {};
    return parse_collection_children(response);
}
}  // namespace pz
