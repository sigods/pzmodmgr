#include "core/steamcmd.hpp"
#include "core/paths.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <string_view>
#include <system_error>

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

// Runs a shell command, streams each line to log, returns exit status.
int run_capture(const std::string& cmd, const LogFn& log, std::string& tail) {
#ifdef _WIN32
    FILE* p = _popen(cmd.c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    if (!p) return -1;

    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), p)) {
        std::string line = buf.data();
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (!line.empty()) {
            log(line);
            tail += line + "\n";
        }
    }
#ifdef _WIN32
    return _pclose(p);
#else
    return pclose(p);
#endif
}

// Runs a shell command and captures stdout/stderr verbatim (no line splitting).
int run_capture_raw(const std::string& cmd, std::string& out) {
#ifdef _WIN32
    FILE* p = _popen(cmd.c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    if (!p) return -1;

    std::array<char, 4096> buf{};
    std::size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0)
        out.append(buf.data(), n);
#ifdef _WIN32
    return _pclose(p);
#else
    return pclose(p);
#endif
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
}  // namespace

fs::path ensure_steamcmd(Config& cfg, const LogFn& log) {
    std::error_code ec;
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
    int rc = run_capture(cmd, log, tail);

    out.content_dir = install_dir / "steamapps" / "workshop" / "content"
                    / std::to_string(kProjectZomboidAppId) / std::to_string(item_id);

    std::error_code ec;
    bool has_content = fs::is_directory(out.content_dir, ec) && !fs::is_empty(out.content_dir, ec);
    out.ok = (rc == 0) && has_content;
    if (!out.ok) {
        out.detail = "steamcmd failed (rc=" + std::to_string(rc) +
                     "). Check: logged in once manually? Steam Guard? account owns PZ?";
    }
    return out;
}

std::uint64_t parse_published_file_time_updated(const std::string& json) {
    // Walk the JSON, skipping string bodies, so a title/description that
    // embeds `"time_updated":...` cannot spoof the real field.
    std::size_t i = 0;
    while (i < json.size()) {
        if (json[i] != '"') { ++i; continue; }

        const std::size_t key_start = ++i;
        while (i < json.size() && json[i] != '"') {
            if (json[i] == '\\' && i + 1 < json.size()) ++i;
            ++i;
        }
        std::string_view key(json.data() + key_start, i - key_start);
        if (i < json.size()) ++i;   // closing quote
        if (key != "time_updated") continue;

        // A plain string value can also equal "time_updated"; only a
        // following ':' makes it the field we want.
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
        if (i >= json.size() || json[i] != ':') continue;
        ++i;
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;

        auto first = json.data() + i;
        auto last = first;
        while (last != json.data() + json.size() &&
               std::isdigit(static_cast<unsigned char>(*last))) ++last;
        if (first == last) return 0;

        std::uint64_t value = 0;
        auto r = std::from_chars(first, last, value);
        return r.ec == std::errc{} ? value : 0;
    }
    return 0;
}

std::uint64_t fetch_remote_time_updated(std::uint64_t item_id) {
    const std::string url =
        "https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/";
    const std::string body =
        "itemcount=1&publishedfileids[0]=" + std::to_string(item_id);

    std::string response;
    if (!steam_api_post(url, body, response)) return 0;
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
