#include "core/url.hpp"

#include "core/url_c.h"

namespace pz {
std::optional<std::uint64_t> parse_workshop_id(std::string_view in) {
    std::uint64_t id = 0;
    if (!pz_url_parse_workshop_id(in.data(), in.size(), &id)) return std::nullopt;
    return id;
}

std::vector<std::uint64_t> parse_workshop_ids(std::string_view in) {
    pz_url_ids ids{};
    if (!pz_url_parse_workshop_ids(in.data(), in.size(), &ids)) return {};

    std::vector<std::uint64_t> result;
    if (ids.count != 0) result.assign(ids.items, ids.items + ids.count);
    pz_url_ids_free(&ids);
    return result;
}
}  // namespace pz
