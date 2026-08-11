#include "core/render/chunk_scheduling.hpp"
#include <map>
#include <set>
#include <stdexcept>
#include <vector>
using namespace mcvr::chunkScheduling;
using namespace std::chrono_literals;
void require(bool b) {
    if (!b) throw std::runtime_error("chunk scheduling regression");
}
int main() {
    auto now = Clock::now();
    Key loading{0, now - 1ms, 0}, interaction{1, now, 999}, aged{0, now - 251ms, 99999};
    require(before(interaction, loading, now));
    require(before(aged, interaction, now));
    require(ready(true, 1, 128, 0ms));
    require(!ready(false, 1, 128, 24ms));
    require(ready(false, 1, 128, 25ms));
    require(ready(false, 128, 128, 0ms));
    require(withinBudget(0, 0, 500ms));
    require(!withinBudget(1, uploadBudget, 0ms));
    require(!withinBudget(1, 0, preparationBudget));
    // Large geometry is admitted once, then no additional work is admitted in that round.
    require(!withinBudget(1, uploadBudget * 2, 0ms));
    for (auto frame : {8ms, 16ms, 50ms, 100ms}) {
        require(ready(true, 1, 128, frame));
        if (frame >= 25ms) require(ready(false, 1, 128, frame));
    }
    std::set<int64_t> queue;
    std::map<int64_t, Key> keys;
    for (int64_t i = 0; i < 1000; ++i) {
        queue.insert(i);
        keys.emplace(i, Key{0, now, double(i)});
    }
    queue.insert(5000);
    keys.emplace(5000, Key{1, now, 1e9});
    auto key = [&](int64_t id) { return keys.at(id); };
    auto bytes = [](int64_t) { return 1024ull; };
    auto batch = selectBatch(queue, 4, now, key, bytes, 0);
    require(batch == std::vector<int64_t>({5000, 0, 1, 2}));
    // Aged background gets a reserved slot without moving all old work before interaction.
    keys[999].queued = now - 251ms;
    batch = selectBatch(queue, 4, now, key, bytes, 0);
    require(batch == std::vector<int64_t>({5000, 999, 0, 1}));
    require(queue.size() == 1001); // selection does not silently drop deferred owners
    require(selectBatch(queue, 4, now, key, bytes, inFlightInputBudget).empty());
    require(selectBatch(queue, 4, now, key, bytes, inFlightInputBudget - 1024).size() == 1);
    auto huge = [](int64_t) { return inFlightInputBudget + 1; };
    require(selectBatch(queue, 4, now, key, huge, 0).size() == 1);
    require(selectBatch(queue, 4, now, key, huge, 1).empty());
    // A thousand old background sections cannot bury a new interaction.
    for (auto &[id, k] : keys)
        if (id != 5000) k.queued = now - 1s;
    for (uint64_t offset = 0; offset < 4; ++offset) {
        batch = selectBatch(queue, 4, now, key, bytes, 0, offset);
        require(std::find(batch.begin(), batch.end(), 5000) != batch.end());
    }
    // Continuous fresh interaction, one-section batches: each lane makes bounded progress.
    uint64_t serial = 0;
    size_t backgroundDrained = 0, interactionsDrained = 0;
    queue.erase(5000);
    for (int n = 0; n < 400; ++n) {
        int64_t id = 10000 + n;
        queue.insert(id);
        keys[id] = Key{1, now, 0};
        auto selected = selectBatch(queue, 1, now, key, bytes, 0, serial);
        require(selected.size() == 1);
        serial += selected.size();
        for (auto chosen : selected) {
            if (keys[chosen].priority == 0)
                ++backgroundDrained;
            else
                ++interactionsDrained;
            require(queue.erase(chosen) == 1);
        }
    }
    require(backgroundDrained == 100);
    require(interactionsDrained == 300);
    // The production queue retains every deferred owner and drains after new input stops.
    size_t remaining = queue.size();
    while (!queue.empty()) {
        auto selected = selectBatch(queue, 4, now, key, bytes, 0, serial);
        require(!selected.empty());
        serial += selected.size();
        for (auto id : selected) require(queue.erase(id) == 1);
        remaining -= selected.size();
    }
    require(remaining == 0);
}
