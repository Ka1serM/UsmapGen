#include "UsmapGen/Schema.h"

#include <cstring>
#include <span>

namespace UsmapGen {

namespace {

constexpr std::uint16_t usmapMagic = 0x30C4;
//! ExplicitEnumValues: the newest version whose body this writer fills. EngineVersioning only adds fields to the versioning header, which stays empty.
constexpr std::uint8_t usmapVersionExplicitEnumValues = 4;
constexpr std::uint8_t compressionNone = 0;

class Writer {
public:
    explicit Writer(std::vector<std::byte>& out) : out(out) {}

    template <class T>
    void write(T value) {
        const std::size_t at = out.size();
        out.resize(at + sizeof(T));
        std::memcpy(out.data() + at, &value, sizeof(T));
    }

    void writeBytes(std::string_view text) {
        const auto* begin = reinterpret_cast<const std::byte*>(text.data());
        out.insert(out.end(), begin, begin + text.size());
    }

    void writeName(std::uint32_t name) { write<std::int32_t>(name == noName ? -1 : std::int32_t(name)); }

    std::size_t writeType(std::span<const TypeNode> types, std::size_t at) {
        const TypeNode& node = types[at++];
        write(std::uint8_t(node.type));
        switch (node.type) {
        case PropertyType::Enum:
            at = writeType(types, at);
            writeName(node.name);
            return at;
        case PropertyType::Struct:
            writeName(node.name);
            return at;
        case PropertyType::Array:
        case PropertyType::Set:
        case PropertyType::Optional:
            return writeType(types, at);
        case PropertyType::Map:
            return writeType(types, writeType(types, at));
        default:
            return at;
        }
    }

private:
    std::vector<std::byte>& out;
};

}

std::uint32_t NameTable::add(const std::string_view name) {
    if (const auto found = indices.find(name); found != indices.end())
        return found->second;
    const std::uint32_t index = std::uint32_t(names.size());
    names.emplace_back(name);
    indices.emplace(names.back(), index);
    return index;
}

std::vector<std::byte> writeUsmap(const Schema& schema) {
    std::vector<std::byte> data;
    Writer body(data);
    body.write(std::uint32_t(schema.names.getNames().size()));
    for (const std::string& name : schema.names.getNames()) {
        body.write(std::uint16_t(name.size()));
        body.writeBytes(name);
    }
    body.write(std::uint32_t(schema.enums.size()));
    for (const Enum& entry : schema.enums) {
        body.writeName(entry.name);
        body.write(entry.valueCount);
        for (const EnumValue& value : std::span(schema.enumValues).subspan(entry.firstValue, entry.valueCount)) {
            body.write(value.value);
            body.writeName(value.name);
        }
    }
    body.write(std::uint32_t(schema.structs.size()));
    for (const Struct& entry : schema.structs) {
        body.writeName(entry.name);
        body.writeName(entry.super);
        body.write(entry.indexCount);
        body.write(entry.propertyCount);
        for (const Property& property : std::span(schema.properties).subspan(entry.firstProperty, entry.propertyCount)) {
            body.write(property.index);
            body.write(property.arrayDim);
            body.writeName(property.name);
            body.writeType(schema.types, property.firstType);
        }
    }

    std::vector<std::byte> file;
    file.reserve(data.size() + 16);
    Writer header(file);
    header.write(usmapMagic);
    header.write(usmapVersionExplicitEnumValues);
    header.write(std::int32_t(0));
    header.write(compressionNone);
    header.write(std::uint32_t(data.size()));
    header.write(std::uint32_t(data.size()));
    file.insert(file.end(), data.begin(), data.end());
    return file;
}

}
