#include "core/render/submitted_geometry_cache.hpp"
#include <iostream>

int main() {
    mcvr::SubmittedGeometryCache<int> cache;
    auto first = std::make_shared<int>(17);
    cache.beginFrame();
    cache.store(1, first);
    if (cache.find(1)) return 1; // CPU creation and recording do not imply successful submission.
    cache.beginFrame();
    if (cache.find(1)) return 2; // Abandoned/unsubmitted frame still cannot be reused.
    cache.submitted();
    if (cache.find(1) != first || cache.find(2)) return 3;
    auto inFlight = cache.find(1);
    std::weak_ptr<int> alive = first;
    cache.store(2, std::make_shared<int>(29));
    first.reset();
    if (alive.expired() || *inFlight != 17 || cache.find(1) || cache.find(2)) return 4;
    cache.submitted();
    if (!cache.find(2) || *cache.find(2) != 29) return 5;
    cache.beginFrame();
    cache.beginFrame(); // Unused for a complete frame: release the cache's ownership.
    if (cache.find(2)) return 6;
    cache.clear(); cache.clear();
    if (alive.expired()) return 7; // Cache eviction does not invalidate real in-flight ownership.
    inFlight.reset();
    if (!alive.expired()) return 8;
    std::cout << "Submitted-only cache, revisions, inactivity and in-flight ownership passed\n";
}
