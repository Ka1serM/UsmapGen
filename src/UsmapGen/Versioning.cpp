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

#include <Zydis/Decoder.h>
#include <Zydis/Register.h>

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

//! VER_UE4_OLDEST_LOADABLE_PACKAGE, the lowest UE4 package file version of any release.
constexpr std::int32_t oldestLoadableUE4 = 214;
//! EUnrealEngineObjectUE5Version::INITIAL_VERSION; the UE4 versions stay below it.
constexpr std::int32_t initialUE5 = 1000;
constexpr std::int32_t maxUE5 = 2000;
//! PACKAGE_FILE_TAG, which the package file summary starts with.
constexpr std::uint32_t packageFileTag = 0x9E2A83C1;
constexpr std::u16string_view networkVersionOverride = u"networkversionoverride";
constexpr std::u16string_view netChangelistMark = u"NetCL: %";
constexpr std::uint32_t licenseeMask = 0x7FFFFFFF;
constexpr std::size_t maxArguments = 8;
constexpr std::size_t registerArguments = 4;
constexpr std::int64_t stackArguments = 0x20;
constexpr std::uint64_t maxScan = 0x10000;
constexpr int maxFollow = 16;

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


//! Decodes single instructions of the code section.
class Decoder {
public:
    explicit Decoder(Image& image) : image(image) { ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64); }

    bool decode(std::uint64_t address) {
        if (!image.isCode(address))
            return false;
        const Section& code = image.getCode();
        std::byte bytes[ZYDIS_MAX_INSTRUCTION_LENGTH];
        const std::size_t length = std::size_t(std::min<std::uint64_t>(sizeof(bytes), code.address + code.virtualSize - address));
        image.read(address, std::span(bytes, length));
        return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes, length, &instruction, operands));
    }

    std::span<const ZydisDecodedOperand> visible() const { return std::span(operands, instruction.operand_count_visible); }

    //! The address that a RIP-relative memory operand names.
    std::optional<std::uint64_t> ripTarget(std::uint64_t address, const ZydisDecodedOperand& operand) const {
        if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY || operand.mem.base != ZYDIS_REGISTER_RIP || operand.mem.index != ZYDIS_REGISTER_NONE)
            return std::nullopt;
        return address + instruction.length + std::uint64_t(operand.mem.disp.value);
    }

    std::optional<std::uint64_t> branchTarget(std::uint64_t address) const {
        const std::span<const ZydisDecodedOperand> operands = visible();
        if (operands.empty() || operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !operands[0].imm.is_relative)
            return std::nullopt;
        return address + instruction.length + operands[0].imm.value.u;
    }

    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

private:
    Image& image;
    ZydisDecoder decoder;
};

ZydisRegister widest(ZydisRegister reg) { return ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg); }

bool isRegister(const ZydisDecodedOperand& operand, ZydisRegister reg) { return operand.type == ZYDIS_OPERAND_TYPE_REGISTER && widest(operand.reg.value) == reg; }

//! The wide literals equal to `text`, terminator included.
std::vector<std::uint64_t> findWideLiterals(Image& image, std::u16string_view text) {
    std::u16string terminated(text);
    terminated += u'\0';
    const std::span<const std::byte> pattern = std::as_bytes(std::span(terminated));
    std::vector<std::uint64_t> found;
    image.scan(image.getReadOnlyData(), pattern.size(), [&](std::uint64_t at, std::span<const std::byte> bytes, std::size_t positions) {
        findAll(bytes, positions, pattern, [&](std::size_t i) {
            if ((at + i) % 2 == 0)
                found.push_back(at + i);
        });
    });
    return found;
}

//! Each `lea r64, [rip + target]` of a target, with the address of the instruction.
std::vector<std::pair<std::uint64_t, std::uint64_t>> findLeas(Image& image, std::span<const std::uint64_t> literals) {
    const std::unordered_set<std::uint64_t> targets(literals.begin(), literals.end());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> found;
    if (targets.empty())
        return found;
    image.scan(image.getCode(), leaLength + 1, [&](std::uint64_t at, std::span<const std::byte> bytes, std::size_t positions) {
        findByte(bytes.subspan(1), std::min(positions, bytes.size() - 1), leaOpcode, [&](std::size_t lea) {
            const std::uint8_t prefix = std::uint8_t(bytes[lea]);
            if (lea + leaLength > bytes.size() || (prefix != 0x48 && prefix != 0x4C) || (std::uint8_t(bytes[lea + 2]) & 0xC7) != 0x05)
                return;
            const std::uint64_t target = at + lea + leaLength + std::uint64_t(std::int64_t(std::int32_t(readU32(bytes, lea + 3))));
            if (targets.contains(target))
                found.emplace_back(at + lea, target);
        });
    });
    return found;
}

//! Where a value comes from: a constant, such as the address of a lea, or the 32 bits of a global, optionally without the licensee bit.
struct Source {
    enum class Kind : std::uint8_t { Unknown, Constant, Global } kind = Kind::Unknown;
    std::uint64_t value = 0;
    bool masked = false;
};

std::optional<std::uint32_t> valueOf(Image& image, const Source& source) {
    if (source.kind == Source::Kind::Constant)
        return std::uint32_t(source.value);
    if (source.kind != Source::Kind::Global || !image.isData(source.value))
        return std::nullopt;
    const std::uint32_t value = image.read<std::uint32_t>(source.value);
    return source.masked ? value & licenseeMask : value;
}

bool clearsLicenseeBit(const Decoder& decoder) {
    const std::span<const ZydisDecodedOperand> operands = decoder.visible();
    return operands.size() == 2 && operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
           ((decoder.instruction.mnemonic == ZYDIS_MNEMONIC_BTR && operands[1].imm.value.u == 31) ||
            (decoder.instruction.mnemonic == ZYDIS_MNEMONIC_AND && std::uint32_t(operands[1].imm.value.u) == licenseeMask));
}

//! What a small function returns: `xor eax, eax`, `mov eax, imm`, `lea rax, [global]`, or a load of a global or of a member of the object in rcx, optionally without the licensee bit, then `ret`. FEngineVersion::CompatibleWith and GetChangelist look so.
Source callResult(Code& code, Decoder& decoder, std::uint64_t function, const Source& object) {
    std::uint64_t address = code.followThunks(function);
    if (!decoder.decode(address) || decoder.visible().size() != 2 || !isRegister(decoder.visible()[0], ZYDIS_REGISTER_RAX))
        return {};
    Source source;
    const ZydisDecodedOperand value = decoder.visible()[1];
    const bool wide = decoder.visible()[0].size == 64;
    if (decoder.instruction.mnemonic == ZYDIS_MNEMONIC_XOR && isRegister(value, ZYDIS_REGISTER_RAX))
        source = {Source::Kind::Constant, 0};
    else if (decoder.instruction.mnemonic == ZYDIS_MNEMONIC_MOV && value.type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
        source = {Source::Kind::Constant, value.imm.value.u};
    else if (decoder.instruction.mnemonic == ZYDIS_MNEMONIC_LEA && wide)
        if (const std::optional<std::uint64_t> target = decoder.ripTarget(address, value))
            source = {Source::Kind::Constant, *target};
    if (decoder.instruction.mnemonic == ZYDIS_MNEMONIC_MOV && !wide && value.type == ZYDIS_OPERAND_TYPE_MEMORY) {
        if (const std::optional<std::uint64_t> target = decoder.ripTarget(address, value))
            source = {Source::Kind::Global, *target};
        else if (value.mem.base == ZYDIS_REGISTER_RCX && value.mem.index == ZYDIS_REGISTER_NONE && object.kind == Source::Kind::Constant)
            source = {Source::Kind::Global, object.value + std::uint64_t(value.mem.disp.value)};
    }
    if (source.kind == Source::Kind::Unknown)
        return {};
    address += decoder.instruction.length;
    if (source.kind == Source::Kind::Global && decoder.decode(address) && clearsLicenseeBit(decoder)) {
        source.masked = true;
        address += decoder.instruction.length;
    }
    if (!decoder.decode(address) || decoder.instruction.mnemonic != ZYDIS_MNEMONIC_RET)
        return {};
    return source;
}

constexpr std::array<ZydisRegister, registerArguments> argumentRegisters{ZYDIS_REGISTER_RCX, ZYDIS_REGISTER_RDX, ZYDIS_REGISTER_R8, ZYDIS_REGISTER_R9};
constexpr std::array<ZydisRegister, 7> volatileRegisters{ZYDIS_REGISTER_RAX, ZYDIS_REGISTER_RCX, ZYDIS_REGISTER_RDX, ZYDIS_REGISTER_R8, ZYDIS_REGISTER_R9, ZYDIS_REGISTER_R10, ZYDIS_REGISTER_R11};

//! The sources of the first arguments of the first call at or after `anchor`, read by decoding its function from the start of the code range that contains it. Only straight-line constants, leas and loads of globals are followed.
std::optional<std::array<Source, maxArguments>> argumentsAt(Image& image, Code& code, Decoder& decoder, std::uint64_t anchor) {
    const std::optional<std::uint64_t> start = image.findFunction(anchor);
    if (!start)
        return std::nullopt;
    std::unordered_map<ZydisRegister, Source> registers;
    std::unordered_map<std::int64_t, Source> stack;
    bool reached = false;
    for (std::uint64_t address = *start; address < anchor + maxScan && address < *start + 16 * maxScan && decoder.decode(address); address += decoder.instruction.length) {
        reached = reached || address == anchor;
        if (!reached && address > anchor)
            return std::nullopt;
        const ZydisDecodedInstruction& instruction = decoder.instruction;
        const std::span<const ZydisDecodedOperand> operands = decoder.visible();
        if (instruction.mnemonic == ZYDIS_MNEMONIC_CALL) {
            if (reached) {
                std::array<Source, maxArguments> arguments;
                for (std::size_t i = 0; i < maxArguments; i++) {
                    if (i < registerArguments) {
                        if (const auto found = registers.find(argumentRegisters[i]); found != registers.end())
                            arguments[i] = found->second;
                    } else if (const auto found = stack.find(stackArguments + std::int64_t(8 * (i - registerArguments))); found != stack.end()) {
                        arguments[i] = found->second;
                    }
                }
                return arguments;
            }
            const std::optional<std::uint64_t> target = decoder.branchTarget(address);
            const auto object = registers.find(ZYDIS_REGISTER_RCX);
            const Source result = target ? callResult(code, decoder, *target, object != registers.end() ? object->second : Source{}) : Source{};
            for (const ZydisRegister reg : volatileRegisters)
                registers.erase(reg);
            registers[ZYDIS_REGISTER_RAX] = result;
            //! callResult decoded the callee, so the call is decoded again for its length.
            if (!decoder.decode(address))
                return std::nullopt;
            continue;
        }
        if (operands.empty())
            continue;
        const ZydisDecodedOperand& destination = operands[0];
        if (destination.type == ZYDIS_OPERAND_TYPE_MEMORY && destination.mem.base == ZYDIS_REGISTER_RSP && destination.mem.index == ZYDIS_REGISTER_NONE) {
            if (instruction.mnemonic == ZYDIS_MNEMONIC_MOV && operands.size() == 2) {
                Source source;
                if (operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
                    source = {Source::Kind::Constant, operands[1].imm.value.u};
                else if (operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
                    if (const auto found = registers.find(widest(operands[1].reg.value)); found != registers.end())
                        source = found->second;
                stack[destination.mem.disp.value] = source;
            }
            continue;
        }
        if (destination.type != ZYDIS_OPERAND_TYPE_REGISTER || !(destination.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
            continue;
        const ZydisRegister reg = widest(destination.reg.value);
        Source source;
        if (instruction.mnemonic == ZYDIS_MNEMONIC_LEA) {
            if (const std::optional<std::uint64_t> target = decoder.ripTarget(address, operands[1]))
                source = {Source::Kind::Constant, *target};
        } else if (instruction.mnemonic == ZYDIS_MNEMONIC_MOV && operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            source = {Source::Kind::Constant, operands[1].imm.value.u};
        } else if (instruction.mnemonic == ZYDIS_MNEMONIC_MOV && destination.size == 32 && operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            if (const std::optional<std::uint64_t> target = decoder.ripTarget(address, operands[1]))
                source = {Source::Kind::Global, *target};
        } else if (instruction.mnemonic == ZYDIS_MNEMONIC_MOV && operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            if (const auto found = registers.find(widest(operands[1].reg.value)); found != registers.end())
                source = found->second;
        } else if (instruction.mnemonic == ZYDIS_MNEMONIC_XOR && operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER && widest(operands[1].reg.value) == reg) {
            source = {Source::Kind::Constant, 0};
        } else if (const auto found = registers.find(reg); found != registers.end() && found->second.kind == Source::Kind::Global && clearsLicenseeBit(decoder)) {
            source = found->second;
            source.masked = true;
        }
        registers[reg] = source;
    }
    return std::nullopt;
}

//! FNetworkVersion::GetNetworkCompatibleChangelist returns its static ReturnedVersion, or the compatible changelist when that is 0: after `mov r32, [ReturnedVersion]`, `test` and a conditional jump comes the call or jump to the function that returns it.
std::optional<std::uint32_t> readFallback(Image& image, Code& code, Decoder& decoder, std::uint64_t returnedVersion) {
    std::optional<std::uint32_t> found;
    bool conflicting = false;
    image.scan(image.getCode(), 8, [&](std::uint64_t at, std::span<const std::byte> bytes, std::size_t positions) {
        findByte(bytes, positions, 0x8B, [&](std::size_t i) {
            if (i + 6 > bytes.size() || (std::uint8_t(bytes[i + 1]) & 0xC7) != 0x05 ||
                at + i + 6 + std::uint64_t(std::int64_t(std::int32_t(readU32(bytes, i + 2)))) != returnedVersion)
                return;
            //! The load may have a REX prefix.
            for (const std::uint64_t start : {at + i - 1, at + i}) {
                if (!decoder.decode(start) || decoder.instruction.mnemonic != ZYDIS_MNEMONIC_MOV || decoder.ripTarget(start, decoder.visible()[1]) != returnedVersion)
                    continue;
                bool tested = false;
                bool branched = false;
                std::uint64_t address = start + decoder.instruction.length;
                for (int step = 0; step < maxFollow && decoder.decode(address); step++, address += decoder.instruction.length) {
                    const ZydisMnemonic mnemonic = decoder.instruction.mnemonic;
                    if (mnemonic == ZYDIS_MNEMONIC_RET)
                        break;
                    if (mnemonic == ZYDIS_MNEMONIC_TEST) {
                        tested = true;
                    } else if (tested && (mnemonic == ZYDIS_MNEMONIC_JNZ || mnemonic == ZYDIS_MNEMONIC_JZ)) {
                        branched = true;
                    } else if (branched && (mnemonic == ZYDIS_MNEMONIC_CALL || mnemonic == ZYDIS_MNEMONIC_JMP)) {
                        if (const std::optional<std::uint64_t> target = decoder.branchTarget(address)) {
                            if (const std::optional<std::uint32_t> value = valueOf(image, callResult(code, decoder, *target, {}))) {
                                conflicting = conflicting || (found && *found != *value);
                                found = *value;
                            }
                        }
                        break;
                    }
                }
                break;
            }
        });
    });
    return conflicting ? std::nullopt : found;
}

std::expected<std::uint32_t, std::string> readNetChangelist(Image& image, Code& code) {
    Decoder decoder(image);
    //! UE 4.20 and later bind ReturnedVersion to the console variable networkversionoverride; its address is the argument after the name.
    const std::vector<std::uint64_t> names = findWideLiterals(image, networkVersionOverride);
    std::unordered_set<std::uint64_t> variables;
    for (const auto& [lea, name] : findLeas(image, names)) {
        const std::optional<std::array<Source, maxArguments>> arguments = argumentsAt(image, code, decoder, lea);
        if (!arguments)
            continue;
        for (std::size_t i = 0; i + 1 < registerArguments; i++)
            if ((*arguments)[i].kind == Source::Kind::Constant && (*arguments)[i].value == name && (*arguments)[i + 1].kind == Source::Kind::Constant &&
                image.isWritableData((*arguments)[i + 1].value))
                variables.insert((*arguments)[i + 1].value);
    }
    if (variables.size() > 1)
        return std::unexpected("The console variable networkversionoverride is bound to several variables.");
    if (variables.size() == 1) {
        const std::uint64_t returnedVersion = *variables.begin();
        if (const std::uint32_t value = image.read<std::uint32_t>(returnedVersion))
            return value;
        if (const std::optional<std::uint32_t> value = readFallback(image, code, decoder, returnedVersion))
            return *value;
        return std::unexpected("The compatible changelist that FNetworkVersion falls back to is not a constant.");
    }
    //! UE 4.18 and 4.19 have no override: FNetworkVersion::GetLocalNetworkVersion formats the changelist after "NetCL: ".
    std::unordered_map<std::uint64_t, std::size_t> formats;
    const std::span<const std::byte> mark = std::as_bytes(std::span(netChangelistMark));
    image.scan(image.getReadOnlyData(), mark.size(), [&](std::uint64_t at, std::span<const std::byte> bytes, std::size_t positions) {
        findAll(bytes, positions, mark, [&](std::size_t i) {
            if ((at + i) % 2 != 0)
                return;
            //! Bytes before a literal can read as characters, so each start back to the terminator is a candidate; only the true one is referenced.
            const std::u16string before = readBranchBefore(image, at + i);
            for (std::size_t start = 0; start <= before.size(); start++) {
                std::size_t specifiers = 0;
                for (std::size_t c = start; c < before.size(); c++)
                    if (before[c] == u'%')
                        c + 1 < before.size() && before[c + 1] == u'%' ? void(c++) : void(specifiers++);
                formats.emplace(at + i - 2 * (before.size() - start), specifiers);
            }
        });
    });
    std::vector<std::uint64_t> starts;
    for (const auto& [start, specifiers] : formats)
        starts.push_back(start);
    std::optional<std::uint32_t> found;
    for (const auto& [lea, format] : findLeas(image, starts)) {
        //! FString::Printf(Result, Format, ...): the changelist follows the specifiers before it.
        const std::size_t index = 2 + formats[format];
        const std::optional<std::array<Source, maxArguments>> arguments = argumentsAt(image, code, decoder, lea);
        if (!arguments || index >= maxArguments || (*arguments)[1].kind != Source::Kind::Constant || (*arguments)[1].value != format)
            continue;
        const std::optional<std::uint32_t> value = valueOf(image, (*arguments)[index]);
        if (!value)
            continue;
        if (found && *found != *value)
            return std::unexpected("The network version is formatted with different changelists.");
        found = value;
    }
    if (!found)
        return std::unexpected("FNetworkVersion::GetNetworkCompatibleChangelist is not found.");
    return *found;
}

struct PackageFileVersion {
    std::int32_t ue4;
    std::int32_t ue5;
};

//! GPackageFileUEVersion (UE4, UE5) lies right before GOldestLoadablePackageFileUEVersion (VER_UE4_OLDEST_LOADABLE_PACKAGE, 0), as constant data or written by a static initializer.
std::expected<PackageFileVersion, std::string> readPackageFileVersionUE5(Image& image) {
    std::vector<PackageFileVersion> found;
    std::uint8_t oldest[8]{};
    std::memcpy(oldest, &oldestLoadableUE4, sizeof(oldestLoadableUE4));
    const std::span<const std::byte> pattern = std::as_bytes(std::span(oldest));
    for (const Section* section : {&image.getReadOnlyData(), &image.getData()}) {
        std::vector<std::uint64_t> candidates;
        image.scan(*section, pattern.size(), [&](std::uint64_t at, std::span<const std::byte> bytes, std::size_t positions) {
            findAll(bytes, positions, pattern, [&](std::size_t i) {
                if ((at + i) % 8 == 0 && at + i >= section->address + 8)
                    candidates.push_back(at + i - 8);
            });
        });
        for (const std::uint64_t candidate : candidates) {
            const PackageFileVersion version = image.read<PackageFileVersion>(candidate);
            if (version.ue4 >= oldestLoadableUE4 && version.ue4 < initialUE5 && version.ue5 >= initialUE5 && version.ue5 < maxUE5 &&
                std::ranges::none_of(found, [&](const PackageFileVersion& other) { return other.ue4 == version.ue4 && other.ue5 == version.ue5; }))
                found.push_back(version);
        }
    }
    if (found.empty())
        return std::unexpected("No GPackageFileUEVersion is found next to GOldestLoadablePackageFileUEVersion.");
    if (found.size() > 1)
        return std::unexpected("Several candidates for GPackageFileUEVersion are found.");
    return found.front();
}

//! UE 4 reads the constant GPackageFileUE4Version where a package file summary, which starts with PACKAGE_FILE_TAG, is unversioned or checked.
std::expected<PackageFileVersion, std::string> readPackageFileVersionUE4(Image& image, Code& code) {
    std::unordered_set<std::uint64_t> functions;
    const std::uint32_t tag = packageFileTag;
    const std::span<const std::byte> pattern = std::as_bytes(std::span(&tag, 1));
    image.scan(image.getCode(), pattern.size(), [&](std::uint64_t at, std::span<const std::byte> bytes, std::size_t positions) {
        findAll(bytes, positions, pattern, [&](std::size_t i) {
            if (const std::optional<std::uint64_t> function = image.findFunction(at + i))
                functions.insert(*function);
        });
    });
    std::unordered_set<std::int32_t> versions;
    for (const std::uint64_t function : functions)
        for (const std::uint64_t address : code.findReferences(function).data)
            if (image.isReadOnlyData(address) && address % 4 == 0)
                if (const std::int32_t value = image.read<std::int32_t>(address); value >= oldestLoadableUE4 && value < initialUE5)
                    versions.insert(value);
    if (versions.empty())
        return std::unexpected("No code that checks PACKAGE_FILE_TAG reads GPackageFileUE4Version.");
    if (versions.size() > 1)
        return std::unexpected("The code that checks PACKAGE_FILE_TAG reads several package file versions.");
    return PackageFileVersion{*versions.begin(), 0};
}

}

std::expected<Versioning, std::string> readVersioning(Image& image, Code& code, const std::span<const std::uint64_t> initializers, std::string& buildVersion, std::vector<std::string>& warnings) {
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
    std::expected<PackageFileVersion, std::string> package = engine->major >= 5 ? readPackageFileVersionUE5(image) : readPackageFileVersionUE4(image, code);
    if (!package)
        return std::unexpected(std::move(package.error()));
    versioning.fileVersionUE4 = package->ue4;
    versioning.fileVersionUE5 = package->ue5;
    if (std::expected<std::uint32_t, std::string> netChangelist = readNetChangelist(image, code))
        versioning.netChangelist = *netChangelist;
    else
        warnings.push_back(std::format("The usmap has the network changelist 0: {}", netChangelist.error()));
    versioning.engine = std::move(*engine);
    return versioning;
}

}
