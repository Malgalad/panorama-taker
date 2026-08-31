# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.22.1)

# We name the project and the target for the ExternalProject_Add() call
# to something that will highlight to the user what we are working on if
# something goes wrong and an error message is produced.

project(imath-populate NONE)



include(ExternalProject)
ExternalProject_Add(imath-populate
                     "UPDATE_DISCONNECTED" "False" "URL" "https://github.com/AcademySoftwareFoundation/Imath/archive/1e480d11cb98b032a2dece9b9a8730512effc7f6.tar.gz" "URL_HASH" "SHA256=e5847ee5f19aa6adfe5512fd05584338e7656cd84c6f7644707251c6f3fa0cdb"
                    SOURCE_DIR          "/home/pogorelov/panorama-taker/stitcher/native/build-release/_deps/imath-src"
                    BINARY_DIR          "/home/pogorelov/panorama-taker/stitcher/native/build-release/_deps/imath-build"
                    CONFIGURE_COMMAND   ""
                    BUILD_COMMAND       ""
                    INSTALL_COMMAND     ""
                    TEST_COMMAND        ""
                    USES_TERMINAL_DOWNLOAD  YES
                    USES_TERMINAL_UPDATE    YES
)


