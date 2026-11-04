add_executable(usmapgen Generate/Main.cpp)
target_link_libraries(usmapgen PRIVATE UsmapGen::UsmapGen)
target_compile_options(usmapgen PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
