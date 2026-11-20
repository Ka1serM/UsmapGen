#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "UsmapGen/Code.h"
#include "UsmapGen/Image.h"
#include "UsmapGen/Schema.h"

namespace UsmapGen {

//! Versioning is optional in a usmap, so a build whose versions cannot be read still gets a schema; the error says why. `buildVersion` is set whenever the build version string is found, even when the rest is not. A network changelist that cannot be read is written as 0 and adds a warning.
std::expected<Versioning, std::string> readVersioning(Image& image, Code& code, std::span<const std::uint64_t> initializers, std::string& buildVersion, std::vector<std::string>& warnings);

}
