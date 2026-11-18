#include <cstdio>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <vector>

#include "UsmapGen/UsmapGen.h"

#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>
#endif

namespace {

class FileSource final : public UsmapGen::Source {
public:
    explicit FileSource(const std::filesystem::path& path) : file(path, std::ios::binary | std::ios::ate), size(std::uint64_t(file.tellg())) {}

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

std::vector<std::filesystem::path> readArguments([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
    std::vector<std::filesystem::path> arguments;
#ifdef _WIN32
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int index = 1; index < count; index++)
        arguments.emplace_back(wide[index]);
    LocalFree(wide);
#else
    for (int index = 1; index < argc; index++)
        arguments.emplace_back(argv[index]);
#endif
    return arguments;
}

std::string display(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool ownsConsole() {
#ifdef _WIN32
    DWORD process = 0;
    return GetConsoleProcessList(&process, 1) == 1;
#else
    return false;
#endif
}

std::filesystem::path findOutput(const std::filesystem::path& executable) {
    std::filesystem::path output = executable;
    output.replace_extension(".usmap");
    return output;
}

int generate(const std::vector<std::filesystem::path>& arguments) {
    if (arguments.empty() || arguments.size() > 2) {
        std::println(stderr, "usage: usmapgen <executable> [output.usmap]");
        std::println(stderr, "Drag an executable onto usmapgen to write the .usmap file next to it.");
        return 2;
    }
    const std::filesystem::path& executable = arguments[0];
    const std::filesystem::path output = arguments.size() == 2 ? arguments[1] : findOutput(executable);
    if (output.extension() != ".usmap") {
        std::println(stderr, "{} is no .usmap file.", display(output));
        return 2;
    }
    const FileSource source(executable);
    if (!source.isOpen()) {
        std::println(stderr, "{} cannot be opened.", display(executable));
        return 1;
    }
    std::println("Reading {}", display(executable));
    const std::expected<std::vector<std::byte>, UsmapGen::Error> usmap = UsmapGen::generateUsmap(source);
    if (!usmap) {
        std::println(stderr, "{}", usmap.error().message);
        return 1;
    }
    std::ofstream file(output, std::ios::binary);
    file.write(reinterpret_cast<const char*>(usmap->data()), std::streamsize(usmap->size()));
    if (!file) {
        std::println(stderr, "{} cannot be written.", display(output));
        return 1;
    }
    std::println("Wrote {}", display(output));
    return 0;
}

}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const int result = generate(readArguments(argc, argv));
    if (ownsConsole()) {
        std::println("Press Enter to close.");
        std::getchar();
    }
    return result;
}
