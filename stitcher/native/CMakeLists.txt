cmake_minimum_required(VERSION 3.22)

project(PanoramaStitchNative LANGUAGES CXX)

include(CTest)

if(WIN32)
    find_program(FXC_EXECUTABLE NAMES fxc.exe fxc REQUIRED)
    set(PANO_GPU_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_fill_shader.h")
    set(PANO_GPU_RAY_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_ray_shader.h")
    set(PANO_GPU_UINT8_SAMPLE_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_uint8_sample_shader.h")
    set(PANO_GPU_UINT16_SAMPLE_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_uint16_sample_shader.h")
    set(PANO_GPU_FLOAT32_SAMPLE_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_float32_sample_shader.h")
    set(PANO_GPU_UINT8_CANDIDATE_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_uint8_candidate_shader.h")
    set(PANO_GPU_UINT16_CANDIDATE_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_uint16_candidate_shader.h")
    set(PANO_GPU_FLOAT32_CANDIDATE_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_float32_candidate_shader.h")
    set(PANO_GPU_HARD_SELECTION_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_hard_selection_shader.h")
    set(PANO_GPU_FEATHER_WEIGHT_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_feather_weight_shader.h")
    set(PANO_GPU_FEATHER_ACCUMULATE_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_feather_accumulate_shader.h")
    set(PANO_GPU_FEATHER_NORMALIZE_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_feather_normalize_shader.h")
    set(PANO_GPU_FEATHER_NORMALIZE_OUTPUT_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_feather_normalize_output_shader.h")
    set(PANO_GPU_GLOBAL_GAIN_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_global_gain_shader.h")
    set(PANO_GPU_EQUIRECT_LOCAL_EXPOSURE_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_equirect_local_exposure_shader.h")
    set(PANO_GPU_LOCAL_EXPOSURE_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_local_exposure_shader.h")
    set(PANO_GPU_MARK_INCOMPLETE_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_mark_incomplete_shader.h")
    set(PANO_GPU_MARK_INCOMPLETE_OUTPUT_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_mark_incomplete_output_shader.h")
    set(PANO_GPU_CONVERT_LINEAR_SRGB_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_convert_linear_srgb_shader.h")
    set(PANO_GPU_AUTO_CONTRAST_HISTOGRAM_SRGB_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_auto_contrast_histogram_srgb_shader.h")
    set(PANO_GPU_SELECT_AUTO_CONTRAST_LEVELS_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_select_auto_contrast_levels_shader.h")
    set(PANO_GPU_APPLY_AUTO_CONTRAST_SRGB_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_apply_auto_contrast_srgb_shader.h")
    set(PANO_GPU_QUANTIZE_SRGB8_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_quantize_srgb8_shader.h")
    set(PANO_GPU_TONE_MAP_REC2020_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_tone_map_rec2020_shader.h")
    set(PANO_GPU_REC2020_LINEAR_SRGB_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_rec2020_linear_srgb_shader.h")
    set(PANO_GPU_PREVIEW_BASE_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_preview_base_shader.h")
    set(PANO_GPU_PREVIEW_OVERLAY_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_preview_overlay_shader.h")
    set(PANO_GPU_EXPOSURE_PROXY_UINT8_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_proxy_uint8_shader.h")
    set(PANO_GPU_EXPOSURE_PROXY_UINT16_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_proxy_uint16_shader.h")
    set(PANO_GPU_EXPOSURE_PROXY_FLOAT32_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_proxy_float32_shader.h")
    set(PANO_GPU_EXPOSURE_PAIR_PROJECTION_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_pair_projection_shader.h")
    set(PANO_GPU_EXPOSURE_PAIR_SAMPLES_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_pair_samples_shader.h")
    set(PANO_GPU_EXPOSURE_PAIR_CLASSIFY_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_pair_classify_shader.h")
    set(PANO_GPU_EXPOSURE_PAIR_CLASSIFY_RESIDENT_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_pair_classify_resident_shader.h")
    set(PANO_GPU_EXPOSURE_PAIR_GRADIENT_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_pair_gradient_shader.h")
    set(PANO_GPU_EXPOSURE_PAIR_FILTER_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_pair_filter_shader.h")
    set(PANO_GPU_EXPOSURE_PAIR_FILTER_RESIDENT_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_pair_filter_resident_shader.h")
    set(PANO_GPU_EXPOSURE_PAIR_RATIO_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_pair_ratio_shader.h")
    set(PANO_GPU_EXPOSURE_PAIR_SORT_PREPARE_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_pair_sort_prepare_shader.h")
    set(PANO_GPU_EXPOSURE_PAIR_SORT_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_pair_sort_shader.h")
    set(PANO_GPU_EXPOSURE_PAIR_BOUNDS_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_pair_bounds_shader.h")
    set(PANO_GPU_EXPOSURE_PAIR_TRIM_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_pair_trim_shader.h")
    set(PANO_GPU_EXPOSURE_GRADIENT_SORT_PREPARE_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_gradient_sort_prepare_shader.h")
    set(PANO_GPU_EXPOSURE_GRADIENT_SORT_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_gradient_sort_shader.h")
    set(PANO_GPU_EXPOSURE_GRADIENT_BOUNDS_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_gradient_bounds_shader.h")
    set(PANO_GPU_EXPOSURE_PAIR_REDUCE_SUMMARY_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_pair_reduce_summary_shader.h")
    set(PANO_GPU_EXPOSURE_PAIR_REDUCE_DEVIATIONS_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_pair_reduce_deviations_shader.h")
    set(PANO_GPU_EXPOSURE_PAIR_REDUCE_RESULT_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pano_gpu_exposure_pair_reduce_result_shader.h")
    add_custom_command(
        OUTPUT "${PANO_GPU_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E fill /Fh "${PANO_GPU_SHADER_HEADER}"
                /Vn pano_gpu_fill_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fill.hlsl"
        DEPENDS shaders/fill.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_RAY_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E generate_rays /Fh "${PANO_GPU_RAY_SHADER_HEADER}"
                /Vn pano_gpu_ray_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/rays.hlsl"
        DEPENDS shaders/rays.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_UINT8_SAMPLE_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E sample_uint8 /Fh "${PANO_GPU_UINT8_SAMPLE_SHADER_HEADER}"
                /Vn pano_gpu_uint8_sample_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sample_uint8.hlsl"
        DEPENDS shaders/sample_uint8.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_UINT16_SAMPLE_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E sample_uint16 /Fh "${PANO_GPU_UINT16_SAMPLE_SHADER_HEADER}"
                /Vn pano_gpu_uint16_sample_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sample_uint16.hlsl"
        DEPENDS shaders/sample_uint16.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_FLOAT32_SAMPLE_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E sample_float32 /Fh "${PANO_GPU_FLOAT32_SAMPLE_SHADER_HEADER}"
                /Vn pano_gpu_float32_sample_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sample_float32.hlsl"
        DEPENDS shaders/sample_float32.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_UINT8_CANDIDATE_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E generate_uint8_candidate /Fh "${PANO_GPU_UINT8_CANDIDATE_SHADER_HEADER}"
                /Vn pano_gpu_uint8_candidate_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/candidate_uint8.hlsl"
        DEPENDS shaders/candidate_uint8.hlsl shaders/decode_transfer.hlsli
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_UINT16_CANDIDATE_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E generate_uint16_candidate /Fh "${PANO_GPU_UINT16_CANDIDATE_SHADER_HEADER}"
                /Vn pano_gpu_uint16_candidate_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/candidate_uint16.hlsl"
        DEPENDS shaders/candidate_uint16.hlsl shaders/decode_transfer.hlsli
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_FLOAT32_CANDIDATE_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E generate_float32_candidate /Fh "${PANO_GPU_FLOAT32_CANDIDATE_SHADER_HEADER}"
                /Vn pano_gpu_float32_candidate_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/candidate_float32.hlsl"
        DEPENDS shaders/candidate_float32.hlsl shaders/decode_transfer.hlsli
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_HARD_SELECTION_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E hard_select /Fh "${PANO_GPU_HARD_SELECTION_SHADER_HEADER}"
                /Vn pano_gpu_hard_selection_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hard_selection.hlsl"
        DEPENDS shaders/hard_selection.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_FEATHER_WEIGHT_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E feather_weights /Fh "${PANO_GPU_FEATHER_WEIGHT_SHADER_HEADER}"
                /Vn pano_gpu_feather_weight_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/feather_weight.hlsl"
        DEPENDS shaders/feather_weight.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_FEATHER_ACCUMULATE_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E feather_accumulate /Fh "${PANO_GPU_FEATHER_ACCUMULATE_SHADER_HEADER}"
                /Vn pano_gpu_feather_accumulate_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/feather_accumulate.hlsl"
        DEPENDS shaders/feather_accumulate.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_FEATHER_NORMALIZE_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E feather_normalize /Fh "${PANO_GPU_FEATHER_NORMALIZE_SHADER_HEADER}"
                /Vn pano_gpu_feather_normalize_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/feather_normalize.hlsl"
        DEPENDS shaders/feather_normalize.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_FEATHER_NORMALIZE_OUTPUT_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E feather_normalize_output
                /Fh "${PANO_GPU_FEATHER_NORMALIZE_OUTPUT_SHADER_HEADER}"
                /Vn pano_gpu_feather_normalize_output_shader
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/feather_normalize_output.hlsl"
        DEPENDS shaders/feather_normalize_output.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_GLOBAL_GAIN_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E apply_global_gain /Fh "${PANO_GPU_GLOBAL_GAIN_SHADER_HEADER}"
                /Vn pano_gpu_global_gain_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/apply_global_gain.hlsl"
        DEPENDS shaders/apply_global_gain.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EQUIRECT_LOCAL_EXPOSURE_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E build_equirect_local_exposure /Fh "${PANO_GPU_EQUIRECT_LOCAL_EXPOSURE_SHADER_HEADER}"
                /Vn pano_gpu_equirect_local_exposure_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/build_equirect_local_exposure.hlsl"
        DEPENDS shaders/build_equirect_local_exposure.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_LOCAL_EXPOSURE_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E apply_local_exposure /Fh "${PANO_GPU_LOCAL_EXPOSURE_SHADER_HEADER}"
                /Vn pano_gpu_local_exposure_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/apply_local_exposure.hlsl"
        DEPENDS shaders/apply_local_exposure.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_MARK_INCOMPLETE_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E mark_incomplete /Fh "${PANO_GPU_MARK_INCOMPLETE_SHADER_HEADER}"
                /Vn pano_gpu_mark_incomplete_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/mark_incomplete.hlsl"
        DEPENDS shaders/mark_incomplete.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_MARK_INCOMPLETE_OUTPUT_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E mark_incomplete_output
                /Fh "${PANO_GPU_MARK_INCOMPLETE_OUTPUT_SHADER_HEADER}"
                /Vn pano_gpu_mark_incomplete_output_shader
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/mark_incomplete_output.hlsl"
        DEPENDS shaders/mark_incomplete_output.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_CONVERT_LINEAR_SRGB_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E convert_linear_srgb
                /Fh "${PANO_GPU_CONVERT_LINEAR_SRGB_SHADER_HEADER}"
                /Vn pano_gpu_convert_linear_srgb_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/convert_linear_srgb.hlsl"
        DEPENDS shaders/convert_linear_srgb.hlsl shaders/linear_to_srgb.hlsli
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_AUTO_CONTRAST_HISTOGRAM_SRGB_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E build_auto_contrast_histogram_srgb
                /Fh "${PANO_GPU_AUTO_CONTRAST_HISTOGRAM_SRGB_SHADER_HEADER}"
                /Vn pano_gpu_auto_contrast_histogram_srgb_shader
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/auto_contrast_histogram_srgb.hlsl"
        DEPENDS shaders/auto_contrast_histogram_srgb.hlsl shaders/linear_to_srgb.hlsli
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_SELECT_AUTO_CONTRAST_LEVELS_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E select_auto_contrast_levels
                /Fh "${PANO_GPU_SELECT_AUTO_CONTRAST_LEVELS_SHADER_HEADER}"
                /Vn pano_gpu_select_auto_contrast_levels_shader
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/select_auto_contrast_levels.hlsl"
        DEPENDS shaders/select_auto_contrast_levels.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_APPLY_AUTO_CONTRAST_SRGB_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E apply_auto_contrast_srgb
                /Fh "${PANO_GPU_APPLY_AUTO_CONTRAST_SRGB_SHADER_HEADER}"
                /Vn pano_gpu_apply_auto_contrast_srgb_shader
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/apply_auto_contrast_srgb.hlsl"
        DEPENDS shaders/apply_auto_contrast_srgb.hlsl shaders/linear_to_srgb.hlsli
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_QUANTIZE_SRGB8_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E quantize_srgb8
                /Fh "${PANO_GPU_QUANTIZE_SRGB8_SHADER_HEADER}"
                /Vn pano_gpu_quantize_srgb8_shader
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/quantize_srgb8.hlsl"
        DEPENDS shaders/quantize_srgb8.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_TONE_MAP_REC2020_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E apply_tone_map_rec2020
                /Fh "${PANO_GPU_TONE_MAP_REC2020_SHADER_HEADER}"
                /Vn pano_gpu_tone_map_rec2020_shader
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/apply_tone_map_rec2020.hlsl"
        DEPENDS shaders/apply_tone_map_rec2020.hlsl shaders/tone_map_rec2020.hlsli
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_REC2020_LINEAR_SRGB_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E convert_rec2020_linear_srgb
                /Fh "${PANO_GPU_REC2020_LINEAR_SRGB_SHADER_HEADER}"
                /Vn pano_gpu_rec2020_linear_srgb_shader
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/convert_rec2020_linear_srgb.hlsl"
        DEPENDS shaders/convert_rec2020_linear_srgb.hlsl shaders/rec2020_to_linear_srgb.hlsli
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_PREVIEW_BASE_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E render_preview_base
                /Fh "${PANO_GPU_PREVIEW_BASE_SHADER_HEADER}"
                /Vn pano_gpu_preview_base_shader
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/render_preview_base.hlsl"
        DEPENDS shaders/render_preview_base.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_PREVIEW_OVERLAY_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E render_preview_overlay
                /Fh "${PANO_GPU_PREVIEW_OVERLAY_SHADER_HEADER}"
                /Vn pano_gpu_preview_overlay_shader
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/render_preview_overlay.hlsl"
        DEPENDS shaders/render_preview_overlay.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_PROXY_UINT8_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E build_uint8_exposure_proxies /Fh "${PANO_GPU_EXPOSURE_PROXY_UINT8_SHADER_HEADER}"
                /Vn pano_gpu_exposure_proxy_uint8_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_proxy_uint8.hlsl"
        DEPENDS shaders/exposure_proxy_uint8.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_PROXY_UINT16_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E build_uint16_exposure_proxies /Fh "${PANO_GPU_EXPOSURE_PROXY_UINT16_SHADER_HEADER}"
                /Vn pano_gpu_exposure_proxy_uint16_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_proxy_uint16.hlsl"
        DEPENDS shaders/exposure_proxy_uint16.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_PROXY_FLOAT32_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E build_float32_exposure_proxies /Fh "${PANO_GPU_EXPOSURE_PROXY_FLOAT32_SHADER_HEADER}"
                /Vn pano_gpu_exposure_proxy_float32_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_proxy_float32.hlsl"
        DEPENDS shaders/exposure_proxy_float32.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_PAIR_PROJECTION_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E project_exposure_pair /Fh "${PANO_GPU_EXPOSURE_PAIR_PROJECTION_SHADER_HEADER}"
                /Vn pano_gpu_exposure_pair_projection_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_pair_projection.hlsl"
        DEPENDS shaders/exposure_pair_projection.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_PAIR_SAMPLES_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E sample_exposure_pair /Fh "${PANO_GPU_EXPOSURE_PAIR_SAMPLES_SHADER_HEADER}"
                /Vn pano_gpu_exposure_pair_samples_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_pair_samples.hlsl"
        DEPENDS shaders/exposure_pair_samples.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_PAIR_CLASSIFY_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E classify_exposure_pair /Fh "${PANO_GPU_EXPOSURE_PAIR_CLASSIFY_SHADER_HEADER}"
                /Vn pano_gpu_exposure_pair_classify_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_pair_classify.hlsl"
        DEPENDS shaders/exposure_pair_classify.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_PAIR_CLASSIFY_RESIDENT_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E classify_resident_exposure_pair /Fh "${PANO_GPU_EXPOSURE_PAIR_CLASSIFY_RESIDENT_SHADER_HEADER}"
                /Vn pano_gpu_exposure_pair_classify_resident_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_pair_classify_resident.hlsl"
        DEPENDS shaders/exposure_pair_classify_resident.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_PAIR_GRADIENT_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E gradient_exposure_pair /Fh "${PANO_GPU_EXPOSURE_PAIR_GRADIENT_SHADER_HEADER}"
                /Vn pano_gpu_exposure_pair_gradient_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_pair_gradient.hlsl"
        DEPENDS shaders/exposure_pair_gradient.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_PAIR_FILTER_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E filter_exposure_pair /Fh "${PANO_GPU_EXPOSURE_PAIR_FILTER_SHADER_HEADER}"
                /Vn pano_gpu_exposure_pair_filter_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_pair_filter.hlsl"
        DEPENDS shaders/exposure_pair_filter.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_PAIR_FILTER_RESIDENT_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E filter_resident_exposure_pair /Fh "${PANO_GPU_EXPOSURE_PAIR_FILTER_RESIDENT_SHADER_HEADER}"
                /Vn pano_gpu_exposure_pair_filter_resident_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_pair_filter_resident.hlsl"
        DEPENDS shaders/exposure_pair_filter_resident.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_PAIR_RATIO_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E build_exposure_pair_ratios /Fh "${PANO_GPU_EXPOSURE_PAIR_RATIO_SHADER_HEADER}"
                /Vn pano_gpu_exposure_pair_ratio_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_pair_ratio.hlsl"
        DEPENDS shaders/exposure_pair_ratio.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_PAIR_SORT_PREPARE_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E prepare_exposure_pair_sort /Fh "${PANO_GPU_EXPOSURE_PAIR_SORT_PREPARE_SHADER_HEADER}"
                /Vn pano_gpu_exposure_pair_sort_prepare_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_pair_sort_prepare.hlsl"
        DEPENDS shaders/exposure_pair_sort_prepare.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_PAIR_SORT_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E sort_exposure_pair /Fh "${PANO_GPU_EXPOSURE_PAIR_SORT_SHADER_HEADER}"
                /Vn pano_gpu_exposure_pair_sort_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_pair_sort.hlsl"
        DEPENDS shaders/exposure_pair_sort.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_PAIR_BOUNDS_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E extract_exposure_pair_bounds /Fh "${PANO_GPU_EXPOSURE_PAIR_BOUNDS_SHADER_HEADER}"
                /Vn pano_gpu_exposure_pair_bounds_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_pair_bounds.hlsl"
        DEPENDS shaders/exposure_pair_bounds.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_PAIR_TRIM_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E trim_exposure_pair /Fh "${PANO_GPU_EXPOSURE_PAIR_TRIM_SHADER_HEADER}"
                /Vn pano_gpu_exposure_pair_trim_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_pair_trim.hlsl"
        DEPENDS shaders/exposure_pair_trim.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_GRADIENT_SORT_PREPARE_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E prepare_exposure_gradient_sort /Fh "${PANO_GPU_EXPOSURE_GRADIENT_SORT_PREPARE_SHADER_HEADER}"
                /Vn pano_gpu_exposure_gradient_sort_prepare_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_gradient_sort_prepare.hlsl"
        DEPENDS shaders/exposure_gradient_sort_prepare.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_GRADIENT_SORT_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E sort_exposure_gradients /Fh "${PANO_GPU_EXPOSURE_GRADIENT_SORT_SHADER_HEADER}"
                /Vn pano_gpu_exposure_gradient_sort_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_gradient_sort.hlsl"
        DEPENDS shaders/exposure_gradient_sort.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_GRADIENT_BOUNDS_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E extract_exposure_gradient_limits /Fh "${PANO_GPU_EXPOSURE_GRADIENT_BOUNDS_SHADER_HEADER}"
                /Vn pano_gpu_exposure_gradient_bounds_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_gradient_bounds.hlsl"
        DEPENDS shaders/exposure_gradient_bounds.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_PAIR_REDUCE_SUMMARY_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E summarize_exposure_pair /Fh "${PANO_GPU_EXPOSURE_PAIR_REDUCE_SUMMARY_SHADER_HEADER}"
                /Vn pano_gpu_exposure_pair_reduce_summary_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_pair_reduce_summary.hlsl"
        DEPENDS shaders/exposure_pair_reduce_summary.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_PAIR_REDUCE_DEVIATIONS_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E prepare_exposure_pair_deviations /Fh "${PANO_GPU_EXPOSURE_PAIR_REDUCE_DEVIATIONS_SHADER_HEADER}"
                /Vn pano_gpu_exposure_pair_reduce_deviations_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_pair_reduce_deviations.hlsl"
        DEPENDS shaders/exposure_pair_reduce_deviations.hlsl
        VERBATIM)
    add_custom_command(
        OUTPUT "${PANO_GPU_EXPOSURE_PAIR_REDUCE_RESULT_SHADER_HEADER}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_1 /E finish_exposure_pair_reduction /Fh "${PANO_GPU_EXPOSURE_PAIR_REDUCE_RESULT_SHADER_HEADER}"
                /Vn pano_gpu_exposure_pair_reduce_result_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_pair_reduce_result.hlsl"
        DEPENDS shaders/exposure_pair_reduce_result.hlsl
        VERBATIM)
    add_custom_target(pano_gpu_shaders DEPENDS "${PANO_GPU_SHADER_HEADER}" "${PANO_GPU_RAY_SHADER_HEADER}"
            "${PANO_GPU_UINT8_SAMPLE_SHADER_HEADER}" "${PANO_GPU_UINT16_SAMPLE_SHADER_HEADER}"
            "${PANO_GPU_FLOAT32_SAMPLE_SHADER_HEADER}" "${PANO_GPU_UINT8_CANDIDATE_SHADER_HEADER}"
            "${PANO_GPU_UINT16_CANDIDATE_SHADER_HEADER}" "${PANO_GPU_FLOAT32_CANDIDATE_SHADER_HEADER}"
            "${PANO_GPU_HARD_SELECTION_SHADER_HEADER}" "${PANO_GPU_FEATHER_WEIGHT_SHADER_HEADER}"
            "${PANO_GPU_FEATHER_ACCUMULATE_SHADER_HEADER}" "${PANO_GPU_FEATHER_NORMALIZE_SHADER_HEADER}"
            "${PANO_GPU_FEATHER_NORMALIZE_OUTPUT_SHADER_HEADER}"
            "${PANO_GPU_GLOBAL_GAIN_SHADER_HEADER}" "${PANO_GPU_EQUIRECT_LOCAL_EXPOSURE_SHADER_HEADER}"
            "${PANO_GPU_LOCAL_EXPOSURE_SHADER_HEADER}" "${PANO_GPU_MARK_INCOMPLETE_SHADER_HEADER}"
            "${PANO_GPU_MARK_INCOMPLETE_OUTPUT_SHADER_HEADER}"
            "${PANO_GPU_CONVERT_LINEAR_SRGB_SHADER_HEADER}"
            "${PANO_GPU_AUTO_CONTRAST_HISTOGRAM_SRGB_SHADER_HEADER}"
            "${PANO_GPU_SELECT_AUTO_CONTRAST_LEVELS_SHADER_HEADER}"
            "${PANO_GPU_APPLY_AUTO_CONTRAST_SRGB_SHADER_HEADER}"
            "${PANO_GPU_QUANTIZE_SRGB8_SHADER_HEADER}"
            "${PANO_GPU_TONE_MAP_REC2020_SHADER_HEADER}"
            "${PANO_GPU_REC2020_LINEAR_SRGB_SHADER_HEADER}"
            "${PANO_GPU_PREVIEW_BASE_SHADER_HEADER}"
            "${PANO_GPU_PREVIEW_OVERLAY_SHADER_HEADER}"
            "${PANO_GPU_EXPOSURE_PROXY_UINT8_SHADER_HEADER}" "${PANO_GPU_EXPOSURE_PROXY_UINT16_SHADER_HEADER}"
            "${PANO_GPU_EXPOSURE_PROXY_FLOAT32_SHADER_HEADER}" "${PANO_GPU_EXPOSURE_PAIR_PROJECTION_SHADER_HEADER}"
            "${PANO_GPU_EXPOSURE_PAIR_SAMPLES_SHADER_HEADER}" "${PANO_GPU_EXPOSURE_PAIR_CLASSIFY_SHADER_HEADER}"
            "${PANO_GPU_EXPOSURE_PAIR_CLASSIFY_RESIDENT_SHADER_HEADER}"
            "${PANO_GPU_EXPOSURE_PAIR_GRADIENT_SHADER_HEADER}" "${PANO_GPU_EXPOSURE_PAIR_FILTER_SHADER_HEADER}"
            "${PANO_GPU_EXPOSURE_PAIR_FILTER_RESIDENT_SHADER_HEADER}"
            "${PANO_GPU_EXPOSURE_PAIR_RATIO_SHADER_HEADER}" "${PANO_GPU_EXPOSURE_PAIR_SORT_PREPARE_SHADER_HEADER}"
            "${PANO_GPU_EXPOSURE_PAIR_SORT_SHADER_HEADER}" "${PANO_GPU_EXPOSURE_PAIR_BOUNDS_SHADER_HEADER}"
            "${PANO_GPU_EXPOSURE_PAIR_TRIM_SHADER_HEADER}"
            "${PANO_GPU_EXPOSURE_GRADIENT_SORT_PREPARE_SHADER_HEADER}"
            "${PANO_GPU_EXPOSURE_GRADIENT_SORT_SHADER_HEADER}"
            "${PANO_GPU_EXPOSURE_GRADIENT_BOUNDS_SHADER_HEADER}"
            "${PANO_GPU_EXPOSURE_PAIR_REDUCE_SUMMARY_SHADER_HEADER}"
            "${PANO_GPU_EXPOSURE_PAIR_REDUCE_DEVIATIONS_SHADER_HEADER}"
            "${PANO_GPU_EXPOSURE_PAIR_REDUCE_RESULT_SHADER_HEADER}")
endif()

add_library(pano_gpu SHARED src/pano_gpu.cpp)
if(TARGET pano_gpu_shaders)
    add_dependencies(pano_gpu pano_gpu_shaders)
    target_include_directories(pano_gpu PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endif()
target_compile_features(pano_gpu PUBLIC cxx_std_17)
target_include_directories(pano_gpu PUBLIC include)
target_compile_definitions(pano_gpu PRIVATE PANO_GPU_BUILDING_DLL)
set_target_properties(pano_gpu PROPERTIES OUTPUT_NAME "pano_gpu")

if(MSVC)
    target_compile_options(pano_gpu PRIVATE /W4 /WX /permissive-)
    target_link_options(pano_gpu PRIVATE /Brepro)
    target_link_libraries(pano_gpu PRIVATE d3d12 dxgi)
else()
    target_compile_options(pano_gpu PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

if(BUILD_TESTING)
    target_compile_definitions(pano_gpu PRIVATE PANO_GPU_TEST_HOOKS)
    add_executable(pano_gpu_contract_test tests/pano_gpu_contract_test.cpp)
    target_link_libraries(pano_gpu_contract_test PRIVATE pano_gpu)
    target_compile_features(pano_gpu_contract_test PRIVATE cxx_std_17)
    target_compile_definitions(pano_gpu_contract_test PRIVATE PANO_GPU_TEST_HOOKS)
    if(MSVC)
        target_compile_options(pano_gpu_contract_test PRIVATE /W4 /WX /permissive-)
        target_link_options(pano_gpu_contract_test PRIVATE /Brepro)
    else()
        target_compile_options(pano_gpu_contract_test PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    add_test(NAME pano_gpu_contract COMMAND pano_gpu_contract_test)
    add_test(NAME pano_gpu_contract_check_failure COMMAND pano_gpu_contract_test --expect-check-failure)
    set_tests_properties(pano_gpu_contract_check_failure PROPERTIES WILL_FAIL TRUE)
    add_test(NAME pano_gpu_cancellation_token COMMAND pano_gpu_contract_test --token-only)
    if(WIN32)
        add_test(NAME pano_gpu_warp_preflight COMMAND pano_gpu_contract_test --warp-probe-only)
    endif()
endif()
