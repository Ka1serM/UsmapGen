#include "UsmapGen/UsmapGen.h"

#include "UsmapGen/Image.h"
#include "UsmapGen/Reflection.h"
#include "UsmapGen/Schema.h"

namespace UsmapGen {

std::expected<std::vector<std::byte>, Error> generateUsmap(const Source& executable) {
    return generateUsmap(executable, NameCasing::Unknown);
}

std::expected<std::vector<std::byte>, Error> generateUsmap(const Source& executable, NameCasing nameCasing) {
    Image image(executable);
    if (std::expected<void, Error> opened = image.open(); !opened)
        return std::unexpected(opened.error());
    std::expected<Schema, Error> schema = readReflection(image, nameCasing);
    if (!schema)
        return std::unexpected(schema.error());
    return writeUsmap(*schema);
}

}
