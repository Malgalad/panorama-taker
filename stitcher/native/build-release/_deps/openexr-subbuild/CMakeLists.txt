# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.22.1)

# We name the project and the target for the ExternalProject_Add() call
# to something that will highlight to the user what we are working on if
# something goes wrong and an error message is produced.

project(openexr-populate NONE)



include(ExternalProject)
ExternalProject_Add(openexr-populate
                     "UPDATE_DISCONNECTED" "False" "URL" "https://github.com/AcademySoftwareFoundation/openexr/archive/c1194b2cb23a1bdf76fe5e756b22e8436b9a98c9.tar.gz" "URL_HASH" "SHA256=c4a5fb903facf83a1bffcce25a8fed931bfa4d179b3aa8d0541069f56644d7aa"
                    SOURCE_DIR          "/home/pogorelov/panorama-taker/stitcher/native/build-release/_deps/openexr-src"
                    BINARY_DIR          "/home/pogorelov/panorama-taker/stitcher/native/build-release/_deps/openexr-build"
                    CONFIGURE_COMMAND   ""
                    BUILD_COMMAND       ""
                    INSTALL_COMMAND     ""
                    TEST_COMMAND        ""
                    USES_TERMINAL_DOWNLOAD  YES
                    USES_TERMINAL_UPDATE    YES
)


