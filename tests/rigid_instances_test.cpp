#include "core/render/rigid_instances.hpp"

#include <cstring>
#include <limits>
#include <stdexcept>

using namespace mcvr::rigid;

namespace {
void require(bool value, const char *what) {
    if (!value) throw std::runtime_error(what);
}
template <class F> bool throwsInvalid(F &&f) {
    try { f(); } catch (const std::invalid_argument &) { return true; }
    return false;
}
SubmissionRecord valid() {
    SubmissionRecord r{};
    r.model = (7ull << 32) | 3;
    r.history = (1ull << 63) | 42;
    r.vertices = 0x1000;
    r.vertexCount = 8;
    r.group = 1;
    for (int i = 0; i < 16; ++i) r.matrix[i] = i % 5 == 0 ? 1.0f : 0.0f;
    return r;
}
HistoryEntry entry(uint64_t history, uint64_t model, float tx) {
    HistoryEntry e{.history = history, .model = model, .indexAddress = 100 + model, .positionAddress = 200 + model,
                   .vertexCount = 8, .indexCount = 12};
    e.transform = {1, 0, 0, tx, 0, 1, 0, 0, 0, 0, 1, 0};
    return e;
}
} // namespace

int main() {
    // Records: accepted shape, then each rejected field.
    validate(valid(), 2);
    require(throwsInvalid([] { validate(valid(), 1); }), "group index must be inside the table");
    require(throwsInvalid([] { auto r = valid(); r.history = 42; validate(r, 2); }), "history needs its namespace bit");
    require(throwsInvalid([] { auto r = valid(); r.model = 0; validate(r, 2); }), "model key required");
    require(throwsInvalid([] { auto r = valid(); r.vertices = 0; validate(r, 2); }), "vertex pointer required");
    require(throwsInvalid([] { auto r = valid(); r.vertexCount = 6; validate(r, 2); }), "quads only");
    require(throwsInvalid([] { auto r = valid(); r.vertexCount = 0; validate(r, 2); }), "non-empty");
    require(throwsInvalid([] { auto r = valid(); r.group = -1; validate(r, 2); }), "negative group");
    require(throwsInvalid([] { auto r = valid(); r.reserved = 1; validate(r, 2); }), "reserved must be zero");
    require(throwsInvalid([] { auto r = valid(); r.matrix[5] = std::numeric_limits<float>::infinity(); validate(r, 2); }),
            "finite transform");
    require(throwsInvalid([] { auto r = valid(); r.y = std::numeric_limits<double>::quiet_NaN(); validate(r, 2); }),
            "finite origin");

    // History: unsorted input, duplicate identity keeps the last submission.
    HistoryFrame frame;
    frame.add(entry(30, 1, 3));
    frame.add(entry(10, 1, 1));
    frame.add(entry(20, 2, 2));
    frame.add(entry(10, 1, 9));
    bool queriedEarly = false;
    try { (void)frame.find(10); } catch (const std::logic_error &) { queriedEarly = true; }
    require(queriedEarly, "unsorted history cannot be queried");
    frame.finish();
    require(frame.size() == 3, "duplicates collapse");
    require(frame.find(10) && frame.find(10)->transform[3] == 9, "last duplicate wins");
    require(frame.find(20) && frame.find(30) && !frame.find(15) && !frame.find(40), "exact lookup");

    // Previous geometry: transform whenever the identity existed; addresses only for the same topology.
    auto none = previous(nullptr, 1, 8, 12);
    require(!none.hasTransform && none.indexAddress == 0 && none.transform[0] == 1 && none.transform[5] == 1 &&
                none.transform[10] == 1 && none.transform[3] == 0, "missing identity uses identity rows");
    auto same = previous(frame.find(10), 1, 8, 12);
    require(same.hasTransform && same.indexAddress == 101 && same.positionAddress == 201 && same.transform[3] == 9,
            "same model keeps geometry history");
    auto replaced = previous(frame.find(10), 2, 8, 12);
    require(replaced.hasTransform && replaced.indexAddress == 0 && replaced.positionAddress == 0 && replaced.transform[3] == 9,
            "different model keeps only the transform");
    auto changedTopology = previous(frame.find(10), 1, 4, 6);
    require(changedTopology.indexAddress == 0 && changedTopology.positionAddress == 0, "topology change drops addresses");

    HistoryFrame empty;
    empty.finish();
    require(!empty.find(1) && empty.size() == 0, "empty frame");
    return 0;
}
