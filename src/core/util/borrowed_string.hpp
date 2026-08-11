#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace mcvr::detail {

template <typename Character, typename Handle, typename Length, typename Acquire,
          typename Release, typename Pending>
std::optional<std::basic_string<Character>> copyBorrowedString(
    Handle value, Length &&length, Acquire &&acquire, Release &&release, Pending &&pending) {
    if (!value || pending()) return std::nullopt;
    const auto count = length(value);
    if (pending()) return std::nullopt;
    const Character *characters = acquire(value);
    if (characters == nullptr) return std::nullopt;

    struct ReleaseGuard {
        Handle value;
        const Character *characters;
        Release &release;
        ~ReleaseGuard() { release(value, characters); }
    } guard{value, characters, release};

    if (pending()) return std::nullopt;
    return std::basic_string<Character>(characters, static_cast<size_t>(count));
}

} // namespace mcvr::detail
