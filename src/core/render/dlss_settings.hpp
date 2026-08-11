#pragma once
#include <array>
namespace mcvr::dlss {
// Selectable presets in the pinned 310.9.1 SDK. E/F remain selectable legacy
// hints (deprecated); removed and reserved values are never advertised.
inline constexpr std::array srModels{0, 5, 6, 10, 11, 12, 13};
inline constexpr std::array rrModels{0, 4, 5, 6};
inline constexpr std::array fgModels{0}; // NGX exposes no selectable FG model hint.
template <class Values> constexpr bool contains(const Values &values, int value) {
    for (int candidate : values) if (candidate == value) return true;
    return false;
}
inline constexpr bool validModels(int sr, int rr, int fg) {
    return contains(srModels, sr) && contains(rrModels, rr) && contains(fgModels, fg);
}
}
