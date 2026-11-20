#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <print>
#include <string>
#include <vector>

#include "UsmapGen/UsmapGen.h"

#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>
#elif !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

#ifdef __EMSCRIPTEN__
//! Emscripten emulates mmap by copying the whole file into the wasm heap, so the file is read in ranges instead.
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
#else
class FileSource final : public UsmapGen::Source {
public:
    explicit FileSource(const std::filesystem::path& path) {
#ifdef _WIN32
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;
        LARGE_INTEGER size;
        const HANDLE mapping = GetFileSizeEx(file, &size) && size.QuadPart > 0 ? CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
        CloseHandle(file);
        if (!mapping)
            return;
        const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        CloseHandle(mapping);
        if (view)
            bytes = std::span(static_cast<const std::byte*>(view), std::size_t(size.QuadPart));
#else
        const int file = ::open(path.c_str(), O_RDONLY);
        if (file < 0)
            return;
        struct stat status;
        void* view = fstat(file, &status) == 0 && status.st_size > 0 ? mmap(nullptr, std::size_t(status.st_size), PROT_READ, MAP_PRIVATE, file, 0) : MAP_FAILED;
        ::close(file);
        if (view == MAP_FAILED)
            return;
        madvise(view, std::size_t(status.st_size), MADV_WILLNEED);
        bytes = std::span(static_cast<const std::byte*>(view), std::size_t(status.st_size));
#endif
    }

    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    ~FileSource() override {
        if (bytes.empty())
            return;
#ifdef _WIN32
        UnmapViewOfFile(bytes.data());
#else
        munmap(const_cast<std::byte*>(bytes.data()), bytes.size());
#endif
    }

    bool isOpen() const { return !bytes.empty(); }

    std::uint64_t getSize() const noexcept override { return bytes.size(); }

    std::expected<void, UsmapGen::Error> read(std::uint64_t offset, std::span<std::byte> buffer) const noexcept override {
        if (offset > bytes.size() || buffer.size() > bytes.size() - offset)
            return std::unexpected(UsmapGen::Error{"The executable cannot be read."});
        std::memcpy(buffer.data(), bytes.data() + offset, buffer.size());
        return {};
    }

    std::span<const std::byte> getBytes() const noexcept override { return bytes; }

private:
    std::span<const std::byte> bytes;
};
#endif

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

std::optional<UsmapGen::Compression> parseCompression(const std::filesystem::path& name) {
    if (name == "zstd")
        return UsmapGen::Compression::Zstandard;
    if (name == "brotli")
        return UsmapGen::Compression::Brotli;
    if (name == "oodle")
        return UsmapGen::Compression::Oodle;
    if (name == "none")
        return UsmapGen::Compression::None;
    return std::nullopt;
}

std::filesystem::path findOutput(const std::filesystem::path& executable, const UsmapGen::Usmap& usmap, UsmapGen::Compression compression) {
    if (!usmap.fileName.empty())
        return executable.parent_path() / usmap.fileName;
    std::filesystem::path output = executable;
    output.replace_filename(display(executable.stem()) + std::string(UsmapGen::compressionSuffix(compression)) + ".usmap");
    return output;
}

int printUsage() {
    std::println(stderr, "usage: usmapgen <executable> [output.usmap] [--compression zstd|brotli|oodle|none]");
    std::println(stderr, "Drag an executable onto usmapgen to write the .usmap file next to it, named after its build version, such as ++Fortnite+Release-43.10-CL-60118918_zs.usmap.");
    return 2;
}

int generate(const std::vector<std::filesystem::path>& arguments) {
    std::vector<std::filesystem::path> files;
    UsmapGen::Options options;
    for (std::size_t index = 0; index < arguments.size(); index++) {
        if (arguments[index] != "--compression") {
            files.push_back(arguments[index]);
            continue;
        }
        const std::optional<UsmapGen::Compression> compression = index + 1 < arguments.size() ? parseCompression(arguments[++index]) : std::nullopt;
        if (!compression)
            return printUsage();
        options.compression = *compression;
    }
    if (files.empty() || files.size() > 2)
        return printUsage();
    const std::filesystem::path& executable = files[0];
    if (files.size() == 2 && files[1].extension() != ".usmap") {
        std::println(stderr, "{} is no .usmap file.", display(files[1]));
        return 2;
    }
    const FileSource source(executable);
    if (!source.isOpen()) {
        std::println(stderr, "{} cannot be opened.", display(executable));
        return 1;
    }
    std::println("Reading {}", display(executable));
    const std::expected<UsmapGen::Usmap, UsmapGen::Error> usmap = UsmapGen::generateUsmap(source, options);
    if (!usmap) {
        std::println(stderr, "{}", usmap.error().message);
        return 1;
    }
    for (const std::string& warning : usmap->warnings)
        std::println(stderr, "warning: {}", warning);
    const std::filesystem::path output = files.size() == 2 ? files[1] : findOutput(executable, *usmap, options.compression);
    std::ofstream file(output, std::ios::binary);
    file.write(reinterpret_cast<const char*>(usmap->bytes.data()), std::streamsize(usmap->bytes.size()));
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
