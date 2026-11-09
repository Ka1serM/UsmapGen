#pragma once

#include <bit>
#include <cstdint>
#include <optional>
#include <ranges>

#include "UsmapGen/Version.h"

namespace UsmapGen {

enum class EnumMaxBehavior : std::uint8_t { Legacy, Flags, EmptyZero };

//! Epic's Enum.cpp changes flags MAX in 5.5 and empty MAX in 5.7; unverified versions retain the old behavior.
inline EnumMaxBehavior enumMaxBehavior(bool engine53Layout, const std::optional<FileVersion>& version) {
    if (!engine53Layout || !version || version->major != 5 || version->revision != 0)
        return EnumMaxBehavior::Legacy;
    if (version->minor == 5 || version->minor == 6)
        return EnumMaxBehavior::Flags;
    if (version->minor == 7 || version->minor == 8)
        return EnumMaxBehavior::EmptyZero;
    return EnumMaxBehavior::Legacy;
}

//! The flags rule follows Adain Rivers' Ka1serM/UsmapGen PR #4, but only after version and layout agree.
inline std::uint64_t syntheticEnumMax(std::ranges::input_range auto&& values, EnumMaxBehavior behavior, bool flags) {
    const bool combineFlags = flags && behavior != EnumMaxBehavior::Legacy;
    std::uint64_t maximum = 0;
    bool empty = true;
    for (const std::uint64_t value : values) {
        if (combineFlags) {
            if (std::has_single_bit(value))
                maximum |= value;
        } else if (empty || std::bit_cast<std::int64_t>(value) > std::bit_cast<std::int64_t>(maximum)) {
            maximum = value;
        }
        empty = false;
    }
    if (empty && behavior == EnumMaxBehavior::EmptyZero)
        return 0;
    return maximum + 1;
}

}
