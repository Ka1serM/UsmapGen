add_executable(usmapgen-enum-tests Enum.cpp)
target_link_libraries(usmapgen-enum-tests PRIVATE UsmapGen::UsmapGen)
target_compile_options(usmapgen-enum-tests PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME enum-max COMMAND usmapgen-enum-tests)

add_executable(usmapgen-delegate-tests Delegate.cpp)
target_link_libraries(usmapgen-delegate-tests PRIVATE UsmapGen::UsmapGen)
target_compile_options(usmapgen-delegate-tests PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME delegate-policy COMMAND usmapgen-delegate-tests)
