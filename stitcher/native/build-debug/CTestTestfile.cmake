# CMake generated Testfile for 
# Source directory: /home/pogorelov/panorama-taker/stitcher/native
# Build directory: /home/pogorelov/panorama-taker/stitcher/native/build-debug
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[pano_gpu_contract]=] "/home/pogorelov/panorama-taker/stitcher/native/build-debug/pano_gpu_contract_test")
set_tests_properties([=[pano_gpu_contract]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/pogorelov/panorama-taker/stitcher/native/CMakeLists.txt;432;add_test;/home/pogorelov/panorama-taker/stitcher/native/CMakeLists.txt;0;")
add_test([=[pano_gpu_contract_check_failure]=] "/home/pogorelov/panorama-taker/stitcher/native/build-debug/pano_gpu_contract_test" "--expect-check-failure")
set_tests_properties([=[pano_gpu_contract_check_failure]=] PROPERTIES  WILL_FAIL "TRUE" _BACKTRACE_TRIPLES "/home/pogorelov/panorama-taker/stitcher/native/CMakeLists.txt;433;add_test;/home/pogorelov/panorama-taker/stitcher/native/CMakeLists.txt;0;")
add_test([=[pano_gpu_cancellation_token]=] "/home/pogorelov/panorama-taker/stitcher/native/build-debug/pano_gpu_contract_test" "--token-only")
set_tests_properties([=[pano_gpu_cancellation_token]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/pogorelov/panorama-taker/stitcher/native/CMakeLists.txt;435;add_test;/home/pogorelov/panorama-taker/stitcher/native/CMakeLists.txt;0;")
