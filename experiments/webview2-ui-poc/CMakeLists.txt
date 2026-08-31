cmake_minimum_required(VERSION 3.22)

project(PanoramaWebView2UiPoc LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "The WebView2 UI POC requires Windows and MSVC")
endif()

set(WEBVIEW2_SDK_ROOT "" CACHE PATH "Extracted Microsoft.Web.WebView2 NuGet package")
if(NOT EXISTS "${WEBVIEW2_SDK_ROOT}/build/native/include/WebView2.h")
    message(FATAL_ERROR "Set WEBVIEW2_SDK_ROOT to an extracted Microsoft.Web.WebView2 package")
endif()

add_executable(
    panorama-webview2-ui-poc WIN32
    src/app.manifest
    src/main.cpp)
target_compile_features(panorama-webview2-ui-poc PRIVATE cxx_std_17)
target_compile_definitions(
    panorama-webview2-ui-poc PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00)
target_compile_options(panorama-webview2-ui-poc PRIVATE /W4 /WX /permissive- /utf-8)
target_include_directories(
    panorama-webview2-ui-poc PRIVATE
    "${WEBVIEW2_SDK_ROOT}/build/native/include")
target_link_libraries(
    panorama-webview2-ui-poc PRIVATE
    "${WEBVIEW2_SDK_ROOT}/build/native/x64/WebView2LoaderStatic.lib"
    ole32
    shell32
    shlwapi
    user32
    version)
target_link_options(panorama-webview2-ui-poc PRIVATE /Brepro)

add_custom_command(
    TARGET panorama-webview2-ui-poc POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/ui"
            "$<TARGET_FILE_DIR:panorama-webview2-ui-poc>/ui")
