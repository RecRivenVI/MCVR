#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace mcvr::rigid {

// One persistent-model draw, written by RigidModelCapture.Batch in native byte order. The local
// vertex pointer is only read when the model is not resident; the transform stays per instance.
struct SubmissionRecord {
    uint64_t model;       // Java resource id << 32 | encoded geometry type
    uint64_t history;     // stable owner/draw identity, top bit set
    uint64_t vertices;    // local PBR quads, 128 bytes per vertex
    double x, y, z;       // owner origin before camera-relative float conversion
    float matrix[16];     // column-major local transform
    int32_t type;
    int32_t texture;
    int32_t vertexCount;
    int32_t mask;
    int32_t group;        // index into the submission's group-name table
    int32_t reserved;
};
static_assert(sizeof(SubmissionRecord) == 136);
static_assert(offsetof(SubmissionRecord, x) == 24);
static_assert(offsetof(SubmissionRecord, matrix) == 48);
static_assert(offsetof(SubmissionRecord, type) == 112);
static_assert(offsetof(SubmissionRecord, group) == 128);

inline void validate(const SubmissionRecord &record, size_t groupCount) {
    if (!record.model || !(record.history >> 63u) || !record.vertices || record.vertexCount <= 0 ||
        record.vertexCount % 4 || record.group < 0 || static_cast<size_t>(record.group) >= groupCount ||
        record.reserved != 0)
        throw std::invalid_argument("Invalid rigid model submission");
    for (float value : record.matrix)
        if (!std::isfinite(value)) throw std::invalid_argument("Non-finite rigid model transform");
    if (!std::isfinite(record.x) || !std::isfinite(record.y) || !std::isfinite(record.z))
        throw std::invalid_argument("Non-finite rigid model origin");
}

// Previous-frame record for one rigid instance: the camera-relative object-to-world rows and the
// geometry that frame actually referenced. Its owner keeps those buffers alive.
struct HistoryEntry {
    uint64_t history = 0;
    uint64_t model = 0;
    std::array<float, 12> transform{};
    uint64_t indexAddress = 0, positionAddress = 0;
    uint32_t vertexCount = 0, indexCount = 0;
};

// Sorted once per frame instead of allocating one tree node per instance. Duplicate identities
// resolve to the last submission, matching the previous map assignment semantics.
class HistoryFrame {
  public:
    void reserve(size_t count) { entries_.reserve(count); }
    void add(const HistoryEntry &entry) { entries_.push_back(entry); sorted_ = false; }
    void finish() {
        std::stable_sort(entries_.begin(), entries_.end(),
                         [](const HistoryEntry &a, const HistoryEntry &b) { return a.history < b.history; });
        // Keep the last entry of every identity.
        std::vector<HistoryEntry> unique;
        unique.reserve(entries_.size());
        for (size_t i = 0; i < entries_.size(); ++i)
            if (i + 1 == entries_.size() || entries_[i + 1].history != entries_[i].history) unique.push_back(entries_[i]);
        entries_.swap(unique);
        sorted_ = true;
    }
    const HistoryEntry *find(uint64_t history) const {
        if (!sorted_) throw std::logic_error("Rigid history frame queried before finish");
        auto it = std::lower_bound(entries_.begin(), entries_.end(), history,
                                   [](const HistoryEntry &entry, uint64_t key) { return entry.history < key; });
        return it != entries_.end() && it->history == history ? &*it : nullptr;
    }
    size_t size() const { return entries_.size(); }

  private:
    std::vector<HistoryEntry> entries_;
    bool sorted_ = true;
};

// Motion history exactly as the per-entity path assigns it: a previous transform whenever the
// identity existed, but previous geometry addresses only for the same model and topology.
struct PreviousGeometry {
    std::array<float, 12> transform;
    uint64_t indexAddress, positionAddress;
    bool hasTransform;
};
inline PreviousGeometry previous(const HistoryEntry *entry, uint64_t model, uint32_t vertexCount,
                                 uint32_t indexCount) {
    if (!entry) return {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, 0, 0, false};
    const bool sameGeometry = entry->model == model && entry->vertexCount == vertexCount && entry->indexCount == indexCount;
    return {entry->transform, sameGeometry ? entry->indexAddress : 0, sameGeometry ? entry->positionAddress : 0, true};
}

} // namespace mcvr::rigid
