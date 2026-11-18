#include <cstdio>
#include <string_view>

#include "UsmapGen/Name.h"

namespace {

int checks = 0;
int failures = 0;

void check(std::string_view full, std::string_view expected) {
    checks++;
    const std::string_view actual = UsmapGen::runtimeEnumName(full);
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %.*s gives %.*s, not %.*s\n", int(full.size()), full.data(), int(actual.size()), actual.data(),
                     int(expected.size()), expected.data());
        failures++;
    }
}

}

int main() {
    check("NONE", "None");
    check("none", "None");
    check("None", "None");
    check("LINEAR", "Linear");
    check("Vector2d", "Vector2D");
    check("true", "TRUE");
    check("id", "ID");
    check("NONE_MAX", "NONE_MAX");
    check("NONE_0", "NONE_0");
    check("Non", "Non");
    check("Nonex", "Nonex");
    check("", "");
    //! Lerp and MetaData are hardcoded only in later versions.
    check("lerp", "lerp");
    check("Metadata", "Metadata");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
