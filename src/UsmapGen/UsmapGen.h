#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace UsmapGen {

struct Error {
    std::string message;
};

class Source {
public:
    virtual ~Source() = default;
    virtual std::uint64_t getSize() const noexcept = 0;
    virtual std::expected<void, Error> read(std::uint64_t offset, std::span<std::byte> buffer) const noexcept = 0;
};

enum class NameCasing : std::uint8_t { Unknown, NonPreserving, Preserving };

//! UnrealHeaderTool compiles the reflection tables into the executable from UE 4.18.
std::expected<std::vector<std::byte>, Error> generateUsmap(const Source& executable);
std::expected<std::vector<std::byte>, Error> generateUsmap(const Source& executable, NameCasing nameCasing);

}
