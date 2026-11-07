#pragma once
#include "core/types.hpp"
#include <vector>
namespace pz {
struct DownloadOutcome {
    bool ok = false;
    fs::path content_dir;   // where the workshop item landed
    std::string detail;
};

// Returns a usable steamcmd. Uses the configured path, an existing built-in
// copy, or downloads Valve's official build into <Zomboid>/pzmodmgr/steamcmd.
// System installations are only a fallback when the download fails.
fs::path ensure_steamcmd(Config& cfg, const LogFn& log);

DownloadOutcome steamcmd_download_workshop_item(Config& cfg,
                                                std::uint64_t item_id,
                                                const fs::path& install_dir,
                                                const LogFn& log);

// Queries the Steam Web API for the item's time_updated. Returns 0 on failure.
std::uint64_t fetch_remote_time_updated(std::uint64_t item_id);

// Parses time_updated out of a GetPublishedFileDetails JSON response.
// Exposed for tests; returns 0 when the field is missing or invalid.
std::uint64_t parse_published_file_time_updated(const std::string& json);

// Queries the item's own result code from GetPublishedFileDetails: 1 = OK,
// 9 = not found (removed or private), 15 = access denied. 0 means the API could
// not be reached. Anonymous steamcmd can only download items that return 1.
std::uint32_t fetch_published_file_result(std::uint64_t item_id);

// Parses the per-item result out of a GetPublishedFileDetails JSON response.
// Exposed for tests; returns 0 when the field is missing or invalid.
std::uint32_t parse_published_file_result(const std::string& json);

// Returns the child item ids of a collection (empty if not a collection).
std::vector<std::uint64_t> fetch_collection_items(std::uint64_t collection_id);

// Parses child ids out of a GetCollectionDetails JSON response.
// Exposed for tests; the collection's own id is not included.
std::vector<std::uint64_t> parse_collection_children(const std::string& json);
}  // namespace pz
