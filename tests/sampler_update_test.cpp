#include "core/render/sampler_update.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

struct Sampler {
    int filter, mipmap, address;
    int vkSamplingMode() const { return filter; }
    int vkMipmapMode() const { return mipmap; }
};
static void require(bool value) { if (!value) throw std::runtime_error("sampler update contract"); }

int main() {
    auto slot = std::make_shared<Sampler>(Sampler{0, 0, 2});
    std::weak_ptr<Sampler> first = slot;
    std::vector<std::shared_ptr<Sampler>> retired;
    int created = 0, published = 0;
    bool reload = false, fail = false;
    auto update = [&](int filter, int mipmap) {
        return mcvr::render::updateSampler(slot,
            [&](const auto &s) { return mcvr::render::matchesFilter(s, filter, mipmap); },
            [&] {
                if (fail) throw std::runtime_error("injected allocation failure");
                ++created;
                return std::make_shared<Sampler>(Sampler{filter, mipmap, slot->address});
            },
            [&](const auto &old) { retired.push_back(old); },
            [&] { if (!reload) ++published; });
    };
    for (int i = 0; i < 10000; ++i) require(!update(0, 0));
    require(created == 0 && published == 0 && retired.empty());
    require(update(0, 1)); // Only the mipmap mode changes: previously missed by ordinary textures.
    require(created == 1 && published == 1 && !first.expired() && slot->address == 2);
    auto owner = slot;
    fail = true;
    bool allocationFailed = false;
    try { update(1, 1); } catch (const std::runtime_error &error) {
        allocationFailed = std::string(error.what()) == "injected allocation failure";
    }
    require(allocationFailed);
    require(slot == owner && created == 1 && published == 1 && retired.size() == 1);
    fail = false; reload = true;
    require(update(1, 1)); require(!update(1, 1));
    require(created == 2 && published == 1 && retired.size() == 2);
    // Resource reload publishes all textures at its own commit boundary, not on every setFilter.
    ++published; reload = false;
    require(!update(1, 1) && published == 2);
    retired.clear(); require(first.expired());
    // A reused texture slot has its own current sampler, no ID-based cached setting survives it.
    slot = std::make_shared<Sampler>(Sampler{0, 0, 3});
    require(update(1, 1) && created == 3 && published == 3 && slot->address == 3);
    std::cout << "Sampler no-op, mipmap-only, failure, retirement, reload and new-owner contracts passed\n";
}
