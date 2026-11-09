#include "UsmapGen/Version.h"

#include <cstring>
#include <string_view>

namespace UsmapGen {

namespace {

constexpr std::uint32_t directoryBit = 0x80000000;
constexpr std::uint32_t versionResourceType = 16;
constexpr std::uint32_t fixedVersionSignature = 0xFEEF04BD;
constexpr std::uint32_t fixedVersionSize = 52;
constexpr std::uint32_t fixedVersionOffset = 40;
constexpr std::u16string_view versionKey = u"VS_VERSION_INFO";

class VersionResource {
public:
    VersionResource(std::span<const std::byte> bytes, std::uint32_t rva) : bytes(bytes), rva(rva) {}

    std::optional<FileVersion> read() {
        return directory(0, 0) ? version : std::nullopt;
    }

private:
    bool contains(std::uint32_t offset, std::uint32_t size) const {
        return offset <= bytes.size() && size <= bytes.size() - offset;
    }

    template <class T>
    T field(std::uint32_t offset) const {
        T value;
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
        return value;
    }

    bool value(std::uint32_t offset) {
        if (!contains(offset, 16))
            return false;
        const std::uint32_t dataRva = field<std::uint32_t>(offset);
        const std::uint32_t size = field<std::uint32_t>(offset + 4);
        if (dataRva < rva || !contains(dataRva - rva, size) || size < fixedVersionOffset + fixedVersionSize)
            return false;
        const std::uint32_t data = dataRva - rva;
        const std::uint16_t length = field<std::uint16_t>(data);
        if (length > size || length < fixedVersionOffset + fixedVersionSize ||
            field<std::uint16_t>(data + 2) != fixedVersionSize || field<std::uint16_t>(data + 4) != 0)
            return false;
        for (std::uint32_t i = 0; i <= versionKey.size(); i++)
            if (field<std::uint16_t>(data + 6 + 2 * i) != (i == versionKey.size() ? 0 : versionKey[i]))
                return false;
        const std::uint32_t fixed = data + fixedVersionOffset;
        if (field<std::uint32_t>(fixed) != fixedVersionSignature || field<std::uint32_t>(fixed + 4) != 0x10000)
            return false;
        const std::uint32_t ms = field<std::uint32_t>(fixed + 8);
        const std::uint32_t ls = field<std::uint32_t>(fixed + 12);
        if (ms != field<std::uint32_t>(fixed + 16) || ls != field<std::uint32_t>(fixed + 20))
            return false;
        const FileVersion found{std::uint16_t(ms >> 16), std::uint16_t(ms), std::uint16_t(ls >> 16), std::uint16_t(ls)};
        if (version && *version != found)
            return false;
        version = found;
        return true;
    }

    //! Only RT_VERSION -> name -> language -> data is accepted; a budget also bounds aliased directory trees.
    bool directory(std::uint32_t offset, unsigned depth) {
        if (!contains(offset, 16))
            return false;
        const std::uint32_t count = std::uint32_t(field<std::uint16_t>(offset + 12)) + field<std::uint16_t>(offset + 14);
        if (count == 0 || count > budget || !contains(offset + 16, count * 8))
            return false;
        budget -= count;
        bool found = false;
        for (std::uint32_t i = 0; i < count; i++) {
            const std::uint32_t entry = offset + 16 + i * 8;
            const std::uint32_t name = field<std::uint32_t>(entry);
            if (depth == 0 && name != versionResourceType)
                continue;
            if (depth == 0 && found)
                return false;
            found = true;
            const std::uint32_t target = field<std::uint32_t>(entry + 4);
            if (depth < 2) {
                if ((target & directoryBit) == 0 || !directory(target & ~directoryBit, depth + 1))
                    return false;
            } else if ((target & directoryBit) != 0 || !value(target)) {
                return false;
            }
        }
        return found;
    }

    std::span<const std::byte> bytes;
    std::uint32_t rva;
    std::uint32_t budget = 4096;
    std::optional<FileVersion> version;
};

}

std::optional<FileVersion> readVersionResource(std::span<const std::byte> bytes, std::uint32_t resourceRva) {
    return VersionResource(bytes, resourceRva).read();
}

}
