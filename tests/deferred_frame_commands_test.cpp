#include "core/util/deferred_frame_commands.hpp"
#include <stdexcept>
#include <iostream>

struct Commands {
    int begins = 0, ends = 0;
    bool fail = false;
    void begin() { if (fail) throw std::runtime_error("begin failed"); ++begins; }
    void end() { ++ends; }
};
int main() {
    auto check = [](bool ok) { if (!ok) throw std::runtime_error("Deferred recording contract"); };
    mcvr::DeferredFrameCommands<Commands> commands;
    int allocations = 0;
    auto factory = [&] { ++allocations; return std::make_shared<Commands>(); };
    for (int frame = 0; frame < 500; ++frame) {
        commands.newFrame(); commands.end();
        check(!commands.active() && !commands.buffer());
    }
    check(allocations == 0);
    auto shared = commands.storage(factory);
    check(!commands.active() && shared->begins == 0);
    commands.begin(factory); commands.begin(factory);
    check(commands.active() && shared->begins == 1 && allocations == 1);
    commands.end(); check(shared->ends == 1);
    commands.newFrame(); commands.end(); check(shared->ends == 1);
    shared->fail = true;
    try { commands.begin(factory); check(false); } catch (const std::runtime_error &) {}
    check(!commands.active());
    shared->fail = false;
    commands.begin(factory); commands.end();
    check(shared->begins == 2 && shared->ends == 2 && allocations == 1);
    std::cout << "PASS: unused frames allocate/record/submit nothing; shared storage, retry and reuse\n";
}
