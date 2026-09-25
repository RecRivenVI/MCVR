#include "core/render/chunk_scene_metadata.hpp"
#include <map>
#include <cstring>
#include <iostream>

namespace {
void check(bool value) { if (!value) throw std::runtime_error("chunk scene regression"); }
struct Published {
    std::vector<uint64_t> indices{11, 12}, positions{21, 22}, materials{31, 32};
    std::vector<std::string> groups{"block"}; // Published fallback must survive caching.
    std::vector<uint32_t> flags{mcvr::faces::back | 1u, 0};
};
auto make(const std::shared_ptr<Published> &p) {
    return std::make_shared<mcvr::ChunkSceneMetadata>(p, 2, p->indices, p->positions,
        p->materials, p->groups, p->flags);
}
void appendCheck(const std::shared_ptr<mcvr::ChunkSceneMetadata> &metadata, const Published &p, bool custom) {
    std::vector<std::string> groups{"prefix"};
    std::vector<uint64_t> index{7}, position{8}, material{9}, lastI{5}, lastP{6};
    std::vector<vk::VertexFormat::InstanceAppearance> appearances(1);
    metadata->append(custom, groups, index, position, material, lastI, lastP, appearances);
    check(groups == std::vector<std::string>({"prefix", "shadow", "block", "default"}));
    check(index == std::vector<uint64_t>({7, p.indices[0], p.indices[1]}));
    check(position == std::vector<uint64_t>({8, p.positions[0], p.positions[1]}));
    check(material == std::vector<uint64_t>({9, p.materials[0], p.materials[1]}));
    check(lastI == std::vector<uint64_t>({5, custom ? p.indices[0] : 0, custom ? p.indices[1] : 0}));
    check(lastP == std::vector<uint64_t>({6, custom ? p.positions[0] : 0, custom ? p.positions[1] : 0}));
    const mcvr::faces::ModelRules original(p.flags);
    for (size_t j = 0; j < 2; ++j) {
        vk::VertexFormat::InstanceAppearance expected{};
        expected.colorMultiply = glm::vec4(1); expected.colorReplace = glm::vec4(1);
        expected.fluidProgress = 1; expected.materialFlags = original.shaderFlags(p.flags[j]);
        check(std::memcmp(&expected, &appearances[j + 1], sizeof(expected)) == 0);
    }
    for (float sign : {1.0f, -1.0f}) {
        VkTransformMatrixKHR transform{{{sign, 0, 0, 2}, {0, 1, 0, 3}, {0, 0, 1, 4}}};
        check(metadata->faces.instanceFlags(transform) == original.instanceFlags(transform));
    }
}
}
int main() {
    mcvr::ChunkSceneCache cache;
    auto source = std::make_shared<Published>();
    std::weak_ptr<Published> oldOwner = source;
    mcvr::ChunkSceneKey key{3, 2, {source.get()}};
    int creates = 0;
    auto frame = cache.get(key, [&] { ++creates; return make(source); });
    for (int i = 0; i < 100; ++i) {
        check(cache.get(key, [&] { ++creates; return make(source); }) == frame);
        appendCheck(frame, *source, i % 2); // Same geometry, changing custom transform status.
    }
    check(creates == 1 && cache.builds() == 1);
    auto next = std::make_shared<Published>();
    next->positions = {121, 122}; next->flags = {mcvr::faces::front, mcvr::faces::front};
    auto nextKey = key; nextKey.version++;
    try { cache.get(nextKey, []() -> std::shared_ptr<mcvr::ChunkSceneMetadata> { throw std::bad_alloc(); }); check(false); }
    catch (const std::bad_alloc &) {}
    check(cache.get(key, [&] { check(false); return make(source); }) == frame);
    auto nextFrame = cache.get(nextKey, [&] { ++creates; return make(next); });
    check(nextFrame != frame && cache.builds() == 2);
    appendCheck(nextFrame, *next, true);
    source.reset(); cache.reset(); // Unload/reload cannot invalidate already recorded frames.
    check(!oldOwner.expired());
    auto retained = std::static_pointer_cast<Published>(frame->owner);
    appendCheck(frame, *retained, false);
    retained.reset(); frame.reset(); check(oldOwner.expired());
    cache.get(nextKey, [&] { ++creates; return make(next); });
    check(cache.builds() == 3);
    // Resource replacement at an otherwise identical generation is also invalidating.
    nextKey.resources[4] = next.get();
    cache.get(nextKey, [&] { ++creates; return make(next); });
    check(cache.builds() == 4);
    try {
        mcvr::ChunkSceneMetadata broken(next, 3, next->indices, next->positions,
            next->materials, next->groups, next->flags); check(false);
    } catch (const std::invalid_argument &) {}

    using Key = std::shared_ptr<int>;
    using History = std::map<Key, glm::dmat4, std::owner_less<Key>>;
    History optimized, reference;
    auto normal = std::make_shared<int>(1), custom = std::make_shared<int>(2);
    glm::dmat4 current(1), previous(1);
    current[0][0] = -1; // Mirrored current transform still recomputed every frame.
    current[3] = glm::dvec4(30000000.25 - 30000000.125, -4, 8, 1);
    previous[3] = glm::dvec4(0.0625, -3, 7, 1);
    for (auto *history : {&optimized, &reference}) {
        mcvr::recordChunkTransform(*history, normal, previous, false, history == &reference);
        mcvr::recordChunkTransform(*history, custom, previous, true, history == &reference);
    }
    check(optimized.size() == 1 && reference.size() == 2);
    for (bool moving : {false, true}) {
        auto object = moving ? custom : normal;
        check(mcvr::previousChunkTransform(optimized, object, current, moving) ==
              mcvr::previousChunkTransform(reference, object, current, moving));
    }
    check(mcvr::previousChunkTransform(optimized, normal, current, false)[3][0] == 0.125);
    auto reused = std::make_shared<int>(2); // Same numeric slot, different published owner.
    check(mcvr::previousChunkTransform(optimized, reused, current, true) == current);
    auto pendingExternal = std::make_shared<int>(3);
    mcvr::recordChunkTransform(optimized, pendingExternal, previous, true, false);
    // First transform arrives after an earlier ordinary frame: retain that exact history.
    check(mcvr::previousChunkTransform(optimized, pendingExternal, current, true) == previous);
    History otherView;
    check(mcvr::previousChunkTransform(otherView, custom, current, true) == current);
    optimized.clear(); // World / history reset.
    check(mcvr::previousChunkTransform(optimized, custom, current, true) == current);
    std::cout << "cache append, faces, invalidation, retained owners and view history passed\n";
}
