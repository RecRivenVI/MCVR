#include "core/render/ui_frame_batch.hpp"
#include <stdexcept>
void check(bool result) { if (!result) throw std::runtime_error("UI frame batch regression"); }
#include <array>
#include <iostream>
int main() {
    mcvr::ui::FrameBatch batch;
    const std::array<uint64_t, 2> views{9, 12};
    int executions = 0;
    batch.begin(views); batch.receive(9);
    try { batch.finish(true, [&]{ ++executions; }); check(false); } catch (const std::logic_error&) {}
    check(executions == 0);
    batch.begin(views); batch.receive(12); batch.receive(9);
    batch.finish(true, [&]{ ++executions; }); check(executions == 1);
    batch.begin(views); batch.receive(9);
    try { batch.receive(9); check(false); } catch (const std::logic_error&) {}
    batch.finish(false, [&]{ ++executions; }); check(executions == 1);
    batch.begin(views);
    try { batch.receive(99); check(false); } catch (const std::logic_error&) {}
    batch.receive(9); batch.receive(12); batch.finish(true, [&]{ ++executions; });
    check(executions == 2);
    std::cout << "same-frame membership, partial rejection, cancellation and reuse passed\n";
}
