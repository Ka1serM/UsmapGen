add_library(UsmapGen STATIC
    UsmapGen/Code.cpp
    UsmapGen/Image.cpp
    UsmapGen/Layout.cpp
    UsmapGen/Reflection.cpp
    UsmapGen/Schema.cpp
    UsmapGen/UsmapGen.cpp
    UsmapGen/Version.cpp)
add_library(UsmapGen::UsmapGen ALIAS UsmapGen)
target_include_directories(UsmapGen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(UsmapGen PRIVATE Zydis)
target_compile_features(UsmapGen PUBLIC cxx_std_23)
target_compile_options(UsmapGen PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
