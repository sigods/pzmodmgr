#pragma once
#include "core/types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace pz {

enum class ActivationKind { Default, Save, Server };

// One place Project Zomboid reads enabled mods: mods/default.txt, a save's
// mods.txt, or a server's .ini Mods= line.
struct ActivationTarget {
    ActivationKind kind = ActivationKind::Default;
    std::string name;    // Default is empty; saves are "Mode/Save"; servers are the ini stem
    std::string label;   // display text
    fs::path path;
    fs::path thumbnail;  // save's thumb.png preview, when present
    std::vector<std::string> mod_ids;        // as found, without the B42 backslash
    std::vector<std::uint64_t> workshop_ids; // server WorkshopItems= values
    bool backslash = false;                  // server INI writes \ModID; blocks always plain
    bool exists = false;
};

// Discovers mods/default.txt, every Saves/<mode>/<save> directory, and every
// Server/*.ini. Server INIs use B42's backslash form; block files do not.
std::vector<ActivationTarget> discover_activation_targets(const fs::path& zomboid_dir,
                                                          const std::string& game_version);

// Reads one target. Returns false only when a block file cannot be read; a
// missing file is a valid empty target with exists=false. Legacy backslash
// entries in default.txt/save blocks are repaired to plain Mod IDs.
bool read_activation_target(ActivationTarget& target);

// Adds (enabled) or removes mod_ids, and on servers adds workshop_ids to
// WorkshopItems= when enabling. Writes atomically and updates target in place.
bool write_activation_target(ActivationTarget& target,
                             const std::vector<std::string>& mod_ids,
                             const std::vector<std::uint64_t>& workshop_ids,
                             bool enabled);

// Exposed for tests.
std::vector<std::string> parse_block_mod_ids(const std::string& text);
std::vector<std::string> parse_ini_mod_ids(const std::string& text,
                                           bool* backslash = nullptr);
std::vector<std::uint64_t> parse_ini_workshop_ids(const std::string& text);
std::string rewrite_block_mod_ids(const std::string& text,
                                  const std::vector<std::string>& mod_ids,
                                  bool* changed = nullptr);
std::string rewrite_ini_mods(const std::string& text,
                             const std::vector<std::string>& mod_ids,
                             bool backslash, bool* changed = nullptr);
std::string rewrite_ini_workshop_items(const std::string& text,
                                       const std::vector<std::uint64_t>& ids,
                                       bool* changed = nullptr);

}  // namespace pz
