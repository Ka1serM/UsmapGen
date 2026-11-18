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

struct Property {
    const char* name;
    std::uint8_t kind;
    std::uint8_t wire;
    std::uint8_t dimension;
};

//! Dumper-7's generic multicast producer policy uses wire 13 for both multicast kinds.
constexpr std::array properties{
    Property{"Before", 0x03, 2, 1},
    Property{"Single", 0x1A, 6, 2},
    Property{"Inline", 0x1B, 13, 3},
    Property{"OnTakeAnyDamage", 0x1C, 13, 4},
    Property{"After", 0x1D, 11, 1},
};
constexpr std::uint64_t base = 0x180000000;
constexpr std::uint32_t entry = 0x1000, startup = 0x1100, batch = 0x1200;
constexpr std::uint32_t construct = 0x1800, rootConstruct = 0x1900, packageConstruct = 0x1A00;
constexpr std::uint32_t stub = 0x1B00, getter = 0x1C00;
constexpr std::uint32_t packageWide = 0x3100, nameWide = 0x3180, rootWide = 0x3200;
constexpr std::uint32_t packageName = 0x3280, name = 0x3300, propertyNames = 0x3380;
constexpr std::uint32_t classRecords = 0x3500, structRecords = 0x3580, initializers = 0x3600, dependencies = 0x3700;
constexpr std::uint32_t params = 0x5100, rootParams = 0x5180, packageParams = 0x5200, info = 0x5280;
constexpr std::uint32_t propertyPointers = 0x5300, descriptors = 0x5400;

//! Ordinary RegisterCompiledInInfo batch: one class and one struct, using UE 5.3 parameter layouts.
class Fixture final : public UsmapGen::Source {
public:
    std::vector<std::byte> bytes = std::vector<std::byte>(0x6000);
    std::uint32_t cursor = 0;

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
    void parameterCall(std::uint32_t function, std::uint32_t table) {
        cursor = function;
        lea(1, info); lea(2, table); call(stub); emit({0xC3});
    }
    explicit Fixture(std::uint8_t flags) {
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
        text(nameWide, u"DelegateFixture");
        text(rootWide, u"UActor");
        text(packageName, "/Script/Synthetic");
        text(name, "DelegateFixture");
        pointer(initializers, batch);
        pointer(classRecords, rootConstruct);
        pointer(classRecords + 8, getter);
        pointer(classRecords + 16, rootWide);
        pointer(structRecords, construct);
        pointer(structRecords + 16, nameWide);
        pointer(params, packageConstruct);
        pointer(params + 24, name);
        pointer(params + 32, propertyPointers);
        put<std::uint16_t>(params + 40, properties.size());
        for (std::uint32_t i = 0; i < properties.size(); i++) {
            const auto property = descriptors + 0x80 * i;
            const auto propertyName = propertyNames + 0x40 * i;
            std::memcpy(bytes.data() + propertyName, properties[i].name, std::strlen(properties[i].name) + 1);
            pointer(propertyPointers + 8 * i, property);
            pointer(property, propertyName);
            put<std::uint8_t>(property + 24, properties[i].kind | flags);
            put<std::uint16_t>(property + 48, properties[i].dimension);
        }
        pointer(rootParams, getter);
        pointer(rootParams + 24, dependencies);
        pointer(dependencies, packageConstruct);
        pointer(rootParams + 40, propertyPointers);
        put<std::uint32_t>(rootParams + 56, 1 | (properties.size() << 15));
        pointer(packageParams, packageName);
        parameterCall(construct, params);
        parameterCall(rootConstruct, rootParams);
        parameterCall(packageConstruct, packageParams);
        cursor = stub; emit({0xC3});
        cursor = getter; emit({0xC3});
        cursor = entry; call(startup); emit({0xC3});
        cursor = startup;
        lea(1, initializers); lea(2, initializers + 8); call(stub); emit({0xC3});
        cursor = batch;
        emit({0x48, 0x83, 0xEC, 0x38});
        lea(1, packageWide); lea(2, classRecords);
        emit({0x49, 0xB8, 1, 0, 0, 0, 0, 0, 0, 0});
        lea(9, structRecords);
        emit({0x48, 0xC7, 0x44, 0x24, 0x20, 1, 0, 0, 0});
        emit({0x48, 0xC7, 0x44, 0x24, 0x28, 0, 0, 0, 0});
        emit({0x48, 0xC7, 0x44, 0x24, 0x30, 0, 0, 0, 0});
        call(stub); emit({0x48, 0x83, 0xC4, 0x38, 0xC3});
    }
};

void run(std::uint8_t flags) {
    const auto output = UsmapGen::generateUsmap(Fixture(flags));
    if (!output) std::fprintf(stderr, "flags %#x: %s\n", flags, output.error().message.c_str());
    check(bool(output), "public generation succeeds");
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
    check(nameCount == properties.size() + 2, "complete name table");
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
    check(structCount == 2, "exactly the registered class and struct");
    for (std::uint32_t i = 0; i < structCount && valid; i++) {
        check(nameAt(read.operator()<std::uint32_t>()) == (i == 0 ? "Actor" : "DelegateFixture"), "schema name and order");
        check(read.operator()<std::uint32_t>() == UINT32_MAX, "no unexpected super");
        check(read.operator()<std::uint16_t>() == 11, "complete schema slot count");
        const auto count = read.operator()<std::uint16_t>();
        check(count == properties.size(), "complete property count");
        std::uint16_t index = 0;
        for (std::uint16_t j = 0; j < count && valid; j++) {
            if (j >= properties.size()) { valid = false; break; }
            const auto& property = properties[j];
            check(read.operator()<std::uint16_t>() == index, "property index");
            check(read.operator()<std::uint8_t>() == property.dimension, "property array dimension");
            check(nameAt(read.operator()<std::uint32_t>()) == property.name, "property name and order");
            check(read.operator()<std::uint8_t>() == property.wire, property.name);
            index += property.dimension;
        }
    }
    check(valid && offset == output->size(), "complete bounded output parse");
}

}

int main() {
    for (const auto flags : {0x00, 0x40, 0x80, 0xC0})
        run(std::uint8_t(flags));
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
