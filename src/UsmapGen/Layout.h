#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace UsmapGen {

//! UECodeGen_Private::EPropertyGenFlags of UE 5 without the flag bits.
enum class Kind : std::uint8_t {
    Byte,
    Int8,
    Int16,
    Int,
    Int64,
    UInt16,
    UInt32,
    UInt64,
    UnsizedInt,
    UnsizedUInt,
    Float,
    Double,
    Bool,
    SoftClass,
    WeakObject,
    LazyObject,
    SoftObject,
    Class,
    Object,
    Interface,
    Name,
    Str,
    Array,
    Map,
    Set,
    Struct,
    Delegate,
    InlineMulticastDelegate,
    SparseMulticastDelegate,
    Text,
    Enum,
    FieldPath,
    LargeWorldCoordinatesReal,
    Optional,
    VerseValue,
    Utf8Str,
    AnsiStr,
    VerseString,
    VerseCell,
    VerseType,
    VerseLegacyValue,
    MulticastDelegate,
};

enum class TypeNumbering : std::uint8_t { Engine418, Engine422, Engine423, Engine5 };

std::optional<Kind> translateKind(TypeNumbering numbering, std::uint32_t raw);

struct Layout {
    const char* versions;
    std::uint8_t propertyName;
    std::uint8_t propertyType;
    std::uint8_t propertyArrayDim;
    bool propertyArrayDim16;
    std::uint8_t propertyReference;
    std::uint8_t structProperties;
    std::uint8_t structPropertyCount;
    bool structPropertyCount16;
    std::uint8_t classDependencies;
    std::uint8_t classDependencyCount;
    std::uint8_t classProperties;
    std::uint8_t classPropertyCount;
    bool classPropertyCountPacked;
    std::uint8_t packageSingletons;
    std::uint8_t packageSingletonCount;
    std::uint8_t enumEnumerators;
    std::uint8_t enumEnumeratorCount;
    bool enumEnumeratorCount16;
    std::uint8_t enumFlags;
};

constexpr std::uint8_t structSuper = 0x08;
constexpr std::uint8_t structOps = 0x10;
constexpr std::uint8_t structName = 0x18;
constexpr std::uint8_t enumName = 0x10;
constexpr std::uint8_t enumeratorSize = 0x10;

constexpr Layout layout418{"4.18 to 4.21", 0x08, 0x00, 0x20, false, 0x38, 0x38, 0x40, false, 0x08, 0x10, 0x28, 0x30, false, 0x18, 0x20, 0x38, 0x40, false, 0x00};
constexpr Layout layout422{"4.22 to 5.0", 0x00, 0x18, 0x20, false, 0x28, 0x30, 0x38, false, 0x18, 0x38, 0x28, 0x40, false, 0x08, 0x10, 0x20, 0x28, false, 0x00};
constexpr Layout layout51{"5.1 to 5.2", 0x00, 0x18, 0x20, false, 0x40, 0x30, 0x38, false, 0x18, 0x38, 0x28, 0x40, false, 0x08, 0x10, 0x20, 0x28, false, 0x00};
constexpr Layout layout53{"5.3 and later", 0x00, 0x18, 0x30, true, 0x38, 0x20, 0x28, true, 0x18, 0x38, 0x28, 0x38, true, 0x08, 0x10, 0x20, 0x2C, true, 0x2E};

constexpr std::array<const Layout*, 2> engine4Layouts{&layout418, &layout422};
constexpr std::array<const Layout*, 3> engine5Layouts{&layout422, &layout51, &layout53};

}
