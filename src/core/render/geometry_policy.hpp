#pragma once
#include <cstdlib>
#include <filesystem>
#include <string_view>

namespace mcvr::geometry {
// User-selected bilateral world policy. Source-state traversal is recoverable
// with an explicit restart-only override; authored flags are always preserved.
inline bool twoSided() {
    static const bool enabled=[] {
        const char *value=std::getenv("RADIANCE_WORLD_TWO_SIDED");
        return !value || std::string_view(value)!="0";
    }();
    return enabled;
}
inline bool concordantQuads() {
    static const bool enabled=[] {
        const char *value=std::getenv("RADIANCE_CONCORDANT_QUADS");
        return !value || std::string_view(value)!="0";
    }();
    return enabled;
}
inline bool isolatedAudit() {
    static const bool enabled=[] {
        const char *value=std::getenv("RADIANCE_GEOMETRY_AUDIT");
        if (!value || std::string_view(value)!="1") return false;
        std::error_code error;
        return std::filesystem::is_regular_file(std::filesystem::current_path()/".radiance-audit-test-instance",error)
            && !error;
    }();
    return enabled;
}
inline bool backfaceView() {
    static const bool enabled=[] {
        const char *value=std::getenv("RADIANCE_GEOMETRY_BACKFACES");
        return isolatedAudit() && value && std::string_view(value)=="1";
    }();
    return enabled;
}
} // namespace mcvr::geometry
