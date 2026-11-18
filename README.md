# UsmapGen

Generates a `.usmap` mappings file from a Win64 Unreal Engine executable, without running the game.

- Unreal Engine 4.18 to 5.8
- Linux, Windows and web builds
- One dependency: [Zydis](https://github.com/zyantific/zydis)

## Command line

Download `usmapgen.exe` or `usmapgen` from the latest [Build](https://github.com/Ka1serM/UsmapGen/actions/workflows/build.yml) on `main`. On Windows, drag the game executable onto `usmapgen.exe`. The `.usmap` file appears next to the game executable unless an output path is given. Paths can be relative or absolute.

```sh
usmapgen Game.exe
usmapgen C:/Games/Game.exe C:/Mappings/Game.usmap
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

Output: uncompressed usmap version 4, no package versioning.

## Build

```sh
git clone --recursive https://github.com/Ka1serM/UsmapGen.git
cd UsmapGen
cmake --workflow --preset linux-release
```

| Preset | Toolchain |
| --- | --- |
| `linux-release` | Clang, libc++, LLD |
| `windows-release` | [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) in `PATH` |
| `web-release` | Emscripten in `EMSDK` |

## Tests

`linux-release` runs the tests. They need Python 3 and download about 2 GB of free dedicated servers from Steam.

Pull requests need passing tests. Enable GitHub Actions on your fork to run them. For an intended mappings change, copy the files from the `test-summaries` artifact into `tests/Expected/`.
