#include "UsmapGen/Schema.h"

#include <format>
#include <map>
#include <span>

namespace UsmapGen {

namespace {

struct MapStructType {
    std::uint32_t key = noName;
    std::uint32_t value = noName;
};

using MapStructTypes = std::map<std::string_view, MapStructType>;

std::uint32_t structNameOf(const TypeNode& node) {
    return node.type == PropertyType::Struct ? node.name : noName;
}

std::size_t collect(const Schema& schema, const std::string_view property, std::size_t at, MapStructTypes& out) {
    const std::span<const TypeNode> types = schema.types;
    const TypeNode& node = types[at++];
    switch (node.type) {
    case PropertyType::Enum:
    case PropertyType::Array:
    case PropertyType::Set:
    case PropertyType::Optional:
        return collect(schema, property, at, out);
    case PropertyType::Map: {
        const std::size_t value = collect(schema, property, at, out);
        const MapStructType entry{structNameOf(types[at]), structNameOf(types[value])};
        //! Properties of different structs share a name; the first one found keeps it.
        if (entry.key != noName || entry.value != noName)
            out.try_emplace(property, entry);
        return collect(schema, property, value, out);
    }
    default:
        return at;
    }
}

void appendString(std::string& json, const std::string_view text) {
    json += '"';
    for (const char character : text) {
        if (character == '"' || character == '\\')
            json += '\\';
        if (static_cast<unsigned char>(character) < 0x20)
            json += std::format("\\u{:04x}", static_cast<unsigned char>(character));
        else
            json += character;
    }
    json += '"';
}

void appendStruct(std::string& json, const Schema& schema, const std::uint32_t name) {
    if (name == noName)
        json += "null";
    else
        appendString(json, schema.names.getNames()[name]);
}

}

std::string writeMapStructTypes(const Schema& schema) {
    MapStructTypes entries;
    for (const Property& property : schema.properties)
        collect(schema, schema.names.getNames()[property.name], property.firstType, entries);
    if (entries.empty())
        return "{}\n";
    std::string json = "{";
    for (const auto& [property, entry] : entries) {
        json += json.size() == 1 ? "\n  " : ",\n  ";
        appendString(json, property);
        json += ": {\n    \"Key\": ";
        appendStruct(json, schema, entry.key);
        json += ",\n    \"Value\": ";
        appendStruct(json, schema, entry.value);
        json += "\n  }";
    }
    json += "\n}\n";
    return json;
}

}
