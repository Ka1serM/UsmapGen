#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace UsmapGen {

struct FileVersion {
    std::uint16_t major;
    std::uint16_t minor;
    std::uint16_t patch;
    std::uint16_t revision;

    bool operator==(const FileVersion&) const = default;
};

std::optional<FileVersion> readVersionResource(std::span<const std::byte> bytes, std::uint32_t resourceRva);

}
