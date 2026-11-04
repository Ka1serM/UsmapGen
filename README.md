# UsmapGen

UsmapGen generates a `.usmap` mappings file from a Win64 Unreal Engine executable. It reads the reflection tables that UnrealHeaderTool compiles into the executable, so the game does not have to run.

- Unreal Engine 4.18 to 5.8, Win64 PE32+ shipping executables
- Ranged reads through a `Source` interface, so the executable can stream from a network
- Builds for desktop and Emscripten
- One dependency: [Zydis](https://github.com/zyantific/zydis)

## Build

```sh
git clone --recursive https://github.com/Ka1serM/UsmapGen.git
cd UsmapGen
cmake --preset linux-release
cmake --build build-linux-release
```

## Command line

```sh
usmapgen VALORANT-Win64-Shipping.exe VALORANT.usmap
```

## Library

```cmake
add_subdirectory(external/UsmapGen)
target_link_libraries(App PRIVATE UsmapGen::UsmapGen)
```

```cpp
class FileSource final : public UsmapGen::Source {
public:
    std::uint64_t getSize() const noexcept override;
    std::expected<void, UsmapGen::Error> read(std::uint64_t offset, std::span<std::byte> buffer) const noexcept override;
};

const std::expected<std::vector<std::byte>, UsmapGen::Error> usmap = UsmapGen::generateUsmap(source);
```

The output is an uncompressed usmap of version 5 without package versioning.
