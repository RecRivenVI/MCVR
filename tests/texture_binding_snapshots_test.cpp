#include "core/render/texture_binding_snapshots.hpp"
#include <iostream>

struct Image { int content; };
using Table = std::map<unsigned, std::shared_ptr<Image>>;
int main() {
    try {
        mcvr::TextureBindingSnapshots<std::shared_ptr<Image>, Table> bindings;
        int creates = 0;
        auto factory = [&](const auto &resources) { ++creates; return std::make_shared<Table>(resources); };
        auto old = std::make_shared<Image>(Image{11});
        std::weak_ptr<Image> lifetime = old;
        bindings.bind(7, old);
        auto recorded = bindings.acquire(factory);
        if (bindings.bind(7, old) || bindings.acquire(factory) != recorded || creates != 1)
            throw std::runtime_error("unchanged binding recreated a descriptor generation");
        old.reset();
        bindings.bind(7, std::make_shared<Image>(Image{42}));
        auto next = bindings.acquire(factory);
        if (recorded->at(7)->content != 11 || next->at(7)->content != 42 || lifetime.expired())
            throw std::runtime_error("reuse changed a recorded descriptor or retired its image early");
        recorded.reset(); // frame fence retirement, after commands finish
        if (!lifetime.expired()) throw std::runtime_error("retired generation leaked its image");
        bool rejected = false;
        try { bindings.bind(4096, {}); } catch (const std::out_of_range &) { rejected = true; }
        if (!rejected || creates != 2) throw std::runtime_error("capacity or generation count");
        std::cout << "Texture descriptor snapshot ownership passed\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
