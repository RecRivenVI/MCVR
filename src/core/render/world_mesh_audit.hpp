#pragma once

enum class WorldMeshAuditState : int {
    Missing = 0,
    Queued = 1,
    Committed = 2,
    Built = 3,
    RolledBack = 4,
    Stale = 5,
};
