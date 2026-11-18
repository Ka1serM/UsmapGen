#include <format>
#include <fstream>
#include <iterator>
#include <print>
#include <string>
#include <string_view>
#include <unordered_set>

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

class Checker {
public:
    explicit Checker(const UsmapGen::Schema& schema) : schema(schema) {
        for (const UsmapGen::Struct& entry : schema.structs)
            structNames.insert(entry.name);
        for (const UsmapGen::Enum& entry : schema.enums)
            enumNames.insert(entry.name);
    }

    int check() {
        if (structNames.size() != schema.structs.size())
            fail("Two structs have the same name.");
        if (enumNames.size() != schema.enums.size())
            fail("Two enums have the same name.");
        for (const UsmapGen::Struct& entry : schema.structs)
            checkStruct(entry);
        for (const UsmapGen::TypeNode& node : schema.types)
            checkType(node);
        checkSuper("Object", "");
        checkSuper("ObjectRedirector", "Object");
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
        if (entry.super != UsmapGen::noName && !structNames.contains(entry.super))
            fail(std::format("The super {} of {} is no struct.", nameOf(entry.super), nameOf(entry.name)));
    }

    void checkType(const UsmapGen::TypeNode& node) {
        if (node.type == UsmapGen::PropertyType::Struct && !structNames.contains(node.name))
            fail(std::format("A property has the struct {}, which is not in the mappings.", nameOf(node.name)));
        if (node.type == UsmapGen::PropertyType::Enum && !enumNames.contains(node.name))
            fail(std::format("A property has the enum {}, which is not in the mappings.", nameOf(node.name)));
    }

    void checkSuper(std::string_view name, std::string_view super) {
        for (const UsmapGen::Struct& entry : schema.structs) {
            if (nameOf(entry.name) != name)
                continue;
            if (nameOf(entry.super) != super)
                fail(std::format("The super of {} is '{}', not '{}'.", name, nameOf(entry.super), super));
            return;
        }
        fail(std::format("The mappings have no {}.", name));
    }

    const UsmapGen::Schema& schema;
    std::unordered_set<std::uint32_t> structNames;
    std::unordered_set<std::uint32_t> enumNames;
    int failures = 0;
};

std::uint64_t fnv1a(std::span<const std::byte> bytes) {
    std::uint64_t hash = 0xCBF29CE484222325;
    for (const std::byte byte : bytes)
        hash = (hash ^ std::uint8_t(byte)) * 0x100000001B3;
    return hash;
}

std::string summarize(const UsmapGen::Schema& schema) {
    return std::format("names {}\nenums {}\nenum values {}\nstructs {}\nproperties {}\nusmap fnv1a {:016x}\n", schema.names.getNames().size(),
                       schema.enums.size(), schema.enumValues.size(), schema.structs.size(), schema.properties.size(),
                       fnv1a(UsmapGen::writeUsmap(schema)));
}

std::string readText(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

bool writeText(const char* path, std::string_view text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
    return bool(file);
}

int compare(const char* expectedPath, const char* actualPath, const std::string& actual) {
    const std::string expected = readText(expectedPath);
    if (expected == actual)
        return 0;
    if (!writeText(actualPath, actual)) {
        std::println(stderr, "{} cannot be written.", actualPath);
        return 1;
    }
    if (expected.empty()) {
        std::println(stderr, "{} does not exist. Review the new summary and copy it there:\n{}cp {} {}", expectedPath, actual, actualPath, expectedPath);
        return 1;
    }
    std::println(stderr, "The mappings differ from {}.\nExpected:\n{}Actual:\n{}If the change is correct: cp {} {}", expectedPath, expected, actual,
                 actualPath, expectedPath);
    return 1;
}

}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::println(stderr, "usage: usmapgen-executable-test <executable> <expected summary> <actual summary>");
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
    const int failures = Checker(*schema).check();
    return compare(argv[2], argv[3], summarize(*schema)) != 0 || failures != 0;
}
