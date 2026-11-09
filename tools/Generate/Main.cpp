#include <fstream>
#include <print>
#include <string_view>

#include "UsmapGen/UsmapGen.h"

namespace {

class FileSource final : public UsmapGen::Source {
public:
    explicit FileSource(const char* path) : file(path, std::ios::binary | std::ios::ate), size(std::uint64_t(file.tellg())) {}

    bool isOpen() const { return file.is_open(); }

    std::uint64_t getSize() const noexcept override { return size; }

    std::expected<void, UsmapGen::Error> read(std::uint64_t offset, std::span<std::byte> buffer) const noexcept override {
        file.seekg(std::streamoff(offset));
        file.read(reinterpret_cast<char*>(buffer.data()), std::streamsize(buffer.size()));
        if (!file)
            return std::unexpected(UsmapGen::Error{"The executable cannot be read."});
        return {};
    }

private:
    mutable std::ifstream file;
    std::uint64_t size;
};

}

int main(int argc, char** argv) {
    const auto usage = [] {
        std::println(stderr, "usage: usmapgen [--name-casing <unknown|non-preserving|preserving>] <executable> <output.usmap>");
        return 2;
    };
    UsmapGen::NameCasing nameCasing = UsmapGen::NameCasing::Unknown;
    if (argc == 5 && std::string_view(argv[1]) == "--name-casing") {
        const std::string_view value(argv[2]);
        if (value == "non-preserving")
            nameCasing = UsmapGen::NameCasing::NonPreserving;
        else if (value == "preserving")
            nameCasing = UsmapGen::NameCasing::Preserving;
        else if (value != "unknown")
            return usage();
        argc -= 2;
        argv += 2;
    }
    if (argc != 3 || std::string_view(argv[1]).starts_with("--"))
        return usage();
    const FileSource source(argv[1]);
    if (!source.isOpen()) {
        std::println(stderr, "{} cannot be opened.", argv[1]);
        return 1;
    }
    const std::expected<std::vector<std::byte>, UsmapGen::Error> usmap = UsmapGen::generateUsmap(source, nameCasing);
    if (!usmap) {
        std::println(stderr, "{}", usmap.error().message);
        return 1;
    }
    std::ofstream output(argv[2], std::ios::binary);
    output.write(reinterpret_cast<const char*>(usmap->data()), std::streamsize(usmap->size()));
    if (!output) {
        std::println(stderr, "{} cannot be written.", argv[2]);
        return 1;
    }
    return 0;
}
