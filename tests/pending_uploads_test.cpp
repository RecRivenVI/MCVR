#include "core/render/pending_uploads.hpp"
#include "core/render/upload_retirement.hpp"
#include <stdexcept>

void check(bool value) { if (!value) throw std::runtime_error("pending upload regression"); }
struct Payload { bool staged = true; int uploads = 0; };
int main() {
    auto pending = std::make_shared<std::vector<std::shared_ptr<Payload>>>();
    std::vector<decltype(pending)> retired;
    auto flush = [&] {
        auto batch = mcvr::takePendingUploads(pending);
        retired.push_back(batch);
        for (auto &value : *batch) {
            check(value->staged); value->staged = false; ++value->uploads;
        }
    };
    auto first = std::make_shared<Payload>();
    auto second = std::make_shared<Payload>();
    pending->push_back(first); flush();
    pending->push_back(second); flush(); flush();
    check(first->uploads == 1 && second->uploads == 1 && pending->empty());
    std::weak_ptr<Payload> lifetime = first;
    first.reset(); check(!lifetime.expired());
    retired.clear(); check(lifetime.expired());

    struct Batch { VkResult status; std::shared_ptr<int> payload; };
    auto owned = std::make_shared<int>(1);
    std::weak_ptr<int> inFlight = owned;
    std::vector<Batch> batches{{VK_SUCCESS, {}}, {VK_NOT_READY, owned},
        {VK_ERROR_DEVICE_LOST, owned}, {VK_SUCCESS, owned}};
    owned.reset();
    int polls = 0, recycled = 0, nextStage = 0;
    auto poll = [&](const Batch &batch) { ++polls; return batch.status; };
    auto recycle = [&](const Batch &) { ++recycled; };
    auto report = [&](VkResult result) {
        mcvr::failure::record(mcvr::failure::Kind::runtime, result, "vkGetFenceStatus(texture upload)");
    };
    bool caught = false;
    try {
        mcvr::render::retireUploads(batches, poll, recycle, report);
        ++nextStage;
    } catch (const mcvr::failure::FatalError &error) {
        caught = true;
        check(error.result() == VK_ERROR_DEVICE_LOST);
        check(error.operation() == "vkGetFenceStatus(texture upload)");
    }
    check(caught && nextStage == 0 && polls == 3 && recycled == 1);
    check(batches.size() == 3 && !inFlight.expired());
    try { mcvr::render::retireUploads(batches, poll, recycle, report); }
    catch (const mcvr::failure::FatalError &) {}
    check(polls == 3 && recycled == 1); // sticky fatal forbids even a second poll
    batches.clear(); check(inFlight.expired()); // stand-in for device-safe close
    mcvr::failure::clearForInitialization(); // test only; no real GPU work exists
    batches = {{VK_SUCCESS, {}}, {VK_NOT_READY, {}}};
    mcvr::render::retireUploads(batches, poll, recycle, report);
    check(batches.size() == 1 && recycled == 2);
    batches[0].status = VK_SUCCESS;
    mcvr::render::retireUploads(batches, poll, recycle, report);
    check(batches.empty() && recycled == 3);
}
