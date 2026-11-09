# UsmapGen

UsmapGen generates a `.usmap` mappings file from a Win64 Unreal Engine executable. It reads the reflection tables that UnrealHeaderTool compiles into the executable, so the game does not have to run.

- Unreal Engine 4.18 to 5.8, Win64 PE32+ shipping executables
- Ranged reads through a `Source` interface, so the executable can stream from a network
- Builds for Linux, Windows and the web
- One dependency: [Zydis](https://github.com/zyantific/zydis)

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

## Command line

```sh
usmapgen VALORANT-Win64-Shipping.exe VALORANT.usmap
```

The optional `--name-casing <unknown|non-preserving|preserving>` precedes the paths:

```sh
usmapgen --name-casing non-preserving Game.exe Game.usmap
```

The default `unknown` and explicit `preserving` retain the original output. Use
`non-preserving` only when the target's FName implementation is independently known
not to preserve case. Neither the engine version nor a Shipping filename establishes
this; UsmapGen does not detect the setting automatically.

Only in `non-preserving` mode, a **whole bare** enum literal `none` (any ASCII case)
is written as the reserved `None`, before removing enum scope. `Enum::NONE` still
writes `NONE`; `NONE_MAX`, `NONE_0`, `HP`/`Hp` and `rewards`/`Rewards` are unchanged.
This is not general FName casing reconstruction: other names depend on registration
order, and class, property and enum type names are not normalized.

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

The optional second argument is `UsmapGen::NameCasing::Unknown` (default),
`UsmapGen::NameCasing::NonPreserving` or `UsmapGen::NameCasing::Preserving`.
Existing one-argument calls retain their behavior.

The output is an uncompressed usmap of version 4 (ExplicitEnumValues) without package versioning.

## Tests

Configure with `-DUSMAPGEN_BUILD_TESTS=ON`, build, then run
`ctest --test-dir <build-directory> --output-on-failure`. The fixtures are synthetic;
no game executables or assets are needed.
