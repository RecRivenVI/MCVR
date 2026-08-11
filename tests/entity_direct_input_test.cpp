#include "core/render/entity_conversion.hpp"

#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <iostream>

namespace {
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

template <typename Function>
void requireThrows(Function &&function, const char *message) {
    try {
        function();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error(message);
}

mcvr::EntityConvertJob job(uint32_t source, uint32_t output) {
    mcvr::EntityConvertJob value{};
    value.source = source;
    value.indexSource = source + 32;
    value.vertexOffset = output;
    value.indexOffset = output * 2;
    value.vertexCount = 64;
    value.indexCount = 96;
    return value;
}
} // namespace

int main() {
    try {
    static_assert(!std::is_copy_constructible_v<mcvr::EntityDirectInputBatch>);
    static_assert(!std::is_move_constructible_v<mcvr::EntityDirectInputBatch>);

    constexpr size_t stride = sizeof(vk::VertexFormat::PBRVertex);
    require(mcvr::checkedEntitySourceBytes(3, stride, 3 * stride) == 3 * stride,
        "v1 source byte count must match the declared vertex stride");
    requireThrows([&] { mcvr::checkedEntitySourceBytes(3, stride, 3 * stride - 1); },
        "short source byte count was accepted");
    requireThrows([&] { mcvr::checkedEntitySourceBytes(3, stride, 3 * stride + 1); },
        "long source byte count was accepted");
    requireThrows([&] { mcvr::validateEntitySourceAddress(nullptr, stride); },
        "null nonempty source address was accepted");
    mcvr::validateEntitySourceAddress(nullptr, 0);

    mcvr::EntityDirectInputLayout layout(4096);
    require(layout.append(512) == 0, "first direct source offset mismatch");
    require(layout.append(768) == 512, "second direct source offset mismatch");
    require(layout.totalBytes() == 1280 && layout.withinLimit(),
        "direct input layout total mismatch");
    mcvr::EntityDirectInputLayout bounded(1024);
    require(bounded.append(768) == 0, "bounded layout first offset mismatch");
    require(bounded.append(512) == 768 && !bounded.withinLimit(),
        "over-budget direct layout did not select the copy fallback");
    mcvr::EntityDirectInputLayout overflowing(std::numeric_limits<size_t>::max());
    overflowing.append(std::numeric_limits<size_t>::max());
    requireThrows([&] { overflowing.append(1); }, "overflowing direct layout was accepted");
    require(mcvr::entityWordCount(512) == 128, "word offset conversion mismatch");
    requireThrows([&] { mcvr::entityWordCount(3); }, "unaligned word offset was accepted");

    require(mcvr::EntityDirectInputBudget::reservedBytes() == 0,
        "direct input test process started with a leaked reservation");
    require(mcvr::EntityDirectInputBudget::tryReserve(mcvr::maxEntityDirectInputBytes),
        "full direct input budget reservation failed");
    require(!mcvr::EntityDirectInputBudget::tryReserve(1),
        "capacity fallback admitted an over-budget direct source");
    auto owner = std::make_shared<mcvr::EntityDirectInputBatch>();
    owner->byteCount = mcvr::maxEntityDirectInputBytes;
    owner->ownsBudgetReservation = true;
    require(mcvr::EntityDirectInputBudget::reservedBytes() == mcvr::maxEntityDirectInputBytes,
        "direct input owner did not retain its reservation");
    auto inFlight = owner;
    owner.reset();
    require(mcvr::EntityDirectInputBudget::reservedBytes() == mcvr::maxEntityDirectInputBytes,
        "submission release retired an owner still held by an in-flight conversion");
    inFlight.reset();
    require(mcvr::EntityDirectInputBudget::reservedBytes() == 0,
        "direct input owner did not release its reservation");
    bool reachedInjectedFailure=false;
    try {
        auto failed = std::make_shared<mcvr::EntityDirectInputBatch>();
        require(mcvr::EntityDirectInputBudget::tryReserve(4096), "failure fixture reserve failed");
        failed->byteCount=4096; failed->ownsBudgetReservation=true;
        reachedInjectedFailure=true;
        throw std::runtime_error("injected source-fill failure before publication");
    } catch (const std::runtime_error &) {}
    require(reachedInjectedFailure, "source-fill failure fixture did not reach the injected point");
    require(mcvr::EntityDirectInputBudget::reservedBytes() == 0,
        "failed unpublished source retained its direct-input reservation");

    std::vector<mcvr::EntitySourceJobGroup> groups;
    const auto sourceA = mcvr::entitySourceJobGroupIndex(groups, 0xA1u);
    mcvr::appendEntityTiles(groups[sourceA].tiles, job(8, 0));
    const auto sourceB = mcvr::entitySourceJobGroupIndex(groups, 0xB2u);
    mcvr::appendEntityTiles(groups[sourceB].tiles, job(0, 64));
    const auto sourceAAgain = mcvr::entitySourceJobGroupIndex(groups, 0xA1u);
    mcvr::appendEntityTiles(groups[sourceAAgain].tiles, job(40, 128));
    require(groups.size() == 2 && sourceA == sourceAAgain,
        "shared submission input was split into duplicate source groups");
    require(groups[0].tiles.size() == 4 && groups[1].tiles.size() == 2,
        "source group job counts mismatch");
    require(groups[0].tiles[0].vertexOffset == 0 && groups[1].tiles[0].vertexOffset == 64 &&
            groups[0].tiles[2].vertexOffset == 128,
        "grouping changed original global output offsets");
    require(groups[0].tiles[0].source == 8 && groups[0].tiles[2].source == 40,
        "grouping changed source-local word offsets");
    std::cout << "Input bounds, fallback, shared retirement, unwind and grouped output offsets passed\n";
    } catch(const std::exception &failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
