#pragma once
#include <cstdint>
#include <optional>
#include <string_view>
namespace pz {
// Accepts "123456789" or any URL containing "id=123456789".
std::optional<std::uint64_t> parse_workshop_id(std::string_view input);
}  // namespace pz
