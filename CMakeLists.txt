cmake_minimum_required(VERSION 3.16)
project(emugcxbox360 C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Portable emulator core: shared by the SDL host build, the tests and the Xbox 360 build.
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp)
add_library(emucore STATIC ${CORE_SOURCES} third_party/zstd/zstddeclib.c)
target_include_directories(emucore PUBLIC src third_party)
target_compile_options(emucore PRIVATE -fno-strict-aliasing $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>)

find_package(SDL2 REQUIRED)
add_executable(emugc src/platform/sdl/main_sdl.cpp)
target_link_libraries(emugc PRIVATE emucore SDL2::SDL2)

add_executable(emugc_tests tests/test_cpu.cpp)
target_link_libraries(emugc_tests PRIVATE emucore)

add_executable(emugc_disctool tests/disc_tool.cpp)
target_link_libraries(emugc_disctool PRIVATE emucore)

enable_testing()
add_test(NAME cpu COMMAND emugc_tests)

add_executable(emugc_headless tests/headless_main.cpp)
target_link_libraries(emugc_headless PRIVATE emucore)
