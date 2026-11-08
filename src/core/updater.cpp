#include "core/updater.hpp"
#include "core/paths.hpp"
#include "core/steamcmd.hpp"
#include "core/text.hpp"
#include "core/version.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace pz {
namespace {

constexpr const char* kLatestReleaseUrl =
    "https://api.github.com/repos/sigods/pzmodmgr/releases/latest";

std::string trimmed(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lowered(std::string value) {
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

bool ends_with(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void skip_json_string(const std::string& json, std::size_t& i) {
    if (i < json.size() && json[i] == '"') ++i;
    while (i < json.size()) {
        if (json[i] == '\\') {
            i += 2;
        } else if (json[i] == '"') {
            ++i;
            return;
        } else {
            ++i;
        }
    }
}

bool json_string(const std::string& json, const std::string& key,
                 std::size_t from, std::string& value, std::size_t* after = nullptr) {
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = json.find(needle, from);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size() || json[pos] != ':') return false;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;

    value.clear();
    while (pos < json.size()) {
        const char c = json[pos++];
        if (c == '"') {
            if (after) *after = pos;
            return true;
        }
        if (c == '\\' && pos < json.size()) {
            const char escaped = json[pos++];
            switch (escaped) {
            case 'n': value += '\n'; break;
            case 'r': value += '\r'; break;
            case 't': value += '\t'; break;
            case 'b': value += '\b'; break;
            case 'f': value += '\f'; break;
            case 'u':
                // Release tags, asset names, URLs and digests are ASCII.
                // Keep a placeholder for other JSON strings rather than
                // pretending the surrogate pair was decoded.
                if (pos + 4 <= json.size()) pos += 4;
                value += '?';
                break;
            default: value += escaped; break;
            }
        } else {
            value += c;
        }
    }
    return false;
}

bool json_uint(const std::string& json, const std::string& key, std::size_t from,
               std::uint64_t& value) {
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = json.find(needle, from);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size() || json[pos] != ':') return false;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    const char* first = json.data() + pos;
    const char* last = first;
    while (last != json.data() + json.size() &&
           std::isdigit(static_cast<unsigned char>(*last))) ++last;
    if (first == last) return false;
    value = std::strtoull(std::string(first, last).c_str(), nullptr, 10);
    return true;
}

std::vector<int> version_parts(const std::string& version) {
    std::vector<int> parts;
    int number = 0;
    bool have_digits = false;
    for (char c : version) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            have_digits = true;
            number = number * 10 + (c - '0');
        } else if (have_digits) {
            parts.push_back(number);
            number = 0;
            have_digits = false;
            if (parts.size() == 4) break;
        }
    }
    if (have_digits && parts.size() < 4) parts.push_back(number);
    parts.resize(4, 0);
    return parts;
}

bool platform_asset(const std::string& name, const std::string& platform) {
    if (platform == "win") {
        return name.find("win64") != std::string::npos &&
               name.find("setup") == std::string::npos &&
               ends_with(name, ".zip");
    }
    return name.find("linux") != std::string::npos &&
           ends_with(name, ".tar.gz");
}

std::string platform_name() {
#ifdef _WIN32
    return "win";
#else
    return "linux";
#endif
}

std::string safe_path_component(const std::string& value) {
    std::string result;
    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' ||
            c == '_')
            result += c;
        else
            result += '_';
    }
    return result.empty() ? "latest" : result;
}

std::optional<fs::path> update_root(const fs::path& stage, const std::string& exe_name) {
    std::error_code ec;
    if (fs::is_regular_file(stage / exe_name, ec)) return stage;
    fs::directory_iterator it(stage, ec);
    if (ec) return std::nullopt;
    for (const auto& entry : it) {
        if (!entry.is_directory(ec)) continue;
        if (fs::is_regular_file(entry.path() / exe_name, ec)) return entry.path();
    }
    return std::nullopt;
}

unsigned long marker_pid(const fs::path& marker) {
    unsigned long pid = 0;
    for_each_line(marker, [&](const std::string& line) {
        if (line.compare(0, 4, "pid=") == 0)
            pid = std::strtoul(line.c_str() + 4, nullptr, 10);
    });
    return pid;
}

std::string active_staged_update(const fs::path& updates_root) {
    std::error_code ec;
    fs::directory_iterator entries(updates_root, ec);
    if (ec) return {};
    for (const auto& entry : entries) {
        std::error_code item_ec;
        if (!entry.is_directory(item_ec)) continue;
        const fs::path marker = entry.path() / "pending.txt";
        if (!fs::is_regular_file(marker, item_ec)) continue;
        const unsigned long pid = marker_pid(marker);
        if (pid != 0 && process_is_running(pid))
            return entry.path().filename().string();
    }
    return {};
}

void remove_file(const fs::path& path) {
    std::error_code ec;
    fs::remove(path, ec);
}

#ifndef _WIN32
std::FILE* open_append(const fs::path& path) {
    return std::fopen(path.c_str(), "ab");
}
#else
std::FILE* open_append(const fs::path& path) {
    return _wfopen(path.c_str(), L"ab");
}
#endif

std::uint32_t rotate_right(std::uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

std::string sha256_hex(const std::string& data) {
    static constexpr std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
        0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
        0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
        0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    const std::uint64_t bit_length =
        static_cast<std::uint64_t>(data.size()) * 8;
    std::vector<std::uint8_t> message(data.begin(), data.end());
    message.push_back(0x80);
    while (message.size() % 64 != 56) message.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8)
        message.push_back(static_cast<std::uint8_t>(bit_length >> shift));

    for (std::size_t offset = 0; offset < message.size(); offset += 64) {
        std::uint32_t w[64]{};
        for (int i = 0; i < 16; ++i) {
            const std::size_t at = offset + static_cast<std::size_t>(i) * 4;
            w[i] = (static_cast<std::uint32_t>(message[at]) << 24) |
                   (static_cast<std::uint32_t>(message[at + 1]) << 16) |
                   (static_cast<std::uint32_t>(message[at + 2]) << 8) |
                   static_cast<std::uint32_t>(message[at + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotate_right(w[i - 15], 7) ^
                                     rotate_right(w[i - 15], 18) ^
                                     (w[i - 15] >> 3);
            const std::uint32_t s1 = rotate_right(w[i - 2], 17) ^
                                     rotate_right(w[i - 2], 19) ^
                                     (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
                                       rotate_right(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = hh + sum1 + choose + k[i] + w[i];
            const std::uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
                                       rotate_right(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = sum0 + majority;
            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    std::string result;
    result.reserve(64);
    for (std::uint32_t value : h) {
        char part[9]{};
        std::snprintf(part, sizeof part, "%08x", value);
        result += part;
    }
    return result;
}

void append_update_log(const fs::path& log_path, const std::string& message) {
    if (log_path.empty()) return;
    std::error_code ec;
    if (!log_path.parent_path().empty())
        fs::create_directories(log_path.parent_path(), ec);
    std::FILE* out = open_append(log_path);
    if (!out) return;
    std::fputs(message.c_str(), out);
    std::fputc('\n', out);
    std::fclose(out);
}

int update_failed(const fs::path& log_path, const fs::path& marker,
                  const std::string& message) {
    append_update_log(log_path, "ERROR: " + message);
    remove_file(marker);
#ifdef _WIN32
    MessageBoxA(nullptr, message.c_str(), "PZ Mod Manager update failed",
                MB_OK | MB_ICONERROR);
#endif
    return 1;
}

}  // namespace

std::string app_version() {
    return kAppVersion;
}

bool app_version_newer(const std::string& latest, const std::string& current) {
    const auto left = version_parts(latest);
    const auto right = version_parts(current);
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i] != right[i]) return left[i] > right[i];
    }
    return false;
}

AppUpdate parse_latest_app_release(const std::string& json,
                                   const std::string& current_version,
                                   const std::string& platform) {
    AppUpdate update;
    update.current_version = current_version;

    std::string tag;
    if (!json_string(json, "tag_name", 0, tag) || tag.empty()) {
        update.message = "GitHub did not return a release tag";
        return update;
    }
    if (tag.front() == 'v' || tag.front() == 'V') tag.erase(0, 1);
    update.latest_version = trimmed(tag);
    update.available = app_version_newer(update.latest_version, current_version);

    std::string chosen_name, chosen_url, chosen_digest;
    std::uint64_t chosen_size = 0;
    bool found_asset = false;
    const std::size_t assets_key = json.find("\"assets\"");
    if (assets_key != std::string::npos) {
        const std::size_t array = json.find('[', assets_key);
        if (array != std::string::npos) {
            int depth = 0;
            std::size_t object_start = std::string::npos;
            for (std::size_t i = array + 1; i < json.size(); ++i) {
                const char c = json[i];
                if (c == '"') {
                    skip_json_string(json, i);
                    if (i >= json.size()) break;
                    --i;   // let the loop inspect the char after the string
                } else if (c == '{') {
                    if (depth == 0) object_start = i;
                    ++depth;
                } else if (c == '}') {
                    if (depth > 0) --depth;
                    if (depth == 0 && object_start != std::string::npos) {
                        const std::string object =
                            json.substr(object_start, i - object_start + 1);
                        std::string name, url, digest;
                        std::uint64_t size = 0;
                        if (json_string(object, "name", 0, name) &&
                            json_string(object, "browser_download_url", 0, url) &&
                            platform_asset(name, platform)) {
                            json_uint(object, "size", 0, size);
                            json_string(object, "digest", 0, digest);
                            if (!found_asset) {
                                chosen_name = name;
                                chosen_url = url;
                                chosen_digest = digest;
                                chosen_size = size;
                                found_asset = true;
                            }
                        }
                    }
                } else if (c == ']' && depth == 0) {
                    break;
                }
            }
        }
    }

    if (found_asset) {
        update.asset_name = chosen_name;
        update.asset_url = chosen_url;
        update.asset_digest = chosen_digest;
        update.asset_size = chosen_size;
    }

    if (!update.available) {
        update.ok = true;
        update.message = "PZ Mod Manager " + current_version +
                         " is up to date (latest release " + update.latest_version + ").";
        return update;
    }
    if (!found_asset) {
        update.message = "Release " + update.latest_version +
                         " has no compatible " + platform + " archive";
        return update;
    }
    update.ok = true;
    update.message = "PZ Mod Manager " + update.latest_version + " is available.";
    return update;
}

AppUpdate check_app_update() {
    std::string response, detail;
    if (!http_get(kLatestReleaseUrl, response, detail)) {
        AppUpdate update;
        update.current_version = app_version();
        update.message = "Could not check for updates: " + detail;
        return update;
    }
    return parse_latest_app_release(response, app_version(), platform_name());
}

Result stage_app_update(const Config& cfg, const LogFn& log) {
    cleanup_update_cache(cfg);
    const fs::path updates_root = cfg.data_dir / "updates";
    const std::string active = active_staged_update(updates_root);
    if (!active.empty()) {
        return {true, "PZ Mod Manager " + active +
                      " is already staged. Close pzmodmgr to apply it."};
    }

    AppUpdate update = check_app_update();
    if (!update.ok) return {false, update.message};
    if (!update.available) return {true, update.message};

    const fs::path base = updates_root / safe_path_component(update.latest_version);
    const fs::path marker = base / "pending.txt";
    std::error_code ec;
    fs::remove_all(base, ec);

    const fs::path stage = base / "stage";
    fs::create_directories(stage, ec);
    if (ec) return {false, "Could not create the update folder: " + ec.message()};

    const bool zip = ends_with(update.asset_name, ".zip");
    const fs::path archive = base / (zip ? "package.zip" : "package.tar.gz");
    log("Downloading PZ Mod Manager " + update.latest_version + "...");
    std::string detail;
    if (!download_url(update.asset_url, archive, detail)) {
        fs::remove_all(base, ec);
        return {false, "Update download failed: " + detail};
    }

    if (update.asset_size != 0) {
        const auto size = fs::file_size(archive, ec);
        if (ec || size != update.asset_size) {
            fs::remove_all(base, ec);
            return {false, "Update download is incomplete"};
        }
    }

    if (!update.asset_digest.empty()) {
        std::string expected = lowered(update.asset_digest);
        if (expected.compare(0, 7, "sha256:") == 0) expected.erase(0, 7);
        std::string actual, digest_detail;
        if (!sha256_file(archive, actual, digest_detail) || actual != expected) {
            fs::remove_all(base, ec);
            return {false, "Update checksum did not match" +
                           (digest_detail.empty() ? "" : ": " + digest_detail)};
        }
    } else {
        log("GitHub did not provide a checksum for this release; relying on HTTPS.");
    }

    log("Extracting update...");
    if (!extract_archive(archive, stage, detail)) {
        fs::remove_all(base, ec);
        return {false, "Update extraction failed: " + detail};
    }

    const fs::path executable = executable_path();
    if (executable.empty()) {
        fs::remove_all(base, ec);
        return {false, "Could not locate the running executable"};
    }
    const std::string exe_name = executable.filename().string();
    const auto root = update_root(stage, exe_name);
    if (!root) {
        fs::remove_all(base, ec);
        return {false, "The update archive does not contain " + exe_name};
    }

    const fs::path helper_dir = base / "helper";
    fs::create_directories(helper_dir, ec);
    if (ec) {
        fs::remove_all(base, ec);
        return {false, "Could not create the update helper folder: " + ec.message()};
    }
    const fs::path helper = helper_dir / executable.filename();
    fs::copy_file(executable, helper, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        fs::remove_all(base, ec);
        return {false, "Could not prepare the update helper: " + ec.message()};
    }
#ifndef _WIN32
    fs::permissions(helper,
        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
        fs::perm_options::add, ec);
#endif

    const fs::path log_path = base / "update.log";
    const std::vector<std::string> args = {
        "--apply-update",
        "--pid", std::to_string(current_process_id()),
        "--stage", root->string(),
        "--install", executable.parent_path().string(),
        "--exe", exe_name,
        "--marker", marker.string(),
        "--log", log_path.string(),
    };
    unsigned long helper_pid = 0;
    if (!start_detached_process(helper, args, &helper_pid)) {
        fs::remove_all(base, ec);
        return {false, "Could not start the update helper"};
    }

    // The marker doubles as a lock: a second click must not start a second
    // helper while this one is waiting for the current process to exit.
    if (!write_text_atomic(marker, "pid=" + std::to_string(helper_pid) +
                                   "\nversion=" + update.latest_version + "\n"))
        log("warning: could not write the update lock file");

    log("Update staged. Close pzmodmgr to install " + update.latest_version + ".");
    return {true, "PZ Mod Manager " + update.latest_version +
                  " is staged. Close pzmodmgr to apply the update."};
}

void cleanup_update_cache(const Config& cfg) {
    const fs::path updates_root = cfg.data_dir / "updates";
    std::error_code ec;
    fs::directory_iterator entries(updates_root, ec);
    if (ec) return;

    const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24 * 7);
    for (const auto& entry : entries) {
        std::error_code item_ec;
        if (!entry.is_directory(item_ec)) continue;

        // A pending marker without a live helper is an abandoned stage: it can
        // never be applied, so do not keep its potentially large download.
        const fs::path marker = entry.path() / "pending.txt";
        if (fs::is_regular_file(marker, item_ec)) {
            const unsigned long pid = marker_pid(marker);
            if (pid != 0 && process_is_running(pid)) continue;
            fs::remove_all(entry.path(), item_ec);
            continue;
        }

        const auto modified = fs::last_write_time(entry.path(), item_ec);
        if (!item_ec && modified < cutoff) fs::remove_all(entry.path(), item_ec);
    }
    fs::remove(updates_root, ec);   // succeeds only when empty
}

bool sha256_file(const fs::path& file, std::string& hex, std::string& detail) {
    hex.clear();
    detail.clear();
    std::string data;
    if (!read_text_file(file, data)) {
        detail = "could not read " + file.string();
        return false;
    }
    hex = sha256_hex(data);
    return true;
}

bool is_update_helper_invocation(int argc, char** argv) {
    return argc > 1 && argv && argv[1] && std::string(argv[1]) == "--apply-update";
}

int run_update_helper(int argc, char** argv) {
    std::map<std::string, std::string> options;
    for (int i = 2; i + 1 < argc; i += 2) {
        std::string key = argv[i] ? argv[i] : "";
        if (key.compare(0, 2, "--") != 0) continue;
        options[key.substr(2)] = argv[i + 1] ? argv[i + 1] : "";
    }
    const fs::path stage = options["stage"];
    const fs::path install = options["install"];
    const std::string exe_name = options["exe"];
    const fs::path marker = options["marker"];
    const fs::path log_path = options["log"];

    append_update_log(log_path, "update helper started");
    if (stage.empty() || install.empty() || exe_name.empty()) {
        return update_failed(log_path, marker, "The update helper received incomplete paths.");
    }

    const unsigned long parent_pid =
        std::strtoul(options["pid"].c_str(), nullptr, 10);
    append_update_log(log_path, "waiting for pzmodmgr to exit");
    wait_for_process(parent_pid);

    std::error_code ec;
    if (!fs::is_directory(stage, ec) || !fs::is_directory(install, ec)) {
        return update_failed(log_path, marker,
                             "The staged update or installation folder disappeared.");
    }
    if (!fs::is_regular_file(stage / exe_name, ec)) {
        return update_failed(log_path, marker,
                             "The staged update does not contain " + exe_name + ".");
    }

    const std::string suffix = ".update-" + std::to_string(current_process_id());
    const fs::path new_dir = install.parent_path() /
                             (install.filename().string() + suffix + "-new");
    const fs::path old_dir = install.parent_path() /
                             (install.filename().string() + suffix + "-old");
    fs::remove_all(new_dir, ec);
    fs::remove_all(old_dir, ec);

    append_update_log(log_path, "building a complete replacement folder");
    fs::copy(stage, new_dir,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing |
                 fs::copy_options::copy_symlinks,
             ec);
    if (ec) {
        fs::remove_all(new_dir, ec);
        return update_failed(log_path, marker,
                             "Could not copy the staged update: " + ec.message());
    }

    // Configuration and uninstall metadata are not part of the release zip.
    for (const char* keep : {"config.ini", "uninstall.sh", "uninstall.exe"}) {
        const fs::path source = install / keep;
        const fs::path destination = new_dir / keep;
        if (fs::is_regular_file(source, ec) && !fs::exists(destination, ec)) {
            fs::copy_file(source, destination, ec);
            if (ec) {
                fs::remove_all(new_dir, ec);
                return update_failed(log_path, marker,
                                     "Could not preserve " + std::string(keep) + ".");
            }
        }
    }

    if (!fs::is_regular_file(new_dir / exe_name, ec)) {
        fs::remove_all(new_dir, ec);
        return update_failed(log_path, marker,
                             "The prepared update does not contain " + exe_name + ".");
    }
#ifndef _WIN32
    for (const std::string& name : {exe_name, std::string("pzmodmgr_native_host")}) {
        const fs::path target = new_dir / name;
        if (fs::is_regular_file(target, ec))
            fs::permissions(target,
                fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                fs::perm_options::add, ec);
    }
#endif

    append_update_log(log_path, "swapping the installation folder");
    fs::rename(install, old_dir, ec);
    if (ec)
        return update_failed(log_path, marker,
                             "Could not move the old installation aside: " + ec.message());

    fs::rename(new_dir, install, ec);
    if (ec) {
        std::error_code restore_ec;
        fs::rename(old_dir, install, restore_ec);
        fs::remove_all(new_dir, restore_ec);
        return update_failed(log_path, marker,
                             "Could not install the new version; the old one was restored.");
    }

    unsigned long new_pid = 0;
    if (!start_detached_process(install / exe_name, {}, &new_pid)) {
        std::error_code rollback_ec;
        fs::rename(install, new_dir, rollback_ec);
        fs::rename(old_dir, install, rollback_ec);
        fs::remove_all(new_dir, rollback_ec);
        return update_failed(log_path, marker,
                             "The new version was installed but could not be started; "
                             "the old version was restored.");
    }

    fs::remove_all(old_dir, ec);
    fs::remove_all(stage, ec);
    remove_file(stage.parent_path() / "package.zip");
    remove_file(stage.parent_path() / "package.tar.gz");
    remove_file(marker);
    append_update_log(log_path, "update installed successfully");
    return 0;
}

}  // namespace pz
