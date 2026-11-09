#include "UsmapGen/Reflection.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "UsmapGen/Code.h"
#include "UsmapGen/Enum.h"
#include "UsmapGen/Layout.h"

namespace UsmapGen {

namespace {

constexpr std::uint64_t maxPointerGap = 32;
constexpr std::size_t initializerCandidates = 8;
constexpr std::uint64_t maxRegistrations = 4096;
constexpr std::uint32_t maxSingletons = 0xFFFF;
constexpr std::size_t sampleSize = 256;
constexpr std::uint32_t maxProperties = 0xFFFF;
constexpr std::int64_t maxEnumerators = 0xFFFF;
constexpr std::uint32_t maxArrayDim = 0xFFFF;
constexpr std::uint32_t maxUsmapArrayDim = 0xFF;
constexpr int maxGetterDepth = 2;
constexpr std::uint32_t maxDependencies = 0xFF;
constexpr int maxTypeDepth = 16;
constexpr std::size_t engine4SuperArgument = 11;
constexpr std::size_t engine425SuperArgument = 12;
constexpr std::size_t classStorageArgument = 2;
constexpr std::uint32_t rawStruct = 0x19;
constexpr std::uint32_t firstEngine423Kind = 0x1E;
constexpr std::uint32_t maxEngine418Kind = 0x1D;
constexpr std::uint32_t maxGenFlagsKind = 0x3F;
constexpr std::uint8_t flagsEnum = 0x01;
constexpr std::u16string_view scriptPrefix = u"/Script/";
constexpr std::string_view scriptPrefixUtf8 = "/Script/";

struct ClassRecordLayout {
    std::uint8_t stride;
    std::uint8_t name;
};

//! FClassRegisterCompiledInInfo loses InnerRegister in 5.8; WITH_RELOAD builds add a size and a hash.
constexpr std::array<ClassRecordLayout, 4> classRecordLayouts{{{0x28, 0x10}, {0x30, 0x10}, {0x20, 0x08}, {0x28, 0x08}}};
constexpr std::array<std::uint8_t, 2> structRecordStrides{0x28, 0x30};
constexpr std::uint8_t recordName = 0x10;
constexpr std::uint8_t enumRecordStride = 0x20;
constexpr std::uint8_t enumRecordName = 0x08;

struct Batch {
    std::uint64_t package;
    std::array<std::uint64_t, 3> arrays;
    std::array<std::uint64_t, 3> counts;
};

struct ClassRecord {
    std::uint64_t construct;
    std::uint64_t getter;
    std::uint64_t name;
};

struct SingleType {
    std::uint64_t registrar;
    std::uint64_t name;
};

struct TypeRecord {
    std::uint64_t params;
    std::uint32_t package;
};

enum class ParamsKind : std::uint8_t { Struct, Enum, Class, Package };

struct ClassInfo {
    std::uint32_t name;
    std::uint64_t params;
    std::uint64_t construct;
    std::uint64_t getter;
    std::string registeredName;
};

struct ClassBodySignature {
    std::uint64_t function;
    std::size_t superArgument;
};

struct EnumName {
    std::string full;
    std::uint64_t value;
};

using Votes = std::unordered_map<std::uint64_t, std::size_t>;

std::uint64_t findMostVoted(const Votes& votes) {
    return std::ranges::max_element(votes, {}, &Votes::value_type::second)->first;
}

bool isNumeric(Kind kind) { return kind <= Kind::UInt64; }

std::size_t childCountOf(Kind kind) {
    switch (kind) {
    case Kind::Array:
    case Kind::Set:
    case Kind::Enum:
    case Kind::Optional:
        return 1;
    case Kind::Map:
        return 2;
    default:
        return 0;
    }
}

std::optional<PropertyType> usmapTypeOf(Kind kind) {
    switch (kind) {
    case Kind::Byte: return PropertyType::Byte;
    case Kind::Int8: return PropertyType::Int8;
    case Kind::Int16: return PropertyType::Int16;
    case Kind::Int:
    case Kind::UnsizedInt: return PropertyType::Int;
    case Kind::Int64: return PropertyType::Int64;
    case Kind::UInt16: return PropertyType::UInt16;
    case Kind::UInt32:
    case Kind::UnsizedUInt: return PropertyType::UInt32;
    case Kind::UInt64: return PropertyType::UInt64;
    case Kind::Float: return PropertyType::Float;
    case Kind::Double:
    case Kind::LargeWorldCoordinatesReal: return PropertyType::Double;
    case Kind::Bool: return PropertyType::Bool;
    case Kind::SoftClass: return PropertyType::SoftClass;
    case Kind::WeakObject: return PropertyType::WeakObject;
    case Kind::LazyObject: return PropertyType::LazyObject;
    case Kind::SoftObject: return PropertyType::SoftObject;
    case Kind::Class: return PropertyType::Class;
    case Kind::Object: return PropertyType::Object;
    case Kind::Interface: return PropertyType::Interface;
    case Kind::Name: return PropertyType::Name;
    case Kind::Str: return PropertyType::Str;
    case Kind::Array: return PropertyType::Array;
    case Kind::Map: return PropertyType::Map;
    case Kind::Set: return PropertyType::Set;
    case Kind::Struct: return PropertyType::Struct;
    case Kind::Delegate: return PropertyType::Delegate;
    //! The usmap dumpers write a sparse multicast delegate as MulticastInlineDelegate.
    case Kind::InlineMulticastDelegate:
    case Kind::MulticastDelegate: return PropertyType::MulticastDelegate;
    case Kind::SparseMulticastDelegate: return PropertyType::MulticastInlineDelegate;
    case Kind::Text: return PropertyType::Text;
    case Kind::Enum: return PropertyType::Enum;
    case Kind::FieldPath: return PropertyType::FieldPath;
    case Kind::Optional: return PropertyType::Optional;
    case Kind::Utf8Str: return PropertyType::Utf8Str;
    case Kind::AnsiStr: return PropertyType::AnsiStr;
    case Kind::VerseString: return PropertyType::VerseString;
    default: return std::nullopt;
    }
}

std::string_view shortEnumName(std::string_view full) {
    const std::size_t separator = full.rfind("::");
    return separator == std::string_view::npos ? full : full.substr(separator + 2);
}

//! UEnum::GenerateEnumPrefix: the longest common prefix of the names, cut before its last underscore.
std::string enumPrefixOf(std::span<const EnumName> names, std::string_view enumName) {
    std::string prefix;
    if (!names.empty()) {
        prefix = names[0].full;
        for (const EnumName& name : names.subspan(1)) {
            const auto [end, unused] = std::mismatch(prefix.begin(), prefix.end(), name.full.begin(), name.full.end());
            prefix.erase(end, prefix.end());
        }
        const std::size_t underscore = prefix.rfind('_');
        if (underscore != std::string::npos && underscore > 0)
            prefix.resize(underscore);
        else
            prefix.clear();
    }
    if (prefix.empty())
        prefix = enumName;
    return prefix;
}

class PropertyCursor {
public:
    PropertyCursor(std::span<const std::uint64_t> pointers, bool backward)
        : pointers(pointers), remaining(pointers.size()), backward(backward) {}

    bool isEmpty() const { return remaining == 0; }

    std::uint64_t take() {
        remaining--;
        return backward ? pointers[remaining] : pointers[pointers.size() - remaining - 1];
    }

private:
    std::span<const std::uint64_t> pointers;
    std::size_t remaining;
    bool backward;
};

class Reflection {
public:
    explicit Reflection(Image& image) : image(image), code(image) {}

    std::expected<Schema, Error> read();

private:
    std::vector<std::uint64_t> findInitializers();
    std::vector<std::uint64_t> findInitializersInReadOnlyData();
    void runInitializers(std::span<const std::uint64_t> initializers);
    void applyStores(std::uint64_t function);
    bool collectBatches(std::span<const std::uint64_t> initializers);
    void collectSingles(std::span<const std::uint64_t> initializers);
    void readEngine5Records(std::span<const Batch> batches);
    void classifySingles();
    void collectPackageTypes();

    bool isParams(std::uint64_t params, ParamsKind kind);
    std::uint64_t findParams(std::uint64_t function, ParamsKind kind);
    void collectParams(std::uint64_t function, ParamsKind kind, std::vector<std::uint64_t>& found);
    std::uint32_t packageOf(std::uint64_t params);

    bool isWideName(std::uint64_t address);
    bool isName(std::uint64_t address);
    bool isIdentifier(std::uint64_t address);
    void detectLayout();
    std::uint64_t scoreLayout(const Layout& candidate, std::span<const std::uint64_t> sample);
    void detectTypeNumbering();
    void detectPropertyOrder();
    std::optional<Call> findClassBody(std::uint64_t function, std::string_view name, int depth);
    std::optional<ClassBodySignature> findClassBodySignature();
    std::optional<std::uint64_t> findIntrinsicConstruct(std::uint64_t function, std::uint64_t getter, int depth);
    std::optional<std::size_t> findIntrinsicSuper(std::size_t index, std::optional<ClassBodySignature>& signature);
    std::optional<std::size_t> findClassOfBody(std::uint64_t getter, const ClassBodySignature& signature);

    std::uint32_t readPropertyCount(std::uint64_t at, bool count16);
    std::uint32_t readClassPropertyCount(std::uint64_t params);
    std::uint32_t readDependencyCount(std::uint64_t params);
    std::optional<Kind> readKind(std::uint64_t property);
    std::uint32_t readArrayDim(std::uint64_t property);
    std::span<const std::uint64_t> readPointers(std::uint64_t array, std::uint32_t count);
    void readProperties(std::uint64_t array, std::uint32_t count, std::string_view owner, Struct& out);
    bool readType(std::uint64_t property, PropertyCursor& cursor, int depth);

    std::uint32_t resolveStruct(std::uint64_t function);
    std::uint32_t resolveEnum(std::uint64_t function);
    std::uint32_t queueStruct(std::uint64_t params);
    std::uint32_t queueEnum(std::uint64_t params);
    std::optional<std::size_t> findClass(std::uint64_t function);

    void collectClasses();
    void readClasses();
    void readStruct(std::uint64_t params);
    void readEnum(const TypeRecord& record);

    Image& image;
    Code code;
    Schema schema;

    bool engine5 = false;
    EnumMaxBehavior enumMax = EnumMaxBehavior::Legacy;
    const Layout* layout = nullptr;
    TypeNumbering numbering = TypeNumbering::Engine5;
    bool backward = true;

    std::vector<ClassRecord> classRecords;
    std::vector<SingleType> singleTypes;
    std::vector<std::uint64_t> registeredStructs;
    std::unordered_set<std::uint64_t> storedFunctions;
    std::vector<std::uint64_t> registeredEnums;
    std::vector<ClassInfo> classes;
    std::unordered_map<std::uint64_t, std::size_t> classOfFunction;
    std::vector<std::string> packages;
    std::unordered_map<std::uint64_t, std::uint32_t> packageOfParams;
    std::vector<std::uint64_t> structQueue;
    std::vector<TypeRecord> enumQueue;
    std::unordered_map<std::uint64_t, std::uint64_t> paramsOfFunction;
    std::unordered_map<std::uint64_t, std::uint32_t> structNames;
    std::unordered_map<std::uint64_t, std::uint32_t> enumNames;
    std::unordered_set<std::string> usedEnumNames;

    std::vector<std::uint64_t> pointers;
    std::vector<EnumName> enumerators;
    std::string currentOwner;
};

//! The CRT startup code, which the entry point calls or jumps to, runs each table of initializers with _initterm(First, Last) or _initterm_e(First, Last).
std::vector<std::uint64_t> Reflection::findInitializers() {
    if (!image.isCode(image.getEntryPoint()))
        return findInitializersInReadOnlyData();
    std::vector<std::uint64_t> initializers;
    std::vector<std::uint64_t> table;
    for (const Call& startup : code.findCalls(image.getEntryPoint())) {
        for (const Call& call : code.findCalls(startup.target)) {
            const std::uint64_t first = call.getConstant(0).value_or(0);
            const std::uint64_t last = call.getConstant(1).value_or(0);
            if (!image.isReadOnlyData(first) || !image.isReadOnlyData(last) || last <= first || (last - first) % 8 != 0)
                continue;
            table.resize((last - first) / 8);
            image.read(first, std::as_writable_bytes(std::span(table)));
            if (std::ranges::all_of(table, [&](std::uint64_t entry) { return entry == 0 || image.isCode(entry); }))
                std::ranges::copy_if(table, std::back_inserter(initializers), [](std::uint64_t entry) { return entry != 0; });
        }
    }
    if (initializers.empty())
        image.fail("The startup code runs no table of C++ dynamic initializers.");
    return initializers;
}

//! A protector's entry point hides the CRT startup code, but .CRT$XCU stays the longest run of relocated code pointers in .rdata, with zeros between CRT sections.
std::vector<std::uint64_t> Reflection::findInitializersInReadOnlyData() {
    const std::vector<std::uint64_t> slots = image.readPointerSlots(image.getReadOnlyData());
    struct Run {
        std::size_t first;
        std::size_t count;
    };
    std::vector<Run> runs;
    std::size_t first = 0;
    for (std::size_t i = 1; i <= slots.size(); i++) {
        if (i < slots.size() && slots[i] - slots[i - 1] <= maxPointerGap && (slots[i] - slots[i - 1]) % 8 == 0)
            continue;
        runs.push_back({first, i - first});
        first = i;
    }
    const std::size_t candidates = std::min(runs.size(), initializerCandidates);
    std::partial_sort(runs.begin(), runs.begin() + candidates, runs.end(), [](const Run& a, const Run& b) { return a.count > b.count; });
    std::vector<std::uint64_t> table;
    for (const Run& run : std::span(runs).first(candidates)) {
        const std::uint64_t begin = slots[run.first];
        const std::uint64_t end = slots[run.first + run.count - 1] + 8;
        table.resize((end - begin) / 8);
        image.read(begin, std::as_writable_bytes(std::span(table)));
        if (!std::ranges::all_of(table, [&](std::uint64_t entry) { return entry == 0 || image.isCode(entry); }))
            continue;
        std::erase(table, 0);
        return table;
    }
    image.fail("The executable has no table of C++ dynamic initializers.");
    return {};
}

bool isIdentifierChar(const char16_t c) {
    return c == u'_' || (c >= u'0' && c <= u'9') || (c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z');
}

//! A registered type name lacks its prefix, so it can start with a digit, as 2DSparseSpatialHashGrid32 does.
bool Reflection::isWideName(const std::uint64_t address) {
    return image.isReadOnlyData(address) && isIdentifierChar(image.read<char16_t>(address));
}

bool Reflection::isName(const std::uint64_t address) {
    return image.isReadOnlyData(address) && isIdentifierChar(char16_t(image.read<char>(address)));
}

//! The name of a native type is a C++ identifier, with :: in the CppType of a namespaced enum.
bool Reflection::isIdentifier(const std::uint64_t address) {
    if (!isName(address))
        return false;
    const std::string text = image.readUtf8(address);
    return std::ranges::all_of(text, [](char c) { return c == ':' || isIdentifierChar(char16_t(c)); });
}

//! UE 5 calls RegisterCompiledInInfo(Package, Classes, N, Structs, N, Enums, N) once per generated file.
bool Reflection::collectBatches(std::span<const std::uint64_t> initializers) {
    struct Candidate {
        std::uint64_t target;
        std::size_t shift;
        Batch batch;
    };
    std::vector<Candidate> candidates;
    for (const std::uint64_t initializer : initializers) {
        for (const Call& call : code.findCalls(initializer)) {
            for (std::size_t shift = 0; shift < 2; shift++) {
                std::array<std::uint64_t, 7> values;
                bool constant = true;
                for (std::size_t i = 0; i < values.size() && constant; i++) {
                    const std::optional<std::uint64_t> value = call.getConstant(shift + i);
                    constant = value.has_value();
                    values[i] = value.value_or(0);
                }
                if (!constant || values[2] > maxRegistrations || values[4] > maxRegistrations || values[6] > maxRegistrations)
                    continue;
                if (!image.isReadOnlyData(values[0]))
                    continue;
                candidates.push_back({call.target, shift, {values[0], {values[1], values[3], values[5]}, {values[2], values[4], values[6]}}});
            }
        }
    }
    Votes votes;
    for (const Candidate& candidate : candidates)
        if (image.startsWithUtf16(candidate.batch.package, scriptPrefix))
            votes[candidate.target * 2 + candidate.shift]++;
    if (votes.empty())
        return false;
    const std::uint64_t winner = findMostVoted(votes);
    std::vector<Batch> batches;
    for (const Candidate& candidate : candidates)
        if (candidate.target * 2 + candidate.shift == winner)
            batches.push_back(candidate.batch);
    readEngine5Records(batches);
    return true;
}

void Reflection::readEngine5Records(std::span<const Batch> batches) {
    const auto findClassLayout = [&]() -> const ClassRecordLayout* {
        for (const ClassRecordLayout& candidate : classRecordLayouts) {
            const bool valid = std::ranges::all_of(batches, [&](const Batch& batch) {
                for (std::uint64_t i = 0; i < batch.counts[0]; i++) {
                    const std::uint64_t record = batch.arrays[0] + i * candidate.stride;
                    if (!image.isCode(image.read<std::uint64_t>(record)) || !isWideName(image.read<std::uint64_t>(record + candidate.name)))
                        return false;
                    if (candidate.name == recordName && !image.isCode(image.read<std::uint64_t>(record + 8)))
                        return false;
                }
                return true;
            });
            if (valid)
                return &candidate;
        }
        return nullptr;
    };
    const auto findStructStride = [&]() -> std::uint8_t {
        for (const std::uint8_t stride : structRecordStrides) {
            const bool valid = std::ranges::all_of(batches, [&](const Batch& batch) {
                for (std::uint64_t i = 0; i < batch.counts[1]; i++)
                    if (!isWideName(image.read<std::uint64_t>(batch.arrays[1] + i * stride + recordName)))
                        return false;
                return true;
            });
            if (valid)
                return stride;
        }
        return 0;
    };
    const ClassRecordLayout* classLayout = findClassLayout();
    const std::uint8_t structStride = findStructStride();
    if (classLayout == nullptr || structStride == 0) {
        image.fail("The registration records of the classes or structs have an unknown layout.");
        return;
    }
    for (const Batch& batch : batches) {
        for (std::uint64_t i = 0; i < batch.counts[0]; i++) {
            const std::uint64_t record = batch.arrays[0] + i * classLayout->stride;
            const std::uint64_t construct = image.read<std::uint64_t>(record);
            const std::uint64_t getter = classLayout->name == recordName ? image.read<std::uint64_t>(record + 8) : construct;
            classRecords.push_back({construct, getter, image.read<std::uint64_t>(record + classLayout->name)});
        }
        for (std::uint64_t i = 0; i < batch.counts[1]; i++)
            registeredStructs.push_back(image.read<std::uint64_t>(batch.arrays[1] + i * structStride));
        for (std::uint64_t i = 0; i < batch.counts[2]; i++) {
            const std::uint64_t record = batch.arrays[2] + i * enumRecordStride;
            if (!isWideName(image.read<std::uint64_t>(record + enumRecordName))) {
                image.fail(std::format("The enum registration record at {:#x} has no name.", record));
                return;
            }
            registeredEnums.push_back(image.read<std::uint64_t>(record));
        }
    }
}

//! MSVC fills a parameter table that it cannot fold into constant data, such as one with the offset of a member, in a C++ dynamic initializer.
void Reflection::runInitializers(const std::span<const std::uint64_t> initializers) {
    for (const std::uint64_t initializer : initializers)
        applyStores(initializer);
}

void Reflection::applyStores(const std::uint64_t function) {
    for (const Store& store : code.findStores(function))
        image.write(store.address, std::as_bytes(std::span(&store.value, 1)).first(store.size));
}

//! Single registrations: UE 4 class (Register, StaticClass, Name, Package), UE 5 intrinsic class (Register, StaticClass, Package, Name), UE 5.8 intrinsic class (StaticClass, Package, Name), struct or enum (Register, Package, Name).
void Reflection::collectSingles(std::span<const std::uint64_t> initializers) {
    for (const std::uint64_t initializer : initializers) {
        for (const Call& call : code.findCalls(initializer)) {
            const auto argument = [&](std::size_t index) { return call.getConstant(index).value_or(0); };
            for (std::size_t package = 1; package < 5; package++) {
                if (!image.startsWithUtf16(argument(package), scriptPrefix))
                    continue;
                if (package >= 3 && isWideName(argument(package - 1)) && image.isCode(argument(package - 2)) &&
                    image.isCode(argument(package - 3))) {
                    classRecords.push_back({argument(package - 3), argument(package - 2), argument(package - 1)});
                    break;
                }
                if (!image.isCode(argument(package - 1)))
                    continue;
                //! UE 4.20 passes the name of a struct or enum as an FName value.
                if (package == 1 && !call.getConstant(package + 1)) {
                    singleTypes.push_back({argument(package - 1), 0});
                    break;
                }
                if (!isWideName(argument(package + 1)))
                    continue;
                if (package >= 2 && image.isCode(argument(package - 2))) {
                    classRecords.push_back({argument(package - 2), argument(package - 1), argument(package + 1)});
                    break;
                }
                singleTypes.push_back({argument(package - 1), argument(package + 1)});
                break;
            }
        }
    }
}

//! A struct-shaped single registration is a struct, an enum, or a UE 5.8 intrinsic class; its parameter table tells which, for structs before the layout, for enums and classes after it.
void Reflection::classifySingles() {
    for (const auto [registrar, name] : singleTypes) {
        if (findParams(registrar, ParamsKind::Struct) != 0)
            continue;
        if (findParams(registrar, ParamsKind::Enum) != 0) {
            registeredEnums.push_back(registrar);
            continue;
        }
        if (name == 0) {
            image.fail(std::format("The registration of {:#x} names no type and references no struct or enum parameters.", registrar));
            return;
        }
        classRecords.push_back({registrar, registrar, name});
    }
}

//! FStructParams {OuterFunc, SuperFunc, StructOpsFunc, NameUTF8, ...}, FEnumParams {OuterFunc, DisplayNameFunc, NameUTF8, ..., CppTypeUTF8, Enumerators, ...}, FClassParams {ClassNoRegisterFunc, ...}, FPackageParams {NameUTF8, SingletonFuncArray, NumSingletons, ...}.
bool Reflection::isParams(const std::uint64_t params, const ParamsKind kind) {
    if (!image.isData(params))
        return false;
    const auto field = [&](std::uint64_t offset) { return image.read<std::uint64_t>(params + offset); };
    const auto codeOrNull = [&](std::uint64_t offset) { return field(offset) == 0 || image.isCode(field(offset)); };
    switch (kind) {
    case ParamsKind::Package: {
        const std::uint64_t name = field(0);
        return image.isReadOnlyData(name) && image.readUtf8(name).starts_with(scriptPrefixUtf8);
    }
    case ParamsKind::Struct:
        return image.isCode(field(0)) && codeOrNull(8) && codeOrNull(structOps) && isIdentifier(field(structName));
    case ParamsKind::Enum: {
        if (layout == nullptr || !image.isCode(field(0)) || !codeOrNull(8) || !isIdentifier(field(enumName)) || !isIdentifier(field(layout->enumEnumerators - 8)))
            return false;
        const std::uint64_t values = field(layout->enumEnumerators);
        const std::int64_t count = layout->enumEnumeratorCount16 ? image.read<std::int16_t>(params + layout->enumEnumeratorCount)
                                                                 : image.read<std::int32_t>(params + layout->enumEnumeratorCount);
        //! An enum without enumerators has no enumerator array; a delegate's FFunctionParams has other data there.
        if (count < 0 || count >= maxEnumerators || (count == 0 ? values != 0 : !image.isData(values)))
            return false;
        for (std::int64_t i = 0; i < count; i++)
            if (!isIdentifier(image.read<std::uint64_t>(values + std::uint64_t(i) * enumeratorSize)))
                return false;
        //! The outer of an enum is its package; the outer of a delegate can be a class.
        return findParams(field(0), ParamsKind::Package) != 0;
    }
    case ParamsKind::Class: {
        if (layout == nullptr || !image.isCode(field(0)))
            return false;
        const std::uint64_t dependencies = field(layout->classDependencies);
        const std::uint32_t dependencyCount = readDependencyCount(params);
        if (dependencyCount == 0 || dependencyCount > maxDependencies || !image.isData(dependencies))
            return false;
        for (std::uint32_t i = 0; i < dependencyCount; i++)
            if (!image.isCode(image.read<std::uint64_t>(dependencies + 8 * i)))
                return false;
        const std::uint32_t propertyCount = readClassPropertyCount(params);
        return propertyCount <= maxProperties && (propertyCount == 0 || image.isData(field(layout->classProperties)));
    }
    }
    return false;
}

//! Z_Construct passes its parameter table to UECodeGen_Private::Construct*(Out, Params), and can build it as a function-local static first.
void Reflection::collectParams(const std::uint64_t function, const ParamsKind kind, std::vector<std::uint64_t>& found) {
    if (storedFunctions.insert(function).second)
        applyStores(function);
    for (const Call& call : code.findCalls(function)) {
        const std::uint64_t params = call.getConstant(1).value_or(0);
        if (!std::ranges::contains(found, params) && isParams(params, kind))
            found.push_back(params);
    }
}

//! A type's Z_Construct references one parameter table of its kind; StaticStruct and StaticEnum reach it through the Z_Construct that they pass to GetStaticStruct or GetStaticEnum.
std::uint64_t Reflection::findParams(std::uint64_t function, const ParamsKind kind) {
    function = code.followThunks(function);
    const std::uint64_t key = function << 2 | std::uint64_t(kind);
    if (const auto known = paramsOfFunction.find(key); known != paramsOfFunction.end())
        return known->second;
    std::vector<std::uint64_t> found;
    collectParams(function, kind, found);
    if (found.empty() && (kind == ParamsKind::Struct || kind == ParamsKind::Enum)) {
        const std::vector<std::uint64_t> callees = code.findReferences(function).code;
        for (const std::uint64_t callee : callees)
            collectParams(code.followThunks(callee), kind, found);
    }
    if (found.size() > 1) {
        image.fail(std::format("The function at {:#x} references {} parameter tables of one kind, at {:#x} and {:#x}.", function, found.size(), found[0], found[1]));
        found.clear();
    }
    const std::uint64_t params = found.empty() ? 0 : found[0];
    paramsOfFunction.emplace(key, params);
    return params;
}

//! The OuterFunc of a struct or an enum constructs its package.
std::uint32_t Reflection::packageOf(const std::uint64_t params) {
    const std::uint64_t package = findParams(image.read<std::uint64_t>(params), ParamsKind::Package);
    if (package == 0) {
        image.fail(std::format("The outer of the parameters at {:#x} is no package.", params));
        return 0;
    }
    if (const auto found = packageOfParams.find(package); found != packageOfParams.end())
        return found->second;
    packages.push_back(image.readUtf8(image.read<std::uint64_t>(package)));
    const std::uint32_t index = std::uint32_t(packages.size() - 1);
    packageOfParams.emplace(package, index);
    return index;
}

//! FPackageParams lists the singletons after the name, or from 4.18 to 4.21 after the flags and two CRCs of a package; they also build the no-export structs, which nothing registers.
void Reflection::collectPackageTypes() {
    std::vector<std::uint64_t> packageParams;
    for (const auto [params, index] : packageOfParams)
        packageParams.push_back(params);
    std::ranges::sort(packageParams);
    for (const std::uint64_t params : packageParams) {
        const std::uint64_t singletons = image.read<std::uint64_t>(params + layout->packageSingletons);
        const std::uint32_t count = image.read<std::uint32_t>(params + layout->packageSingletonCount);
        if (count > maxSingletons || (count > 0 && !image.isData(singletons))) {
            image.fail(std::format("The package parameters at {:#x} have {} singletons.", params, count));
            return;
        }
        const std::vector<std::uint64_t> functions(std::from_range, readPointers(singletons, count));
        for (const std::uint64_t function : functions) {
            if (const std::uint64_t structParams = findParams(function, ParamsKind::Struct); structParams != 0)
                queueStruct(structParams);
            else if (const std::uint64_t enumParams = findParams(function, ParamsKind::Enum); enumParams != 0)
                queueEnum(enumParams);
            if (image.getError())
                return;
        }
    }
}

std::uint32_t Reflection::readPropertyCount(const std::uint64_t at, const bool count16) {
    return count16 ? image.read<std::uint16_t>(at) : image.read<std::uint32_t>(at);
}

//! UE 5.3 packs NumDependencySingletons:4, NumFunctions:11, NumProperties:11 and NumImplementedInterfaces:6 into one word.
std::uint32_t Reflection::readClassPropertyCount(const std::uint64_t params) {
    const std::uint32_t value = image.read<std::uint32_t>(params + layout->classPropertyCount);
    return layout->classPropertyCountPacked ? value >> 15 & 0x7FF : value;
}

std::uint32_t Reflection::readDependencyCount(const std::uint64_t params) {
    const std::uint32_t value = image.read<std::uint32_t>(params + layout->classDependencyCount);
    return layout->classPropertyCountPacked ? value & 0xF : value;
}

std::uint64_t Reflection::scoreLayout(const Layout& candidate, std::span<const std::uint64_t> sample) {
    const std::uint32_t maxRaw = &candidate == &layout418 ? maxEngine418Kind : maxGenFlagsKind;
    std::uint64_t score = 0;
    for (const std::uint64_t params : sample) {
        const std::uint64_t array = image.read<std::uint64_t>(params + candidate.structProperties);
        const std::uint32_t count = readPropertyCount(params + candidate.structPropertyCount, candidate.structPropertyCount16);
        if (count > maxProperties || (count > 0 && !image.isData(array)))
            continue;
        std::uint64_t valid = 0;
        for (std::uint32_t i = 0; i < count; i++) {
            const std::uint64_t property = image.read<std::uint64_t>(array + 8 * i);
            if (!image.isData(property) || !isName(image.read<std::uint64_t>(property + candidate.propertyName)))
                break;
            const std::uint32_t raw = &candidate == &layout418 ? image.read<std::uint32_t>(property) : image.read<std::uint8_t>(property + candidate.propertyType) & maxGenFlagsKind;
            const std::uint32_t arrayDim = candidate.propertyArrayDim16 ? image.read<std::uint16_t>(property + candidate.propertyArrayDim) : image.read<std::uint32_t>(property + candidate.propertyArrayDim);
            if (raw > maxRaw || arrayDim == 0 || arrayDim > maxArrayDim)
                break;
            if (raw == rawStruct && !image.isCode(image.read<std::uint64_t>(property + candidate.propertyReference)))
                break;
            valid++;
        }
        if (valid == count)
            score += 1 + count;
    }
    return score;
}

void Reflection::detectLayout() {
    std::vector<std::uint64_t> sample;
    for (const std::uint64_t registrar : registeredStructs) {
        const std::uint64_t params = findParams(registrar, ParamsKind::Struct);
        if (params != 0)
            sample.push_back(params);
        if (sample.size() == sampleSize)
            break;
    }
    const std::span<const Layout* const> candidates = engine5 ? std::span<const Layout* const>(engine5Layouts) : std::span<const Layout* const>(engine4Layouts);
    std::uint64_t best = 0;
    for (const Layout* candidate : candidates) {
        const std::uint64_t score = scoreLayout(*candidate, sample);
        if (score <= best)
            continue;
        best = score;
        layout = candidate;
    }
    if (layout == nullptr)
        image.fail("The property parameter tables match no known engine layout.");
}

//! 4.22 numbers the types without the two multicast delegate kinds and FieldPath that 4.23 and 4.25 add.
void Reflection::detectTypeNumbering() {
    if (engine5) {
        numbering = TypeNumbering::Engine5;
        return;
    }
    if (layout == &layout418) {
        numbering = TypeNumbering::Engine418;
        return;
    }
    numbering = TypeNumbering::Engine422;
    const auto scan = [&](std::uint64_t array, std::uint32_t count) {
        for (std::uint32_t i = 0; i < count && count <= maxProperties; i++)
            if ((image.read<std::uint8_t>(image.read<std::uint64_t>(array + 8 * i) + layout->propertyType) & 0x1F) >= firstEngine423Kind)
                numbering = TypeNumbering::Engine423;
    };
    for (const std::uint64_t params : structQueue) {
        scan(image.read<std::uint64_t>(params + layout->structProperties), readPropertyCount(params + layout->structPropertyCount, layout->structPropertyCount16));
        if (numbering == TypeNumbering::Engine423)
            return;
    }
    for (const ClassInfo& info : classes) {
        if (info.params == 0)
            continue;
        scan(image.read<std::uint64_t>(info.params + layout->classProperties), readClassPropertyCount(info.params));
        if (numbering == TypeNumbering::Engine423)
            return;
    }
}

//! ConstructFProperty reads forward until 4.25 and backward from 4.26; an array's inner property follows it.
void Reflection::detectPropertyOrder() {
    if (engine5 || layout == &layout418) {
        backward = engine5;
        return;
    }
    std::int64_t votes = 0;
    for (const std::uint64_t params : std::span(structQueue).first(std::min(structQueue.size(), sampleSize))) {
        const std::uint32_t count = readPropertyCount(params + layout->structPropertyCount, layout->structPropertyCount16);
        const std::span<const std::uint64_t> entries = readPointers(image.read<std::uint64_t>(params + layout->structProperties), count);
        for (std::size_t i = 0; i < entries.size(); i++) {
            if (readKind(entries[i]) != Kind::Array)
                continue;
            const std::string name = image.readUtf8(image.read<std::uint64_t>(entries[i] + layout->propertyName));
            const auto nameAt = [&](std::size_t at) { return image.readUtf8(image.read<std::uint64_t>(entries[at] + layout->propertyName)); };
            if (i > 0 && nameAt(i - 1) == name)
                votes++;
            if (i + 1 < entries.size() && nameAt(i + 1) == name)
                votes--;
        }
    }
    backward = votes > 0;
}

//! StaticClass calls GetPrivateStaticClassBody(Package, Name, ...) with the unprefixed class name through at most two functions; the UE 4 FName constructor takes the name without the package.
std::optional<Call> Reflection::findClassBody(const std::uint64_t function, const std::string_view name, const int depth) {
    std::vector<std::uint64_t> callees;
    for (const Call& call : code.findCalls(function)) {
        const std::optional<std::uint64_t> package = call.getConstant(0);
        const std::optional<std::uint64_t> argument = call.getConstant(1);
        if (package && argument && image.startsWithUtf16(*package, scriptPrefix) && image.isReadOnlyData(*argument) &&
            image.readUtf16(*argument) == name)
            return call;
        if (image.isCode(call.target))
            callees.push_back(call.target);
    }
    if (depth == maxGetterDepth)
        return std::nullopt;
    for (const std::uint64_t callee : callees)
        if (const std::optional<Call> body = findClassBody(code.followThunks(callee), name, depth + 1))
            return body;
    return std::nullopt;
}

std::string_view bodyNameOf(std::string_view registeredName) {
    registeredName.remove_prefix(1);
    if (registeredName.starts_with("DEPRECATED_"))
        registeredName.remove_prefix(std::string_view("DEPRECATED_").size());
    return registeredName;
}

std::optional<Kind> Reflection::readKind(const std::uint64_t property) {
    if (numbering == TypeNumbering::Engine418)
        return translateKind(numbering, image.read<std::uint32_t>(property + layout->propertyType));
    const std::uint32_t mask = numbering == TypeNumbering::Engine5 ? 0x3F : 0x1F;
    return translateKind(numbering, image.read<std::uint8_t>(property + layout->propertyType) & mask);
}

std::uint32_t Reflection::readArrayDim(const std::uint64_t property) {
    const std::uint32_t arrayDim = layout->propertyArrayDim16 ? image.read<std::uint16_t>(property + layout->propertyArrayDim)
                                                              : image.read<std::uint32_t>(property + layout->propertyArrayDim);
    if (arrayDim == 0 || arrayDim > maxArrayDim) {
        image.fail(std::format("The property at {:#x} has the array size {}.", property, arrayDim));
        return 1;
    }
    return arrayDim;
}

std::span<const std::uint64_t> Reflection::readPointers(const std::uint64_t array, const std::uint32_t count) {
    pointers.resize(count);
    image.read(array, std::as_writable_bytes(std::span(pointers)));
    return pointers;
}

bool Reflection::readType(const std::uint64_t property, PropertyCursor& cursor, const int depth) {
    const std::optional<Kind> kind = readKind(property);
    const std::optional<PropertyType> type = kind ? usmapTypeOf(*kind) : std::nullopt;
    if (!type || depth > maxTypeDepth) {
        image.fail(std::format("The property at {:#x} in {} has a type that the usmap format cannot express.", property, currentOwner));
        return false;
    }
    const std::uint64_t reference = image.read<std::uint64_t>(property + layout->propertyReference);
    if (*kind == Kind::Byte && reference != 0) {
        //! A byte property with an enum is a TEnumAsByte.
        const std::uint32_t name = resolveEnum(reference);
        schema.types.push_back({PropertyType::Enum, name});
        schema.types.push_back({PropertyType::Byte});
        return true;
    }
    TypeNode node{*type};
    if (*kind == Kind::Struct)
        node.name = resolveStruct(reference);
    if (*kind == Kind::Enum)
        node.name = resolveEnum(reference);
    schema.types.push_back(node);
    const std::string parent = image.readUtf8(image.read<std::uint64_t>(property + layout->propertyName));
    for (std::size_t i = 0; i < childCountOf(*kind); i++) {
        if (cursor.isEmpty()) {
            image.fail(std::format("The property {} in {} has no inner property.", parent, currentOwner));
            return false;
        }
        const std::uint64_t child = cursor.take();
        const bool valid = *kind == Kind::Enum ? readKind(child).transform(isNumeric).value_or(false)
                                               : image.readUtf8(image.read<std::uint64_t>(child + layout->propertyName)).starts_with(parent);
        if (!valid) {
            image.fail(std::format("The property {} in {} has an inner property that does not belong to it.", parent, currentOwner));
            return false;
        }
        if (!readType(child, cursor, depth + 1))
            return false;
    }
    return true;
}

//! ConstructFProperty puts each new property first in the field list.
void Reflection::readProperties(const std::uint64_t array, const std::uint32_t count, const std::string_view owner, Struct& out) {
    currentOwner = owner;
    out.firstProperty = std::uint32_t(schema.properties.size());
    if (count > maxProperties) {
        image.fail(std::format("{} has {} properties.", owner, count));
        return;
    }
    PropertyCursor cursor(readPointers(array, count), backward);
    while (!cursor.isEmpty() && !image.getError()) {
        const std::uint64_t entry = cursor.take();
        const std::uint32_t name = schema.names.add(image.readUtf8(image.read<std::uint64_t>(entry + layout->propertyName)));
        const std::uint32_t arrayDim = readArrayDim(entry);
        const std::uint32_t firstType = std::uint32_t(schema.types.size());
        if (!readType(entry, cursor, 0))
            return;
        //! A usmap property holds at most 255 elements of a static array; consecutive entries of the same property hold the rest.
        for (std::uint32_t rest = arrayDim; rest > 0;) {
            const std::uint32_t part = std::min(rest, maxUsmapArrayDim);
            schema.properties.push_back({.name = name, .arrayDim = std::uint8_t(part), .firstType = firstType});
            rest -= part;
        }
    }
    const auto own = std::span(schema.properties).subspan(out.firstProperty);
    std::reverse(own.begin(), own.end());
    std::uint32_t index = 0;
    for (Property& property : own) {
        property.index = std::uint16_t(index);
        index += property.arrayDim;
    }
    if (index > maxProperties) {
        image.fail(std::format("{} has {} property slots.", owner, index));
        return;
    }
    out.propertyCount = std::uint16_t(own.size());
    out.indexCount = std::uint16_t(index);
}

std::uint32_t Reflection::queueStruct(const std::uint64_t params) {
    if (const auto found = structNames.find(params); found != structNames.end())
        return found->second;
    const std::uint32_t index = schema.names.add(image.readUtf8(image.read<std::uint64_t>(params + structName)));
    structNames.emplace(params, index);
    structQueue.push_back(params);
    return index;
}

std::uint32_t Reflection::queueEnum(const std::uint64_t params) {
    if (const auto found = enumNames.find(params); found != enumNames.end())
        return found->second;
    const std::uint32_t index = schema.names.add(image.readUtf8(image.read<std::uint64_t>(params + enumName)));
    enumNames.emplace(params, index);
    enumQueue.push_back({params, packageOf(params)});
    return index;
}

std::uint32_t Reflection::resolveStruct(const std::uint64_t function) {
    const std::uint64_t params = findParams(function, ParamsKind::Struct);
    if (params == 0) {
        image.fail(std::format("The struct function at {:#x} in {} references no struct parameters.", function, currentOwner));
        return 0;
    }
    return queueStruct(params);
}

std::uint32_t Reflection::resolveEnum(const std::uint64_t function) {
    const std::uint64_t params = findParams(function, ParamsKind::Enum);
    if (params == 0) {
        image.fail(std::format("The enum function at {:#x} in {} references no enum parameters.", function, currentOwner));
        return 0;
    }
    return queueEnum(params);
}

std::optional<std::size_t> Reflection::findClass(const std::uint64_t function) {
    const auto found = classOfFunction.find(code.followThunks(function));
    if (found == classOfFunction.end())
        return std::nullopt;
    return found->second;
}

//! A class is known by its Z_Construct function and by its StaticClass function; the same class can register twice.
void Reflection::collectClasses() {
    for (const ClassRecord& record : classRecords) {
        const std::uint64_t construct = code.followThunks(record.construct);
        if (classOfFunction.contains(construct))
            continue;
        std::string registeredName = image.readUtf16(record.name);
        const std::uint32_t name = schema.names.add(std::string_view(registeredName).substr(1));
        const std::uint64_t params = findParams(construct, ParamsKind::Class);
        classOfFunction.emplace(construct, classes.size());
        classOfFunction.emplace(code.followThunks(record.getter), classes.size());
        classes.push_back({name, params, construct, record.getter, std::move(registeredName)});
    }
}

//! GetPrivateStaticClassBody takes the super class function as argument 11 until 4.24 and 12 from 4.25; the first dependency of a class with parameters is its super.
std::optional<ClassBodySignature> Reflection::findClassBodySignature() {
    std::vector<std::pair<Call, std::size_t>> bodies;
    Votes votes;
    for (std::size_t index = 0; index < classes.size() && bodies.size() < sampleSize; index++) {
        const ClassInfo& info = classes[index];
        if (info.params == 0)
            continue;
        const std::optional<std::size_t> super = findClass(image.read<std::uint64_t>(image.read<std::uint64_t>(info.params + layout->classDependencies)));
        if (!super)
            continue;
        const std::optional<Call> body = findClassBody(code.followThunks(info.getter), bodyNameOf(info.registeredName), 0);
        if (!body)
            continue;
        bodies.emplace_back(*body, *super);
        votes[body->target]++;
    }
    if (votes.empty())
        return std::nullopt;
    const std::uint64_t function = findMostVoted(votes);
    std::optional<std::size_t> argument;
    for (const auto& [body, super] : bodies) {
        if (body.target != function)
            continue;
        const bool early = findClass(body.getConstant(engine4SuperArgument).value_or(0)) == super;
        const bool late = findClass(body.getConstant(engine425SuperArgument).value_or(0)) == super;
        //! A class whose super is also its within class, UObject, shows nothing.
        if (early == late)
            continue;
        const std::size_t candidate = early ? engine4SuperArgument : engine425SuperArgument;
        if (argument && *argument != candidate) {
            image.fail("The classes disagree on the position of the super class in GetPrivateStaticClassBody.");
            return std::nullopt;
        }
        argument = candidate;
    }
    if (!argument)
        return std::nullopt;
    return ClassBodySignature{function, *argument};
}

//! UE 4 inlines GetPrivateStaticClassBody in CoreUObject, where NotifyRegistrationEvent takes its package and name; IMPLEMENT_INTRINSIC_CLASS constructs the super before it calls StaticClass.
std::optional<std::size_t> Reflection::findIntrinsicSuper(const std::size_t index, std::optional<ClassBodySignature>& signature) {
    const ClassInfo& info = classes[index];
    const std::optional<Call> body = findClassBody(code.followThunks(info.getter), bodyNameOf(info.registeredName), 0);
    if (body && !signature)
        signature = findClassBodySignature();
    if (body && !signature) {
        image.fail("No class shows GetPrivateStaticClassBody and the position of its super class.");
        return std::nullopt;
    }
    if (body && body->target == signature->function) {
        const std::uint64_t superGetter = body->getConstant(signature->superArgument).value_or(0);
        std::optional<std::size_t> super = findClass(superGetter);
        if (!super)
            super = findClassOfBody(superGetter, *signature);
        if (!super)
            image.fail(std::format("The super class function {:#x} of {} belongs to no registered class.", superGetter, info.registeredName));
        return super;
    }
    const std::optional<std::uint64_t> construct = findIntrinsicConstruct(code.followThunks(info.construct), code.followThunks(info.getter), 0);
    if (!construct) {
        image.fail(std::format("The Z_Construct function of the inlined intrinsic class {} does not call its StaticClass.", info.registeredName));
        return std::nullopt;
    }
    std::vector<std::size_t> supers;
    for (const std::uint64_t callee : code.findReferences(*construct).code) {
        const std::optional<std::size_t> candidate = findClass(callee);
        if (candidate && *candidate != index && !std::ranges::contains(supers, *candidate))
            supers.push_back(*candidate);
    }
    if (supers.size() != 1) {
        image.fail(std::format("The inlined intrinsic class {} links to {} super classes.", info.registeredName, supers.size()));
        return std::nullopt;
    }
    return supers[0];
}

//! A class can have a second StaticClass function, and both pass the same UClass* storage to GetPrivateStaticClassBody.
std::optional<std::size_t> Reflection::findClassOfBody(const std::uint64_t getter, const ClassBodySignature& signature) {
    const std::span<const Call> calls = code.findCalls(code.followThunks(getter));
    const auto body = std::ranges::find(calls, signature.function, &Call::target);
    if (body == calls.end())
        return std::nullopt;
    const std::optional<std::uint64_t> storage = body->getConstant(classStorageArgument);
    const std::optional<std::uint64_t> name = body->getConstant(1);
    if (!storage || !name || !image.isReadOnlyData(*name))
        return std::nullopt;
    const std::string bodyName = image.readUtf16(*name);
    for (std::size_t index = 0; index < classes.size(); index++) {
        const ClassInfo& info = classes[index];
        if (bodyNameOf(info.registeredName) != bodyName)
            continue;
        const std::optional<Call> candidate = findClassBody(code.followThunks(info.getter), bodyName, 0);
        if (candidate && candidate->getConstant(classStorageArgument) == storage)
            return index;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> Reflection::findIntrinsicConstruct(const std::uint64_t function, const std::uint64_t getter, const int depth) {
    const std::vector<std::uint64_t> callees(std::from_range, code.findReferences(function).code | std::views::transform([&](std::uint64_t callee) { return code.followThunks(callee); }));
    if (std::ranges::contains(callees, getter))
        return function;
    if (depth == maxGetterDepth)
        return std::nullopt;
    for (const std::uint64_t callee : callees)
        if (const std::optional<std::uint64_t> construct = findIntrinsicConstruct(callee, getter, depth + 1))
            return construct;
    return std::nullopt;
}

//! FClassParams lists the super class first among the dependencies; an intrinsic class, such as UClass, has no parameters and names its super in StaticClass.
void Reflection::readClasses() {
    std::optional<ClassBodySignature> signature;
    for (std::size_t index = 0; index < classes.size() && !image.getError(); index++) {
        const ClassInfo& info = classes[index];
        Struct entry{.name = info.name};
        std::optional<std::size_t> super;
        if (info.params != 0) {
            const std::uint64_t first = image.read<std::uint64_t>(image.read<std::uint64_t>(info.params + layout->classDependencies));
            super = findClass(first);
            if (!super && findParams(code.followThunks(first), ParamsKind::Package) == 0) {
                image.fail(std::format("The first dependency of {} at {:#x} is no registered class and no package.", info.registeredName, first));
                return;
            }
            readProperties(image.read<std::uint64_t>(info.params + layout->classProperties), readClassPropertyCount(info.params), info.registeredName, entry);
        } else {
            super = findIntrinsicSuper(index, signature);
            if (image.getError())
                return;
        }
        if (super && *super != index)
            entry.super = classes[*super].name;
        schema.structs.push_back(entry);
    }
}

void Reflection::readStruct(const std::uint64_t params) {
    const std::uint32_t name = structNames.at(params);
    Struct entry{.name = name};
    const std::uint64_t super = image.read<std::uint64_t>(params + structSuper);
    if (super != 0)
        entry.super = resolveStruct(super);
    const std::uint32_t count = readPropertyCount(params + layout->structPropertyCount, layout->structPropertyCount16);
    readProperties(image.read<std::uint64_t>(params + layout->structProperties), count, schema.names.getNames()[name], entry);
    schema.structs.push_back(entry);
}

std::string toLower(std::string_view text) {
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; });
    return lower;
}

//! UEnum::SetEnums adds MAX unless it exists or another enum has the name; UE 4 compares the existing MAX with case.
void Reflection::readEnum(const TypeRecord& record) {
    const std::uint32_t name = enumNames.at(record.params);
    const std::string enumName = schema.names.getNames()[name];
    const std::uint64_t values = image.read<std::uint64_t>(record.params + layout->enumEnumerators);
    const std::int64_t count = layout->enumEnumeratorCount16 ? image.read<std::int16_t>(record.params + layout->enumEnumeratorCount)
                                                             : image.read<std::int32_t>(record.params + layout->enumEnumeratorCount);
    if (count < 0 || count >= maxEnumerators) {
        image.fail(std::format("The enum {} has {} enumerators.", enumName, count));
        return;
    }
    enumerators.clear();
    for (std::int64_t i = 0; i < count; i++) {
        const std::uint64_t entry = values + std::uint64_t(i) * enumeratorSize;
        enumerators.push_back({image.readUtf8(image.read<std::uint64_t>(entry)), image.read<std::uint64_t>(entry + 8)});
    }
    const bool regular = std::ranges::none_of(enumerators, [](const EnumName& entry) { return entry.full.contains("::"); });
    const auto full = [&](std::string item) { return regular || item.contains("::") ? item : enumName + "::" + item; };
    const auto contains = [&](const std::string& item) {
        if (!engine5)
            return std::ranges::any_of(enumerators, [&](const EnumName& entry) { return entry.full == item; });
        const std::string lower = toLower(item);
        return std::ranges::any_of(enumerators, [&](const EnumName& entry) { return toLower(entry.full) == lower; });
    };
    const std::string scope = engine5 ? packages[record.package] + '\n' : std::string();
    const std::string maxName = full(enumPrefixOf(enumerators, enumName) + "_MAX");
    if (!contains(full("MAX")) && !contains(maxName) && !usedEnumNames.contains(toLower(scope + maxName))) {
        const bool flags = enumMax != EnumMaxBehavior::Legacy && layout->enumFlags != 0 &&
                           (image.read<std::uint8_t>(record.params + layout->enumFlags) & flagsEnum) != 0;
        const std::uint64_t maxValue = syntheticEnumMax(enumerators | std::views::transform([](const EnumName& entry) { return entry.value; }), enumMax, flags);
        enumerators.push_back({maxName, maxValue});
    }
    Enum entry{.name = name, .firstValue = std::uint32_t(schema.enumValues.size()), .valueCount = std::uint16_t(enumerators.size())};
    for (const EnumName& enumerator : enumerators) {
        usedEnumNames.insert(toLower(scope + enumerator.full));
        schema.enumValues.push_back({enumerator.value, schema.names.add(shortEnumName(enumerator.full))});
    }
    schema.enums.push_back(entry);
}

std::expected<Schema, Error> Reflection::read() {
    const std::vector<std::uint64_t> initializers = findInitializers();
    if (image.getError())
        return std::unexpected(*image.getError());
    runInitializers(initializers);
    engine5 = collectBatches(initializers);
    collectSingles(initializers);
    for (const auto [registrar, name] : singleTypes)
        if (findParams(registrar, ParamsKind::Struct) != 0)
            registeredStructs.push_back(registrar);
    if (image.getError())
        return std::unexpected(*image.getError());
    detectLayout();
    if (image.getError())
        return std::unexpected(*image.getError());
    classifySingles();
    if (image.getError())
        return std::unexpected(*image.getError());
    if (classRecords.empty())
        return std::unexpected(Error{"The executable registers no reflected classes."});
    collectClasses();
    for (const std::uint64_t registrar : registeredStructs) {
        const std::uint64_t params = findParams(registrar, ParamsKind::Struct);
        if (params == 0 && !image.getError())
            return std::unexpected(Error{std::format("The struct function at {:#x} references no struct parameters.", registrar)});
        queueStruct(params);
    }
    for (const std::uint64_t registrar : registeredEnums) {
        const std::uint64_t params = findParams(registrar, ParamsKind::Enum);
        if (params == 0 && !image.getError())
            return std::unexpected(Error{std::format("The enum function at {:#x} references no enum parameters.", registrar)});
        queueEnum(params);
    }
    if (image.getError())
        return std::unexpected(*image.getError());
    if (engine5 && layout == &layout53)
        enumMax = enumMaxBehavior(true, image.readFileVersion());
    detectTypeNumbering();
    detectPropertyOrder();
    collectPackageTypes();
    if (image.getError())
        return std::unexpected(*image.getError());
    readClasses();
    for (std::size_t i = 0; i < structQueue.size() && !image.getError(); i++)
        readStruct(structQueue[i]);
    for (std::size_t i = 0; i < enumQueue.size() && !image.getError(); i++)
        readEnum(enumQueue[i]);
    if (image.getError())
        return std::unexpected(*image.getError());
    return std::move(schema);
}

}

std::expected<Schema, Error> readReflection(Image& image) {
    Reflection reflection(image);
    return reflection.read();
}

}
