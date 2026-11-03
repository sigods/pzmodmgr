#pragma once
#include "core/mod_info.hpp"
#include "core/types.hpp"
#include <cstdint>
#include <map>
#include <vector>

namespace pz {
class ModManager {
public:
    explicit ModManager(Config cfg);

    Result add(const std::string& url_or_id, const LogFn& log);
    Result remove(std::uint64_t workshop_id, const LogFn& log);
    Result update_one(std::uint64_t workshop_id, const LogFn& log);
    Result update_all(const LogFn& log);
    Result check_updates(const LogFn& log);

    std::vector<InstalledMod> list() const { return db_; }
    Config& config() { return cfg_; }
    std::optional<ModInfo> info_for(const std::string& folder) const {
        return read_mod_info(cfg_.user_mods_dir / folder, game_version_);
    }
    const std::string& game_version() const { return game_version_; }
    fs::path icon_for(const std::string& folder) const;
    bool update_available(std::uint64_t workshop_id) const;

private:
    Config cfg_;
    std::string game_version_;
    std::vector<InstalledMod> db_;
    std::map<std::uint64_t, bool> updates_;

    fs::path db_path() const { return cfg_.data_dir / "installed.tsv"; }
    void load_db();
    void save_db() const;
    Result install(std::uint64_t id, std::uint64_t remote_time, const LogFn& log);
    Result install_collection(std::uint64_t collection_id,
                              const std::vector<std::uint64_t>& children,
                              const LogFn& log);
    bool is_installed(std::uint64_t id) const;
    std::uint64_t recorded_time(std::uint64_t id) const;
};
}  // namespace pz
