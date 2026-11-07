#pragma once
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>
namespace pz {
// Accepts "123456789" or any URL containing "id=123456789".
std::optional<std::uint64_t> parse_workshop_id(std::string_view input);

// Parses one or more whitespace-separated Workshop IDs/URLs, removing
// duplicates. Returns empty when any token does not contain a Workshop ID.
std::vector<std::uint64_t> parse_workshop_ids(std::string_view input);
}  // namespace pz
