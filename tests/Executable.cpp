#include <format>
#include <fstream>
#include <initializer_list>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>

#include "UsmapGen/Image.h"
#include "UsmapGen/Reflection.h"
#include "UsmapGen/Schema.h"

namespace {

class FileSource final : public UsmapGen::Source {
public:
    explicit FileSource(const char* path) : file(path, std::ios::binary | std::ios::ate), size(std::uint64_t(file.tellg())) {}

    bool isOpen() const { return file.is_open(); }

    std::uint64_t getSize() const noexcept override { return size; }

    std::expected<void, UsmapGen::Error> read(std::uint64_t offset, std::span<std::byte> buffer) const noexcept override {
        file.seekg(std::streamoff(offset));
        file.read(reinterpret_cast<char*>(buffer.data()), std::streamsize(buffer.size()));
        if (!file)
            return std::unexpected(UsmapGen::Error{"The executable cannot be read."});
        return {};
    }

private:
    mutable std::ifstream file;
    std::uint64_t size;
};

struct Minimum {
    std::string_view what;
    std::size_t count;
    std::size_t minimum;
};

struct RequiredStruct {
    std::string_view name;
    std::string_view super;
    std::initializer_list<std::string_view> properties;
};

struct RequiredEnum {
    std::string_view name;
    std::initializer_list<std::string_view> values;
};

constexpr RequiredStruct requiredStructs[] = {
    {"Object", "", {}},
    {"ObjectRedirector", "Object", {}},
    {"Actor", "Object", {"RootComponent"}},
    {"Info", "Actor", {}},
    {"GameModeBase", "Info", {}},
    {"Pawn", "Actor", {"Controller"}},
    {"Character", "Pawn", {"CharacterMovement"}},
    {"Controller", "Actor", {}},
    {"PlayerController", "Controller", {}},
    {"ActorComponent", "Object", {}},
    {"SceneComponent", "ActorComponent", {"AttachParent"}},
    {"PrimitiveComponent", "SceneComponent", {}},
    {"World", "Object", {}},
    {"Vector", "", {"X", "Y", "Z"}},
    {"Rotator", "", {"Pitch", "Yaw", "Roll"}},
    {"Transform", "", {"Rotation", "Translation", "Scale3D"}},
    {"Guid", "", {"A", "B", "C", "D"}},
    {"LinearColor", "", {"R", "G", "B", "A"}},
    {"Color", "", {"B", "G", "R", "A"}},
};

constexpr RequiredEnum requiredEnums[] = {
    {"ENetRole", {"ROLE_None", "ROLE_SimulatedProxy", "ROLE_AutonomousProxy", "ROLE_Authority"}},
    {"ECollisionChannel", {"ECC_WorldStatic", "ECC_WorldDynamic", "ECC_Pawn", "ECC_Visibility"}},
};

class Checker {
public:
    explicit Checker(const UsmapGen::Schema& schema) : schema(schema) {
        for (const UsmapGen::Struct& entry : schema.structs)
            structs.emplace(nameOf(entry.name), &entry);
        for (const UsmapGen::Enum& entry : schema.enums)
            enums.emplace(nameOf(entry.name), &entry);
    }

    int check(std::size_t usmapSize) {
        const Minimum minimums[] = {
            {"names", schema.names.getNames().size(), 7000},
            {"enums", schema.enums.size(), 300},
            {"enum values", schema.enumValues.size(), 1800},
            {"structs", schema.structs.size(), 1500},
            {"properties", schema.properties.size(), 6000},
            {"usmap bytes", usmapSize, 250000},
        };
        for (const Minimum& minimum : minimums) {
            std::println("{} {}", minimum.what, minimum.count);
            if (minimum.count < minimum.minimum)
                fail(std::format("The mappings have {} {}, fewer than {}.", minimum.count, minimum.what, minimum.minimum));
        }
        if (structs.size() != schema.structs.size())
            fail("Two structs have the same name.");
        if (enums.size() != schema.enums.size())
            fail("Two enums have the same name.");
        for (const UsmapGen::Struct& entry : schema.structs)
            checkStruct(entry);
        for (const UsmapGen::TypeNode& node : schema.types)
            checkType(node);
        for (const RequiredStruct& required : requiredStructs)
            checkRequired(required);
        for (const RequiredEnum& required : requiredEnums)
            checkRequired(required);
        return failures;
    }

private:
    std::string_view nameOf(std::uint32_t name) const { return name == UsmapGen::noName ? std::string_view() : schema.names.getNames()[name]; }

    void fail(const std::string& message) {
        std::println(stderr, "{}", message);
        failures++;
    }

    void checkStruct(const UsmapGen::Struct& entry) {
        if (nameOf(entry.name).starts_with("DEPRECATED_"))
            fail(std::format("The struct {} keeps the DEPRECATED_ prefix.", nameOf(entry.name)));
        if (entry.super != UsmapGen::noName && !structs.contains(nameOf(entry.super)))
            fail(std::format("The super {} of {} is no struct.", nameOf(entry.super), nameOf(entry.name)));
    }

    void checkType(const UsmapGen::TypeNode& node) {
        if (node.type == UsmapGen::PropertyType::Struct && !structs.contains(nameOf(node.name)))
            fail(std::format("A property has the struct {}, which is not in the mappings.", nameOf(node.name)));
        if (node.type == UsmapGen::PropertyType::Enum && !enums.contains(nameOf(node.name)))
            fail(std::format("A property has the enum {}, which is not in the mappings.", nameOf(node.name)));
    }

    void checkRequired(const RequiredStruct& required) {
        const auto found = structs.find(required.name);
        if (found == structs.end()) {
            fail(std::format("The mappings have no struct {}.", required.name));
            return;
        }
        const UsmapGen::Struct& entry = *found->second;
        if (nameOf(entry.super) != required.super)
            fail(std::format("The super of {} is '{}', not '{}'.", required.name, nameOf(entry.super), required.super));
        for (const std::string_view property : required.properties) {
            bool present = false;
            for (std::uint32_t index = entry.firstProperty; index < entry.firstProperty + entry.propertyCount; index++)
                present = present || nameOf(schema.properties[index].name) == property;
            if (!present)
                fail(std::format("The struct {} has no property {}.", required.name, property));
        }
    }

    void checkRequired(const RequiredEnum& required) {
        const auto found = enums.find(required.name);
        if (found == enums.end()) {
            fail(std::format("The mappings have no enum {}.", required.name));
            return;
        }
        const UsmapGen::Enum& entry = *found->second;
        for (const std::string_view value : required.values) {
            bool present = false;
            for (std::uint32_t index = entry.firstValue; index < entry.firstValue + entry.valueCount; index++)
                present = present || nameOf(schema.enumValues[index].name) == value;
            if (!present)
                fail(std::format("The enum {} has no value {}.", required.name, value));
        }
    }

    const UsmapGen::Schema& schema;
    std::unordered_map<std::string_view, const UsmapGen::Struct*> structs;
    std::unordered_map<std::string_view, const UsmapGen::Enum*> enums;
    int failures = 0;
};

}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::println(stderr, "usage: usmapgen-executable-test <executable>");
        return 2;
    }
    const FileSource source(argv[1]);
    if (!source.isOpen()) {
        std::println(stderr, "{} cannot be opened.", argv[1]);
        return 1;
    }
    UsmapGen::Image image(source);
    if (const std::expected<void, UsmapGen::Error> opened = image.open(); !opened) {
        std::println(stderr, "{}", opened.error().message);
        return 1;
    }
    const std::expected<UsmapGen::Schema, UsmapGen::Error> schema = UsmapGen::readReflection(image);
    if (!schema) {
        std::println(stderr, "{}", schema.error().message);
        return 1;
    }
    return Checker(*schema).check(UsmapGen::writeUsmap(*schema).size()) != 0;
}
