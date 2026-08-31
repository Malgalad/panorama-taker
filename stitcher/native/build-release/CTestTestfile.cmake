# CMake generated Testfile for 
# Source directory: /home/pogorelov/panorama-taker/stitcher/native
# Build directory: /home/pogorelov/panorama-taker/stitcher/native/build-release
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[pano_app_contract]=] "/home/pogorelov/panorama-taker/stitcher/native/build-release/pano_app_contract_test")
set_tests_properties([=[pano_app_contract]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/pogorelov/panorama-taker/stitcher/native/CMakeLists.txt;599;add_test;/home/pogorelov/panorama-taker/stitcher/native/CMakeLists.txt;0;")
add_test([=[pano_gpu_contract]=] "/home/pogorelov/panorama-taker/stitcher/native/build-release/pano_gpu_contract_test")
set_tests_properties([=[pano_gpu_contract]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/pogorelov/panorama-taker/stitcher/native/CMakeLists.txt;637;add_test;/home/pogorelov/panorama-taker/stitcher/native/CMakeLists.txt;0;")
add_test([=[pano_gpu_contract_check_failure]=] "/home/pogorelov/panorama-taker/stitcher/native/build-release/pano_gpu_contract_test" "--expect-check-failure")
set_tests_properties([=[pano_gpu_contract_check_failure]=] PROPERTIES  WILL_FAIL "TRUE" _BACKTRACE_TRIPLES "/home/pogorelov/panorama-taker/stitcher/native/CMakeLists.txt;638;add_test;/home/pogorelov/panorama-taker/stitcher/native/CMakeLists.txt;0;")
add_test([=[pano_gpu_cancellation_token]=] "/home/pogorelov/panorama-taker/stitcher/native/build-release/pano_gpu_contract_test" "--token-only")
set_tests_properties([=[pano_gpu_cancellation_token]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/pogorelov/panorama-taker/stitcher/native/CMakeLists.txt;640;add_test;/home/pogorelov/panorama-taker/stitcher/native/CMakeLists.txt;0;")
subdirs("_deps/imath-build")
subdirs("_deps/openexr-build")
