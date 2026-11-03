#include "core/url.hpp"
#include <cctype>
#include <charconv>
#include <string>

namespace pz {
std::optional<std::uint64_t> parse_workshop_id(std::string_view in) {
    auto b = in.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return std::nullopt;
    auto e = in.find_last_not_of(" \t\r\n");
    std::string_view s = in.substr(b, e - b + 1);

    auto digits_at = [&](std::size_t pos) -> std::optional<std::uint64_t> {
        std::size_t end = pos;
        while (end < s.size() && std::isdigit(static_cast<unsigned char>(s[end]))) ++end;
        if (end == pos || end - pos > 20) return std::nullopt;
        std::uint64_t v = 0;
        auto r = std::from_chars(s.data() + pos, s.data() + end, v);
        if (r.ec != std::errc{}) return std::nullopt;
        return v;
    };

    if (s.find_first_not_of("0123456789") == std::string_view::npos)
        return digits_at(0);

    auto p = s.find("id=");
    if (p == std::string_view::npos) return std::nullopt;
    return digits_at(p + 3);
}
}  // namespace pz
