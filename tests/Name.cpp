#include <array>
#include <cstdio>
#include <string>
#include <string_view>

#include "UsmapGen/Reflection.h"
#include "UsmapGen/UsmapGen.h"

namespace {

using UsmapGen::NameCasing;
using UsmapGen::shortEnumName;
using namespace std::string_view_literals;

int checks = 0;
int failures = 0;

void check(bool condition, const char* label) {
    checks++;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        failures++;
    }
}

constexpr std::array modes{NameCasing::Unknown, NameCasing::NonPreserving, NameCasing::Preserving};

void testBareNone() {
    for (unsigned bits = 0; bits < 16; bits++) {
        std::string bare = "none";
        for (unsigned i = 0; i < bare.size(); i++)
            if (bits & (1u << i))
                bare[i] = char(bare[i] - 'a' + 'A');
        for (const NameCasing mode : modes) {
            check(shortEnumName(bare, mode) == (mode == NameCasing::NonPreserving ? "None" : bare), "all 16 ASCII cases, all three modes");
            check(shortEnumName("Enum::" + bare, mode) == bare, "scope is not stripped before reserved-name check");
            check(shortEnumName("Outer::Enum::" + bare, mode) == bare, "nested scopes retain display spelling");
            for (const std::string_view suffix : {"_0", "_1", "_01", "_MAX", "X"})
                check(shortEnumName(bare + std::string(suffix), mode) == bare + std::string(suffix), "numbered and suffixed None unchanged");
        }
    }
    //! No locale folding, prefix matching or embedded-NUL truncation.
    for (unsigned position = 0; position < 4; position++) {
        for (unsigned byte = 0; byte < 256; byte++) {
            std::string name = "none";
            const bool match = byte == unsigned(name[position]) || byte == unsigned(name[position] - 'a' + 'A');
            name[position] = char(byte);
            for (const NameCasing mode : modes)
                check(shortEnumName(name, mode) == (match && mode == NameCasing::NonPreserving ? "None" : name), "only exact ASCII bare None matches");
        }
    }
}

void testUnchanged() {
    constexpr std::array names{
        ""sv, "N"sv, "NON"sv, "NONES"sv, " NONE"sv, "NONE "sv, "NONE\0"sv,
        "NONE_MAX"sv, "NONE_0"sv, "NONE_2147483647"sv, "NONE_-1"sv,
        "HP"sv, "Hp"sv, "rewards"sv, "Rewards"sv, "none_value"sv,
        "n\xC3\xB6ne"sv, "\xEF\xBC\xAEONE"sv
    };
    for (const auto name : names)
        for (const NameCasing mode : modes)
            check(shortEnumName(name, mode) == name, "unrelated literals are byte-identical");
    for (const NameCasing mode : modes) {
        check(shortEnumName("Enum::NONE", mode) == "NONE", "scoped NONE remains uppercase");
        check(shortEnumName("Enum::none", mode) == "none", "scoped none remains lowercase");
        check(shortEnumName("Enum::NONE_MAX", mode) == "NONE_MAX", "scoped MAX unchanged");
        check(shortEnumName("NONE::Value", mode) == "Value", "None scope is not a bare None");
        check(shortEnumName("Enum::", mode).empty(), "legacy empty suffix behavior");
        check(shortEnumName("Enum:NONE", mode) == "Enum:NONE", "single colon unchanged");
    }
    check(shortEnumName("NONE", static_cast<NameCasing>(255)) == "NONE", "only explicitly non-preserving authorizes normalization");
}

UsmapGen::Schema fixture(NameCasing mode, bool legacy) {
    UsmapGen::Schema schema;
    const auto name = schema.names.add("Fixture");
    //! Shared string-table entries must not be globally case-folded.
    for (const auto property : {"NONE", "None", "HP", "Hp", "rewards", "Rewards"})
        schema.names.add(property);
    constexpr std::array literals{"NONE"sv, "none"sv, "NoNe"sv, "None"sv, "Enum::NONE"sv,
        "Enum::none"sv, "NONE_0"sv, "NONE_MAX"sv, "HP"sv, "Hp"sv, "rewards"sv, "Rewards"sv};
    for (std::size_t i = 0; i < literals.size(); i++) {
        const auto full = literals[i];
        const auto separator = full.rfind("::");
        const auto old = separator == std::string_view::npos ? full : full.substr(separator + 2);
        schema.enumValues.push_back({i, schema.names.add(legacy ? old : shortEnumName(full, mode))});
    }
    schema.enums.push_back({name, 0, std::uint16_t(literals.size())});
    return schema;
}

void testSerialization() {
    const auto legacy = fixture(NameCasing::Unknown, true);
    const auto original = UsmapGen::writeUsmap(legacy);
    for (const NameCasing mode : {NameCasing::Unknown, NameCasing::Preserving})
        check(UsmapGen::writeUsmap(fixture(mode, false)) == original, "unknown and preserving serialize byte-identically to legacy");
    const auto changed = fixture(NameCasing::NonPreserving, false);
    check(UsmapGen::writeUsmap(changed) != original, "opt-in changes bare enum values");
    for (std::size_t i = 0; i < changed.enumValues.size(); i++) {
        const auto& value = changed.enumValues[i];
        check(value.value == legacy.enumValues[i].value, "numeric enum values unchanged");
        check(changed.names.getNames()[value.name] == (i < 4 ? "None" : legacy.names.getNames()[legacy.enumValues[i].name]), "only bare None serialized names change");
    }
    for (std::size_t i = 0; i < 7; i++)
        check(changed.names.getNames()[i] == legacy.names.getNames()[i], "existing shared name-table spellings unchanged");
}

}

int main() {
    testBareNone();
    testUnchanged();
    testSerialization();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
