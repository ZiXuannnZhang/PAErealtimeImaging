#pragma once

#include <cstdint>

namespace paimage {

// Physical measurement-round identity shared by the acquisition, Ring and
// presentation paths.  The wire trigger sequence is only a 16-bit value and
// is deliberately not part of this identity.
struct RoundIdentity {
    std::uint64_t measurementSession = 0;
    std::uint64_t roundGeneration = 0;

    constexpr bool valid() const noexcept
    {
        // Generation zero is a valid first round; a zero session is reserved
        // for untagged/legacy data and is never a production Ring identity.
        return measurementSession != 0;
    }
};

constexpr bool operator==(const RoundIdentity &lhs,
                          const RoundIdentity &rhs) noexcept
{
    return lhs.measurementSession == rhs.measurementSession
        && lhs.roundGeneration == rhs.roundGeneration;
}

constexpr bool operator!=(const RoundIdentity &lhs,
                          const RoundIdentity &rhs) noexcept
{
    return !(lhs == rhs);
}

constexpr bool operator<(const RoundIdentity &lhs,
                         const RoundIdentity &rhs) noexcept
{
    return lhs.measurementSession < rhs.measurementSession
        || (lhs.measurementSession == rhs.measurementSession
            && lhs.roundGeneration < rhs.roundGeneration);
}

constexpr bool operator<=(const RoundIdentity &lhs,
                          const RoundIdentity &rhs) noexcept
{
    return lhs < rhs || lhs == rhs;
}

} // namespace paimage
