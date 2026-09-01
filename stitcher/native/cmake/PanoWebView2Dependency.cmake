include_guard(GLOBAL)

if(NOT WIN32)
    return()
endif()

set(PANO_WEBVIEW2_VERSION "1.0.4191.47")
set(
    PANO_WEBVIEW2_SDK_ROOT ""
    CACHE PATH
    "Extracted Microsoft.Web.WebView2 NuGet package (optional offline override)")

if(PANO_WEBVIEW2_SDK_ROOT)
    set(_pano_webview2_source "${PANO_WEBVIEW2_SDK_ROOT}")
else()
    include(FetchContent)
    cmake_policy(PUSH)
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Declare(
        pano_webview2_sdk
        URL "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${PANO_WEBVIEW2_VERSION}"
        URL_HASH "SHA256=f492bbf547d0da329553b6727435b677579b1e9f91cc9e4a1ad029366d5f23d0"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_GetProperties(pano_webview2_sdk)
    if(NOT pano_webview2_sdk_POPULATED)
        FetchContent_Populate(pano_webview2_sdk)
    endif()
    cmake_policy(POP)
    set(_pano_webview2_source "${pano_webview2_sdk_SOURCE_DIR}")
endif()

set(_pano_webview2_include "${_pano_webview2_source}/build/native/include")
set(_pano_webview2_library "${_pano_webview2_source}/build/native/x64/WebView2LoaderStatic.lib")
if(NOT EXISTS "${_pano_webview2_include}/WebView2.h" OR
   NOT EXISTS "${_pano_webview2_library}")
    message(FATAL_ERROR "PANO_WEBVIEW2_SDK_ROOT does not contain the x64 native WebView2 SDK")
endif()

add_library(pano_webview2_loader STATIC IMPORTED GLOBAL)
set_target_properties(
    pano_webview2_loader PROPERTIES
    IMPORTED_LOCATION "${_pano_webview2_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${_pano_webview2_include}")

unset(_pano_webview2_include)
unset(_pano_webview2_library)
unset(_pano_webview2_source)
