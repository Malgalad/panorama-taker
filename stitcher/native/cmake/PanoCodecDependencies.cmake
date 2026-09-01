include(FetchContent)

cmake_policy(PUSH)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build dependencies statically" FORCE)
set(IMATH_INSTALL OFF CACHE BOOL "Do not install Imath" FORCE)
set(IMATH_BUILD_TESTS OFF CACHE BOOL "Do not build Imath tests" FORCE)
set(OPENEXR_BUILD_TOOLS OFF CACHE BOOL "Do not build OpenEXR tools" FORCE)
set(OPENEXR_INSTALL_TOOLS OFF CACHE BOOL "Do not install OpenEXR tools" FORCE)
set(OPENEXR_BUILD_EXAMPLES OFF CACHE BOOL "Do not build OpenEXR examples" FORCE)
set(OPENEXR_INSTALL OFF CACHE BOOL "Do not install OpenEXR" FORCE)
set(OPENEXR_INSTALL_DOCS OFF CACHE BOOL "Do not install OpenEXR documentation" FORCE)
set(OPENEXR_INSTALL_PKG_CONFIG OFF CACHE BOOL "Do not install OpenEXR pkg-config files" FORCE)
set(OPENEXR_FORCE_INTERNAL_DEFLATE ON CACHE BOOL "Use OpenEXR's pinned libdeflate" FORCE)
set(OPENEXR_FORCE_INTERNAL_IMATH ON CACHE BOOL "Use the pinned Imath target" FORCE)

set(pano_download_timestamp_option)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
    list(APPEND pano_download_timestamp_option DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
endif()

FetchContent_Declare(
    imath
    URL https://github.com/AcademySoftwareFoundation/Imath/archive/1e480d11cb98b032a2dece9b9a8730512effc7f6.tar.gz
    URL_HASH SHA256=e5847ee5f19aa6adfe5512fd05584338e7656cd84c6f7644707251c6f3fa0cdb
    ${pano_download_timestamp_option})
FetchContent_GetProperties(imath)
if(NOT imath_POPULATED)
    FetchContent_Populate(imath)
    add_subdirectory("${imath_SOURCE_DIR}" "${imath_BINARY_DIR}" EXCLUDE_FROM_ALL)
endif()

FetchContent_Declare(
    openexr
    URL https://github.com/AcademySoftwareFoundation/openexr/archive/c1194b2cb23a1bdf76fe5e756b22e8436b9a98c9.tar.gz
    URL_HASH SHA256=c4a5fb903facf83a1bffcce25a8fed931bfa4d179b3aa8d0541069f56644d7aa
    ${pano_download_timestamp_option})
FetchContent_GetProperties(openexr)
if(NOT openexr_POPULATED)
    FetchContent_Populate(openexr)
    add_subdirectory("${openexr_SOURCE_DIR}" "${openexr_BINARY_DIR}" EXCLUDE_FROM_ALL)
endif()

cmake_policy(POP)

add_library(pano_app_codecs INTERFACE)
target_link_libraries(pano_app_codecs INTERFACE OpenEXR::OpenEXRCore)
if(WIN32)
    target_link_libraries(pano_app_codecs INTERFACE windowscodecs ole32 oleaut32)
endif()
