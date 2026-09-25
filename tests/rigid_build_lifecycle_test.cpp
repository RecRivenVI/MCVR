#include "core/render/rigid_build_lifecycle.hpp"
#include <stdexcept>
int main() {
    auto require=[](bool v){ if(!v) throw std::runtime_error("Rigid build lifecycle mismatch"); };
    mcvr::RigidBuildLifecycle state;
    require(!state.submitted());
    int records=0;
    try { state.record([]{throw std::runtime_error("record failure");}); }
    catch(const std::runtime_error &) {}
    require(!state.submitted());
    require(state.record([&]{++records;}));
    require(!state.record([&]{++records;}));
    state.beginFrame(); // Failed/abandoned submission: build is still pending.
    require(state.record([&]{++records;}));
    require(state.submitted());
    require(!state.submitted());
    for(int frame=0;frame<20;++frame) {
        state.beginFrame();
        require(!state.record([&]{++records;}));
        require(!state.submitted());
    }
    require(records==2);
}
