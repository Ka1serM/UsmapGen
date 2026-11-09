#pragma once

#include <expected>

#include "UsmapGen/Image.h"
#include "UsmapGen/Schema.h"

namespace UsmapGen {

std::string_view shortEnumName(std::string_view full, NameCasing nameCasing);
std::expected<Schema, Error> readReflection(Image& image, NameCasing nameCasing);

}
