cmake_minimum_required(VERSION 3.21)

project(PanoramaCapture LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PANORAMA_CAPTURE_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/red4ext.sdk" CACHE PATH
    "Path to the RED4ext SDK checkout")
set(PANORAMA_PROBE_STAGE 0 CACHE STRING "Camera probe stage (0-4)")

if(NOT EXISTS "${PANORAMA_CAPTURE_SDK_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "RED4ext SDK not found. Clone it into mod/deps/red4ext.sdk or set PANORAMA_CAPTURE_SDK_DIR.")
endif()

set(RED4EXT_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${PANORAMA_CAPTURE_SDK_DIR}" "${CMAKE_BINARY_DIR}/red4ext-sdk" EXCLUDE_FROM_ALL)

add_library(PanoramaCaptureProbe SHARED src/plugin.cpp)
target_link_libraries(PanoramaCaptureProbe PRIVATE RED4ext::SDK)
target_compile_definitions(PanoramaCaptureProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                            PANORAMA_PROBE_STAGE=${PANORAMA_PROBE_STAGE})

if(MSVC)
  target_compile_options(PanoramaCaptureProbe PRIVATE /W4 /WX /permissive-)
endif()

set_target_properties(PanoramaCaptureProbe PROPERTIES OUTPUT_NAME "PanoramaCaptureProbe")
