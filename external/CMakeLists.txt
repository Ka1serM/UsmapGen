if(NOT TARGET Zydis)
    set(ZYDIS_FEATURE_ENCODER OFF CACHE BOOL "" FORCE)
    set(ZYDIS_FEATURE_FORMATTER OFF CACHE BOOL "" FORCE)
    set(ZYDIS_FEATURE_KNC OFF CACHE BOOL "" FORCE)
    set(ZYAN_NO_LIBC ON CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_MAN OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZYCORE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
    add_subdirectory(zydis EXCLUDE_FROM_ALL SYSTEM)
endif()


if(USMAPGEN_ZSTANDARD)
    file(GLOB ZSTD_SOURCES CONFIGURE_DEPENDS zstd/lib/common/*.c zstd/lib/compress/*.c)
    add_library(UsmapGenZstd STATIC EXCLUDE_FROM_ALL ${ZSTD_SOURCES})
    target_include_directories(UsmapGenZstd SYSTEM PUBLIC zstd/lib)
    target_compile_definitions(UsmapGenZstd PRIVATE XXH_NAMESPACE=ZSTD_ ZSTD_DISABLE_ASM)
    set_target_properties(UsmapGenZstd PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
endif()

if(USMAPGEN_BROTLI)
    file(GLOB BROTLI_SOURCES CONFIGURE_DEPENDS brotli/c/common/*.c brotli/c/enc/*.c)
    add_library(UsmapGenBrotli STATIC EXCLUDE_FROM_ALL ${BROTLI_SOURCES})
    target_include_directories(UsmapGenBrotli SYSTEM PUBLIC brotli/c/include)
    set_target_properties(UsmapGenBrotli PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
    if(UNIX AND NOT EMSCRIPTEN)
        target_link_libraries(UsmapGenBrotli PRIVATE m)
    endif()
endif()

if(USMAPGEN_OODLE)
    add_library(UsmapGenOoz STATIC EXCLUDE_FROM_ALL
        ooz/compress.cpp
        ooz/compr_entropy.cpp
        ooz/compr_kraken.cpp
        ooz/compr_leviathan.cpp
        ooz/compr_match_finder.cpp
        ooz/compr_mermaid.cpp
        ooz/compr_multiarray.cpp
        ooz/compr_tans.cpp
        ooz/kraken_block_size.cpp)
    target_include_directories(UsmapGenOoz SYSTEM PRIVATE ooz ooz/simde)
    #! ooz reads unaligned integers through casts, so a sanitized build would trap on them.
    target_compile_options(UsmapGenOoz PRIVATE -w -fno-sanitize=all)
    #! ooz takes _rotl from <x86gprintrin.h>, which declares it only in GCC; Clang declares it in <x86intrin.h>.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT EMSCRIPTEN)
        target_compile_options(UsmapGenOoz PRIVATE -include x86intrin.h)
    endif()
    set_target_properties(UsmapGenOoz PROPERTIES CXX_STANDARD 17 POSITION_INDEPENDENT_CODE ON)
endif()
