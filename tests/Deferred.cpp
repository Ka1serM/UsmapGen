#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "UsmapGen/UsmapGen.h"

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const char* label) {
    checks++;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        failures++;
    }
}

constexpr std::uint64_t base = 0x180000000;
constexpr std::uint32_t entry = 0x1000, startup = 0x1100, batch = 0x1200, deferred = 0x1400;
constexpr std::uint32_t construct = 0x1800, seedConstruct = 0x1900, rootConstruct = 0x1A00, packageConstruct = 0x1B00;
constexpr std::uint32_t stub = 0x1C00, getter = 0x1D00;
constexpr std::uint32_t packageWide = 0x3100, nameWide = 0x3180, rootWide = 0x3200;
constexpr std::uint32_t packageName = 0x3280, name = 0x3300, seedName = 0x3340;
constexpr std::uint32_t firstName = 0x3380, secondName = 0x33C0;
constexpr std::uint32_t pointers = 0x3400, classRecords = 0x3500, structRecords = 0x3580, initializers = 0x3600;
constexpr std::uint32_t params = 0x5100, seedParams = 0x5180, rootParams = 0x5200, packageParams = 0x5280;
constexpr std::uint32_t info = 0x5300, properties = 0x5400, firstProperty = 0x5480, secondProperty = 0x5500;

class Fixture final : public UsmapGen::Source {
public:
    std::vector<std::byte> bytes = std::vector<std::byte>(0x6000);
    std::uint32_t cursor = 0;
    std::array<std::uint32_t, 5> stores{};
    std::uint32_t stride = 0;
    std::uint32_t branch = 0;

    std::uint64_t getSize() const noexcept override { return bytes.size(); }
    std::expected<void, UsmapGen::Error> read(std::uint64_t offset, std::span<std::byte> out) const noexcept override {
        if (offset > bytes.size() || out.size() > bytes.size() - offset)
            return std::unexpected(UsmapGen::Error{"Synthetic PE read outside file"});
        std::memcpy(out.data(), bytes.data() + offset, out.size());
        return {};
    }
    template <class T> void put(std::uint32_t at, T value) {
        std::memcpy(bytes.data() + at, &value, sizeof(value));
    }
    void pointer(std::uint32_t at, std::uint32_t rva) { put(at, base + rva); }
    template <class C, std::size_t N> void text(std::uint32_t at, const C (&value)[N]) {
        std::memcpy(bytes.data() + at, value, sizeof(value));
    }
    void emit(std::initializer_list<unsigned char> values) {
        for (auto value : values) bytes[cursor++] = std::byte(value);
    }
    void relative(std::uint32_t target) {
        put(cursor, std::int32_t(target - (cursor + 4)));
        cursor += 4;
    }
    void call(std::uint32_t target) { emit({0xE8}); relative(target); }
    void lea(int reg, std::uint32_t target) {
        emit({static_cast<unsigned char>(0x48 | (reg >= 8 ? 4 : 0)), 0x8D,
              static_cast<unsigned char>(0x05 | ((reg & 7) << 3))});
        relative(target);
    }
    void load(int reg, std::uint32_t target) {
        emit({static_cast<unsigned char>(0x48 | (reg >= 8 ? 4 : 0)), 0x8B,
              static_cast<unsigned char>(0x05 | ((reg & 7) << 3))});
        relative(target);
    }
    void immediate(int reg, std::uint64_t value) {
        emit({static_cast<unsigned char>(0x48 | (reg >= 8 ? 1 : 0)), static_cast<unsigned char>(0xB8 + (reg & 7))});
        put(cursor, value);
        cursor += 8;
    }
    void parameterCall(std::uint32_t function, std::uint32_t table, std::uint32_t storage = info) {
        cursor = function;
        lea(1, storage);
        lea(2, table);
        call(stub);
        emit({0xC3});
    }
    Fixture() {
        put<std::uint32_t>(0x3C, 0x80);
        put<std::uint32_t>(0x80, 0x4550);
        put<std::uint16_t>(0x84, 0x8664);
        put<std::uint16_t>(0x86, 3);
        put<std::uint16_t>(0x94, 240);
        put<std::uint16_t>(0x98, 0x20B);
        put<std::uint32_t>(0x98 + 16, entry);
        put<std::uint64_t>(0x98 + 24, base);
        put<std::uint32_t>(0x98 + 108, 16);
        put<std::uint32_t>(0x98 + 112 + 24, 0x4E00);
        put<std::uint32_t>(0x98 + 112 + 28, 12);
        constexpr std::array names{".text", ".rdata", ".data"};
        for (std::uint32_t i = 0; i < names.size(); i++) {
            const auto at = 0x188 + 40 * i;
            std::memcpy(bytes.data() + at, names[i], std::strlen(names[i]));
            put<std::uint32_t>(at + 8, i == 2 ? 0x1000 : 0x2000);
            put<std::uint32_t>(at + 12, 0x1000 + 0x2000 * i);
            put<std::uint32_t>(at + 16, i == 2 ? 0x1000 : 0x2000);
            put<std::uint32_t>(at + 20, 0x1000 + 0x2000 * i);
        }
        text(packageWide, u"/Script/Synthetic");
        text(nameWide, u"DeferredFixture");
        text(rootWide, u"UFixtureRoot");
        text(packageName, "/Script/Synthetic");
        text(name, "DeferredFixture");
        text(seedName, "SeedFixture");
        text(firstName, "First");
        text(secondName, "Second");
        pointer(pointers, nameWide);
        pointer(pointers + 8, info);
        pointer(initializers, batch);
        pointer(initializers + 8, deferred);
        pointer(classRecords, rootConstruct);
        pointer(classRecords + 8, getter);
        pointer(classRecords + 16, rootWide);
        pointer(structRecords, seedConstruct);
        pointer(structRecords + 16, nameWide);
        for (auto table : {params, seedParams}) {
            pointer(table, packageConstruct);
            pointer(table + 24, table == params ? name : seedName);
            pointer(table + 32, properties);
            put<std::uint16_t>(table + 40, 2);
        }
        pointer(properties, firstProperty);
        pointer(properties + 8, secondProperty);
        for (auto property : {firstProperty, secondProperty}) {
            pointer(property, property == firstProperty ? firstName : secondName);
            put<std::uint8_t>(property + 24, 3);
            put<std::uint16_t>(property + 48, 1);
        }
        pointer(rootParams, getter);
        pointer(rootParams + 24, 0x3700);
        pointer(0x3700, packageConstruct);
        put<std::uint32_t>(rootParams + 56, 1);
        pointer(packageParams, packageName);
        parameterCall(construct, params);
        parameterCall(seedConstruct, seedParams);
        parameterCall(rootConstruct, rootParams);
        parameterCall(packageConstruct, packageParams);
        cursor = stub; emit({0xC3});
        cursor = getter; emit({0xC3});
        cursor = entry; call(startup); emit({0xC3});
        cursor = startup;
        lea(1, initializers); lea(2, initializers + 16); call(stub); emit({0xC3});
        cursor = batch;
        emit({0x48, 0x83, 0xEC, 0x38});
        lea(1, packageWide); lea(2, classRecords); immediate(8, 1); lea(9, structRecords);
        emit({0x48, 0xC7, 0x44, 0x24, 0x20, 1, 0, 0, 0});
        emit({0x48, 0xC7, 0x44, 0x24, 0x28, 0, 0, 0, 0});
        emit({0x48, 0xC7, 0x44, 0x24, 0x30, 0, 0, 0, 0});
        call(stub); emit({0x48, 0x83, 0xC4, 0x38, 0xC3});
        cursor = deferred;
        load(6, pointers + 8); load(7, pointers);
        call(stub);
        //! A forward capacity branch may call a grow helper; both paths retain nonvolatile pointers.
        branch = cursor;
        emit({0x75, 0x05}); call(stub);
        lea(14, construct); lea(15, packageWide);
        stride = cursor;
        emit({0x48, 0x8D, 0x0C, 0x80, 0x48, 0x8B, 0x03});
        stores[0] = cursor; emit({0x4C, 0x89, 0x34, 0xC8});
        stores[1] = cursor; emit({0x48, 0xC7, 0x44, 0xC8, 8, 0, 0, 0, 0});
        stores[2] = cursor; emit({0x4C, 0x89, 0x7C, 0xC8, 16});
        stores[3] = cursor; emit({0x48, 0x89, 0x7C, 0xC8, 24});
        stores[4] = cursor; emit({0x48, 0x89, 0x74, 0xC8, 32});
        call(stub); emit({0xC3});
    }
};

//! This fixture emits only IntProperty fields and no enums; check its complete serialized output through the original public API.
void run(Fixture& fixture, bool present, const char* label) {
    auto output = UsmapGen::generateUsmap(fixture);
    if (!output) std::fprintf(stderr, "%s: %s\n", label, output.error().message.c_str());
    check(bool(output), label);
    if (!output) return;
    std::size_t offset = 0;
    bool valid = true;
    const auto read = [&]<class T>() {
        T value{};
        if (offset > output->size() || sizeof(T) > output->size() - offset) {
            valid = false;
            return value;
        }
        std::memcpy(&value, output->data() + offset, sizeof(T));
        offset += sizeof(T);
        return value;
    };
    check(read.operator()<std::uint16_t>() == 0x30C4, "usmap magic");
    check(read.operator()<std::uint8_t>() == 4, "explicit-enum-values format");
    check(read.operator()<std::uint32_t>() == 0, "no versioning header");
    check(read.operator()<std::uint8_t>() == 0, "uncompressed fixture");
    check(read.operator()<std::uint32_t>() == output->size() - 16, "compressed size");
    check(read.operator()<std::uint32_t>() == output->size() - 16, "decompressed size");
    std::vector<std::string> names;
    const auto nameCount = read.operator()<std::uint32_t>();
    for (std::uint32_t i = 0; i < nameCount && valid; i++) {
        const auto length = read.operator()<std::uint16_t>();
        if (offset > output->size() || length > output->size() - offset) { valid = false; break; }
        names.emplace_back(reinterpret_cast<const char*>(output->data() + offset), length);
        offset += length;
    }
    const auto nameAt = [&](std::uint32_t index) -> std::string_view {
        if (index >= names.size()) { valid = false; return {}; }
        return names[index];
    };
    check(read.operator()<std::uint32_t>() == 0, "no unexpected enums");
    const auto structCount = read.operator()<std::uint32_t>();
    check(structCount == (present ? 3u : 2u), "no spurious or duplicate schemas");
    std::size_t found = 0;
    for (std::uint32_t i = 0; i < structCount && valid; i++) {
        const auto typeName = nameAt(read.operator()<std::uint32_t>());
        check(read.operator()<std::uint32_t>() == UINT32_MAX, "no unexpected super");
        const auto slots = read.operator()<std::uint16_t>();
        const auto count = read.operator()<std::uint16_t>();
        const bool target = typeName == "DeferredFixture";
        found += target;
        check(slots == count && count == (typeName == "FixtureRoot" ? 0 : 2), "complete schema slot count");
        for (std::uint16_t j = 0; j < count && valid; j++) {
            check(read.operator()<std::uint16_t>() == j, "property index");
            check(read.operator()<std::uint8_t>() == 1, "property array dimension");
            check(nameAt(read.operator()<std::uint32_t>()) == (j == 0 ? "First" : "Second"), "property name and order");
            check(read.operator()<std::uint8_t>() == 2, "IntProperty wire type");
        }
    }
    check(valid && offset == output->size(), "complete bounded output parse");
    check(found == (present ? 1 : 0), label);
}

}

int main() {
    Fixture good;
    run(good, true, "inlined deferred registration is discovered");
    Fixture absent;
    absent.bytes[deferred] = std::byte{0xC3};
    run(absent, false, "baseline collector control");
    for (std::size_t i = 0; i < 5; i++) {
        Fixture truncated;
        truncated.bytes[truncated.stores[i]] = std::byte{0xC3};
        run(truncated, false, "partial record rejected");
    }
    Fixture duplicate;
    duplicate.pointer(initializers + 16, deferred);
    duplicate.cursor = startup;
    duplicate.lea(1, initializers); duplicate.lea(2, initializers + 24); duplicate.call(stub); duplicate.emit({0xC3});
    run(duplicate, true, "duplicate initializer deduplicated");
    Fixture batched;
    batched.pointer(structRecords + 40, construct);
    batched.pointer(structRecords + 56, nameWide);
    batched.bytes[batch + 35 + 5] = std::byte{2};
    run(batched, true, "batch and deferred registration deduplicated");
    Fixture single;
    single.cursor = 0x1E00;
    single.lea(1, construct); single.lea(2, packageWide); single.lea(8, nameWide); single.call(stub); single.emit({0xC3});
    single.pointer(initializers + 16, 0x1E00);
    single.cursor = startup;
    single.lea(1, initializers); single.lea(2, initializers + 24); single.call(stub); single.emit({0xC3});
    run(single, true, "single and deferred registration deduplicated");
    Fixture alias;
    alias.parameterCall(0x1F00, params);
    alias.cursor = alias.stores[4] + 5;
    alias.lea(14, 0x1F00);
    const auto appendSize = alias.stores[4] + 5 - alias.stride;
    std::copy_n(alias.bytes.begin() + alias.stride, appendSize, alias.bytes.begin() + alias.cursor);
    alias.cursor += appendSize;
    alias.call(stub); alias.emit({0xC3});
    run(alias, true, "different constructors sharing params deduplicated");
    for (std::size_t i = 1; i < 5; i++) {
        Fixture wrongOffset;
        wrongOffset.bytes[wrongOffset.stores[i] + 4] = std::byte{0x38};
        run(wrongOffset, false, "near-match field offset rejected");
    }
    Fixture wrongStride;
    wrongStride.bytes[wrongStride.stride + 3] = std::byte{0x40};
    run(wrongStride, false, "non-40-byte stride rejected");
    Fixture nonzero;
    nonzero.bytes[nonzero.stores[1] + 5] = std::byte{1};
    run(nonzero, false, "non-null secondary constructor rejected");
    Fixture wrongName;
    wrongName.text(nameWide, u"OtherFixture");
    run(wrongName, false, "registered name must match params");
    Fixture wrongPackage;
    wrongPackage.text(packageWide, u"/Script/Other");
    run(wrongPackage, false, "registered package must match outer");
    Fixture wrongInfo;
    wrongInfo.pointer(pointers + 8, info + 16);
    run(wrongInfo, false, "registration info must be construction output");
    Fixture notWritable;
    notWritable.pointer(pointers + 8, nameWide);
    run(notWritable, false, "registration info must be writable");
    Fixture malformed;
    malformed.pointer(params + 24, nameWide);
    run(malformed, false, "malformed struct name rejected");
    Fixture noParams;
    noParams.bytes[construct] = std::byte{0xC3};
    run(noParams, false, "code pointer alone is not a registration");
    Fixture registers;
    registers.cursor = registers.stride - 14;
    registers.lea(12, construct); registers.lea(13, packageWide);
    registers.bytes[registers.stride + 2] = std::byte{0x14};
    registers.bytes[registers.stores[0] + 2] = std::byte{0x24};
    registers.bytes[registers.stores[2] + 2] = std::byte{0x6C};
    for (auto at : registers.stores) registers.bytes[at + 3] = std::byte{0xD0};
    run(registers, true, "different source and index registers");
    Fixture direct;
    direct.cursor = deferred;
    direct.lea(6, info); direct.lea(7, nameWide);
    run(direct, true, "direct pointers instead of read-only pointer loads");
    Fixture conflictingBranch;
    conflictingBranch.cursor = conflictingBranch.branch + 2;
    conflictingBranch.emit({0x31, 0xFF, 0x90, 0x90, 0x90});
    run(conflictingBranch, false, "conflicting forward branch value rejected");
    Fixture skippedDefinition;
    skippedDefinition.cursor = skippedDefinition.branch + 2;
    skippedDefinition.emit({0x90, 0x90, 0x90, 0x90, 0x90});
    skippedDefinition.bytes[skippedDefinition.branch + 1] = std::byte{0x13};
    run(skippedDefinition, false, "definition skipped by branch rejected");
    Fixture backward;
    backward.bytes[backward.branch + 1] = std::byte{0xFE};
    run(backward, false, "loop requires unsupported fixpoint and is rejected");
    Fixture volatileName;
    volatileName.cursor = deferred + 7;
    volatileName.load(2, pointers);
    volatileName.bytes[volatileName.stores[3] + 2] = std::byte{0x54};
    run(volatileName, false, "call-clobbered volatile pointer rejected");
    Fixture narrowName;
    narrowName.bytes[deferred + 7] = std::byte{0x40};
    run(narrowName, false, "32-bit pointer load rejected");
    Fixture mutablePointer;
    mutablePointer.pointer(0x5800, nameWide);
    mutablePointer.cursor = deferred + 7;
    mutablePointer.load(7, 0x5800);
    run(mutablePointer, false, "mutable pointer source is not a constant");
    Fixture truncatedPointer;
    truncatedPointer.cursor = deferred + 7;
    truncatedPointer.load(7, 0x4FFC);
    run(truncatedPointer, false, "read-only pointer must fit in section");
    Fixture nameBoundary;
    nameBoundary.pointer(pointers, 0x4FFC);
    nameBoundary.put<std::uint32_t>(0x4FFC, 0x00410041);
    run(nameBoundary, false, "wide name must terminate inside section");
    Fixture longName;
    longName.pointer(pointers, 0x3800);
    for (std::uint32_t at = 0x3800; at < 0x3A00; at += 2) longName.put<char16_t>(at, u'A');
    run(longName, false, "name scan is bounded");
    Fixture headerBoundary;
    headerBoundary.parameterCall(construct, 0x5FF0);
    run(headerBoundary, false, "struct params header must fit in section");
    Fixture propertyBoundary;
    propertyBoundary.pointer(params + 32, 0x5FF8);
    run(propertyBoundary, false, "property pointer array must fit in section");
    Fixture noProperties;
    noProperties.put<std::uint64_t>(params + 32, 0);
    run(noProperties, false, "nonempty struct requires property array");
    Fixture ambiguous;
    ambiguous.cursor = construct + 19;
    ambiguous.lea(1, info); ambiguous.lea(2, seedParams); ambiguous.call(stub); ambiguous.emit({0xC3});
    run(ambiguous, false, "ambiguous constructor parameter tables rejected");
    Fixture wrongScale;
    wrongScale.bytes[wrongScale.stores[2] + 3] = std::byte{0x88};
    run(wrongScale, false, "store scale must match stride");
    Fixture differentBase;
    differentBase.bytes[differentBase.stores[3] + 3] = std::byte{0xCB};
    run(differentBase, false, "all fields must share a destination base");
    Fixture narrowStore;
    narrowStore.bytes[narrowStore.stores[3]] = std::byte{0x40};
    run(narrowStore, false, "all fields must be qword stores");
    Fixture interrupted;
    interrupted.cursor = interrupted.stores[3]; interrupted.call(stub);
    run(interrupted, false, "intervening call breaks record construction");
    //! Both diamond arms reach the append: a JMP predecessor must participate in the merge.
    for (const auto secondary : {0ull, 1ull}) {
        Fixture diamond;
        diamond.cursor = deferred;
        diamond.emit({0x85, 0xC0, 0x0F, 0x85}); diamond.relative(0x1460);
        diamond.immediate(3, secondary);
        diamond.emit({0xE9}); diamond.relative(0x1480);
        diamond.cursor = 0x1460;
        diamond.immediate(3, 0);
        while (diamond.cursor < 0x1480) diamond.emit({0x90});
        diamond.load(6, pointers + 8); diamond.load(7, pointers);
        diamond.lea(14, construct); diamond.lea(15, packageWide);
        diamond.emit({0x48, 0x8D, 0x0C, 0x80, 0x49, 0x8B, 0x04, 0x24});
        diamond.emit({0x4C, 0x89, 0x34, 0xC8});
        diamond.emit({0x48, 0x89, 0x5C, 0xC8, 8});
        diamond.emit({0x4C, 0x89, 0x7C, 0xC8, 16});
        diamond.emit({0x48, 0x89, 0x7C, 0xC8, 24});
        diamond.emit({0x48, 0x89, 0x74, 0xC8, 32});
        diamond.call(stub); diamond.emit({0xC3});
        run(diamond, secondary == 0, secondary == 0 ? "agreeing diamond predecessors accepted" : "conflicting JMP predecessor rejected");
    }
    //! An unreachable linear fallthrough is not a reachable predecessor with unknown registers.
    for (const auto gap : {0u, 16u}) {
        Fixture jump;
        jump.cursor = jump.stride;
        const auto target = jump.stride + 5 + gap;
        const auto length = jump.stores[4] + 5 - jump.stride;
        std::vector<std::byte> append(jump.bytes.begin() + jump.stride, jump.bytes.begin() + jump.stride + length);
        jump.emit({0xE9}); jump.relative(target);
        std::copy(append.begin(), append.end(), jump.bytes.begin() + target);
        jump.cursor = target + length;
        jump.call(stub); jump.emit({0xC3});
        run(jump, true, "sole forward JMP predecessor retains pointers");
    }
    Fixture nameEnd;
    nameEnd.put<std::uint32_t>(0x188 + 40 + 8, 0x1F00);
    nameEnd.put<std::uint32_t>(0x188 + 40 + 16, 0x1F00);
    nameEnd.text(0x4EF0, "Other");
    nameEnd.pointer(params + 24, 0x4EF0);
    run(nameEnd, false, "bounded mismatching params name does not poison generation");
    nameEnd.bytes[deferred] = std::byte{0xC3};
    run(nameEnd, false, "name boundary control without speculative append");
    //! Accepted names are read again by ordinary reflection, whose chunk reads must also respect section ends.
    for (const auto kind : {0, 1, 2}) {
        Fixture acceptedEnd;
        acceptedEnd.put<std::uint32_t>(0x188 + 40 + 8, 0x1F00);
        acceptedEnd.put<std::uint32_t>(0x188 + 40 + 16, 0x1F00);
        if (kind == 0) {
            acceptedEnd.text(0x4EF0, "DeferredFixture");
            acceptedEnd.pointer(params + 24, 0x4EF0);
        } else if (kind == 1) {
            acceptedEnd.text(0x4EFA, "First");
            acceptedEnd.pointer(firstProperty, 0x4EFA);
        } else {
            constexpr auto at = 0x4F00 - sizeof("/Script/Synthetic");
            acceptedEnd.text(at, "/Script/Synthetic");
            acceptedEnd.pointer(packageParams, at);
        }
        run(acceptedEnd, true, kind == 0 ? "accepted struct name at section end" :
                               kind == 1 ? "accepted property name at section end" : "accepted package name at section end");
    }
    Fixture unterminatedEnd;
    unterminatedEnd.put<std::uint32_t>(0x188 + 40 + 8, 0x1F00);
    unterminatedEnd.put<std::uint32_t>(0x188 + 40 + 16, 0x1F00);
    unterminatedEnd.text(0x4EF0, "DeferredFixture");
    unterminatedEnd.bytes[0x4EFF] = std::byte{'X'};
    unterminatedEnd.pointer(params + 24, 0x4EF0);
    run(unterminatedEnd, false, "unterminated deferred name at section end rejected without poisoning");
    //! The normal reader must not manufacture a terminator from bytes outside the section either.
    unterminatedEnd.pointer(seedParams + 24, 0x4EF0);
    check(!UsmapGen::generateUsmap(unterminatedEnd), "unterminated admitted name at section end remains an error");
    //! A 32-bit GPR write clears its upper half; 16-bit writes do not establish a full pointer.
    const std::vector<std::vector<unsigned char>> zeroing{
        {0x31, 0xDB},                         // xor ebx, ebx
        {0xBB, 0, 0, 0, 0},                   // mov ebx, 0
        {0x48, 0x31, 0xDB},                   // xor rbx, rbx
        {0x89, 0xDB},                         // mov ebx, ebx: truncate the known high half
        {0x66, 0x31, 0xDB},                   // xor bx, bx: high half survives
        {0x66, 0xBB, 0, 0},                   // mov bx, 0: high half survives
        {0x31, 0xCB},                         // xor ebx, ecx: not a self-XOR
        {0xBB, 1, 0, 0, 0},                   // mov ebx, 1: non-null secondary
    };
    for (std::size_t i = 0; i < zeroing.size(); i++) {
        Fixture zero;
        zero.cursor = deferred;
        zero.immediate(3, 0x100000000ull);
        for (const auto byte : zeroing[i]) zero.emit({byte});
        zero.load(6, pointers + 8); zero.load(7, pointers);
        zero.lea(14, construct); zero.lea(15, packageWide);
        zero.emit({0x48, 0x8D, 0x0C, 0x80, 0x49, 0x8B, 0x04, 0x24});
        zero.emit({0x4C, 0x89, 0x34, 0xC8});
        zero.emit({0x48, 0x89, 0x5C, 0xC8, 8});
        zero.emit({0x4C, 0x89, 0x7C, 0xC8, 16});
        zero.emit({0x48, 0x89, 0x7C, 0xC8, 24});
        zero.emit({0x48, 0x89, 0x74, 0xC8, 32});
        zero.call(stub); zero.emit({0xC3});
        const std::array labels{
            "32-bit self-XOR establishes null secondary", "32-bit immediate MOV establishes null secondary",
            "64-bit self-XOR establishes null secondary", "32-bit register MOV zero-extends secondary",
            "16-bit self-XOR cannot establish null secondary", "16-bit immediate MOV cannot establish null secondary",
            "different-register XOR cannot establish null secondary", "32-bit nonzero secondary rejected"};
        run(zero, i < 4, labels[i]);
    }
    //! Cover every truncation of the reference qword, plus an exactly fitting descriptor.
    for (const auto property : {0x5FCCu, 0x5FC1u, 0x5FC2u, 0x5FC3u, 0x5FC4u, 0x5FC5u, 0x5FC6u, 0x5FC7u, 0x5FC0u}) {
        Fixture propertyEnd;
        propertyEnd.pointer(params + 32, 0x5600);
        propertyEnd.pointer(0x5600, property);
        propertyEnd.pointer(0x5608, secondProperty);
        propertyEnd.pointer(property, firstName);
        propertyEnd.put<std::uint8_t>(property + 24, 3);
        propertyEnd.put<std::uint16_t>(property + 48, 1);
        run(propertyEnd, property == 0x5FC0, "entire property descriptor must fit before admission");
        propertyEnd.bytes[deferred] = std::byte{0xC3};
        run(propertyEnd, false, "property boundary control without speculative append");
    }
    Fixture dynamicName;
    dynamicName.put<std::uint64_t>(params + 24, 0);
    dynamicName.cursor = construct;
    dynamicName.lea(0, name);
    dynamicName.emit({0x48, 0x89, 0x05}); dynamicName.relative(params + 24);
    dynamicName.lea(1, info); dynamicName.lea(2, params); dynamicName.call(stub); dynamicName.emit({0xC3});
    run(dynamicName, true, "constructor initializes function-local params");
    dynamicName.pointer(structRecords + 40, construct);
    dynamicName.pointer(structRecords + 56, nameWide);
    dynamicName.bytes[batch + 35 + 5] = std::byte{2};
    run(dynamicName, true, "identical local params initialized by batch control");
    Fixture dynamicPackage;
    dynamicPackage.parameterCall(rootConstruct, rootParams);
    //! The deferred struct uses a separate package constructor not prepared by the batch seed.
    dynamicPackage.pointer(params, 0x1E00);
    dynamicPackage.put<std::uint64_t>(0x5700, 0);
    dynamicPackage.cursor = 0x1E00;
    dynamicPackage.lea(0, packageName);
    dynamicPackage.emit({0x48, 0x89, 0x05}); dynamicPackage.relative(0x5700);
    dynamicPackage.lea(1, info); dynamicPackage.lea(2, 0x5700); dynamicPackage.call(stub); dynamicPackage.emit({0xC3});
    run(dynamicPackage, true, "outer constructor initializes function-local package params");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
