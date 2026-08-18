cmake_minimum_required(VERSION 3.20)

project(PanoramaCapture LANGUAGES CXX)

# Packet 1 intentionally has no RED4ext SDK dependency or production source.
# The interface target makes the scaffold configure cleanly until Packet 4 pins
# the SDK and adds the camera feasibility probe.
add_library(PanoramaCapture INTERFACE)
target_compile_features(PanoramaCapture INTERFACE cxx_std_20)
