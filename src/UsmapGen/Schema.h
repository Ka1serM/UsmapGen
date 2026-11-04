#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace UsmapGen {

enum class PropertyType : std::uint8_t {
    Byte,
    Bool,
    Int,
    Float,
    Object,
    Name,
    Delegate,
    Double,
    Array,
    Struct,
    Str,
    Text,
    Interface,
    MulticastDelegate,
    WeakObject,
    LazyObject,
    AssetObject,
    SoftObject,
    UInt64,
    UInt32,
    UInt16,
    Int64,
    Int16,
    Int8,
    Map,
    Set,
    Enum,
    FieldPath,
    Optional,
    Utf8Str,
    AnsiStr,
    Class,
    MulticastInlineDelegate,
    SoftClass,
    VerseString,
    VerseDynamic,
    VerseFunction,
};

constexpr std::uint32_t noName = ~0u;

struct TypeNode {
    PropertyType type = PropertyType::Byte;
    std::uint32_t name = noName;
};

struct Property {
    std::uint32_t name = 0;
    std::uint16_t index = 0;
    std::uint8_t arrayDim = 1;
    std::uint32_t firstType = 0;
};

struct Struct {
    std::uint32_t name = 0;
    std::uint32_t super = noName;
    std::uint32_t firstProperty = 0;
    std::uint16_t propertyCount = 0;
    std::uint16_t indexCount = 0;
};

struct EnumValue {
    std::uint64_t value = 0;
    std::uint32_t name = 0;
};

struct Enum {
    std::uint32_t name = 0;
    std::uint32_t firstValue = 0;
    std::uint16_t valueCount = 0;
};

class NameTable {
public:
    std::uint32_t add(std::string_view name);
    const std::vector<std::string>& getNames() const { return names; }

private:
    struct Hash {
        using is_transparent = void;
        std::size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
    };

    std::vector<std::string> names;
    std::unordered_map<std::string, std::uint32_t, Hash, std::equal_to<>> indices;
};

struct Schema {
    NameTable names;
    std::vector<Enum> enums;
    std::vector<EnumValue> enumValues;
    std::vector<Struct> structs;
    std::vector<Property> properties;
    std::vector<TypeNode> types;
};

std::vector<std::byte> writeUsmap(const Schema& schema);

}
