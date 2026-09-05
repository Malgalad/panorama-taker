# Keep libdeflate isolated from the independent copy inside OpenEXRCore.
find_program(PANO_CARGO_EXECUTABLE NAMES cargo.exe cargo REQUIRED)
set(pano_oxi_source "${CMAKE_CURRENT_SOURCE_DIR}/oxipng")
set(pano_oxi_target "${CMAKE_CURRENT_BINARY_DIR}/oxipng-target")
set(pano_oxi_library "${pano_oxi_target}/release/pano_oxipng.dll")
add_custom_command(
    OUTPUT "${pano_oxi_library}"
    COMMAND ${CMAKE_COMMAND} -E env
        "CARGO_TARGET_DIR=${pano_oxi_target}"
        "RUSTFLAGS=-C target-feature=+crt-static"
        "${PANO_CARGO_EXECUTABLE}" build --release --locked
        --manifest-path "${pano_oxi_source}/Cargo.toml"
    DEPENDS "${pano_oxi_source}/Cargo.toml" "${pano_oxi_source}/Cargo.lock"
            "${pano_oxi_source}/src/lib.rs"
    VERBATIM)
add_custom_target(pano_oxipng ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:pano-stitch-native>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${pano_oxi_library}"
            "$<TARGET_FILE_DIR:pano-stitch-native>/pano_oxipng.dll"
    DEPENDS "${pano_oxi_library}"
    VERBATIM)
add_dependencies(pano_app_core pano_oxipng)
