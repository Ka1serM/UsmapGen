#pragma once

#include <expected>

#include "UsmapGen/Image.h"
#include "UsmapGen/Schema.h"

namespace UsmapGen {

std::expected<Schema, Error> readReflection(Image& image);

}
