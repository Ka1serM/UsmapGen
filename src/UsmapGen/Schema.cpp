#include "UsmapGen/Schema.h"

#include <cstring>
#include <format>
#include <span>

#ifdef USMAPGEN_BROTLI
#include <brotli/encode.h>
#endif
#ifdef USMAPGEN_ZSTANDARD
#include <zstd.h>
#endif

#ifdef USMAPGEN_OODLE
//! The compressor of ooz, which writes Oodle Kraken data that OodleLZ_Decompress reads.
struct CompressOptions;
struct LRMCascade;
int CompressBlock(int codec_id, std::uint8_t* src_in, std::uint8_t* dst_in, int src_size, int level, const CompressOptions* compressopts, std::uint8_t* src_window_base, LRMCascade* lrm);
#endif

namespace UsmapGen {

namespace {

constexpr std::uint16_t usmapMagic = 0x30C4;
constexpr std::uint8_t usmapVersionEngineVersioning = 5;
//! The network changelist (FNetworkVersion) is not read from the executable.
constexpr std::uint32_t unknownNetChangelist = 0;
//! Mappings are compressed once and read many times, so the strongest levels are used.
#ifdef USMAPGEN_ZSTANDARD
constexpr int zstdLevel = 19;
#endif
#ifdef USMAPGEN_BROTLI
constexpr int brotliQuality = BROTLI_MAX_QUALITY;
constexpr int brotliWindowBits = 24;
#endif
#ifdef USMAPGEN_OODLE
constexpr int oodleKraken = 8;
constexpr int oodleLevel = 7;
#endif

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

    //! An FString is its length with the terminator, then ANSI characters; the branch name is ASCII.
    void writeString(std::string_view text) {
        write(std::int32_t(text.size() + 1));
        writeBytes(text);
        write(char(0));
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

namespace {

std::expected<std::vector<std::byte>, Error> compress(std::span<const std::byte> data, const Options& options) {
    switch (options.compression) {
    case Compression::None:
        return std::vector<std::byte>(data.begin(), data.end());
#ifdef USMAPGEN_ZSTANDARD
    case Compression::Zstandard: {
        std::vector<std::byte> out(ZSTD_compressBound(data.size()));
        const std::size_t size = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), zstdLevel);
        if (ZSTD_isError(size))
            return std::unexpected(Error{std::format("Zstandard cannot compress the usmap: {}", ZSTD_getErrorName(size))});
        out.resize(size);
        return out;
    }
#endif
#ifdef USMAPGEN_BROTLI
    case Compression::Brotli: {
        std::size_t size = BrotliEncoderMaxCompressedSize(data.size());
        std::vector<std::byte> out(size);
        if (!BrotliEncoderCompress(brotliQuality, brotliWindowBits, BROTLI_MODE_GENERIC, data.size(), reinterpret_cast<const std::uint8_t*>(data.data()), &size, reinterpret_cast<std::uint8_t*>(out.data())))
            return std::unexpected(Error{"Brotli cannot compress the usmap."});
        out.resize(size);
        return out;
    }
#endif
#ifdef USMAPGEN_OODLE
    case Compression::Oodle: {
        //! ooz takes its input as mutable, and Kraken grows data by at most 274 bytes per 256 KiB block.
        std::vector<std::byte> input(data.begin(), data.end());
        std::vector<std::byte> out(data.size() + data.size() / 64 + 65536);
        const int size = CompressBlock(oodleKraken, reinterpret_cast<std::uint8_t*>(input.data()), reinterpret_cast<std::uint8_t*>(out.data()), int(input.size()), oodleLevel, nullptr, nullptr, nullptr);
        if (size < 0)
            return std::unexpected(Error{"Oodle cannot compress the usmap."});
        out.resize(std::size_t(size));
        return out;
    }
#endif
    default:
        break;
    }
    return std::unexpected(Error{std::format("UsmapGen is built without {} compression.", compressionName(options.compression))});
}

}

bool hasCompression(const Compression compression) {
    switch (compression) {
    case Compression::None:
        return true;
    case Compression::Oodle:
#ifdef USMAPGEN_OODLE
        return true;
#else
        return false;
#endif
    case Compression::Brotli:
#ifdef USMAPGEN_BROTLI
        return true;
#else
        return false;
#endif
    case Compression::Zstandard:
#ifdef USMAPGEN_ZSTANDARD
        return true;
#else
        return false;
#endif
    }
    return false;
}

std::string_view compressionName(const Compression compression) {
    switch (compression) {
    case Compression::Oodle:
        return "Oodle";
    case Compression::Brotli:
        return "Brotli";
    case Compression::Zstandard:
        return "Zstandard";
    case Compression::None:
        break;
    }
    return "no";
}

std::string_view compressionSuffix(const Compression compression) {
    switch (compression) {
    case Compression::Oodle:
        return "_oo";
    case Compression::Brotli:
        return "_br";
    case Compression::Zstandard:
        return "_zs";
    case Compression::None:
        break;
    }
    return "";
}

std::expected<std::vector<std::byte>, Error> writeUsmap(const Schema& schema, const Options& options) {
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
    const std::optional<Versioning>& versioning = schema.versioning;
    file.reserve(data.size() + 64 + (versioning ? versioning->engine.branch.size() + 20 * versioning->customVersions.size() : 0));
    Writer header(file);
    header.write(usmapMagic);
    header.write(usmapVersionEngineVersioning);
    header.write(std::int32_t(versioning.has_value()));
    if (versioning) {
        header.write(versioning->engine.major);
        header.write(versioning->engine.minor);
        header.write(versioning->engine.patch);
        header.write(versioning->engine.changelist);
        header.writeString(versioning->engine.branch);
        header.write(versioning->fileVersionUE4);
        header.write(versioning->fileVersionUE5);
        header.write(std::int32_t(versioning->customVersions.size()));
        for (const CustomVersion& custom : versioning->customVersions) {
            header.write(custom.key);
            header.write(custom.version);
        }
        header.write(unknownNetChangelist);
    }
    const std::expected<std::vector<std::byte>, Error> payload = compress(data, options);
    if (!payload)
        return std::unexpected(payload.error());
    header.write(std::uint8_t(options.compression));
    header.write(std::uint32_t(payload->size()));
    header.write(std::uint32_t(data.size()));
    file.insert(file.end(), payload->begin(), payload->end());
    return file;
}

}
