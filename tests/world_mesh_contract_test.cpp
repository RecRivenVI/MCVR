#include "core/render/world_mesh_audit.hpp"
#include "core/render/world_mesh_contract.hpp"
#include "core/render/material_faces.hpp"

#include <cassert>

int main() {
    bool incompleteRejected=false;
    try {WorldMeshContract::validateSourceSurfaceCount(7,5);} catch(const std::invalid_argument &) {incompleteRejected=true;}
    assert(incompleteRejected);
    for(size_t n=0;n<12;++n) {WorldMeshContract::validateSourceSurfaceCount(5,n);WorldMeshContract::validateSourceSurfaceCount(6,n);}
    const std::array<std::vector<uint32_t>, 7> expectedStrips{
        std::vector<uint32_t>{}, {}, {}, {0, 1, 2},
        {0, 1, 2, 2, 1, 3}, {0, 1, 2, 2, 1, 3, 2, 3, 4},
        {0, 1, 2, 2, 1, 3, 2, 3, 4, 4, 3, 5}};
    for (size_t count = 0; count < expectedStrips.size(); ++count) {
        auto indices = WorldMeshContract::sequentialSurfaceTriangles(5, count);
        assert(indices == expectedStrips[count]);
        assert(std::all_of(indices.begin(), indices.end(), [count](auto index) { return index < count; }));
    }
    static_assert(static_cast<int>(WorldMeshAuditState::Missing) == 0);
    static_assert(static_cast<int>(WorldMeshAuditState::Queued) == 1);
    static_assert(static_cast<int>(WorldMeshAuditState::Committed) == 2);
    static_assert(static_cast<int>(WorldMeshAuditState::Built) == 3);
    static_assert(static_cast<int>(WorldMeshAuditState::RolledBack) == 4);
    static_assert(static_cast<int>(WorldMeshAuditState::Stale) == 5);
    const std::vector<uint32_t> sortedQuads{4, 5, 6, 6, 7, 4, 0, 1, 2, 2, 3, 0};
    assert(WorldMeshContract::triangulate(7, sortedQuads) == sortedQuads);

    // Simulated's source emits a second spring quad with the same corners in
    // reverse cyclic order and a different UV lane. On a warped quad this
    // necessarily chooses the other diagonal. The world sink must retain both.
    const std::vector<uint32_t> springQuads{0, 1, 2, 2, 3, 0,
                                            4, 5, 6, 6, 7, 4};
    assert(WorldMeshContract::triangulate(7, springQuads) == springQuads);
    const std::array<int, 4> twinCornersInBase{3, 2, 1, 0};
    assert(twinCornersInBase[0] == 3 && twinCornersInBase[2] == 1);
    // The face decision depends on triangle winding, never on the matching
    // per-corner shading normals from the original renderer.
    assert(mcvr::faces::accepts(mcvr::faces::back, true));
    assert(!mcvr::faces::accepts(mcvr::faces::back, false));
    const std::array<uint32_t, 2> springFaces{mcvr::faces::back, mcvr::faces::back};
    const mcvr::faces::ModelRules springRules(springFaces, false);
    VkTransformMatrixKHR ordinary{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    VkTransformMatrixKHR mirrored{{{-1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    assert((springRules.instanceFlags(ordinary) & VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR) == 0);
    assert((springRules.instanceFlags(mirrored) & VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR) != 0);

    const std::vector<uint32_t> fan{8, 2, 5, 7};
    const std::vector<uint32_t> fanTriangles{8, 2, 5, 8, 5, 7};
    assert(WorldMeshContract::triangulate(6, fan) == fanTriangles);

    const auto translation = WorldMeshContract::worldTranslation(
        100.0, 70.0, -40.0, 90.0, 64.0, -50.0);
    assert((translation == std::array<double, 3>{10.0, 6.0, 10.0}));

    assert(WorldMeshContract::generationMatches(7, 11, 3, 7, 11, 3));
    assert(!WorldMeshContract::generationMatches(8, 11, 3, 7, 11, 3));
    assert(!WorldMeshContract::generationMatches(7, 12, 3, 7, 11, 3));
    assert(!WorldMeshContract::generationMatches(7, 11, 4, 7, 11, 3));
    assert(WorldMeshContract::alphaMode(true, 4, 1) == 4);
    assert(WorldMeshContract::alphaMode(false, 0, 3) == 3);
    assert(WorldMeshContract::emission(0.25f, 1.0f) == 1.0f);
    return 0;
}
