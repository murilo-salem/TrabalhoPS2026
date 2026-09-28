cmake_minimum_required(VERSION 3.16)
project(sicxe_simulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(sicxe src/sicxe.cpp)
target_include_directories(sicxe PUBLIC src)
target_compile_options(sicxe PRIVATE -Wall -Wextra -Wpedantic)

add_executable(sicxe_sim src/main.cpp)
target_link_libraries(sicxe_sim PRIVATE sicxe)
target_compile_options(sicxe_sim PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_executable(sicxe_tests tests/test_sicxe.cpp)
target_link_libraries(sicxe_tests PRIVATE sicxe)
add_test(NAME sicxe_tests COMMAND sicxe_tests)
