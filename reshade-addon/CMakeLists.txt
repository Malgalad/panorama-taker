cmake_minimum_required(VERSION 3.21)

project(PanoramaCaptureReShade LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "PanoramaCaptureReShade must be built with a Windows 64-bit toolchain")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "PanoramaCaptureReShade requires a 64-bit Windows toolchain")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(minhook GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537)
FetchContent_MakeAvailable(minhook)
add_library(PanoramaCaptureReShade SHARED src/addon.cpp src/freeze.cpp)
target_link_libraries(PanoramaCaptureReShade PRIVATE minhook)
target_include_directories(PanoramaCaptureReShade PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/deps/reshade-api")
target_compile_definitions(PanoramaCaptureReShade PRIVATE
  WIN32_LEAN_AND_MEAN NOMINMAX
  PANORAMA_ADDON_TEST_KEY=0x79)

if(MSVC)
  target_compile_options(PanoramaCaptureReShade PRIVATE /W4 /WX /permissive-)
  target_link_options(PanoramaCaptureReShade PRIVATE /Brepro)
endif()

set_target_properties(PanoramaCaptureReShade PROPERTIES
  PREFIX ""
  SUFFIX ".addon64")
