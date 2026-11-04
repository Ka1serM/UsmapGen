#include "UsmapGen/Reflection.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "UsmapGen/Code.h"
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
constexpr std::uint32_t maxArrayDim = 0xFF;
constexpr int maxGetterDepth = 2;
constexpr int maxTypeDepth = 16;
constexpr std::size_t engine4SuperArgument = 11;
constexpr std::size_t engine425SuperArgument = 12;
constexpr std::uint32_t rawStruct = 0x19;
constexpr std::uint32_t firstEngine423Kind = 0x1E;
constexpr std::uint32_t maxEngine418Kind = 0x1D;
constexpr std::uint32_t maxGenFlagsKind = 0x3F;
constexpr std::uint32_t unknownPackage = ~0u;
constexpr std::u16string_view scriptPrefix = u"/Script/";

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
    bool intrinsic;
};

struct TypeRecord {
    std::uint64_t params;
    std::uint32_t package;
};

struct Construction {
    std::uint64_t target;
    std::uint64_t params;
};

struct ClassBody {
    std::uint64_t name;
    std::uint64_t superGetter;
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
    bool collectBatches(std::span<const std::uint64_t> initializers);
    void collectSingles(std::span<const std::uint64_t> initializers);
    void readEngine5Records(std::span<const Batch> batches);
    std::uint32_t addPackage(std::uint64_t name);
    void collectPackageTypes(std::uint64_t structConstructor, std::uint64_t enumConstructor);

    std::uint64_t findParams(std::uint64_t function);
    Construction findConstruction(std::uint64_t function);
    Construction findDirectConstruction(std::uint64_t function);
    std::uint64_t findStaticTypeConstruct(std::uint64_t function);

    bool isWideName(std::uint64_t address);
    bool isName(std::uint64_t address);
    void detectLayout();
    std::uint64_t scoreLayout(const Layout& candidate, std::span<const std::uint64_t> sample);
    void detectTypeNumbering();
    void detectPropertyOrder();
    void detectClassBody();
    std::optional<ClassBody> findClassBody(std::uint64_t getter, int depth);

    std::uint32_t readPropertyCount(std::uint64_t at, bool count16);
    std::optional<Kind> readKind(std::uint64_t property);
    std::uint8_t readArrayDim(std::uint64_t property);
    std::span<const std::uint64_t> readPointers(std::uint64_t array, std::uint32_t count);
    void readProperties(std::uint64_t array, std::uint32_t count, std::string_view owner, Struct& out);
    bool readType(std::uint64_t property, PropertyCursor& cursor, int depth);

    std::uint32_t resolveStruct(std::uint64_t function);
    std::uint32_t resolveEnum(std::uint64_t function);
    std::uint32_t queueStruct(std::uint64_t params);
    std::uint32_t queueEnum(std::uint64_t params, std::uint32_t package);
    std::uint32_t resolveClass(std::uint64_t getter);

    void readClasses();
    void readStruct(std::uint64_t params);
    void readEnum(const TypeRecord& record);

    Image& image;
    Code code;
    Schema schema;

    bool engine5 = false;
    const Layout* layout = nullptr;
    TypeNumbering numbering = TypeNumbering::Engine5;
    bool backward = true;
    std::uint64_t classBody = 0;
    std::size_t superArgument = engine425SuperArgument;

    std::vector<std::string> packages;
    std::vector<ClassRecord> classRecords;
    std::vector<std::uint64_t> structQueue;
    std::vector<TypeRecord> enumQueue;
    std::unordered_map<std::uint64_t, Construction> constructionOfFunction;
    std::unordered_map<std::uint64_t, std::uint32_t> structNames;
    std::unordered_map<std::uint64_t, std::uint32_t> enumNames;
    std::unordered_map<std::uint64_t, std::uint32_t> classNames;
    std::unordered_set<std::string> usedEnumNames;

    std::vector<std::uint64_t> pointers;
    std::vector<EnumName> enumerators;
    std::string currentOwner;
};

std::vector<std::uint64_t> Reflection::findInitializers() {
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
    std::partial_sort(runs.begin(), runs.begin() + candidates, runs.end(),
                      [](const Run& a, const Run& b) { return a.count > b.count; });
    //! The .CRT$XCU initializers are the longest run of code pointers in .rdata, with zeros between CRT sections.
    std::vector<std::uint64_t> table;
    for (const Run& run : std::span(runs).first(candidates)) {
        const std::uint64_t begin = slots[run.first];
        const std::uint64_t end = slots[run.first + run.count - 1] + 8;
        table.resize((end - begin) / 8);
        image.read(begin, std::as_writable_bytes(std::span(table)));
        const bool valid = std::ranges::all_of(table, [&](std::uint64_t entry) { return entry == 0 || image.isCode(entry); });
        if (!valid)
            continue;
        std::erase(table, 0);
        return table;
    }
    image.fail("The executable has no table of C++ dynamic initializers.");
    return {};
}

std::uint32_t Reflection::addPackage(const std::uint64_t name) {
    packages.push_back(image.readUtf16(name));
    return std::uint32_t(packages.size() - 1);
}

bool Reflection::isWideName(const std::uint64_t address) {
    if (!image.isReadOnlyData(address))
        return false;
    const char16_t first = image.read<char16_t>(address);
    return first == u'_' || (first >= u'A' && first <= u'Z') || (first >= u'a' && first <= u'z');
}

bool Reflection::isName(const std::uint64_t address) {
    if (!image.isReadOnlyData(address))
        return false;
    const char first = image.read<char>(address);
    return first == '_' || (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
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
        const std::uint32_t package = addPackage(batch.package);
        for (std::uint64_t i = 0; i < batch.counts[0]; i++) {
            const std::uint64_t record = batch.arrays[0] + i * classLayout->stride;
            const std::uint64_t construct = image.read<std::uint64_t>(record);
            const std::uint64_t getter = classLayout->name == recordName ? image.read<std::uint64_t>(record + 8) : construct;
            classRecords.push_back({construct, getter, image.read<std::uint64_t>(record + classLayout->name), false});
        }
        for (std::uint64_t i = 0; i < batch.counts[1]; i++)
            structQueue.push_back(image.read<std::uint64_t>(batch.arrays[1] + i * structStride));
        for (std::uint64_t i = 0; i < batch.counts[2]; i++) {
            const std::uint64_t record = batch.arrays[2] + i * enumRecordStride;
            if (!isWideName(image.read<std::uint64_t>(record + enumRecordName))) {
                image.fail(std::format("The enum registration record at {:#x} has no name.", record));
                return;
            }
            enumQueue.push_back({image.read<std::uint64_t>(record), package});
        }
    }
}

//! Single registrations: UE 4 class (Register, StaticClass, Name, Package), UE 5 intrinsic class (Register, StaticClass, Package, Name), struct or enum (Register, Package, Name).
void Reflection::collectSingles(std::span<const std::uint64_t> initializers) {
    std::vector<std::pair<std::uint64_t, std::uint32_t>> types;
    for (const std::uint64_t initializer : initializers) {
        for (const Call& call : code.findCalls(initializer)) {
            const auto argument = [&](std::size_t index) { return call.getConstant(index).value_or(0); };
            for (std::size_t package = 1; package < 5; package++) {
                if (!image.startsWithUtf16(argument(package), scriptPrefix))
                    continue;
                if (package >= 3 && isWideName(argument(package - 1)) && image.isCode(argument(package - 2)) &&
                    image.isCode(argument(package - 3))) {
                    classRecords.push_back({argument(package - 3), argument(package - 2), argument(package - 1), false});
                    break;
                }
                if (!image.isCode(argument(package - 1)) || !isWideName(argument(package + 1)))
                    continue;
                if (package >= 2 && image.isCode(argument(package - 2))) {
                    classRecords.push_back({argument(package - 2), argument(package - 1), argument(package + 1), true});
                    break;
                }
                types.emplace_back(argument(package - 1), addPackage(argument(package)));
                break;
            }
        }
    }
    for (const auto [registrar, package] : types) {
        const std::uint64_t params = findParams(registrar);
        if (params == 0) {
            image.fail(std::format("The registered type at {:#x} has no parameter table.", registrar));
            return;
        }
        const std::uint64_t field = image.read<std::uint64_t>(params + structOps);
        //! FStructParams has the StructOps function at 0x10, FEnumParams has the name there.
        if (image.isReadOnlyData(field))
            enumQueue.push_back({registrar, package});
        else
            structQueue.push_back(registrar);
    }
}

Construction Reflection::findDirectConstruction(const std::uint64_t function) {
    for (const Call& call : code.findCalls(function)) {
        const std::optional<std::uint64_t> singleton = call.getConstant(0);
        const std::optional<std::uint64_t> params = call.getConstant(1);
        if (singleton && params && image.isWritableData(*singleton) && image.isData(*params))
            return {call.target, *params};
    }
    return {};
}

//! A StaticStruct or StaticEnum function passes the Z_Construct function to GetStaticStruct or GetStaticEnum.
std::uint64_t Reflection::findStaticTypeConstruct(const std::uint64_t function) {
    for (const Call& call : code.findCalls(function)) {
        const std::optional<std::uint64_t> construct = call.getConstant(0);
        const std::optional<std::uint64_t> name = call.getConstant(2);
        if (construct && name && image.isCode(*construct) && isWideName(*name))
            return *construct;
    }
    return 0;
}

//! A Z_Construct function calls UECodeGen_Private::ConstructU*(Singleton, Params).
Construction Reflection::findConstruction(std::uint64_t function) {
    function = code.followThunks(function);
    if (const auto found = constructionOfFunction.find(function); found != constructionOfFunction.end())
        return found->second;
    Construction construction = findDirectConstruction(function);
    if (construction.params == 0) {
        const std::uint64_t construct = findStaticTypeConstruct(function);
        if (construct != 0)
            construction = findDirectConstruction(code.followThunks(construct));
    }
    constructionOfFunction.emplace(function, construction);
    return construction;
}

std::uint64_t Reflection::findParams(const std::uint64_t function) { return findConstruction(function).params; }

//! FPackageParams is {NameUTF8, SingletonFuncArray, NumSingletons}; the singletons also build unregistered no-export structs.
void Reflection::collectPackageTypes(const std::uint64_t structConstructor, const std::uint64_t enumConstructor) {
    std::vector<std::uint64_t> outers;
    std::unordered_set<std::uint64_t> seen;
    const auto addOuter = [&](std::uint64_t params) {
        const std::uint64_t outer = code.followThunks(image.read<std::uint64_t>(params));
        if (seen.insert(outer).second)
            outers.push_back(outer);
    };
    for (const std::uint64_t params : structQueue)
        addOuter(params);
    for (const TypeRecord& record : enumQueue)
        addOuter(record.params);
    for (const std::uint64_t outer : outers) {
        const std::uint64_t params = findParams(outer);
        const std::uint64_t name = image.read<std::uint64_t>(params);
        if (params == 0 || !image.isReadOnlyData(name) || !image.readUtf8(name).starts_with("/Script/"))
            continue;
        const std::uint64_t singletons = image.read<std::uint64_t>(params + 8);
        const std::uint32_t count = image.read<std::uint32_t>(params + 0x10);
        if (count > maxSingletons || (count > 0 && !image.isData(singletons))) {
            image.fail(std::format("The package parameters at {:#x} have {} singletons.", params, count));
            return;
        }
        packages.push_back(image.readUtf8(name));
        const std::uint32_t package = std::uint32_t(packages.size() - 1);
        for (const std::uint64_t function : readPointers(singletons, count)) {
            const Construction construction = findConstruction(function);
            if (construction.params == 0)
                continue;
            if (construction.target == structConstructor)
                queueStruct(construction.params);
            if (construction.target == enumConstructor)
                queueEnum(construction.params, package);
        }
    }
}

std::uint32_t Reflection::readPropertyCount(const std::uint64_t at, const bool count16) {
    return count16 ? image.read<std::uint16_t>(at) : image.read<std::uint32_t>(at);
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
    for (const std::uint64_t registrar : std::span(structQueue).first(std::min(structQueue.size(), sampleSize))) {
        const std::uint64_t params = findParams(registrar);
        if (params != 0)
            sample.push_back(params);
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
    for (const std::uint64_t registrar : structQueue) {
        const std::uint64_t params = findParams(registrar);
        scan(image.read<std::uint64_t>(params + layout->structProperties), readPropertyCount(params + layout->structPropertyCount, layout->structPropertyCount16));
        if (numbering == TypeNumbering::Engine423)
            return;
    }
    for (const ClassRecord& record : classRecords) {
        const std::uint64_t params = findParams(record.construct);
        scan(image.read<std::uint64_t>(params + layout->classProperties), image.read<std::uint32_t>(params + layout->classPropertyCount));
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
    for (const std::uint64_t registrar : std::span(structQueue).first(std::min(structQueue.size(), sampleSize))) {
        const std::uint64_t params = findParams(registrar);
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

std::optional<ClassBody> Reflection::findClassBody(const std::uint64_t getter, const int depth) {
    const std::uint64_t function = code.followThunks(getter);
    std::vector<std::uint64_t> callees;
    for (const Call& call : code.findCalls(function)) {
        if (call.target == classBody) {
            const std::optional<std::uint64_t> name = call.getConstant(1);
            if (!name || !image.isReadOnlyData(*name))
                return std::nullopt;
            return ClassBody{*name, call.getConstant(superArgument).value_or(0)};
        }
        if (image.isCode(call.target))
            callees.push_back(call.target);
    }
    if (depth == maxGetterDepth)
        return std::nullopt;
    for (const std::uint64_t callee : callees)
        if (const std::optional<ClassBody> body = findClassBody(callee, depth + 1))
            return body;
    return std::nullopt;
}

//! GetPrivateStaticClassBody(Package, Name, ReturnClass, RegisterNatives, Size, ...); 4.25 adds the alignment before the super.
void Reflection::detectClassBody() {
    Votes votes;
    const std::span<const ClassRecord> sample = std::span(classRecords).first(std::min(classRecords.size(), sampleSize));
    for (const ClassRecord& record : sample) {
        for (const Call& call : code.findCalls(code.followThunks(record.getter))) {
            const std::optional<std::uint64_t> name = call.getConstant(1);
            if (name && image.isReadOnlyData(*name) && call.getConstant(4))
                votes[call.target]++;
        }
    }
    if (votes.empty()) {
        image.fail("No StaticClass function calls GetPrivateStaticClassBody.");
        return;
    }
    classBody = findMostVoted(votes);
    if (engine5 || layout != &layout418) {
        std::unordered_set<std::uint64_t> getters;
        for (const ClassRecord& record : classRecords)
            getters.insert(code.followThunks(record.getter));
        std::size_t known = 0;
        for (const ClassRecord& record : sample) {
            for (const Call& call : code.findCalls(code.followThunks(record.getter))) {
                if (call.target != classBody)
                    continue;
                if (getters.contains(code.followThunks(call.getConstant(engine4SuperArgument).value_or(0))))
                    known++;
                break;
            }
        }
        superArgument = known * 2 > sample.size() ? engine4SuperArgument : engine425SuperArgument;
        return;
    }
    superArgument = engine4SuperArgument;
}

std::optional<Kind> Reflection::readKind(const std::uint64_t property) {
    if (numbering == TypeNumbering::Engine418)
        return translateKind(numbering, image.read<std::uint32_t>(property + layout->propertyType));
    const std::uint32_t mask = numbering == TypeNumbering::Engine5 ? 0x3F : 0x1F;
    return translateKind(numbering, image.read<std::uint8_t>(property + layout->propertyType) & mask);
}

std::uint8_t Reflection::readArrayDim(const std::uint64_t property) {
    const std::uint32_t arrayDim = layout->propertyArrayDim16 ? image.read<std::uint16_t>(property + layout->propertyArrayDim)
                                                              : image.read<std::uint32_t>(property + layout->propertyArrayDim);
    if (arrayDim == 0 || arrayDim > maxArrayDim) {
        image.fail(std::format("The property at {:#x} has the array size {}.", property, arrayDim));
        return 1;
    }
    return std::uint8_t(arrayDim);
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
        Property property{.name = schema.names.add(image.readUtf8(image.read<std::uint64_t>(entry + layout->propertyName))),
                          .arrayDim = readArrayDim(entry),
                          .firstType = std::uint32_t(schema.types.size())};
        if (!readType(entry, cursor, 0))
            return;
        schema.properties.push_back(property);
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
    const std::uint64_t name = image.read<std::uint64_t>(params + structName);
    if (!isName(name)) {
        image.fail(std::format("The struct parameters at {:#x} have no name.", params));
        return 0;
    }
    const std::uint32_t index = schema.names.add(image.readUtf8(name));
    structNames.emplace(params, index);
    structQueue.push_back(params);
    return index;
}

std::uint32_t Reflection::queueEnum(const std::uint64_t params, const std::uint32_t package) {
    if (const auto found = enumNames.find(params); found != enumNames.end())
        return found->second;
    const std::uint64_t name = image.read<std::uint64_t>(params + enumName);
    if (!isName(name)) {
        image.fail(std::format("The enum parameters at {:#x} have no name.", params));
        return 0;
    }
    const std::uint32_t index = schema.names.add(image.readUtf8(name));
    enumNames.emplace(params, index);
    enumQueue.push_back({params, package});
    return index;
}

std::uint32_t Reflection::resolveStruct(const std::uint64_t function) {
    const std::uint64_t params = findParams(function);
    if (params == 0) {
        image.fail(std::format("The struct function at {:#x} in {} has no parameter table.", function, currentOwner));
        return 0;
    }
    return queueStruct(params);
}

std::uint32_t Reflection::resolveEnum(const std::uint64_t function) {
    const std::uint64_t params = findParams(function);
    if (params == 0) {
        image.fail(std::format("The enum function at {:#x} in {} has no parameter table.", function, currentOwner));
        return 0;
    }
    return queueEnum(params, unknownPackage);
}

std::uint32_t Reflection::resolveClass(const std::uint64_t getter) {
    const std::uint64_t function = code.followThunks(getter);
    if (const auto found = classNames.find(function); found != classNames.end())
        return found->second;
    const std::optional<ClassBody> body = findClassBody(function, 0);
    if (!body) {
        image.fail(std::format("The class getter at {:#x} does not call GetPrivateStaticClassBody.", getter));
        return noName;
    }
    const std::uint32_t name = schema.names.add(image.readUtf16(body->name));
    classNames.emplace(function, name);
    Struct entry{.name = name};
    const std::uint32_t super = body->superGetter == 0 ? noName : resolveClass(body->superGetter);
    entry.super = super == name ? noName : super;
    schema.structs.push_back(entry);
    return name;
}

//! The name that StaticClass passes to GetPrivateStaticClassBody lacks the DEPRECATED_ part of a deprecated class.
void Reflection::readClasses() {
    std::vector<std::uint64_t> superGetters;
    std::vector<const ClassRecord*> records;
    for (const ClassRecord& record : classRecords) {
        const std::string registeredName = image.readUtf16(record.name);
        const auto [found, inserted] = classNames.emplace(code.followThunks(record.getter), schema.names.add(std::string_view(registeredName).substr(1)));
        if (!inserted)
            continue;
        const std::optional<ClassBody> body = findClassBody(record.getter, 0);
        if (!body) {
            image.fail(std::format("The StaticClass function of {} does not call GetPrivateStaticClassBody.", registeredName));
            return;
        }
        superGetters.push_back(body->superGetter);
        records.push_back(&record);
    }
    for (std::size_t i = 0; i < records.size() && !image.getError(); i++) {
        const ClassRecord& record = *records[i];
        const std::uint32_t name = classNames.at(code.followThunks(record.getter));
        Struct entry{.name = name};
        const std::uint32_t super = superGetters[i] == 0 ? noName : resolveClass(superGetters[i]);
        entry.super = super == name ? noName : super;
        const std::uint64_t params = findParams(record.construct);
        if (params == 0 && !record.intrinsic) {
            image.fail(std::format("The class {} has no parameter table.", schema.names.getNames()[name]));
            return;
        }
        //! An intrinsic class, such as UClass or UPackage, has no parameter table and no reflected properties.
        if (params != 0) {
            const std::uint32_t count = layout->classPropertyCountPacked ? image.read<std::uint32_t>(params + layout->classPropertyCount) >> 15 & 0x7FF
                                                                         : image.read<std::uint32_t>(params + layout->classPropertyCount);
            readProperties(image.read<std::uint64_t>(params + layout->classProperties), count, schema.names.getNames()[name], entry);
        }
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
    const std::string scope = engine5 && record.package != unknownPackage ? packages[record.package] + '\n' : std::string();
    const std::string maxName = full(enumPrefixOf(enumerators, enumName) + "_MAX");
    if (!contains(full("MAX")) && !contains(maxName) && !usedEnumNames.contains(toLower(scope + maxName))) {
        std::int64_t maxValue = 0;
        if (!enumerators.empty())
            maxValue = std::ranges::max(enumerators | std::views::transform([](const EnumName& entry) { return std::int64_t(entry.value); }));
        enumerators.push_back({maxName, std::uint64_t(maxValue + 1)});
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
    engine5 = collectBatches(initializers);
    collectSingles(initializers);
    if (image.getError())
        return std::unexpected(*image.getError());
    if (classRecords.empty())
        return std::unexpected(Error{"The executable registers no reflected classes."});
    detectLayout();
    if (image.getError())
        return std::unexpected(*image.getError());
    detectTypeNumbering();
    detectPropertyOrder();
    detectClassBody();
    if (image.getError())
        return std::unexpected(*image.getError());

    Votes enumConstructors;
    const std::vector<TypeRecord> registeredEnums = std::exchange(enumQueue, {});
    for (const TypeRecord& record : registeredEnums) {
        const Construction construction = findConstruction(record.params);
        if (construction.params == 0)
            return std::unexpected(Error{std::format("The enum function at {:#x} has no parameter table.", record.params)});
        enumConstructors[construction.target]++;
        queueEnum(construction.params, record.package);
    }
    Votes structConstructors;
    const std::vector<std::uint64_t> registeredStructs = std::exchange(structQueue, {});
    for (const std::uint64_t registrar : registeredStructs) {
        const Construction construction = findConstruction(registrar);
        if (construction.params == 0)
            return std::unexpected(Error{std::format("The struct function at {:#x} has no parameter table.", registrar)});
        structConstructors[construction.target]++;
        queueStruct(construction.params);
    }
    if (structConstructors.empty() || enumConstructors.empty())
        return std::unexpected(Error{"The executable registers no reflected structs or no reflected enums."});
    collectPackageTypes(findMostVoted(structConstructors), findMostVoted(enumConstructors));
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
