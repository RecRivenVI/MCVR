#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace mcvr::render {
// All accesses are serialized by Buffers::mtx_. A ticket describes CPU staging
// ownership, not GPU completion; queued/in-flight buffers retain their own owners.
class PersistentWriteBatch {
  public:
    using Ticket = uint32_t;
    explicit PersistentWriteBatch(Ticket initial = 1) : current_(initial) {
        if (initial == 0) throw std::invalid_argument("Zero persistent write batch");
    }
    bool open(Ticket ticket) const noexcept { return ticket == current_; }
    void mark(Ticket &ticket) const noexcept { ticket = current_; }

    template<class ResetTickets> void close(ResetTickets &&resetTickets, bool referenceSweep = false) {
        const bool rollover = current_ == std::numeric_limits<Ticket>::max();
        if (referenceSweep || rollover) {
            // Do not let a stale owner become current after wraparound.
            // Normal closes are O(1); the explicit audit reference intentionally
            // repeats the old sweep for a same-binary performance comparison.
            resetTickets();
        }
        current_ = rollover ? 1 : current_ + 1;
    }
  private:
    Ticket current_;
};

// This is the actual range-staging path, also exercised with owned test buffers.
// Factories and retention remain with Buffers so no Vulkan lifetime rule changes.
template<class Record, class Pending, class Create, class Retain>
void stagePersistentRange(Record &record, PersistentWriteBatch &batch, Pending &pending,
                          uint8_t *source, uint32_t size, uint32_t offset,
                          Create &&create, Retain &&retain) {
    if (offset > record.size || size > record.size - offset)
        throw std::out_of_range("Persistent buffer range upload exceeds storage");
    if (size == 0) return;
    if (!source) throw std::invalid_argument("Null persistent buffer range upload");
    if (!batch.open(record.writeBatch)) {
        auto replacement = create(std::max(4u, record.size), record.usage);
        if (record.size != 0)
            replacement->uploadToStagingBuffer(record.buffer->mappedPtr(), record.size, 0);
        retain(record.buffer);
        record.buffer = std::move(replacement);
        batch.mark(record.writeBatch);
    }
    record.buffer->uploadToStagingBuffer(source, size, offset);
    if (std::find(pending.begin(), pending.end(), record.buffer) == pending.end())
        pending.push_back(record.buffer);
}
} // namespace mcvr::render
