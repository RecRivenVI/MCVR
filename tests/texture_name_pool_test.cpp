#include "core/render/texture_name_pool.hpp"

#include <iostream>
#include <stdexcept>

int main() {
    try {
        mcvr::TextureNamePool pool(4);
        const auto a = pool.allocate(), b = pool.allocate(), c = pool.allocate();
        if (a != 1 || b != 2 || c != 3) throw std::runtime_error("unexpected initial names");
        bool exhausted = false;
        try { (void) pool.allocate(); } catch (const std::overflow_error &) { exhausted = true; }
        if (!exhausted) throw std::runtime_error("descriptor capacity was not enforced");
        pool.release(b);
        pool.release(b);
        if (pool.allocate() != b) throw std::runtime_error("released name was not reused");
        bool duplicateReuse = false;
        try { (void) pool.allocate(); } catch (const std::overflow_error &) { duplicateReuse = true; }
        if (!duplicateReuse) throw std::runtime_error("double release duplicated a live name");
        for (int i = 0; i < 100000; ++i) { pool.release(b); if (pool.allocate() != b) return 2; }
        if (pool.highWatermark() != 4) throw std::runtime_error("name pool grew under churn");
        pool.reset();
        if (pool.allocate() != 1) throw std::runtime_error("reset did not restore namespace");
        std::cout << "Texture name reuse and descriptor bound passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
