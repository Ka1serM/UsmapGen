#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "UsmapGen/UsmapGen.h"
#include "UsmapGen/Version.h"

namespace UsmapGen {

struct Section {
    std::uint64_t address = 0;
    std::uint32_t virtualSize = 0;
    std::uint32_t fileOffset = 0;
    std::uint32_t fileSize = 0;

    bool contains(std::uint64_t at) const { return at >= address && at < address + virtualSize; }
};

class Image {
public:
    explicit Image(const Source& source);
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    std::expected<void, Error> open();

    std::optional<FileVersion> readFileVersion();

    const Section& getCode() const { return code; }
    const Section& getReadOnlyData() const { return readOnlyData; }
    std::uint64_t getEntryPoint() const { return entryPoint; }
    bool isCode(std::uint64_t address) const { return code.contains(address); }
    bool isData(std::uint64_t address) const { return readOnlyData.contains(address) || data.contains(address); }
    bool isWritableData(std::uint64_t address) const { return data.contains(address); }
    bool isReadOnlyData(std::uint64_t address) const { return readOnlyData.contains(address); }

    void read(std::uint64_t address, std::span<std::byte> out);
    template <class T>
    T read(std::uint64_t address) {
        std::byte bytes[sizeof(T)];
        read(address, bytes);
        T value;
        std::memcpy(&value, bytes, sizeof(T));
        return value;
    }
    std::string readUtf8(std::uint64_t address);
    std::string readUtf16(std::uint64_t address);
    bool startsWithUtf16(std::uint64_t address, std::u16string_view prefix);

    void write(std::uint64_t address, std::span<const std::byte> bytes);

    bool isFunctionStart(std::uint64_t address);
    bool hasRelocations() const { return relocationsSize != 0; }
    std::vector<std::uint64_t> readPointerSlots(const Section& section);

    const std::optional<Error>& getError() const { return error; }
    void fail(std::string message);

private:
    void readFile(std::uint64_t offset, std::span<std::byte> out);
    void readSections(std::uint64_t address, std::span<std::byte> out);
    void applyWrites(std::uint64_t address, std::span<std::byte> out) const;
    std::span<const std::byte> loadPage(std::uint64_t page);
    const Section* findSection(std::uint64_t address) const;

    const Source& source;
    std::uint64_t base = 0;
    std::vector<Section> sections;
    Section code;
    Section readOnlyData;
    Section data;
    std::uint64_t functionTable = 0;
    std::uint32_t functionCount = 0;
    std::uint64_t entryPoint = 0;
    std::uint32_t resourceRva = 0;
    std::uint32_t resourceSize = 0;
    std::uint64_t relocations = 0;
    std::uint32_t relocationsSize = 0;
    std::unordered_map<std::uint64_t, std::size_t> writtenPageOffsets;
    std::vector<std::byte> writtenPages;

    std::vector<std::byte> pages;
    std::vector<std::uint32_t> slotOfPage;
    std::vector<std::uint64_t> pageOfSlot;
    std::vector<std::uint8_t> recentlyUsed;
    std::uint32_t usedSlots = 0;
    std::uint32_t clockHand = 0;

    std::optional<Error> error;
};

}
