#pragma once
#include "core/activation.hpp"
#include "core/mod_info.hpp"
#include "core/types.hpp"
#include <cstdint>
#include <map>
#include <vector>

namespace pz {
struct MissingRequirement {
    std::string mod_name;      // installed mod that declares require=
    std::string folder;        // its folder inside user_mods_dir
    std::string required_id;   // Mod ID that is not present
};

class ModManager {
public:
    explicit ModManager(Config cfg);

    Result add(const std::string& url_or_id, const LogFn& log);
    // Changes the Zomboid base folder, recomputes derived paths, and reloads
    // the installed-mod database from the new data_dir.
    Result set_zomboid_dir(const fs::path& dir);
    Result remove(std::uint64_t workshop_id, const LogFn& log);
    Result update_one(std::uint64_t workshop_id, const LogFn& log);
    Result update_all(const LogFn& log);
    Result check_updates(const LogFn& log);
    // Checks the latest GitHub release, records the result, and exposes whether
    // an app update is available. A failed check keeps the last known state.
    Result check_app_release(const LogFn& log);

    std::vector<InstalledMod> list() const { return db_; }
    // Tracked records plus manual folders in mods/ that installed.tsv does not
    // know about. Manual rows have workshop_id 0 and can still be enabled or
    // disabled in the game's mod lists.
    std::vector<InstalledMod> list_all() const;
    Config& config() { return cfg_; }
    // Every place the game reads enabled mods: default.txt, each save's
    // mods.txt, and each server .ini.
    std::vector<ActivationTarget> activation_targets() const;
    // Mod IDs declared by the folders of one Workshop item.
    std::vector<std::string> mod_ids_for(std::uint64_t workshop_id) const;
    // Workshop IDs to share/install for a target. Servers use enabled Mods=
    // mapped through installed.tsv, then WorkshopItems= as a fallback.
    std::vector<std::uint64_t> workshop_ids_for(const ActivationTarget& target) const;
    // Enabled Mod IDs with no installed folder, so their Workshop ID is unknown.
    std::vector<std::string> unmapped_mod_ids(const ActivationTarget& target) const;
    // Human-readable names for the same list, for sharing when IDs are missing.
    std::vector<std::string> enabled_mod_names(const ActivationTarget& target) const;
    // Enables or disables every Mod ID belonging to workshop_ids in one target.
    Result set_mods_enabled(ActivationKind kind, const std::string& name,
                            const std::vector<std::uint64_t>& workshop_ids,
                            bool enabled, const LogFn& log);
    // Same, but also accepts manual folder names. Multi-folder Workshop items
    // enable one game-compatible variant and remove its siblings from the target.
    Result set_selection_enabled(ActivationKind kind, const std::string& name,
                                 const std::vector<std::uint64_t>& workshop_ids,
                                 const std::vector<std::string>& folders,
                                 bool enabled, const LogFn& log);
    // Preferred variant for a multi-folder Workshop item (newest compatible).
    std::string preferred_mod_id_for(std::uint64_t workshop_id) const;
    std::optional<ModInfo> info_for(const std::string& folder) const {
        return read_mod_info(cfg_.user_mods_dir / folder, game_version_);
    }
    const std::string& game_version() const { return game_version_; }
    // Scans all mod folders for require= IDs and reports requirements that are
    // not installed. Manual mods count as installed if their mod.info is present.
    std::vector<MissingRequirement> missing_requirements() const;
    bool update_available(std::uint64_t workshop_id) const;
    bool app_update_available() const { return app_update_available_; }
    // True when either an installed mod or the app release is due for a check.
    bool update_check_due() const;
    bool app_update_check_due() const;
    // Re-reads installed.tsv after another process changed it. Returns true
    // when the visible records or game version changed; keeps live update flags.
    bool reload_installed();

private:
    Config cfg_;
    std::string game_version_;
    std::vector<InstalledMod> db_;
    std::map<std::uint64_t, bool> updates_;
    bool app_update_available_ = false;

    fs::path db_path() const { return cfg_.data_dir / "installed.tsv"; }
    fs::path last_check_path() const { return cfg_.data_dir / "last_check"; }
    fs::path last_app_check_path() const { return cfg_.data_dir / "last_app_check"; }
    fs::path app_update_path() const { return cfg_.data_dir / "app_update"; }
    std::uint64_t read_last_check() const;
    std::uint64_t read_last_app_check() const;
    void record_update_check() const;
    void record_app_update_check() const;
    void load_app_update_state();
    void load_db();
    void save_db() const;
    Result install(std::uint64_t id, std::uint64_t remote_time, const LogFn& log,
                   bool* changed = nullptr, bool enable_new = false);
    Result install_collection(std::uint64_t collection_id,
                              const std::vector<std::uint64_t>& children,
                              const LogFn& log);
    bool is_installed(std::uint64_t id) const;
    std::vector<std::uint64_t> unique_installed_ids() const;
    std::uint64_t recorded_time(std::uint64_t id) const;
};
}  // namespace pz
