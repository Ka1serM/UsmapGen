#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <vector>

#include "UsmapGen/Enum.h"
#include "UsmapGen/Image.h"

namespace {

using UsmapGen::EnumMaxBehavior;
using UsmapGen::FileVersion;

int checks = 0;
int failures = 0;

void check(bool condition, const char* label) {
    checks++;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        failures++;
    }
}

template <class T>
void put(std::vector<std::byte>& bytes, std::size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

void putVersion(std::vector<std::byte>& bytes, std::uint32_t at, FileVersion version) {
    put<std::uint16_t>(bytes, at, 92);
    put<std::uint16_t>(bytes, at + 2, 52);
    constexpr char16_t key[] = u"VS_VERSION_INFO";
    std::memcpy(bytes.data() + at + 6, key, sizeof(key));
    put<std::uint32_t>(bytes, at + 40, 0xFEEF04BD);
    put<std::uint32_t>(bytes, at + 44, 0x10000);
    const std::uint32_t ms = std::uint32_t(version.major) << 16 | version.minor;
    const std::uint32_t ls = std::uint32_t(version.patch) << 16 | version.revision;
    put(bytes, at + 48, ms);
    put(bytes, at + 52, ls);
    put(bytes, at + 56, ms);
    put(bytes, at + 60, ls);
}

constexpr std::uint32_t resourceRva = 0x4000;
constexpr FileVersion engine57{5, 7, 4, 0};

std::vector<std::byte> resource(FileVersion version = engine57) {
    std::vector<std::byte> bytes(256);
    put<std::uint16_t>(bytes, 14, 1);
    put<std::uint32_t>(bytes, 16, 16);
    put<std::uint32_t>(bytes, 20, 0x80000020);
    put<std::uint16_t>(bytes, 0x20 + 14, 1);
    put<std::uint32_t>(bytes, 0x30, 1);
    put<std::uint32_t>(bytes, 0x34, 0x80000040);
    put<std::uint16_t>(bytes, 0x40 + 14, 1);
    put<std::uint32_t>(bytes, 0x50, 0x409);
    put<std::uint32_t>(bytes, 0x54, 0x60);
    put<std::uint32_t>(bytes, 0x60, resourceRva + 0x80);
    put<std::uint32_t>(bytes, 0x64, 92);
    putVersion(bytes, 0x80, version);
    return bytes;
}

class MemorySource final : public UsmapGen::Source {
public:
    std::vector<std::byte> bytes;
    std::uint64_t failAt = UINT64_MAX;
    mutable int failedReads = 0;
    std::uint64_t getSize() const noexcept override { return bytes.size(); }
    std::expected<void, UsmapGen::Error> read(std::uint64_t offset, std::span<std::byte> out) const noexcept override {
        if (offset >= failAt || out.size() > failAt - offset) {
            failedReads++;
            return std::unexpected(UsmapGen::Error{"Fixture resource I/O failure"});
        }
        if (offset > bytes.size() || out.size() > bytes.size() - offset)
            return std::unexpected(UsmapGen::Error{"Fixture read outside file"});
        std::memcpy(out.data(), bytes.data() + offset, out.size());
        return {};
    }
};

MemorySource imageSource() {
    MemorySource source;
    source.bytes.resize(0xC00);
    auto& bytes = source.bytes;
    put<std::uint32_t>(bytes, 0x3C, 0x80);
    put<std::uint32_t>(bytes, 0x80, 0x4550);
    put<std::uint16_t>(bytes, 0x84, 0x8664);
    put<std::uint16_t>(bytes, 0x86, 4);
    put<std::uint16_t>(bytes, 0x94, 240);
    constexpr std::size_t optional = 0x98;
    put<std::uint16_t>(bytes, optional, 0x20B);
    put<std::uint64_t>(bytes, optional + 24, 0x140000000);
    put<std::uint32_t>(bytes, optional + 108, 16);
    put<std::uint32_t>(bytes, optional + 112 + 16, resourceRva);
    put<std::uint32_t>(bytes, optional + 112 + 20, 256);
    put<std::uint32_t>(bytes, optional + 112 + 24, 0x2000);
    put<std::uint32_t>(bytes, optional + 112 + 28, 12);
    constexpr std::array names{".text", ".rdata", ".data", ".rsrc"};
    for (std::uint32_t i = 0; i < names.size(); i++) {
        const std::size_t at = optional + 240 + i * 40;
        std::memcpy(bytes.data() + at, names[i], std::strlen(names[i]));
        put<std::uint32_t>(bytes, at + 8, 0x200);
        put<std::uint32_t>(bytes, at + 12, (i + 1) * 0x1000);
        put<std::uint32_t>(bytes, at + 16, 0x200);
        put<std::uint32_t>(bytes, at + 20, 0x400 + i * 0x200);
    }
    const auto version = resource();
    std::memcpy(bytes.data() + 0xA00, version.data(), version.size());
    return source;
}

void testResources() {
    auto bytes = resource();
    check(UsmapGen::readVersionResource(bytes, resourceRva) == engine57, "fixed numeric version");
    for (std::size_t size = 0; size < 0x80 + 92; size++)
        check(!UsmapGen::readVersionResource(std::span(bytes).first(size), resourceRva), "truncated resource");

    struct Mutation { std::size_t offset; std::uint32_t value; };
    constexpr Mutation mutations[] = {
        {16, 10}, {20, 0x20}, {20, 0xFFFFFFFC}, {0x34, 0x80000000},
        {0x54, 0x80000060}, {0x54, 0xFFFFFFF0}, {0x60, resourceRva - 1},
        {0x60, 0xFFFFFFF0}, {0x64, 91}, {0x64, 0xFFFFFFFF},
        {0x80, 0}, {0x80, 0x0034FFFF}, {0x84, 1}, {0x88, 0},
        {0xA8, 0}, {0xAC, 0}, {0xB8, 0x00050006}, {0xBC, 1},
        {12, 0xFFFFFFFF}, {0x4C, 0}
    };
    for (const Mutation mutation : mutations) {
        auto bad = bytes;
        put(bad, mutation.offset, mutation.value);
        check(!UsmapGen::readVersionResource(bad, resourceRva), "invalid resource does not authorize new semantics");
    }
    auto duplicate = bytes;
    put<std::uint16_t>(duplicate, 14, 2);
    put<std::uint32_t>(duplicate, 24, 16);
    put<std::uint32_t>(duplicate, 28, 0x80000020);
    check(!UsmapGen::readVersionResource(duplicate, resourceRva), "duplicate version resource type");

    auto languages = bytes;
    languages.resize(384);
    put<std::uint16_t>(languages, 0x4E, 2);
    put<std::uint32_t>(languages, 0x58, 0x407);
    put<std::uint32_t>(languages, 0x5C, 0x70);
    put<std::uint32_t>(languages, 0x70, resourceRva + 256);
    put<std::uint32_t>(languages, 0x74, 92);
    putVersion(languages, 256, engine57);
    check(UsmapGen::readVersionResource(languages, resourceRva) == engine57, "consistent language versions");
    putVersion(languages, 256, FileVersion{5, 4, 4, 0});
    check(!UsmapGen::readVersionResource(languages, resourceRva), "conflicting language versions");
    put<std::uint32_t>(languages, 256 + 40, 0);
    check(!UsmapGen::readVersionResource(languages, resourceRva), "invalid second language does not keep first version");

    auto source = imageSource();
    UsmapGen::Image image(source);
    check(bool(image.open()), "minimal PE opens");
    check(image.readFileVersion() == engine57, "PE resource directory resolves RVA to file offset");
    constexpr std::size_t directory = 0x98 + 112 + 16;
    constexpr Mutation imageMutations[] = {
        {directory, 0}, {directory, 0xFFFFFFF0}, {directory + 4, 0},
        {directory + 4, 0xFFFFFFFF}, {directory + 4, 0x201},
        {0x98 + 108, 2}, {0x188 + 3 * 40 + 20, 0xFFFFFFF0}
    };
    for (const Mutation mutation : imageMutations) {
        auto bad = imageSource();
        put(bad.bytes, mutation.offset, mutation.value);
        UsmapGen::Image candidate(bad);
        check(bool(candidate.open()), "optional version failure does not reject reflection image");
        check(!candidate.readFileVersion() && !candidate.getError(), "unusable resource remains unknown without out-of-bounds reads");
    }
}

void testOptionalResourceIo() {
    auto source = imageSource();
    constexpr std::uint32_t resourceOffset = 64 * 1024;
    source.bytes.resize(resourceOffset + 0x200);
    put(source.bytes, 0x188 + 3 * 40 + 20, resourceOffset);
    const auto version = resource();
    std::memcpy(source.bytes.data() + resourceOffset, version.data(), version.size());
    source.failAt = resourceOffset;
    UsmapGen::Image image(source);
    check(bool(image.open()), "PE opens with unreadable resource on separate 64 KiB page");
    check(source.failedReads == 0, "opening PE does not cache or read resource page");
    const auto missing = image.readFileVersion();
    check(source.failedReads == 1, "optional metadata attempts uncached resource I/O");
    check(!missing, "resource I/O failure returns unknown version");
    check(!image.getError(), "optional resource I/O failure does not poison reflection image");
    check(UsmapGen::enumMaxBehavior(true, missing) == EnumMaxBehavior::Legacy, "resource I/O failure retains legacy enum semantics");
    check(image.read<std::uint32_t>(image.getCode().address) == 0 && !image.getError(), "reflection reads remain usable after optional I/O failure");
    source.failAt = UINT64_MAX;
    check(image.readFileVersion() == engine57 && !image.getError(), "optional resource read can recover after transient I/O failure");

    for (const bool failResource : {false, true}) {
        source.failAt = failResource ? resourceOffset : UINT64_MAX;
        UsmapGen::Image failedImage(source);
        check(bool(failedImage.open()), "PE opens for pre-existing error regression");
        failedImage.fail("Existing reflection error");
        failedImage.readFileVersion();
        check(failedImage.getError() && failedImage.getError()->message == "Existing reflection error", failResource
            ? "failed optional I/O preserves pre-existing reflection error"
            : "successful optional I/O preserves pre-existing reflection error");
    }
}

void testEnums() {
    const auto next = [](std::initializer_list<std::uint64_t> values, EnumMaxBehavior behavior, bool flags) {
        return UsmapGen::syntheticEnumMax(values, behavior, flags);
    };
    for (const std::uint16_t minor : {0, 1, 2, 3, 4, 5, 6, 7, 8}) {
        const FileVersion version{5, minor, 0, 0};
        const auto behavior = UsmapGen::enumMaxBehavior(minor >= 3, version);
        const auto expected = minor >= 7 ? EnumMaxBehavior::EmptyZero : minor >= 5 ? EnumMaxBehavior::Flags : EnumMaxBehavior::Legacy;
        check(behavior == expected, "source-verified engine boundary");
        check(next({}, behavior, false) == (minor >= 7 ? 0 : 1), "empty regular enum by engine");
        check(next({}, behavior, true) == (minor >= 7 ? 0 : 1), "empty flags enum by engine");
        check(next({0, 1, 2, 4}, behavior, true) == (minor >= 5 ? 8 : 5), "UE5.3/5.4 flags remain signed maximum");
        check(next({0, 1, 2, 4, 8}, behavior, false) == 9, "non-flags enum unchanged");
        check(next({1, 8, 32, 41}, behavior, true) == 42, "sparse flags do not round to next power of two");
        check(next({1, 2, 4, 128, 255}, behavior, true) == (minor >= 5 ? 136 : 256), "combined flags do not contribute bits");
    }
    for (const std::uint16_t minor : {18, 21, 22, 27}) {
        const auto behavior = UsmapGen::enumMaxBehavior(false, FileVersion{4, minor, 0, 0});
        check(behavior == EnumMaxBehavior::Legacy, "UE4 layouts retain legacy behavior");
        check(next({}, behavior, false) == 1, "UE4 empty enum");
        check(next({0, 1, 2, 4}, behavior, true) == 5, "UE4 ignores flags for synthetic MAX");
    }
    for (const EnumMaxBehavior behavior : {EnumMaxBehavior::Legacy, EnumMaxBehavior::Flags, EnumMaxBehavior::EmptyZero}) {
        check(next({std::uint64_t(-7), std::uint64_t(-2)}, behavior, false) == std::uint64_t(-1), "negative-only signed maximum");
        check(next({std::uint64_t(-1), 3, 2}, behavior, false) == 4, "signed rather than unsigned order");
        check(next({std::uint64_t(std::numeric_limits<std::int64_t>::max())}, behavior, false) == (1ull << 63), "increment has no signed overflow");
        check(next({std::uint64_t(-1)}, behavior, false) == 0, "negative one wraps to zero");
    }
    check(next({0, 1, 2, 4, 8}, EnumMaxBehavior::EmptyZero, true) == 16, "Grounded2 EMovieSceneSequenceFlags");
    check(next({1, 8, 32, 41}, EnumMaxBehavior::Flags, true) == 42, "EMaterialShaderFrequency regression from PR4");
    check(next({1ull << 34}, EnumMaxBehavior::Flags, true) == (1ull << 34) + 1, "flags retain 64-bit width");
    check(next({1ull << 63}, EnumMaxBehavior::Flags, true) == (1ull << 63) + 1, "sign bit is a single-bit flag");
    check(next({0, 3, 12}, EnumMaxBehavior::Flags, true) == 1, "no single-bit flags");
    check(UsmapGen::enumMaxBehavior(true, std::nullopt) == EnumMaxBehavior::Legacy, "missing version fallback");
    for (const FileVersion version : {FileVersion{4, 27, 2, 0}, FileVersion{5, 9, 0, 0}, FileVersion{6, 0, 0, 0}, FileVersion{5, 7, 4, 1}})
        check(UsmapGen::enumMaxBehavior(true, version) == EnumMaxBehavior::Legacy, "unverified version fallback");
    check(UsmapGen::enumMaxBehavior(false, engine57) == EnumMaxBehavior::Legacy, "version cannot override an older UHT layout");
}

}

int main() {
    testResources();
    testOptionalResourceIo();
    testEnums();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
