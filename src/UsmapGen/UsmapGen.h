#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
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
    //! The whole executable when it is already in memory, such as a mapped file, so it is read in place instead of copied; empty otherwise.
    virtual std::span<const std::byte> getBytes() const noexcept { return {}; }
};

//! How the usmap payload is compressed, with the values that the usmap header stores.
enum class Compression : std::uint8_t {
    None = 0,
    Oodle = 1,
    Brotli = 2,
    Zstandard = 3,
};

struct Options {
    Compression compression = Compression::Zstandard;
};

struct Usmap {
    std::vector<std::byte> bytes;
    //! The name that mappings are shared under: the build version, such as ++Fortnite+Release-43.10-CL-60118918, then the suffix of the compression; empty when the executable has no build version.
    std::string fileName;
    //! What the usmap lacks and why, such as versioning that cannot be read.
    std::vector<std::string> warnings;
};

//! Whether the compression is compiled in; the CMake options USMAPGEN_ZSTANDARD, USMAPGEN_BROTLI and USMAPGEN_OODLE leave each one out.
bool hasCompression(Compression compression);

//! The name of each compression in messages, such as Zstandard.
std::string_view compressionName(Compression compression);

//! The file name suffix of each compression: _oo, _br, _zs, or none.
std::string_view compressionSuffix(Compression compression);

//! UnrealHeaderTool compiles the reflection tables into the executable from UE 4.18.
std::expected<Usmap, Error> generateUsmap(const Source& executable, const Options& options = {});

}
