#include "core/render/ui_history_retirement.hpp"
#include <memory>
#include <iostream>
#include <stdexcept>

struct History { bool ready=true, recordedNow=false; };
void require(bool condition) { if (!condition) throw std::runtime_error("history retirement order"); }
int main() {
    try {
        auto a=std::make_shared<History>(), b=std::make_shared<History>();
        std::vector<std::weak_ptr<History>> owners{a,b,{}};
        bool waited=false;
        auto current=[](const History &h) { return h.recordedNow; };
        auto close=[&](History &h) { require(waited); h.ready=false; };
        b->recordedNow=true;
        try { mcvr::ui::retireHistories(owners,current,[&]{ waited=true; },close); return 1; }
        catch (const std::logic_error &) {}
        require(!waited && a->ready && b->ready);
        b->recordedNow=false;
        try { mcvr::ui::retireHistories(owners,current,[]{throw std::runtime_error("fence failed");},close); return 1; }
        catch (const std::runtime_error &) {}
        require(a->ready && b->ready);
        mcvr::ui::retireHistories(owners,current,[&]{ waited=true; },close);
        require(!a->ready && !b->ready);
        waited=false;
        mcvr::ui::retireHistories(owners,current,[&]{ waited=true; },close);
        require(!waited);
        std::cout << "History retirement waits before close, rejects current recording, preserves owners on failure\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
