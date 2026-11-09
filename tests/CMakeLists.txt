add_executable(usmapgen-deferred-tests Deferred.cpp)
target_link_libraries(usmapgen-deferred-tests PRIVATE UsmapGen::UsmapGen)
target_compile_options(usmapgen-deferred-tests PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME deferred-registration COMMAND usmapgen-deferred-tests)
