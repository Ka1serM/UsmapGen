#include "UsmapGen/Image.h"

#include <algorithm>
#include <format>
#include <string_view>

namespace UsmapGen {

namespace {

constexpr std::uint32_t pageSize = 64 * 1024;
constexpr std::size_t writtenPageSize = 4096;
constexpr std::uint32_t maxCachedPages = 512;
constexpr std::uint32_t noSlot = 0;
constexpr std::uint32_t peSignature = 0x4550;
constexpr std::uint16_t machineAmd64 = 0x8664;
constexpr std::uint16_t optionalHeaderPe32Plus = 0x20B;
constexpr std::uint32_t resourceDirectory = 2;
constexpr std::uint32_t maxResourceSize = 16 * 1024 * 1024;
constexpr std::uint32_t exceptionDirectory = 3;
constexpr std::uint32_t relocationDirectory = 5;
constexpr std::uint16_t relocationDir64 = 10;
constexpr std::uint32_t runtimeFunctionSize = 12;
constexpr std::uint8_t unwindChainInfo = 4;
constexpr std::size_t maxStringLength = 4096;
constexpr std::size_t stringChunk = 128;

void appendUtf8(std::string& out, char32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
        return;
    }
    if (code < 0x800) {
        out += static_cast<char>(0xC0 | code >> 6);
        out += static_cast<char>(0x80 | (code & 0x3F));
        return;
    }
    if (code < 0x10000) {
        out += static_cast<char>(0xE0 | code >> 12);
        out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
        return;
    }
    out += static_cast<char>(0xF0 | code >> 18);
    out += static_cast<char>(0x80 | (code >> 12 & 0x3F));
    out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
    out += static_cast<char>(0x80 | (code & 0x3F));
}

}

Image::Image(const Source& source) : source(source) {}

std::expected<void, Error> Image::open() {
    const std::uint64_t pageCount = (source.getSize() + pageSize - 1) / pageSize;
    const std::uint32_t slots = static_cast<std::uint32_t>(std::min<std::uint64_t>(pageCount, maxCachedPages));
    pages.resize(std::size_t(slots) * pageSize);
    slotOfPage.assign(pageCount, noSlot);
    pageOfSlot.assign(slots, 0);
    recentlyUsed.assign(slots, 0);

    std::byte header[4];
    readFile(0x3C, header);
    std::uint32_t pe;
    std::memcpy(&pe, header, 4);
    std::byte headers[24 + 240];
    readFile(pe, headers);
    if (error)
        return std::unexpected(*error);
    const auto field = [&headers]<class T>(std::size_t at, T) {
        T value;
        std::memcpy(&value, headers + at, sizeof(T));
        return value;
    };
    if (field(0, std::uint32_t{}) != peSignature || field(4, std::uint16_t{}) != machineAmd64)
        return std::unexpected(Error{"The executable is not a Win64 PE file."});
    if (field(24, std::uint16_t{}) != optionalHeaderPe32Plus)
        return std::unexpected(Error{"The executable has no PE32+ optional header."});
    const std::uint16_t sectionCount = field(6, std::uint16_t{});
    const std::uint16_t optionalSize = field(20, std::uint16_t{});
    base = field(24 + 24, std::uint64_t{});
    const auto directory = [&](std::uint32_t index) {
        return std::pair{field(24 + 112 + 8 * index, std::uint32_t{}), field(24 + 112 + 8 * index + 4, std::uint32_t{})};
    };
    if (optionalSize >= 112 + 8 * (resourceDirectory + 1) && field(24 + 108, std::uint32_t{}) > resourceDirectory) {
        const auto [rva, size] = directory(resourceDirectory);
        resourceRva = rva;
        resourceSize = size;
    }
    const auto [exceptionRva, exceptionSize] = directory(exceptionDirectory);
    functionTable = base + exceptionRva;
    functionCount = exceptionSize / runtimeFunctionSize;
    entryPoint = base + field(24 + 16, std::uint32_t{});
    const auto [relocationRva, relocationSize] = directory(relocationDirectory);
    relocations = base + relocationRva;
    relocationsSize = relocationSize;

    std::vector<std::byte> table(std::size_t(sectionCount) * 40);
    readFile(pe + 24 + optionalSize, table);
    if (error)
        return std::unexpected(*error);
    for (std::uint16_t i = 0; i < sectionCount; i++) {
        const std::byte* entry = table.data() + 40 * i;
        const std::string_view name(reinterpret_cast<const char*>(entry), strnlen(reinterpret_cast<const char*>(entry), 8));
        std::uint32_t values[4];
        std::memcpy(values, entry + 8, sizeof(values));
        const Section section{.address = base + values[1], .virtualSize = values[0], .fileOffset = values[3], .fileSize = values[2]};
        sections.push_back(section);
        if (name == ".text")
            code = section;
        else if (name == ".rdata")
            readOnlyData = section;
        else if (name == ".data")
            data = section;
    }
    if (code.virtualSize == 0 || readOnlyData.virtualSize == 0 || data.virtualSize == 0)
        return std::unexpected(Error{"The executable lacks a .text, .rdata or .data section."});
    if (functionCount == 0)
        return std::unexpected(Error{"The executable has no exception directory."});
    return {};
}

//! Default.rc2 writes the engine version into the fixed fields; the displayed version strings are project-defined.
std::optional<FileVersion> Image::readFileVersion() {
    if (resourceRva == 0 || resourceSize == 0 || resourceSize > maxResourceSize || base > UINT64_MAX - resourceRva)
        return std::nullopt;
    const std::uint64_t address = base + resourceRva;
    const Section* section = findSection(address);
    if (section == nullptr)
        return std::nullopt;
    const std::uint64_t within = address - section->address;
    if (within > section->fileSize || resourceSize > section->fileSize - within || resourceSize > section->virtualSize - within)
        return std::nullopt;
    const std::uint64_t offset = section->fileOffset + within;
    if (offset > source.getSize() || resourceSize > source.getSize() - offset)
        return std::nullopt;
    std::vector<std::byte> bytes(resourceSize);
    //! Optional metadata I/O must not set the reflection image's permanent error.
    if (!source.read(offset, bytes))
        return std::nullopt;
    return error ? std::nullopt : readVersionResource(bytes, resourceRva);
}

void Image::fail(std::string message) {
    if (!error)
        error = Error{std::move(message)};
}

std::span<const std::byte> Image::loadPage(const std::uint64_t page) {
    if (page >= slotOfPage.size())
        return {};
    std::uint32_t slot = slotOfPage[page];
    if (slot != noSlot) {
        recentlyUsed[slot - 1] = 1;
        return std::span(pages).subspan(std::size_t(slot - 1) * pageSize, pageSize);
    }
    if (usedSlots < pageOfSlot.size()) {
        slot = ++usedSlots;
    } else {
        while (recentlyUsed[clockHand]) {
            recentlyUsed[clockHand] = 0;
            clockHand = (clockHand + 1) % pageOfSlot.size();
        }
        slot = clockHand + 1;
        slotOfPage[pageOfSlot[clockHand]] = noSlot;
        clockHand = (clockHand + 1) % pageOfSlot.size();
    }
    const std::uint64_t offset = page * pageSize;
    const std::size_t size = static_cast<std::size_t>(std::min<std::uint64_t>(pageSize, source.getSize() - offset));
    std::span<std::byte> bytes = std::span(pages).subspan(std::size_t(slot - 1) * pageSize, pageSize);
    if (std::expected<void, Error> read = source.read(offset, bytes.first(size)); !read) {
        fail(read.error().message);
        return {};
    }
    std::fill(bytes.begin() + size, bytes.end(), std::byte{0});
    slotOfPage[page] = slot;
    pageOfSlot[slot - 1] = page;
    recentlyUsed[slot - 1] = 1;
    return bytes;
}

void Image::readFile(std::uint64_t offset, std::span<std::byte> out) {
    while (!out.empty()) {
        const std::span<const std::byte> page = loadPage(offset / pageSize);
        if (page.empty()) {
            fail(std::format("The executable has no byte at file offset {:#x}.", offset));
            std::fill(out.begin(), out.end(), std::byte{0});
            return;
        }
        const std::size_t within = offset % pageSize;
        const std::size_t count = std::min(out.size(), pageSize - within);
        std::memcpy(out.data(), page.data() + within, count);
        out = out.subspan(count);
        offset += count;
    }
}

const Section* Image::findSection(const std::uint64_t address) const {
    for (const Section& section : sections)
        if (section.contains(address))
            return &section;
    return nullptr;
}

//! A write copies the page of writable data that it changes, so later reads see the value after static initialization.
void Image::write(std::uint64_t address, std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        if (!data.contains(address)) {
            fail(std::format("The write at {:#x} is outside the writable data.", address));
            return;
        }
        const std::uint64_t page = address / writtenPageSize;
        auto [found, inserted] = writtenPageOffsets.try_emplace(page, writtenPages.size());
        if (inserted) {
            writtenPages.resize(writtenPages.size() + writtenPageSize);
            const std::uint64_t begin = std::max(page * writtenPageSize, data.address);
            const std::uint64_t end = std::min((page + 1) * writtenPageSize, data.address + data.virtualSize);
            readSections(begin, std::span(writtenPages).subspan(found->second + std::size_t(begin - page * writtenPageSize), std::size_t(end - begin)));
        }
        const std::size_t within = std::size_t(address % writtenPageSize);
        const std::size_t count = std::min(bytes.size(), writtenPageSize - within);
        std::memcpy(writtenPages.data() + found->second + within, bytes.data(), count);
        bytes = bytes.subspan(count);
        address += count;
    }
}

void Image::applyWrites(const std::uint64_t address, const std::span<std::byte> out) const {
    for (std::uint64_t page = address / writtenPageSize; page * writtenPageSize < address + out.size(); page++) {
        const auto found = writtenPageOffsets.find(page);
        if (found == writtenPageOffsets.end())
            continue;
        const std::uint64_t begin = std::max(address, page * writtenPageSize);
        const std::uint64_t end = std::min(address + out.size(), (page + 1) * writtenPageSize);
        std::memcpy(out.data() + (begin - address), writtenPages.data() + found->second + (begin - page * writtenPageSize), std::size_t(end - begin));
    }
}

void Image::read(const std::uint64_t address, const std::span<std::byte> out) {
    readSections(address, out);
    if (!writtenPageOffsets.empty())
        applyWrites(address, out);
}

void Image::readSections(std::uint64_t address, std::span<std::byte> out) {
    while (!out.empty()) {
        const Section* section = findSection(address);
        if (section == nullptr) {
            fail(std::format("The address {:#x} is outside the executable.", address));
            std::fill(out.begin(), out.end(), std::byte{0});
            return;
        }
        const std::uint64_t within = address - section->address;
        std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), section->virtualSize - within));
        //! The part of a section past its file data is zero-initialized when the image loads.
        if (within < section->fileSize) {
            count = static_cast<std::size_t>(std::min<std::uint64_t>(count, section->fileSize - within));
            readFile(section->fileOffset + within, out.first(count));
        } else {
            std::fill(out.begin(), out.begin() + count, std::byte{0});
        }
        out = out.subspan(count);
        address += count;
    }
}

std::vector<std::uint64_t> Image::readPointerSlots(const Section& section) {
    std::vector<std::uint64_t> slots;
    std::vector<std::uint16_t> entries;
    std::uint64_t at = relocations;
    const std::uint64_t end = relocations + relocationsSize;
    while (at + 8 <= end) {
        const std::uint32_t page = read<std::uint32_t>(at);
        const std::uint32_t blockSize = read<std::uint32_t>(at + 4);
        if (blockSize < 8 || at + blockSize > end) {
            fail(std::format("The base relocation block at {:#x} has the size {}.", at, blockSize));
            return {};
        }
        entries.resize((blockSize - 8) / 2);
        read(at + 8, std::as_writable_bytes(std::span(entries)));
        for (const std::uint16_t entry : entries) {
            if (entry >> 12 != relocationDir64)
                continue;
            const std::uint64_t slot = base + page + (entry & 0xFFF);
            if (section.contains(slot))
                slots.push_back(slot);
        }
        at += blockSize;
    }
    std::sort(slots.begin(), slots.end());
    return slots;
}

std::string Image::readUtf8(std::uint64_t address) {
    std::string text;
    std::byte chunk[stringChunk];
    while (text.size() < maxStringLength) {
        read(address, chunk);
        if (error)
            return {};
        const char* characters = reinterpret_cast<const char*>(chunk);
        const std::size_t length = strnlen(characters, stringChunk);
        text.append(characters, length);
        if (length < stringChunk)
            return text;
        address += stringChunk;
    }
    fail(std::format("The string at {:#x} has no end.", address));
    return {};
}

std::string Image::readUtf16(std::uint64_t address) {
    std::string text;
    char16_t chunk[stringChunk / 2];
    for (std::size_t units = 0; units < maxStringLength; units += std::size(chunk)) {
        read(address, std::as_writable_bytes(std::span(chunk)));
        if (error)
            return {};
        for (std::size_t i = 0; i < std::size(chunk); i++) {
            const char16_t unit = chunk[i];
            if (unit == 0)
                return text;
            const bool lead = unit >= 0xD800 && unit < 0xDC00;
            if (lead && i + 1 < std::size(chunk) && chunk[i + 1] >= 0xDC00 && chunk[i + 1] < 0xE000) {
                appendUtf8(text, 0x10000 + ((char32_t(unit) - 0xD800) << 10) + (char32_t(chunk[i + 1]) - 0xDC00));
                i++;
                continue;
            }
            appendUtf8(text, unit);
        }
        address += sizeof(chunk);
    }
    fail(std::format("The wide string at {:#x} has no end.", address));
    return {};
}

bool Image::startsWithUtf16(const std::uint64_t address, const std::u16string_view prefix) {
    if (!readOnlyData.contains(address) || !readOnlyData.contains(address + prefix.size() * 2))
        return false;
    char16_t units[16];
    if (prefix.size() > std::size(units))
        return false;
    read(address, std::as_writable_bytes(std::span(units, prefix.size())));
    return std::u16string_view(units, prefix.size()) == prefix;
}

//! A chained entry (UNW_FLAG_CHAININFO) is a fragment of a function, not the start of one.
bool Image::isFunctionStart(const std::uint64_t address) {
    if (address < base)
        return false;
    const std::uint64_t rva = address - base;
    std::uint32_t low = 0;
    std::uint32_t high = functionCount;
    //! The exception directory is sorted by start address.
    while (low < high) {
        const std::uint32_t middle = low + (high - low) / 2;
        const std::uint32_t begin = read<std::uint32_t>(functionTable + std::uint64_t(middle) * runtimeFunctionSize);
        if (begin < rva)
            low = middle + 1;
        else
            high = middle;
    }
    if (low == functionCount)
        return false;
    const std::uint64_t entry = functionTable + std::uint64_t(low) * runtimeFunctionSize;
    if (read<std::uint32_t>(entry) != rva)
        return false;
    return !(read<std::uint8_t>(base + read<std::uint32_t>(entry + 8)) >> 3 & unwindChainInfo);
}

}
