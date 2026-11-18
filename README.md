# UsmapGen

UsmapGen generates a `.usmap` mappings file from a Win64 Unreal Engine executable. It reads the reflection tables that UnrealHeaderTool compiles into the executable, so the game does not have to run.

- Unreal Engine 4.18 to 5.8, Win64 PE32+ shipping executables
- Ranged reads through a `Source` interface, so the executable can stream from a network
- Builds for Linux, Windows and the web
- One dependency: [Zydis](https://github.com/zyantific/zydis)

## Command line

Download `usmapgen.exe` for Windows or `usmapgen` for Linux from the artifacts of the latest [Build](https://github.com/Ka1serM/UsmapGen/actions/workflows/build.yml) run on `main`. On Windows, drag the game executable onto `usmapgen.exe`. The `.usmap` file then appears next to the game executable.

```sh
usmapgen VALORANT-Win64-Shipping.exe
usmapgen VALORANT-Win64-Shipping.exe VALORANT.usmap
usmapgen VALORANT-Win64-Shipping.exe Mappings/
```

The second path is optional. It is the output file, or a directory for `<executable name>.usmap`. The Linux build needs glibc 2.34 or later.

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

The output is an uncompressed usmap of version 4 (ExplicitEnumValues) without package versioning.

## Build

```sh
git clone --recursive https://github.com/Ka1serM/UsmapGen.git
cd UsmapGen
cmake --workflow --preset linux-release
```

| Preset | Toolchain |
| --- | --- |
| `linux-release` | Clang, libc++ and LLD |
| `windows-release` | [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) in `PATH`, from a Windows or Linux host |
| `web-release` | Emscripten from the emsdk in `EMSDK` |

## Tests

The Linux workflows also run the tests. They need Python 3 and download the free dedicated servers in `tests/Executables.json` anonymously from Steam, about 2 GB on the first run.

A pull request can only be merged when the tests pass. To run them on a fork, enable GitHub Actions in the fork: the Build workflow then runs on every push. When a change to the mappings is intended, copy the new summaries from the `test-summaries` artifact, or from `build-linux-release/tests/`, into `tests/Expected/`.
