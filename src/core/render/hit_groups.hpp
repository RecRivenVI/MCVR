#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mcvr::hitgroups {

using Id = uint32_t;
// Instance records start with this group; the SBT maps it to the pass's shadow hit group.
inline constexpr Id shadow = 0;

// Process-wide, append-only interning of hit-group names. Producers with persistent geometry
// resolve their names once; per-frame scene assembly then appends integers instead of strings,
// and SBT setup resolves each distinct name once per pass instead of once per geometry.
class Registry {
  public:
    // Names come from RenderType/layer names and a fixed outline palette; exceeding this bound
    // indicates an unbounded producer, which must be fixed rather than silently interned.
    static constexpr size_t limit = 65536;

    Registry() { internLocked("shadow"); }

    Id intern(std::string_view name) {
        std::lock_guard lock(mutex_);
        return internLocked(name);
    }
    size_t size() const {
        std::lock_guard lock(mutex_);
        return names_.size();
    }
    std::string name(Id id) const {
        std::lock_guard lock(mutex_);
        if (id >= names_.size()) throw std::out_of_range("Unknown hit-group id");
        return names_[id];
    }
    // Table indexed by Id: shadow -> shadowIndex, a name present in the pass -> its index,
    // anything else -> fallbackIndex. Exactly the per-name rule of the former string lookup.
    template <class NameToIndex>
    std::vector<uint32_t> resolve(const NameToIndex &nameToIndex, uint32_t fallbackIndex, uint32_t shadowIndex) const {
        std::lock_guard lock(mutex_);
        std::vector<uint32_t> table(names_.size(), fallbackIndex);
        for (size_t id = 0; id < names_.size(); ++id) {
            if (id == shadow) { table[id] = shadowIndex; continue; }
            auto found = nameToIndex.find(names_[id]);
            if (found != nameToIndex.end()) table[id] = found->second;
        }
        return table;
    }

    static Registry &global() {
        static Registry registry;
        return registry;
    }

  private:
    struct Hash {
        using is_transparent = void;
        size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view>{}(value); }
    };
    Id internLocked(std::string_view name) {
        if (auto found = ids_.find(name); found != ids_.end()) return found->second;
        if (names_.size() >= limit) throw std::length_error("Hit-group name registry exceeded its bound");
        const auto id = static_cast<Id>(names_.size());
        names_.emplace_back(name);
        ids_.emplace(names_.back(), id);
        return id;
    }
    mutable std::mutex mutex_;
    std::vector<std::string> names_;
    std::unordered_map<std::string, Id, Hash, std::equal_to<>> ids_;
};

inline Id intern(std::string_view name) { return Registry::global().intern(name); }

} // namespace mcvr::hitgroups
