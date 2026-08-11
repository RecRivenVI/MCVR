#include "core/render/world_mesh_audit.hpp"
#include "core/render/world_mesh_contract.hpp"

#include <cassert>

int main() {
    static_assert(static_cast<int>(WorldMeshAuditState::Missing) == 0);
    static_assert(static_cast<int>(WorldMeshAuditState::Queued) == 1);
    static_assert(static_cast<int>(WorldMeshAuditState::Committed) == 2);
    static_assert(static_cast<int>(WorldMeshAuditState::Built) == 3);
    static_assert(static_cast<int>(WorldMeshAuditState::RolledBack) == 4);
    static_assert(static_cast<int>(WorldMeshAuditState::Stale) == 5);
    const std::vector<uint32_t> sortedQuads{4, 5, 6, 6, 7, 4, 0, 1, 2, 2, 3, 0};
    assert(WorldMeshContract::triangulate(7, sortedQuads) == sortedQuads);

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
