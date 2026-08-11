#include "core/render/chunk_trace.hpp"
#include <stdexcept>
#include <thread>
#include <vector>

void require(bool value) {
    if (!value) throw std::runtime_error("chunk trace regression");
}
int main() {
    using namespace mcvr::chunkTrace;
    if (!enabled) {
        note("disabled-test", 1, 2, 3, 4);
        require(count == 0 && drain() == "disabled");
        return 0;
    }
    auto before = now();
    for (int i = 0; i < 17000; ++i) note("enqueue", 7, i, 0, 11);
    require(count == events.size() && dropped == 17000 - events.size());
    auto output = drain();
    require(output.starts_with("clock_ns=") && output.find(",enqueue,7,16999,0,11") != std::string::npos);
    require(output.find(",enqueue,7,0,0,11") == std::string::npos);
    require(count == 0 && now() >= before);
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker)
        workers.emplace_back([worker] {
            for (int i = 0; i < 100; ++i) note("worker", worker, i);
        });
    for (auto &worker : workers) worker.join();
    require(count == 400);
    drain();
    require(count == 0);
}
