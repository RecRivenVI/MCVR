#pragma once
#include <stdexcept>
#include <utility>
#include <vector>

namespace mcvr::ui {
// Strongly hold the selected owners through the wait and shutdown. A failed
// wait must leave all histories alive, and an unsubmitted recording cannot be
// made safe by waiting on an earlier submission's fence.
template<class Range, class RecordedNow, class Wait, class Close>
void retireHistories(const Range &weakOwners, RecordedNow recordedNow, Wait wait, Close close) {
    using Owner = decltype(weakOwners.front().lock());
    std::vector<Owner> live;
    for (const auto &weak : weakOwners) {
        if (auto owner = weak.lock(); owner && owner->ready) {
            if (recordedNow(*owner))
                throw std::logic_error("UI PT capacity cannot change after recording another batch in the same frame");
            live.push_back(std::move(owner));
        }
    }
    if (live.empty()) return;
    wait();
    for (auto &owner : live) close(*owner);
}
}
