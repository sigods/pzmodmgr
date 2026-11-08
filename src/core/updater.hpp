#pragma once
#include "core/types.hpp"
#include <string>

namespace pz {

struct AppUpdate {
    bool ok = false;          // the release check itself succeeded
    bool available = false;   // a newer release exists
    std::string current_version;
    std::string latest_version;
    std::string asset_name;
    std::string asset_url;
    std::string asset_digest; // "sha256:..." from the GitHub asset, when present
    std::uint64_t asset_size = 0;
    std::string message;
};

std::string app_version();

// Numeric comparison of "0.2.1" and "v0.2.2"; pre-release suffixes are ignored.
bool app_version_newer(const std::string& latest, const std::string& current);

// Exposed for tests. platform is "win" or "linux".
AppUpdate parse_latest_app_release(const std::string& json,
                                   const std::string& current_version,
                                   const std::string& platform);

bool sha256_file(const fs::path& file, std::string& hex, std::string& detail);

// Fetches the latest GitHub release with curl.
AppUpdate check_app_update();

// Removes abandoned staged releases and old update folders. An update whose
// helper is still running is preserved.
void cleanup_update_cache(const Config& cfg);

// Checks, downloads, verifies and extracts a release, then starts a detached
// copy of this executable that applies it after the current process exits.
Result stage_app_update(const Config& cfg, const LogFn& log);

bool is_update_helper_invocation(int argc, char** argv);
int run_update_helper(int argc, char** argv);

}  // namespace pz
