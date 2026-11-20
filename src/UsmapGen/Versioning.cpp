#include "UsmapGen/Versioning.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace UsmapGen {

namespace {

constexpr std::uint64_t maxCustomVersion = 0xFFFF;
constexpr std::size_t maxBranchLength = 128;
constexpr std::size_t maxChangelistDigits = 10;
constexpr std::uint16_t maxMinor = 30;
constexpr std::uint16_t maxPatch = 30;
constexpr std::uint8_t callOpcode = 0xE8;
constexpr std::uint8_t jumpOpcode = 0xE9;
constexpr std::uint8_t returnOpcode = 0xC3;
constexpr std::uint8_t leaOpcode = 0x8D;
constexpr std::size_t leaLength = 7;
constexpr std::size_t branchLength = 5;
constexpr std::u16string_view changelistMark = u"-CL-";

struct PackageVersions {
    std::uint16_t major;
    std::uint16_t minor;
    std::int32_t ue4;
    std::int32_t ue5;
};

//! VER_LATEST_ENGINE_UE4 and EUnrealEngineObjectUE5Version::AUTOMATIC_VERSION of each release.
constexpr std::array<PackageVersions, 19> packageVersions{{
    {4, 18, 514, 0},    {4, 19, 516, 0},    {4, 20, 516, 0},    {4, 21, 517, 0},    {4, 22, 517, 0},
    {4, 23, 517, 0},    {4, 24, 518, 0},    {4, 25, 518, 0},    {4, 26, 522, 0},    {4, 27, 522, 0},
    {5, 0, 522, 1004},  {5, 1, 522, 1008},  {5, 2, 522, 1009},  {5, 3, 522, 1009},  {5, 4, 522, 1012},
    {5, 5, 522, 1013},  {5, 6, 522, 1017},  {5, 7, 522, 1018},  {5, 8, 522, 1018},
}};

struct BuildVersion {
    std::string branch;
    std::uint32_t changelist;
    std::vector<std::uint64_t> branchLiterals;
};

std::uint32_t readU32(std::span<const std::byte> bytes, std::size_t at) {
    std::uint32_t value;
    std::memcpy(&value, bytes.data() + at, sizeof(value));
    return value;
}

char16_t readUnit(std::span<const std::byte> bytes, std::size_t at) {
    char16_t value;
    std::memcpy(&value, bytes.data() + at, sizeof(value));
    return value;
}

//! Visits the offset of each occurrence of `pattern` that starts before `positions`.
template <class Visit>
void findAll(std::span<const std::byte> bytes, std::size_t positions, std::span<const std::byte> pattern, Visit visit) {
    const std::boyer_moore_horspool_searcher searcher(pattern.begin(), pattern.end());
    for (auto match = std::search(bytes.begin(), bytes.end(), searcher); match != bytes.end(); match = std::search(match + 1, bytes.end(), searcher)) {
        const std::size_t offset = std::size_t(match - bytes.begin());
        if (offset >= positions)
            return;
        visit(offset);
    }
}

//! Visits the offset of each byte equal to `value` before `positions`.
template <class Visit>
void findByte(std::span<const std::byte> bytes, std::size_t positions, std::uint8_t value, Visit visit) {
    const std::byte* const first = bytes.data();
    for (const void* match = std::memchr(first, value, positions); match; ) {
        const std::size_t offset = std::size_t(static_cast<const std::byte*>(match) - first);
        visit(offset);
        match = std::memchr(first + offset + 1, value, positions - offset - 1);
    }
}

bool isPrintable(char16_t unit) { return unit >= u' ' && unit <= u'~'; }

std::u16string readBranchBefore(Image& image, std::uint64_t mark) {
    const std::uint64_t first = std::max(image.getReadOnlyData().address, mark - 2 * maxBranchLength);
    std::vector<char16_t> units(std::size_t(mark - first) / 2);
    image.read(mark - 2 * units.size(), std::as_writable_bytes(std::span(units)));
    std::size_t start = units.size();
    while (start > 0 && units[start - 1] != 0) {
        if (!isPrintable(units[start - 1]))
            return {};
        start--;
    }
    if (start == 0)
        return {};
    return std::u16string(units.begin() + std::ptrdiff_t(start), units.end());
}

//! BuildSettings::GetBuildVersion returns BRANCH_NAME "-CL-" CURRENT_CHANGELIST and GetBranchName returns BRANCH_NAME, both as wide literals; UE 4.18 to 4.21 also build the FEngineVersion branch from an ANSI literal.
std::optional<BuildVersion> findBuildVersion(Image& image) {
    struct Candidate {
        std::u16string branch;
        std::uint32_t changelist;
    };
    std::vector<Candidate> candidates;
    const std::span<const std::byte> mark = std::as_bytes(std::span(changelistMark));
    image.scan(image.getReadOnlyData(), 2 * (changelistMark.size() + maxChangelistDigits + 1), [&](std::uint64_t at, std::span<const std::byte> bytes, std::size_t positions) {
        findAll(bytes, positions, mark, [&](std::size_t i) {
            if ((at + i) % 2 != 0)
                return;
            std::uint64_t changelist = 0;
            std::size_t digit = i + mark.size();
            std::size_t digits = 0;
            for (; digits <= maxChangelistDigits && digit + 2 <= bytes.size(); digits++, digit += 2) {
                const char16_t unit = readUnit(bytes, digit);
                if (unit < u'0' || unit > u'9')
                    break;
                changelist = changelist * 10 + (unit - u'0');
            }
            if (digits == 0 || digits > maxChangelistDigits || digit + 2 > bytes.size() || readUnit(bytes, digit) != 0 || changelist > 0x7FFFFFFF)
                return;
            std::u16string branch = readBranchBefore(image, at + i);
            if (!branch.empty())
                candidates.push_back({std::move(branch), std::uint32_t(changelist)});
        });
    });
    for (const Candidate& candidate : candidates) {
        const std::u16string wide = candidate.branch + u'\0';
        std::string ansi(candidate.branch.begin(), candidate.branch.end());
        ansi += '\0';
        const std::span<const std::byte> wideBytes = std::as_bytes(std::span(wide));
        const std::span<const std::byte> ansiBytes = std::as_bytes(std::span(ansi));
        std::vector<std::uint64_t> wideAddresses;
        std::vector<std::uint64_t> ansiAddresses;
        image.scan(image.getReadOnlyData(), wideBytes.size(), [&](std::uint64_t at, std::span<const std::byte> bytes, std::size_t positions) {
            findAll(bytes, positions, wideBytes, [&](std::size_t i) {
                if ((at + i) % 2 == 0)
                    wideAddresses.push_back(at + i);
            });
            findAll(bytes, positions, ansiBytes, [&](std::size_t i) { ansiAddresses.push_back(at + i); });
        });
        if (wideAddresses.empty())
            continue;
        std::ranges::copy(ansiAddresses, std::back_inserter(wideAddresses));
        return BuildVersion{std::string(candidate.branch.begin(), candidate.branch.end()), candidate.changelist, std::move(wideAddresses)};
    }
    return std::nullopt;
}

//! A literal also matches the end of a longer string, but code references only the start of a string. The engine version is set where the branch name is read: from the literal in UE 4.18 to 4.21, through BuildSettings::GetBranchName later.
std::vector<std::uint64_t> findBranchUsers(Image& image, std::span<const std::uint64_t> literals) {
    const std::unordered_set<std::uint64_t> targets(literals.begin(), literals.end());
    std::unordered_set<std::uint64_t> getters;
    std::unordered_set<std::uint64_t> users;
    const auto addUser = [&](std::uint64_t address) {
        if (const std::optional<std::uint64_t> function = image.findFunction(address))
            users.insert(*function);
    };
    image.scan(image.getCode(), leaLength + 1, [&](std::uint64_t at, std::span<const std::byte> bytes, std::size_t positions) {
        findByte(bytes.subspan(1), std::min(positions, bytes.size() - 1), leaOpcode, [&](std::size_t lea) {
            const std::uint8_t prefix = std::uint8_t(bytes[lea]);
            if (lea + leaLength >= bytes.size() || (prefix != 0x48 && prefix != 0x4C) || (std::uint8_t(bytes[lea + 2]) & 0xC7) != 0x05)
                return;
            const std::uint64_t target = at + lea + leaLength + std::uint64_t(std::int64_t(std::int32_t(readU32(bytes, lea + 3))));
            if (!targets.contains(target))
                return;
            if (std::uint8_t(bytes[lea + leaLength]) == returnOpcode)
                getters.insert(at + lea);
            else
                addUser(at + lea);
        });
    });
    if (!getters.empty()) {
        image.scan(image.getCode(), branchLength, [&](std::uint64_t at, std::span<const std::byte> bytes, std::size_t positions) {
            const auto visit = [&](std::size_t i) {
                if (i + branchLength <= bytes.size() && getters.contains(at + i + branchLength + std::uint64_t(std::int64_t(std::int32_t(readU32(bytes, i + 1))))))
                    addUser(at + i);
            };
            findByte(bytes, positions, callOpcode, visit);
            findByte(bytes, positions, jumpOpcode, visit);
        });
    }
    return {users.begin(), users.end()};
}

struct VersionStore {
    std::uint16_t major;
    std::uint16_t minor;
    std::uint16_t patch;
};

std::optional<VersionStore> versionOf(std::uint64_t value, std::uint64_t patch) {
    const std::uint16_t major = std::uint16_t(value);
    const std::uint16_t minor = std::uint16_t(value >> 16);
    if ((major != 4 && major != 5) || minor > maxMinor || patch > maxPatch)
        return std::nullopt;
    return VersionStore{major, minor, std::uint16_t(patch)};
}

//! FEngineVersion starts with uint16 Major, Minor and Patch; MSVC writes Major and Minor as one 32-bit constant and Patch after it.
std::vector<VersionStore> findVersionStores(Code& code, std::uint64_t function) {
    std::vector<VersionStore> found;
    const std::span<const Store> stores = code.findStores(function);
    for (const Store& store : stores) {
        if (store.size != 4)
            continue;
        for (const Store& patch : stores)
            if (patch.size == 2 && patch.address == store.address + 4)
                if (const std::optional<VersionStore> version = versionOf(store.value, patch.value))
                    found.push_back(*version);
    }
    const std::span<const FieldStore> fields = code.findFieldStores(function);
    for (const FieldStore& store : fields) {
        if (store.size != 4)
            continue;
        for (const FieldStore& patch : fields)
            if (patch.size == 2 && patch.base == store.base && patch.displacement == store.displacement + 4)
                if (const std::optional<VersionStore> version = versionOf(store.value, patch.value))
                    found.push_back(*version);
    }
    return found;
}

std::expected<EngineVersion, std::string> readEngineVersion(Image& image, Code& code, std::optional<BuildVersion>& build) {
    if (!build)
        return std::unexpected("The executable has no build version string of the form <branch>-CL-<changelist>.");
    std::vector<VersionStore> versions;
    for (const std::uint64_t function : findBranchUsers(image, build->branchLiterals))
        std::ranges::copy(findVersionStores(code, function), std::back_inserter(versions));
    if (versions.empty())
        return std::unexpected(std::format("No code that reads the branch {} sets an engine version.", build->branch));
    const VersionStore& first = versions.front();
    if (!std::ranges::all_of(versions, [&](const VersionStore& version) { return version.major == first.major && version.minor == first.minor; }))
        return std::unexpected("The code that reads the branch name sets different engine versions.");
    //! FEngineVersion::CompatibleWith has the patch 0 unless the build is a licensee version, so the current version has the highest patch.
    const std::uint16_t patch = std::ranges::max(versions, {}, &VersionStore::patch).patch;
    return EngineVersion{first.major, first.minor, patch, build->changelist, std::move(build->branch)};
}

struct KeyHash {
    std::size_t operator()(const std::array<std::uint64_t, 2>& key) const noexcept { return std::hash<std::uint64_t>{}(key[0] ^ key[1]); }
};

//! FCustomVersionRegistration passes its FGuid by reference and the version right after it: UE 4.18 to 4.23 call the constructor (This, Key, Version, FriendlyName), later builds call FCurrentCustomVersions::Register(Key, Version, FriendlyName, ...).
std::expected<std::vector<CustomVersion>, std::string> readCustomVersions(Code& code, std::span<const std::uint64_t> initializers) {
    std::unordered_map<std::uint64_t, std::vector<CustomVersion>> byTarget;
    std::unordered_map<std::uint64_t, std::unordered_set<std::array<std::uint64_t, 2>, KeyHash>> keysByTarget;
    for (const std::uint64_t initializer : initializers) {
        const std::span<const Call> calls = code.findCalls(initializer);
        for (const ArgumentBlock& block : code.findArgumentBlocks(initializer)) {
            const Call& call = calls[block.call];
            const std::optional<std::uint64_t> version = call.getConstant(block.argument + 1);
            if (!version || *version > maxCustomVersion || block.bytes[0] == 0 || block.bytes[1] == 0)
                continue;
            if (keysByTarget[call.target].insert(block.bytes).second)
                byTarget[call.target].push_back({block.bytes, std::int32_t(*version)});
        }
    }
    if (byTarget.empty())
        return std::unexpected("The executable registers no custom versions.");
    return std::move(std::ranges::max_element(byTarget, {}, [](const auto& entry) { return entry.second.size(); })->second);
}

}

std::expected<Versioning, std::string> readVersioning(Image& image, Code& code, const std::span<const std::uint64_t> initializers, std::string& buildVersion) {
    std::optional<BuildVersion> build = findBuildVersion(image);
    if (build)
        buildVersion = std::format("{}-CL-{}", build->branch, build->changelist);
    Versioning versioning;
    std::expected<std::vector<CustomVersion>, std::string> customVersions = readCustomVersions(code, initializers);
    if (!customVersions)
        return std::unexpected(std::move(customVersions.error()));
    versioning.customVersions = std::move(*customVersions);
    std::expected<EngineVersion, std::string> engine = readEngineVersion(image, code, build);
    if (!engine)
        return std::unexpected(std::move(engine.error()));
    const auto package = std::ranges::find_if(packageVersions, [&](const PackageVersions& entry) { return entry.major == engine->major && entry.minor == engine->minor; });
    if (package == packageVersions.end())
        return std::unexpected(std::format("The engine version {}.{} has no known package file version.", engine->major, engine->minor));
    versioning.fileVersionUE4 = package->ue4;
    versioning.fileVersionUE5 = package->ue5;
    versioning.engine = std::move(*engine);
    return versioning;
}

}
