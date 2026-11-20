#include "UsmapGen/UsmapGen.h"

#include <algorithm>
#include <format>
#include <string_view>

#include "UsmapGen/Image.h"
#include "UsmapGen/Reflection.h"
#include "UsmapGen/Schema.h"

namespace UsmapGen {

namespace {

//! Windows file names cannot hold these characters, which a branch name may contain.
constexpr std::string_view invalidFileNameCharacters = "<>:\"/\\|?*";

std::string toFileName(std::string name) {
    std::ranges::replace_if(name, [](char character) { return static_cast<unsigned char>(character) < 0x20 || invalidFileNameCharacters.contains(character); }, '_');
    return name;
}

}

std::expected<Usmap, Error> generateUsmap(const Source& executable, const Options& options) {
    //! An unavailable compression fails before the executable is read, which takes seconds.
    if (!hasCompression(options.compression))
        return std::unexpected(Error{std::format("UsmapGen is built without {} compression.", compressionName(options.compression))});
    Image image(executable);
    if (std::expected<void, Error> opened = image.open(); !opened)
        return std::unexpected(opened.error());
    std::expected<Schema, Error> schema = readReflection(image);
    if (!schema)
        return std::unexpected(schema.error());
    std::expected<std::vector<std::byte>, Error> bytes = writeUsmap(*schema, options);
    if (!bytes)
        return std::unexpected(bytes.error());
    std::string fileName;
    if (!schema->buildVersion.empty())
        fileName = toFileName(std::format("{}{}.usmap", schema->buildVersion, compressionSuffix(options.compression)));
    return Usmap{std::move(*bytes), std::move(fileName), std::move(schema->warnings)};
}

}
