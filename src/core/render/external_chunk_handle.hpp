#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace mcvr {

class ExternalChunkHandleTable {
  public:
    struct Allocation {
        int64_t handle;
        uint32_t slot;
        bool reused;
    };

    void reset(uint32_t primaryCount) {
        primaryCount_ = primaryCount;
        generations_.assign(primaryCount, 0);
        active_.assign(primaryCount, true);
        freeSlots_.clear();
    }

    Allocation allocate(uint32_t storageSize) {
        uint32_t slot;
        bool reused = !freeSlots_.empty();
        if (reused) {
            slot = freeSlots_.back();
            freeSlots_.pop_back();
        } else {
            slot = storageSize;
            generations_.push_back(1);
            active_.push_back(false);
        }
        active_[slot] = true;
        return {encode(slot, generations_[slot]), slot, reused};
    }

    std::optional<uint32_t> resolve(int64_t handle) const {
        const uint32_t slot = static_cast<uint32_t>(handle);
        const uint32_t generation = static_cast<uint32_t>(static_cast<uint64_t>(handle) >> 32);
        if (generation == 0) {
            return slot < primaryCount_ ? std::optional<uint32_t>(slot) : std::nullopt;
        }
        if (slot < primaryCount_ || slot >= generations_.size() || !active_[slot]
            || generations_[slot] != generation) {
            return std::nullopt;
        }
        return slot;
    }

    std::optional<uint32_t> release(int64_t handle) {
        auto slot = resolve(handle);
        if (!slot || *slot < primaryCount_) return std::nullopt;
        active_[*slot] = false;
        generations_[*slot] = generations_[*slot] >= 0x7FFFFFFFu
            ? 1u : generations_[*slot] + 1u;
        freeSlots_.push_back(*slot);
        return slot;
    }

    uint32_t generation(uint32_t slot) const {
        return slot < generations_.size() ? generations_[slot] : 0;
    }

    bool isCurrent(uint32_t slot, uint32_t generation) const {
        if (generation == 0) return slot < primaryCount_;
        return slot >= primaryCount_ && slot < generations_.size() && active_[slot]
            && generations_[slot] == generation;
    }

    static int64_t encode(uint32_t slot, uint32_t generation) {
        return static_cast<int64_t>((static_cast<uint64_t>(generation) << 32) | slot);
    }

  private:
    uint32_t primaryCount_ = 0;
    std::vector<uint32_t> generations_;
    std::vector<bool> active_;
    std::vector<uint32_t> freeSlots_;
};

} // namespace mcvr
