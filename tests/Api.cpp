#include <cstdio>

#include "UsmapGen/UsmapGen.h"

namespace {

class EmptySource final : public UsmapGen::Source {
public:
    std::uint64_t getSize() const noexcept override { return 0; }
    std::expected<void, UsmapGen::Error> read(std::uint64_t, std::span<std::byte>) const noexcept override {
        return std::unexpected(UsmapGen::Error{"Empty source"});
    }
};

int checks = 0;
int failures = 0;

void check(bool condition, const char* label) {
    checks++;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        failures++;
    }
}

}

int main() {
    //! Binding and calling the original function type also requires its out-of-line symbol at link time.
    std::expected<std::vector<std::byte>, UsmapGen::Error> (*generate)(const UsmapGen::Source&) = &UsmapGen::generateUsmap;
    const EmptySource source;
    const auto original = generate(source);
    const auto direct = UsmapGen::generateUsmap(source);
    const auto explicitUnknown = UsmapGen::generateUsmap(source, UsmapGen::NameCasing::Unknown);
    check(!original, "original typed function pointer rejects invalid input");
    check(!original && !direct && original.error().message == direct.error().message, "one-argument call remains unambiguous and matches the original API");
    check(!original && !explicitUnknown && original.error().message == explicitUnknown.error().message, "original API forwards to unknown casing");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
