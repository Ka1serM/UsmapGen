# UsmapGen

Generates a `.usmap` mappings file from a Win64 Unreal Engine executable, without running the game.

- Unreal Engine 4.18 to 5.8
- Linux, Windows and web builds
- usmap version 5 with engine and package versioning
- Zstandard, Brotli or Oodle compression, each one a CMake option
- Dependencies, as submodules in `external`: [Zydis](https://github.com/zyantific/zydis), and the compressors of [zstd](https://github.com/facebook/zstd), [Brotli](https://github.com/google/brotli) and [ooz](https://github.com/Ka1serM/ooz), a fork of [zao/ooz](https://github.com/zao/ooz) with fixes

## Command line

[Download](https://github.com/Ka1serM/UsmapGen/releases/latest), then drag the game executable onto `usmapgen.exe`.

```sh
usmapgen Game.exe
usmapgen Game.exe --compression brotli
usmapgen C:/Games/Game.exe C:/Mappings/Game.usmap
```

Without an output path, the usmap goes next to the executable. Its name is the build version and the suffix of the compression, such as `++Fortnite+Release-43.10-CL-60118918_zs.usmap`. Without a build version, the executable gives the name. The default compression is Zstandard.

| Compression | Suffix |
| --- | --- |
| `zstd` | `_zs` |
| `brotli` | `_br` |
| `oodle` | `_oo` |
| `none` | none |

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

const std::expected<UsmapGen::Usmap, UsmapGen::Error> usmap = UsmapGen::generateUsmap(source, {.compression = UsmapGen::Compression::Zstandard});
// usmap->bytes: the file; usmap->fileName: such as ++Fortnite+Release-43.10-CL-60118918_zs.usmap, or empty without a build version; usmap->warnings: such as versioning that cannot be read
```

A `Source` can return the whole executable from `getBytes()`, such as a mapped file, so it is read in place.

Output: usmap version 5. The header has the engine version, the package file versions, the custom versions and the network changelist, all read from the executable. If the versions cannot be read, the header has none, and `usmap->warnings` gives the reason; a network changelist that cannot be read is 0, with a warning.

| CMake option | Default | Compiles |
| --- | --- | --- |
| `USMAPGEN_ZSTANDARD` | `ON` | zstd |
| `USMAPGEN_BROTLI` | `ON` | Brotli |
| `USMAPGEN_OODLE` | `ON` | ooz |

A compression that is not compiled gives an error. Turn off the ones you do not use to make your binary smaller.

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

`linux-release` runs the tests. They download 56 free dedicated servers from Steam (about 2 GB, needs Python 3) and check that each one gives complete mappings.

Pull requests need passing tests. Enable GitHub Actions on your fork to run them.
