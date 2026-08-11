#include "core/render/hit_groups.hpp"

#include <stdexcept>
#include <string>
#include <unordered_map>

using namespace mcvr::hitgroups;

namespace {
void require(bool value, const char *what) {
    if (!value) throw std::runtime_error(what);
}
} // namespace

int main() {
    Registry registry;
    require(registry.size() == 1 && registry.name(shadow) == "shadow", "shadow is id 0");
    require(registry.intern("shadow") == shadow, "shadow name maps to the shadow id");
    const Id block = registry.intern("block");
    const Id outline = registry.intern(std::string("priority_outline_red"));
    require(block != shadow && outline != block, "distinct names get distinct ids");
    require(registry.intern(std::string_view("block")) == block, "ids are stable");
    require(registry.size() == 3 && registry.name(outline) == "priority_outline_red", "append-only table");

    // Same rule as the former per-geometry string lookup: shadow -> shadow index, known -> its
    // index, unknown -> fallback. A later pipeline map simply produces a new table.
    std::unordered_map<std::string, uint32_t> pass{{"block", 7}, {"shadow", 99}};
    auto table = registry.resolve(pass, 1, 2);
    require(table.size() == 3, "one entry per registered name");
    require(table[shadow] == 2 && table[block] == 7 && table[outline] == 1, "resolution rule");
    pass["priority_outline_red"] = 5;
    table = registry.resolve(pass, 1, 2);
    require(table[outline] == 5 && table[block] == 7, "resolution follows the current pass map");

    bool unknown = false;
    try { (void)registry.name(1000); } catch (const std::out_of_range &) { unknown = true; }
    require(unknown, "unknown ids are rejected");

    Registry bounded;
    bool overflow = false;
    try {
        for (size_t i = 0; i <= Registry::limit; ++i) bounded.intern("name-" + std::to_string(i));
    } catch (const std::length_error &) { overflow = true; }
    require(overflow && bounded.size() == Registry::limit, "registry growth is bounded");
    return 0;
}
