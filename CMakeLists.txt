cmake_minimum_required(VERSION 3.16)
project(sicxe_simulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Also works when the build directory is nested or outside the repository.
file(COPY web DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")

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

add_executable(sicxe_requirements_tests tests/test_requirements.cpp)
target_link_libraries(sicxe_requirements_tests PRIVATE sicxe)
target_compile_options(sicxe_requirements_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME sicxe_requirements_tests COMMAND sicxe_requirements_tests)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME sicxe_http_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_http.py" "$<TARGET_FILE:sicxe_sim>")
endif()

install(TARGETS sicxe_sim RUNTIME DESTINATION bin)
install(DIRECTORY web examples docs DESTINATION .)
install(FILES README.md DESTINATION .)
set(CPACK_GENERATOR TGZ)
set(CPACK_PACKAGE_VERSION "1.0.0")
include(CPack)
