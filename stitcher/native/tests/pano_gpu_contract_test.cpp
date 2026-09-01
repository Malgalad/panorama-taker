#include "pano_gpu.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include <numeric>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
bool expect(const bool condition, const char *const expression, const int line)
{
    if (condition)
        return true;
    std::fprintf(stderr, "contract check failed at line %d: %s\n", line, expression);
    return false;
}
} // namespace

#define EXPECT(expression) \
    do \
    { \
        if (!expect((expression), #expression, __LINE__)) \
            return 1; \
    } while (false)

int main(const int argc, char **const argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--expect-check-failure") == 0)
        EXPECT(false);
    const bool token_only = argc == 2 && std::strcmp(argv[1], "--token-only") == 0;
    const bool warp_probe_only = argc == 2 && std::strcmp(argv[1], "--warp-probe-only") == 0;
    const bool hardware_probe_only = argc == 2 && std::strcmp(argv[1], "--hardware-probe-only") == 0;
    const bool hardware_full = argc == 2 && std::strcmp(argv[1], "--hardware-full") == 0;
    if (argc != 1 && !token_only && !warp_probe_only && !hardware_probe_only && !hardware_full)
    {
        std::fprintf(stderr, "unknown test argument\n");
        return 2;
    }
    EXPECT(pano_gpu_abi_version() == PANO_GPU_ABI_VERSION);
    EXPECT(sizeof(pano_gpu_session_create_options) == 72);
    EXPECT(offsetof(pano_gpu_session_create_options, transfer_function) == 24);
    EXPECT(offsetof(pano_gpu_session_create_options, device_luid) == 32);
    EXPECT(offsetof(pano_gpu_session_create_options, rotations) == 40);
    EXPECT(offsetof(pano_gpu_session_create_options, encoding_metadata) == 56);
    EXPECT(sizeof(pano_gpu_one_frame_composite_request) == 88);
    EXPECT(sizeof(pano_gpu_exposure_equation) == 24);
    EXPECT(sizeof(pano_gpu_exposure_pair_report) == 24);
    EXPECT(sizeof(pano_gpu_exposure_graph_diagnostics) == 24);
    EXPECT(sizeof(pano_gpu_exposure_solve_result) == 24);
    EXPECT(sizeof(pano_gpu_exposure_report) == 32);
    EXPECT(sizeof(pano_gpu_histogram_request) == 16);
    EXPECT(sizeof(pano_gpu_histogram_layout) == 32);
    EXPECT(sizeof(pano_gpu_histogram_diagnostics) == 32);
    EXPECT(sizeof(pano_gpu_auto_contrast_levels) == 24);
    EXPECT(sizeof(pano_gpu_output_download_request) == 40);
    EXPECT(sizeof(pano_gpu_output_transfer_diagnostics) == 32);
    EXPECT(sizeof(pano_gpu_preview_create_options) == 88);
    EXPECT(sizeof(pano_gpu_preview_diagnostics) == 72);
    EXPECT(sizeof(pano_gpu_preview_render_request) == 48);
    EXPECT(sizeof(pano_gpu_preview_overlay_request) == 80);
    EXPECT(sizeof(pano_gpu_preview_surface_create_options) == 24);
    EXPECT(sizeof(pano_gpu_preview_surface_diagnostics) == 48);
    EXPECT(sizeof(pano_gpu_preview_surface_present_request) == 28);
    EXPECT(sizeof(pano_gpu_preview_surface_overlay_request) == 64);
    EXPECT(sizeof(pano_gpu_exposure_pair_scratch_diagnostics) == 48);
    EXPECT(sizeof(pano_gpu_exposure_pair_reduction) == 40);

    std::array<char, 256> error {};
    EXPECT(pano_gpu_probe(error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
    EXPECT(error.front() != '\0');

    pano_gpu_probe_options options {};
    options.size = sizeof(options);
    options.abi_version = PANO_GPU_ABI_VERSION;
    pano_gpu_adapter_info adapter {};
    std::memset(&adapter, 0xa5, sizeof(adapter));
    adapter.size = sizeof(adapter);
    adapter.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_probe_adapter(&options, &adapter, error.data(), static_cast<uint32_t>(error.size())) !=
           PANO_GPU_INVALID_ARGUMENT);
#if !defined(_WIN32)
    EXPECT(adapter.vendor_id == 0);
    EXPECT(adapter.device_id == 0);
    EXPECT(adapter.local_budget_bytes == 0);
    EXPECT(adapter.local_usage_bytes == 0);
    EXPECT(adapter.name[0] == '\0');
#endif
    options.abi_version = PANO_GPU_ABI_VERSION + 1;
    EXPECT(pano_gpu_probe_adapter(&options, &adapter, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);

    pano_gpu_memory_request request {};
    request.size = sizeof(request);
    request.abi_version = PANO_GPU_ABI_VERSION;
    request.frame_count = 1;
    request.source_width = 64;
    request.source_height = 32;
    request.source_sample_bytes = 1;
    request.output_width = 64;
    request.output_height = 32;
    request.output_sample_bytes = 1;
    request.needs_sdr_conversion = 1;
    request.free_bytes = 1024ULL * 1024 * 1024;
    request.total_bytes = request.free_bytes;
    request.session_workspace_bytes = 64 * 1024;
    request.output_workspace_bytes_per_pixel = 16;
    request.output_workspace_fixed_bytes = 4096 * sizeof(uint32_t);
    request.upload_bytes = 128 * 1024;
    request.readback_bytes_per_pixel = 4;
    request.descriptor_count = 5;
    pano_gpu_memory_plan memory_plan {};
    memory_plan.size = sizeof(memory_plan);
    memory_plan.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_plan_memory(&request, &memory_plan, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(memory_plan.output_band_rows == 0);
    EXPECT(memory_plan.session_workspace_bytes == 64 * 1024);
    EXPECT(memory_plan.upload_bytes == 128 * 1024);
    EXPECT(memory_plan.output_workspace_bytes == 64 * 1024);
    EXPECT(memory_plan.readback_bytes == 64 * 1024);
    EXPECT(memory_plan.required_bytes == 384 * 1024);
    EXPECT(memory_plan.required_bytes <= memory_plan.available_bytes);
    pano_gpu_memory_request capped_request = request;
    capped_request.free_bytes = 31ULL * 1024 * 1024 * 1024;
    capped_request.total_bytes = 32ULL * 1024 * 1024 * 1024;
    capped_request.requested_budget_bytes = 512ULL * 1024 * 1024;
    EXPECT(pano_gpu_plan_memory(
               &capped_request, &memory_plan, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(memory_plan.available_bytes == capped_request.requested_budget_bytes);
    pano_gpu_memory_request low_free_request = capped_request;
    low_free_request.free_bytes = 4ULL * 1024 * 1024 * 1024;
    low_free_request.requested_budget_bytes = 8ULL * 1024 * 1024 * 1024;
    EXPECT(pano_gpu_plan_memory(
               &low_free_request, &memory_plan, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
    uint32_t pair_count = 99;
    EXPECT(pano_gpu_exposure_pair_count(
               0, &pair_count, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    EXPECT(pair_count == 0);
    EXPECT(pano_gpu_exposure_pair_count(
               1, &pair_count, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pair_count == 0);
    EXPECT(pano_gpu_exposure_pair_count(
               2, &pair_count, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pair_count == 1);
    EXPECT(pano_gpu_exposure_pair_count(
               4, &pair_count, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pair_count == 6);
    EXPECT(pano_gpu_exposure_pair_count(
               100000, &pair_count, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    pano_gpu_histogram_request histogram_request {};
    histogram_request.size = sizeof(histogram_request);
    histogram_request.abi_version = PANO_GPU_ABI_VERSION;
    histogram_request.output_width = 1;
    histogram_request.output_height = 1;
    pano_gpu_histogram_layout histogram_layout {};
    histogram_layout.size = sizeof(histogram_layout);
    histogram_layout.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_plan_auto_contrast_histogram(
               &histogram_request, &histogram_layout, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(histogram_layout.bin_count == 4096);
    EXPECT(histogram_layout.counter_bytes == sizeof(uint32_t));
    EXPECT(histogram_layout.histogram_bytes == 4096ULL * sizeof(uint32_t));
    EXPECT(histogram_layout.maximum_population == 1);
    histogram_request.output_width = 65535;
    histogram_request.output_height = 65535;
    EXPECT(pano_gpu_plan_auto_contrast_histogram(
               &histogram_request, &histogram_layout, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(histogram_layout.maximum_population == 65535ULL * 65535);
    histogram_request.output_width = std::numeric_limits<uint32_t>::max();
    histogram_request.output_height = 1;
    EXPECT(pano_gpu_plan_auto_contrast_histogram(
               &histogram_request, &histogram_layout, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(histogram_layout.maximum_population == std::numeric_limits<uint32_t>::max());
    histogram_request.output_width = 65536;
    histogram_request.output_height = 65536;
    EXPECT(pano_gpu_plan_auto_contrast_histogram(
               &histogram_request, &histogram_layout, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    EXPECT(histogram_layout.bin_count == 0);
    EXPECT(histogram_layout.histogram_bytes == 0);
    EXPECT(pair_count == 0);
    request.upload_bytes = 64 * 1024;
    EXPECT(pano_gpu_plan_memory(&request, &memory_plan, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    request.upload_bytes = 128 * 1024;
    request.output_width = 65536;
    request.output_height = 65536;
    EXPECT(pano_gpu_plan_memory(&request, &memory_plan, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_UNAVAILABLE);
    request.output_width = 1000000;
    request.output_height = 40;
    request.needs_sdr_conversion = 0;
    request.free_bytes = 1020ULL * 1024 * 1024;
    request.total_bytes = 2ULL * 1024 * 1024 * 1024;
    EXPECT(pano_gpu_plan_memory(&request, &memory_plan, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(memory_plan.output_band_rows == 32);
    EXPECT(memory_plan.required_bytes <= memory_plan.available_bytes);
    pano_gpu_memory_request natural_request {};
    natural_request.size = sizeof(natural_request);
    natural_request.abi_version = PANO_GPU_ABI_VERSION;
    natural_request.frame_count = 30;
    natural_request.source_width = 3840;
    natural_request.source_height = 2160;
    natural_request.source_sample_bytes = 2;
    natural_request.output_width = 17552;
    natural_request.output_height = 8776;
    natural_request.output_sample_bytes = 1;
    natural_request.needs_sdr_conversion = 1;
    natural_request.free_bytes = 31ULL * 1024 * 1024 * 1024;
    natural_request.total_bytes = 32ULL * 1024 * 1024 * 1024;
    natural_request.requested_budget_bytes = 4096ULL * 1024 * 1024;
    natural_request.preview_cache_bytes = 24ULL * 1024 * 1024;
    natural_request.session_workspace_bytes = 64ULL * 1024;
    natural_request.output_workspace_bytes_per_pixel = 62 + 21;
    natural_request.output_workspace_fixed_bytes =
        4096ULL * sizeof(uint32_t) + 20ULL * 64 * 1024;
    natural_request.upload_bytes = 100ULL * 1024 * 1024;
    natural_request.readback_bytes_per_pixel = 12;
    natural_request.descriptor_count = 34;
    EXPECT(pano_gpu_plan_memory(
               &natural_request, &memory_plan, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(memory_plan.available_bytes == natural_request.requested_budget_bytes);
    EXPECT(memory_plan.output_band_rows == 1024);
    EXPECT(memory_plan.required_bytes <= natural_request.requested_budget_bytes);
    pano_gpu_diagnostics diagnostics {};
    diagnostics.size = sizeof(diagnostics);
    diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_query_diagnostics(&diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(diagnostics.live_device_count == 0);
    EXPECT(diagnostics.live_queue_count == 0);
    EXPECT(diagnostics.live_fence_count == 0);
    EXPECT(diagnostics.live_session_count == 0);
    EXPECT(diagnostics.live_output_count == 0);
    diagnostics.abi_version = PANO_GPU_ABI_VERSION + 1;
    EXPECT(pano_gpu_query_diagnostics(&diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    pano_gpu_cancellation_token *token = reinterpret_cast<pano_gpu_cancellation_token *>(1);
    EXPECT(pano_gpu_cancellation_token_create(&token, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(token != nullptr);
    EXPECT(pano_gpu_cancellation_token_is_cancelled(token) == 0);
    pano_gpu_cancellation_token_cancel(token);
    EXPECT(pano_gpu_cancellation_token_is_cancelled(token) == 1);
    pano_gpu_cancellation_token_destroy(&token);
    EXPECT(token == nullptr);
    pano_gpu_cancellation_token_destroy(&token);
    EXPECT(pano_gpu_cancellation_token_create(nullptr, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    token = reinterpret_cast<pano_gpu_cancellation_token *>(1);
    pano_gpu_test_fail_next_allocation();
    EXPECT(pano_gpu_cancellation_token_create(&token, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_OUT_OF_MEMORY);
    EXPECT(token == nullptr);
    EXPECT(error.front() != '\0');
    if (token_only)
        return 0;

#if defined(_WIN32)
    options.abi_version = PANO_GPU_ABI_VERSION;
    options.allow_warp = 0;
    pano_gpu_device *failed_device = reinterpret_cast<pano_gpu_device *>(1);
    pano_gpu_test_fail_next_device_creation();
    EXPECT(pano_gpu_device_create(
               &options, &failed_device, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_UNAVAILABLE);
    EXPECT(failed_device == nullptr);
    diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_query_diagnostics(&diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(diagnostics.live_device_count == 0);
    EXPECT(diagnostics.live_queue_count == 0);
    EXPECT(diagnostics.live_fence_count == 0);
    pano_gpu_device *product_device = reinterpret_cast<pano_gpu_device *>(1);
    const pano_gpu_result product_create_result =
        pano_gpu_device_create(&options, &product_device, error.data(), static_cast<uint32_t>(error.size()));
    if (hardware_probe_only || hardware_full)
        EXPECT(product_create_result == PANO_GPU_SUCCESS);
    else
        EXPECT(product_create_result == PANO_GPU_SUCCESS || product_create_result == PANO_GPU_UNAVAILABLE);
    if (product_create_result == PANO_GPU_SUCCESS)
    {
        if (hardware_probe_only || hardware_full)
        {
            pano_gpu_device_diagnostics hardware_diagnostics {};
            hardware_diagnostics.size = sizeof(hardware_diagnostics);
            hardware_diagnostics.abi_version = PANO_GPU_ABI_VERSION;
            EXPECT(pano_gpu_device_query_diagnostics(
                       product_device, &hardware_diagnostics, error.data(),
                       static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
            std::printf(
                "adapter=%s vendor=0x%04x device=0x%04x luid=0x%016llx dedicated=%llu budget=%llu "
                "usage=%llu usable=%llu\n",
                hardware_diagnostics.adapter.name, hardware_diagnostics.adapter.vendor_id,
                hardware_diagnostics.adapter.device_id,
                static_cast<unsigned long long>(hardware_diagnostics.adapter.luid),
                static_cast<unsigned long long>(hardware_diagnostics.adapter.dedicated_bytes),
                static_cast<unsigned long long>(hardware_diagnostics.adapter.local_budget_bytes),
                static_cast<unsigned long long>(hardware_diagnostics.adapter.local_usage_bytes),
                static_cast<unsigned long long>(hardware_diagnostics.usable_local_bytes));
        }
        if (!hardware_full)
            pano_gpu_device_destroy(&product_device);
    }
    else
        EXPECT(product_device == nullptr);
    if (hardware_probe_only)
        return 0;
    pano_gpu_device *device = nullptr;
    if (hardware_full)
    {
        device = product_device;
        product_device = nullptr;
        EXPECT(pano_gpu_probe_adapter(
                   &options, &adapter, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    }
    else
    {
        options.allow_warp = 1;
        EXPECT(pano_gpu_probe_adapter(&options, &adapter, error.data(), static_cast<uint32_t>(error.size())) ==
               PANO_GPU_SUCCESS);
        device = reinterpret_cast<pano_gpu_device *>(1);
        EXPECT(pano_gpu_device_create(&options, &device, error.data(), static_cast<uint32_t>(error.size())) ==
               PANO_GPU_SUCCESS);
    }
    EXPECT(adapter.name[0] != '\0');
    EXPECT(adapter.local_budget_bytes >= adapter.local_usage_bytes);
    EXPECT(device != nullptr);
    if (warp_probe_only)
    {
        EXPECT(pano_gpu_device_dispatch_self_test(
                   device, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
        pano_gpu_device_destroy(&device);
        EXPECT(device == nullptr);
        return 0;
    }
    diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_query_diagnostics(&diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(diagnostics.live_device_count == 1);
    EXPECT(diagnostics.live_queue_count == 1);
    EXPECT(diagnostics.live_fence_count == 1);
    pano_gpu_device_diagnostics device_diagnostics {};
    device_diagnostics.size = sizeof(device_diagnostics);
    device_diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_device_query_diagnostics(
               device, &device_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(device_diagnostics.adapter.luid == adapter.luid);
    if (!hardware_full)
    {
        EXPECT(device_diagnostics.adapter.local_budget_bytes == adapter.local_budget_bytes);
        EXPECT(device_diagnostics.adapter.local_usage_bytes == adapter.local_usage_bytes);
    }
    EXPECT(device_diagnostics.adapter.local_budget_bytes >= device_diagnostics.adapter.local_usage_bytes);
    EXPECT(device_diagnostics.usable_local_bytes ==
           device_diagnostics.adapter.local_budget_bytes - device_diagnostics.adapter.local_usage_bytes);
    std::array<float, 9> rotation {};
    for (size_t index = 0; index < rotation.size(); ++index)
        rotation[index] = static_cast<float>(index) + 0.25F;
    pano_gpu_session_create_options session_options {};
    session_options.size = sizeof(session_options);
    session_options.abi_version = PANO_GPU_ABI_VERSION;
    session_options.frame_count = 1;
    session_options.source_width = 4;
    session_options.source_height = 4;
    session_options.source_sample_type = PANO_GPU_SAMPLE_UINT8;
    session_options.transfer_function = PANO_GPU_TRANSFER_SRGB;
    session_options.source_row_stride_bytes = 12;
    session_options.device_luid = adapter.luid;
    session_options.rotations = rotation.data();
    session_options.rotations_bytes = sizeof(rotation);
    EXPECT(pano_gpu_validate_session_create_options(
               device, &session_options, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    session_options.transfer_function = PANO_GPU_TRANSFER_PQ;
    EXPECT(pano_gpu_validate_session_create_options(
               device, &session_options, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    session_options.transfer_function = PANO_GPU_TRANSFER_SRGB;
    std::array<uint8_t, 48> source_data {};
    pano_gpu_source_upload source_upload {};
    source_upload.size = sizeof(source_upload);
    source_upload.abi_version = PANO_GPU_ABI_VERSION;
    source_upload.source_sample_type = PANO_GPU_SAMPLE_UINT8;
    source_upload.source_row_stride_bytes = 12;
    source_upload.data = source_data.data();
    source_upload.data_bytes = source_data.size();
    for (size_t index = 0; index < source_data.size(); ++index)
        source_data[index] = static_cast<uint8_t>(index * 3U + 1U);
    session_options.source_row_stride_bytes = 11;
    EXPECT(pano_gpu_validate_session_create_options(
               device, &session_options, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    session_options.source_row_stride_bytes = 12;
    session_options.source_sample_type = 0;
    EXPECT(pano_gpu_validate_session_create_options(
               device, &session_options, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    session_options.source_sample_type = PANO_GPU_SAMPLE_UINT8;
    session_options.transfer_function = 0;
    EXPECT(pano_gpu_validate_session_create_options(
               device, &session_options, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    session_options.transfer_function = PANO_GPU_TRANSFER_SRGB;
    session_options.device_luid += 1;
    EXPECT(pano_gpu_validate_session_create_options(
               device, &session_options, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    session_options.device_luid = adapter.luid;
    pano_gpu_session *session = reinterpret_cast<pano_gpu_session *>(1);
    EXPECT(pano_gpu_session_create(
               device, &session_options, &session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    const std::array<float, 15> linear_srgb_input {
        -1.0F, -1.0F, -1.0F, 0.0F, 0.0F, 0.0F, 0.0031308F, 0.0031308F, 0.0031308F,
        1.0F, 1.0F, 1.0F, 4.0F, 4.0F, 4.0F};
    std::array<float, 15> normalized_srgb {};
    const pano_gpu_result linear_srgb_result = pano_gpu_test_convert_linear_srgb(
        session, linear_srgb_input.data(), 5, normalized_srgb.data(), error.data(),
        static_cast<uint32_t>(error.size()));
#if defined(_WIN32)
    EXPECT(linear_srgb_result == PANO_GPU_SUCCESS);
    for (size_t index = 0; index < linear_srgb_input.size(); ++index)
    {
        const double linear = std::max(static_cast<double>(linear_srgb_input[index]), 0.0);
        const double encoded = linear <= 0.0031308
            ? linear * 12.92
            : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
        const float expected = static_cast<float>(std::clamp(encoded, 0.0, 1.0));
        EXPECT(std::fabs(normalized_srgb[index] - expected) < 2.0e-6F);
    }
#else
    EXPECT(linear_srgb_result == PANO_GPU_UNAVAILABLE);
#endif
    auto nonfinite_srgb = linear_srgb_input;
    nonfinite_srgb[0] = std::numeric_limits<float>::infinity();
    EXPECT(pano_gpu_test_convert_linear_srgb(
               session, nonfinite_srgb.data(), 5, normalized_srgb.data(), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    std::array<float, 24> histogram_colors {};
    const std::array<float, 8> histogram_values {-1.0F, 0.0F, 0.01F, 0.1F, 0.25F, 0.5F, 1.0F, 2.0F};
    for (size_t pixel = 0; pixel < histogram_values.size(); ++pixel)
        for (size_t channel = 0; channel < 3; ++channel)
            histogram_colors[3 * pixel + channel] = histogram_values[pixel];
    std::array<uint8_t, 8> histogram_coverage {};
    std::array<uint32_t, 4096> histogram {};
    const pano_gpu_result empty_histogram_result = pano_gpu_test_build_linear_srgb_histogram(
        session, histogram_colors.data(), histogram_coverage.data(),
        static_cast<uint32_t>(histogram_coverage.size()),
        histogram.data(), sizeof(histogram), error.data(), static_cast<uint32_t>(error.size()));
#if defined(_WIN32)
    EXPECT(empty_histogram_result == PANO_GPU_SUCCESS);
    EXPECT(std::all_of(histogram.begin(), histogram.end(), [](const uint32_t count) { return count == 0; }));
    histogram_coverage = {1, 0, 1, 0, 1, 0, 1, 0};
    histogram_colors[6] = std::numeric_limits<float>::quiet_NaN();
    EXPECT(pano_gpu_test_build_linear_srgb_histogram(
               session, histogram_colors.data(), histogram_coverage.data(),
               static_cast<uint32_t>(histogram_coverage.size()),
               histogram.data(), sizeof(histogram), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(std::accumulate(histogram.begin(), histogram.end(), 0U) == 3);
    histogram_colors[6] = histogram_values[2];
    histogram_coverage.fill(1);
    EXPECT(pano_gpu_test_build_linear_srgb_histogram(
               session, histogram_colors.data(), histogram_coverage.data(),
               static_cast<uint32_t>(histogram_coverage.size()),
               histogram.data(), sizeof(histogram), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<uint32_t, 4096> expected_histogram {};
    for (const float value : histogram_values)
    {
        const float nonnegative = std::max(value, 0.0F);
        const float encoded = nonnegative <= 0.0031308F
            ? nonnegative * 12.92F
            : 1.055F * std::pow(nonnegative, 1.0F / 2.4F) - 0.055F;
        const uint32_t bin = std::min(
            4095U, static_cast<uint32_t>(std::floor(std::clamp(encoded, 0.0F, 1.0F) * 4096.0F)));
        ++expected_histogram[bin];
    }
    EXPECT(histogram == expected_histogram);
#else
    EXPECT(empty_histogram_result == PANO_GPU_UNAVAILABLE);
#endif
#if defined(_WIN32)
    std::array<uint8_t, 96> retained_preview_pixels {};
    std::array<uint8_t, 24> retained_overview_pixels {};
    std::array<uint8_t, 8> retained_compact_masks {};
    std::iota(retained_preview_pixels.begin(), retained_preview_pixels.end(), uint8_t {1});
    std::iota(retained_overview_pixels.begin(), retained_overview_pixels.end(), uint8_t {7});
    retained_compact_masks = {1, 1, 0, 0, 1, 0, 0, 0};
    pano_gpu_preview_create_options preview_options {};
    preview_options.size = sizeof(preview_options);
    preview_options.abi_version = PANO_GPU_ABI_VERSION;
    preview_options.frame_count = 1;
    preview_options.preview_width = 8;
    preview_options.preview_height = 4;
    preview_options.overview_width = 4;
    preview_options.overview_height = 2;
    preview_options.mask_width = 4;
    preview_options.mask_height = 2;
    preview_options.preview_rgb8 = retained_preview_pixels.data();
    preview_options.preview_rgb8_bytes = retained_preview_pixels.size();
    preview_options.overview_rgb8 = retained_overview_pixels.data();
    preview_options.overview_rgb8_bytes = retained_overview_pixels.size();
    preview_options.compact_masks = retained_compact_masks.data();
    preview_options.compact_mask_bytes = retained_compact_masks.size();
    pano_gpu_preview *retained_preview = reinterpret_cast<pano_gpu_preview *>(1);
    --preview_options.compact_mask_bytes;
    EXPECT(pano_gpu_preview_create(
               session, &preview_options, &retained_preview, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    EXPECT(retained_preview == nullptr);
    ++preview_options.compact_mask_bytes;
    pano_gpu_test_fail_next_preview_allocation();
    EXPECT(pano_gpu_preview_create(
               session, &preview_options, &retained_preview, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_OUT_OF_MEMORY);
    EXPECT(retained_preview == nullptr);
    EXPECT(pano_gpu_test_live_preview_count() == 0);
    EXPECT(pano_gpu_preview_create(
               session, &preview_options, &retained_preview, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_preview_diagnostics preview_diagnostics {};
    preview_diagnostics.size = sizeof(preview_diagnostics);
    preview_diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_preview_query_diagnostics(
               retained_preview, &preview_diagnostics, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(preview_diagnostics.frame_count == 1);
    EXPECT(preview_diagnostics.preview_width == 8);
    EXPECT(preview_diagnostics.preview_height == 4);
    EXPECT(preview_diagnostics.overview_width == 4);
    EXPECT(preview_diagnostics.overview_height == 2);
    EXPECT(preview_diagnostics.mask_width == 4);
    EXPECT(preview_diagnostics.mask_height == 2);
    EXPECT(preview_diagnostics.live_preview_count == 1);
    EXPECT(preview_diagnostics.retained_bytes == 128);
    std::array<uint8_t, 96> read_preview {};
    std::array<uint8_t, 24> read_overview {};
    std::array<uint8_t, 8> read_masks {};
    EXPECT(pano_gpu_test_read_preview_retained(
               retained_preview, read_preview.data(), read_preview.size(), read_overview.data(),
               read_overview.size(), read_masks.data(), read_masks.size(), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(read_preview == retained_preview_pixels);
    EXPECT(read_overview == retained_overview_pixels);
    EXPECT(read_masks == retained_compact_masks);
    HWND preview_window = CreateWindowExW(
        0, L"STATIC", L"", WS_POPUP, 0, 0, 4, 2, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    EXPECT(preview_window != nullptr);
    pano_gpu_preview_surface_create_options surface_options {};
    surface_options.size = sizeof(surface_options);
    surface_options.abi_version = PANO_GPU_ABI_VERSION;
    surface_options.native_window = static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(preview_window));
    surface_options.width = 4;
    surface_options.height = 2;
    pano_gpu_preview_surface *surface = nullptr;
    EXPECT(pano_gpu_preview_surface_create(
               device, &surface_options, &surface, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_preview_surface_sdr_color_space_set_count(surface) == 1);
    EXPECT(pano_gpu_preview_surface_resize(
               surface, 5, 3, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_preview_surface_sdr_color_space_set_count(surface) == 2);
    EXPECT(pano_gpu_preview_surface_resize(
               surface, 4, 2, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_preview_surface_sdr_color_space_set_count(surface) == 3);
    pano_gpu_preview_surface_present_request surface_request {};
    surface_request.size = sizeof(surface_request);
    surface_request.abi_version = PANO_GPU_ABI_VERSION;
    surface_request.use_overview = 1;
    EXPECT(pano_gpu_preview_surface_present_base(
               surface, retained_preview, &surface_request, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<uint8_t, 32> surface_pixels {};
    EXPECT(pano_gpu_test_read_preview_surface(
               surface, surface_pixels.data(), surface_pixels.size(),
               error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<uint8_t, 32> expected_surface {};
    for (size_t pixel = 0; pixel < 8; ++pixel)
    {
        std::copy_n(retained_overview_pixels.begin() + pixel * 3, 3,
                    expected_surface.begin() + pixel * 4);
        expected_surface[pixel * 4 + 3] = 255;
    }
    EXPECT(surface_pixels == expected_surface);
    surface_request.use_overview = 0;
    surface_request.crop_left = 2;
    surface_request.crop_top = 1;
    surface_request.crop_width = 4;
    surface_request.crop_height = 2;
    EXPECT(pano_gpu_preview_surface_present_base(
               surface, retained_preview, &surface_request, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_read_preview_surface(
               surface, surface_pixels.data(), surface_pixels.size(),
               error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    for (uint32_t y = 0; y < 2; ++y)
        for (uint32_t x = 0; x < 4; ++x)
        {
            const size_t source = ((1U + y) * 8U + 2U + x) * 3U;
            const size_t destination = (y * 4U + x) * 4U;
            std::copy_n(retained_preview_pixels.begin() + source, 3,
                        expected_surface.begin() + destination);
            expected_surface[destination + 3] = 255;
        }
    EXPECT(surface_pixels == expected_surface);
    std::array<uint8_t, 1> surface_hovered {1};
    std::array<uint8_t, 24> expected_overlay_rgb {};
    pano_gpu_preview_overlay_request expected_overlay {};
    expected_overlay.size = sizeof(expected_overlay);
    expected_overlay.abi_version = PANO_GPU_ABI_VERSION;
    expected_overlay.use_overview = 1;
    expected_overlay.hovered_frames = surface_hovered.data();
    expected_overlay.hovered_frame_bytes = surface_hovered.size();
    expected_overlay.target_pose = 0;
    expected_overlay.target_mode = 1;
    expected_overlay.show_boundaries = 1;
    expected_overlay.output_rgb8 = expected_overlay_rgb.data();
    expected_overlay.output_rgb8_bytes = expected_overlay_rgb.size();
    EXPECT(pano_gpu_preview_render_overlay(
               retained_preview, &expected_overlay, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_preview_surface_overlay_request surface_overlay {};
    surface_overlay.size = sizeof(surface_overlay);
    surface_overlay.abi_version = PANO_GPU_ABI_VERSION;
    surface_overlay.use_overview = 1;
    surface_overlay.hovered_frames = surface_hovered.data();
    surface_overlay.hovered_frame_bytes = surface_hovered.size();
    surface_overlay.target_pose = 0;
    surface_overlay.target_mode = 1;
    surface_overlay.show_boundaries = 1;
    EXPECT(pano_gpu_preview_surface_present_overlay(
               surface, retained_preview, &surface_overlay, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_read_preview_surface(
               surface, surface_pixels.data(), surface_pixels.size(),
               error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < 8; ++pixel)
    {
        EXPECT(std::equal(expected_overlay_rgb.begin() + pixel * 3,
                          expected_overlay_rgb.begin() + pixel * 3 + 3,
                          surface_pixels.begin() + pixel * 4));
        EXPECT(surface_pixels[pixel * 4 + 3] == 255);
    }
    if (hardware_full)
    {
        std::vector<double> surface_latencies_ms;
        surface_latencies_ms.reserve(31);
        for (unsigned iteration = 0; iteration < 31; ++iteration)
        {
            surface_hovered[0] = static_cast<uint8_t>(iteration & 1U);
            const auto started = std::chrono::steady_clock::now();
            EXPECT(pano_gpu_preview_surface_present_overlay(
                       surface, retained_preview, &surface_overlay,
                       error.data(), static_cast<uint32_t>(error.size())) ==
                   PANO_GPU_SUCCESS);
            surface_latencies_ms.push_back(
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started)
                    .count());
        }
        std::sort(surface_latencies_ms.begin(), surface_latencies_ms.end());
        const double p95 = surface_latencies_ms[29];
        std::fprintf(stderr,
                     "physical swap-chain preview latency: median=%.3fms "
                     "p95=%.3fms\n",
                     surface_latencies_ms[15], p95);
        EXPECT(p95 < 1000.0 / 60.0);
    }
    pano_gpu_test_fail_next_preview_surface_device_removed();
    EXPECT(pano_gpu_preview_surface_present_base(
               surface, retained_preview, &surface_request, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
    pano_gpu_preview_surface_diagnostics lost_diagnostics {};
    lost_diagnostics.size = sizeof(lost_diagnostics);
    lost_diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_preview_surface_query_diagnostics(
               surface, &lost_diagnostics, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(lost_diagnostics.device_lost == 1U);
    pano_gpu_preview_surface_destroy(&surface);
    pano_gpu_preview_surface_destroy(&surface);
    EXPECT(surface == nullptr);
    EXPECT(pano_gpu_preview_surface_create(
               device, &surface_options, &surface, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_preview_surface_present_base(
               surface, retained_preview, &surface_request, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    lost_diagnostics.device_lost = 1U;
    EXPECT(pano_gpu_preview_surface_query_diagnostics(
               surface, &lost_diagnostics, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(lost_diagnostics.device_lost == 0U &&
           lost_diagnostics.live_surface_count == 1U);
    pano_gpu_preview_surface_destroy(&surface);
    EXPECT(DestroyWindow(preview_window) != FALSE);
    std::array<uint8_t, 24> viewport_pixels {};
    pano_gpu_preview_render_request preview_render {};
    preview_render.size = sizeof(preview_render);
    preview_render.abi_version = PANO_GPU_ABI_VERSION;
    preview_render.use_overview = 1;
    preview_render.output_rgb8 = viewport_pixels.data();
    preview_render.output_rgb8_bytes = viewport_pixels.size();
    EXPECT(pano_gpu_preview_render_base(
               retained_preview, &preview_render, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(viewport_pixels == retained_overview_pixels);
    const auto expected_crop = [&](const uint32_t left, const uint32_t top) {
        std::array<uint8_t, 24> expected {};
        for (uint32_t y = 0; y < 2; ++y)
            std::copy_n(
                retained_preview_pixels.begin() + ((top + y) * 8 + left) * 3, 12,
                expected.begin() + y * 12);
        return expected;
    };
    preview_render.use_overview = 0;
    preview_render.crop_width = 4;
    preview_render.crop_height = 2;
    preview_render.crop_left = 2;
    preview_render.crop_top = 1;
    EXPECT(pano_gpu_preview_render_base(
               retained_preview, &preview_render, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(viewport_pixels == expected_crop(2, 1));
    preview_render.crop_left = 4;
    preview_render.crop_top = 2;
    EXPECT(pano_gpu_preview_render_base(
               retained_preview, &preview_render, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(viewport_pixels == expected_crop(4, 2));
    preview_render.crop_left = 5;
    viewport_pixels.fill(0xA5);
    EXPECT(pano_gpu_preview_render_base(
               retained_preview, &preview_render, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    EXPECT(std::all_of(
        viewport_pixels.begin(), viewport_pixels.end(), [](const uint8_t value) { return value == 0xA5; }));
    std::array<uint8_t, 1> hovered_frames {0};
    pano_gpu_preview_overlay_request overlay_request {};
    overlay_request.size = sizeof(overlay_request);
    overlay_request.abi_version = PANO_GPU_ABI_VERSION;
    overlay_request.use_overview = 1;
    overlay_request.hovered_frames = hovered_frames.data();
    overlay_request.hovered_frame_bytes = hovered_frames.size();
    overlay_request.target_pose = -1;
    overlay_request.output_rgb8 = viewport_pixels.data();
    overlay_request.output_rgb8_bytes = viewport_pixels.size();
    EXPECT(pano_gpu_preview_render_overlay(
               retained_preview, &overlay_request, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(viewport_pixels == retained_overview_pixels);
    hovered_frames[0] = 1;
    EXPECT(pano_gpu_preview_render_overlay(
               retained_preview, &overlay_request, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(viewport_pixels[0] == 255);
    EXPECT(viewport_pixels[1] == 0);
    EXPECT(viewport_pixels[2] == 255);
    EXPECT(std::equal(
        viewport_pixels.begin() + 9, viewport_pixels.begin() + 12,
        retained_overview_pixels.begin() + 9));
    overlay_request.target_pose = 0;
    overlay_request.target_mode = 1;
    EXPECT(pano_gpu_preview_render_overlay(
               retained_preview, &overlay_request, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(viewport_pixels[0] == 0);
    EXPECT(viewport_pixels[1] == 102);
    EXPECT(viewport_pixels[2] == 255);
    std::array<uint8_t, 8> solid_masks {};
    solid_masks.fill(1);
    preview_options.compact_masks = solid_masks.data();
    pano_gpu_preview *solid_preview = nullptr;
    EXPECT(pano_gpu_preview_create(
               session, &preview_options, &solid_preview, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    overlay_request.target_mode = 0;
    overlay_request.show_boundaries = 0;
    EXPECT(pano_gpu_preview_render_overlay(
               solid_preview, &overlay_request, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(viewport_pixels[0] < 16);
    EXPECT(viewport_pixels[1] > 20);
    EXPECT(viewport_pixels[2] > 50);
    pano_gpu_preview_destroy(&solid_preview);
    preview_options.compact_masks = retained_compact_masks.data();
    hovered_frames[0] = 0;
    overlay_request.show_boundaries = 1;
    EXPECT(pano_gpu_preview_render_overlay(
               retained_preview, &overlay_request, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(viewport_pixels[0] == 0);
    EXPECT(viewport_pixels[1] == 102);
    EXPECT(viewport_pixels[2] == 255);
    EXPECT(pano_gpu_preview_set_generation(
               retained_preview, 1, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    viewport_pixels.fill(0xA5);
    pano_gpu_test_stale_after_next_preview_wait();
    EXPECT(pano_gpu_preview_render_overlay_generation(
               retained_preview, &overlay_request, 1, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_CANCELLED);
    EXPECT(std::all_of(
        viewport_pixels.begin(), viewport_pixels.end(), [](const uint8_t value) { return value == 0xA5; }));
    EXPECT(pano_gpu_preview_render_overlay_generation(
               retained_preview, &overlay_request, 1, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_CANCELLED);
    EXPECT(pano_gpu_preview_render_overlay_generation(
               retained_preview, &overlay_request, 2, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_preview_set_generation(
               retained_preview, 1, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    pano_gpu_cancellation_token *preview_token = nullptr;
    EXPECT(pano_gpu_cancellation_token_create(
               &preview_token, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    pano_gpu_cancellation_token_cancel(preview_token);
    EXPECT(pano_gpu_preview_set_generation(
               retained_preview, 3, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    viewport_pixels.fill(0xA5);
    EXPECT(pano_gpu_preview_render_overlay_generation(
               retained_preview, &overlay_request, 3, preview_token, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_CANCELLED);
    EXPECT(std::all_of(
        viewport_pixels.begin(), viewport_pixels.end(), [](const uint8_t value) { return value == 0xA5; }));
    pano_gpu_cancellation_token_destroy(&preview_token);
    EXPECT(pano_gpu_test_claim_preview_rendering(retained_preview) == 1);
    viewport_pixels.fill(0xA5);
    EXPECT(pano_gpu_preview_render_overlay_generation(
               retained_preview, &overlay_request, 3, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    EXPECT(std::string(error.data()).find("concurrent preview rendering is rejected") !=
           std::string::npos);
    EXPECT(std::all_of(
        viewport_pixels.begin(), viewport_pixels.end(), [](const uint8_t value) { return value == 0xA5; }));
    pano_gpu_test_release_preview_rendering(retained_preview);
    for (uint64_t generation = 4; generation < 9; ++generation)
    {
        EXPECT(pano_gpu_preview_set_generation(
                   retained_preview, generation, error.data(),
                   static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
        EXPECT(pano_gpu_preview_render_overlay_generation(
                   retained_preview, &overlay_request, generation, nullptr, error.data(),
                   static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    }
    if (hardware_full)
    {
        std::vector<double> preview_latencies_ms;
        preview_latencies_ms.reserve(31);
        for (uint64_t generation = 9; generation < 40; ++generation)
        {
            EXPECT(pano_gpu_preview_set_generation(
                       retained_preview, generation, error.data(),
                       static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
            const auto started = std::chrono::steady_clock::now();
            EXPECT(pano_gpu_preview_render_overlay_generation(
                       retained_preview, &overlay_request, generation, nullptr, error.data(),
                       static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
            const auto finished = std::chrono::steady_clock::now();
            preview_latencies_ms.push_back(
                std::chrono::duration<double, std::milli>(finished - started).count());
        }
        std::sort(preview_latencies_ms.begin(), preview_latencies_ms.end());
        std::fprintf(
            stderr, "physical preview latency: median=%.3fms p95=%.3fms\n",
            preview_latencies_ms[preview_latencies_ms.size() / 2],
            preview_latencies_ms[static_cast<size_t>(
                std::ceil(preview_latencies_ms.size() * 0.95)) - 1]);
    }
    pano_gpu_preview_destroy(&retained_preview);
    pano_gpu_preview_destroy(&retained_preview);
    EXPECT(retained_preview == nullptr);
    EXPECT(pano_gpu_test_live_preview_count() == 0);
    const auto histogram_bin = [](const float value) {
        const float nonnegative = std::max(value, 0.0F);
        const float encoded = nonnegative <= 0.0031308F
            ? nonnegative * 12.92F
            : 1.055F * std::pow(nonnegative, 1.0F / 2.4F) - 0.055F;
        return std::min(
            4095U, static_cast<uint32_t>(std::floor(std::clamp(encoded, 0.0F, 1.0F) * 4096.0F)));
    };
    pano_gpu_output_create_options histogram_output_options {};
    histogram_output_options.size = sizeof(histogram_output_options);
    histogram_output_options.abi_version = PANO_GPU_ABI_VERSION;
    histogram_output_options.output_width = 4;
    histogram_output_options.output_height = 4;
    histogram_output_options.output_sample_bytes = 1;
    histogram_output_options.descriptor_count = 5;
    histogram_output_options.output_workspace_bytes = 4096 * sizeof(uint32_t);
    pano_gpu_output *histogram_output = nullptr;
    EXPECT(pano_gpu_output_create_empty(
               session, &histogram_output_options, &histogram_output, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_linear(
               histogram_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_coverage(
               histogram_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_prepare_auto_contrast_histogram(
               histogram_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_prepare_auto_contrast_histogram(
               histogram_output, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    std::array<float, 48> resident_histogram_color {};
    for (size_t index = 0; index < resident_histogram_color.size(); ++index)
        resident_histogram_color[index] = 0.1F;
    std::array<uint8_t, 16> resident_histogram_coverage {};
    resident_histogram_coverage.fill(1);
    EXPECT(pano_gpu_test_upload_output_histogram_band(
               histogram_output, resident_histogram_color.data(), sizeof(resident_histogram_color),
               resident_histogram_coverage.data(), sizeof(resident_histogram_coverage), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_apply_auto_contrast_srgb(
               histogram_output, 0, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_accumulate_auto_contrast_histogram_srgb(
               histogram_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_accumulate_auto_contrast_histogram_srgb(
               histogram_output, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    std::array<uint32_t, 4096> retained_histogram {};
    EXPECT(pano_gpu_test_read_output_histogram(
               histogram_output, retained_histogram.data(), sizeof(retained_histogram), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(retained_histogram[histogram_bin(0.1F)] == 16);
    pano_gpu_auto_contrast_levels selected_levels {};
    selected_levels.size = sizeof(selected_levels);
    selected_levels.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_output_select_auto_contrast_levels(
               histogram_output, &selected_levels, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(selected_levels.black == 0.0F);
    EXPECT(selected_levels.white == 0.0F);
    EXPECT(selected_levels.processed_pixels == 16);
    EXPECT(pano_gpu_output_select_auto_contrast_levels(
               histogram_output, &selected_levels, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    pano_gpu_histogram_diagnostics histogram_diagnostics {};
    histogram_diagnostics.size = sizeof(histogram_diagnostics);
    histogram_diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_output_query_histogram_diagnostics(
               histogram_output, &histogram_diagnostics, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(histogram_diagnostics.clear_count == 1);
    EXPECT(histogram_diagnostics.accumulated_band_count == 1);
    EXPECT(histogram_diagnostics.accumulated_pixels == 16);
    EXPECT(pano_gpu_output_apply_auto_contrast_srgb(
               histogram_output, 0, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_quantize_normalized_srgb8(
               histogram_output, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<uint8_t, 48> resident_srgb8 {};
    EXPECT(pano_gpu_test_read_output_srgb8(
               histogram_output, resident_srgb8.data(), sizeof(resident_srgb8), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const float encoded_tenth =
        1.055F * std::pow(0.1F, 1.0F / 2.4F) - 0.055F;
    const uint8_t expected_tenth =
        static_cast<uint8_t>(std::nearbyint(encoded_tenth * 255.0F));
    for (const uint8_t value : resident_srgb8)
        EXPECT(value == expected_tenth);
    EXPECT(pano_gpu_output_copy_linear_float(
               histogram_output, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 48> resident_float_output {};
    EXPECT(pano_gpu_test_read_output_float(
               histogram_output, resident_float_output.data(), sizeof(resident_float_output),
               error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(std::memcmp(
               resident_float_output.data(), resident_histogram_color.data(),
               sizeof(resident_float_output)) == 0);
    pano_gpu_output_destroy(&histogram_output);
    histogram_output_options.output_width = 2;
    histogram_output_options.output_height = 64;
    histogram_output_options.output_band_rows = 32;
    EXPECT(pano_gpu_output_create_empty(
               session, &histogram_output_options, &histogram_output, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_linear(
               histogram_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_coverage(
               histogram_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_prepare_auto_contrast_histogram(
               histogram_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<float, 192> band_histogram_color {};
    std::array<uint8_t, 64> band_histogram_coverage {};
    band_histogram_coverage.fill(1);
    for (const std::pair<uint32_t, float> band :
         {std::pair<uint32_t, float> {0, 0.1F}, {32, 0.5F}})
    {
        EXPECT(pano_gpu_test_set_output_band(
                   histogram_output, band.first, 32, error.data(),
                   static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
        band_histogram_color.fill(band.second);
        EXPECT(pano_gpu_test_upload_output_histogram_band(
                   histogram_output, band_histogram_color.data(), sizeof(band_histogram_color),
                   band_histogram_coverage.data(), sizeof(band_histogram_coverage), error.data(),
                   static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
        EXPECT(pano_gpu_output_accumulate_auto_contrast_histogram_srgb(
                   histogram_output, error.data(), static_cast<uint32_t>(error.size())) ==
               PANO_GPU_SUCCESS);
    }
    retained_histogram.fill(0);
    EXPECT(pano_gpu_test_read_output_histogram(
               histogram_output, retained_histogram.data(), sizeof(retained_histogram), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(retained_histogram[histogram_bin(0.1F)] == 64);
    EXPECT(retained_histogram[histogram_bin(0.5F)] == 64);
    selected_levels = {};
    selected_levels.size = sizeof(selected_levels);
    selected_levels.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_output_select_auto_contrast_levels(
               histogram_output, &selected_levels, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const float black_rank = 0.005F * 127.0F;
    const float white_rank = 0.995F * 127.0F;
    const float expected_black =
        (histogram_bin(0.1F) + black_rank / 64.0F) / 4096.0F;
    const float expected_white =
        (histogram_bin(0.5F) + (white_rank - 64.0F) / 64.0F) / 4096.0F;
    EXPECT(std::fabs(selected_levels.black - expected_black) < 1.0e-7F);
    EXPECT(std::fabs(selected_levels.white - expected_white) < 1.0e-7F);
    std::array<float, 192> normalized_band {};
    EXPECT(pano_gpu_output_apply_auto_contrast_srgb(
               histogram_output, 0, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_read_output_normalized_srgb(
               histogram_output, normalized_band.data(), sizeof(normalized_band), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const float encoded_half =
        1.055F * std::pow(0.5F, 1.0F / 2.4F) - 0.055F;
    for (const float value : normalized_band)
        EXPECT(std::fabs(value - encoded_half) < 2.0e-6F);
    EXPECT(pano_gpu_output_apply_auto_contrast_srgb(
               histogram_output, 1, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_read_output_normalized_srgb(
               histogram_output, normalized_band.data(), sizeof(normalized_band), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const float contrasted_half = std::clamp(
        (encoded_half - selected_levels.black) /
            (selected_levels.white - selected_levels.black),
        0.0F, 1.0F);
    for (const float value : normalized_band)
        EXPECT(std::fabs(value - contrasted_half) < 2.0e-6F);
    EXPECT(pano_gpu_output_quantize_normalized_srgb8(
               histogram_output, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<uint8_t, 192> band_srgb8 {};
    EXPECT(pano_gpu_test_read_output_srgb8(
               histogram_output, band_srgb8.data(), sizeof(band_srgb8), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const int expected_contrasted_half =
        static_cast<int>(std::nearbyint(contrasted_half * 255.0F));
    for (const uint8_t value : band_srgb8)
        EXPECT(std::abs(static_cast<int>(value) - expected_contrasted_half) <= 1);
    for (size_t pixel = 0; pixel < band_histogram_coverage.size(); ++pixel)
    {
        band_histogram_color[pixel * 3] = pixel % 3 == 0 ? -0.25F : 0.5F;
        band_histogram_color[pixel * 3 + 1] = pixel % 3 == 1 ? 4.0F : 0.5F;
        band_histogram_color[pixel * 3 + 2] = pixel % 3 == 2 ? 16.0F : 0.5F;
    }
    EXPECT(pano_gpu_test_upload_output_histogram_band(
               histogram_output, band_histogram_color.data(), sizeof(band_histogram_color),
               band_histogram_coverage.data(), sizeof(band_histogram_coverage), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_tone_map_rec2020(
               histogram_output, 0.0F, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    EXPECT(pano_gpu_output_tone_map_rec2020(
               histogram_output, 203.0F, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 192> tone_mapped_rec2020 {};
    EXPECT(pano_gpu_test_read_output_tone_mapped_rec2020(
               histogram_output, tone_mapped_rec2020.data(), sizeof(tone_mapped_rec2020),
               error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < band_histogram_coverage.size(); ++pixel)
    {
        const float red = std::max(band_histogram_color[pixel * 3], 0.0F) * (10000.0F / 203.0F);
        const float green =
            std::max(band_histogram_color[pixel * 3 + 1], 0.0F) * (10000.0F / 203.0F);
        const float blue =
            std::max(band_histogram_color[pixel * 3 + 2], 0.0F) * (10000.0F / 203.0F);
        const float luminance = red * 0.2627F + green * 0.6780F + blue * 0.0593F;
        const float mapped_luminance = luminance / (1.0F + luminance);
        const float scale = luminance > 0.0F ? mapped_luminance / luminance : 0.0F;
        EXPECT(std::fabs(tone_mapped_rec2020[pixel * 3] - red * scale) < 2.0e-6F);
        EXPECT(std::fabs(tone_mapped_rec2020[pixel * 3 + 1] - green * scale) < 2.0e-6F);
        EXPECT(std::fabs(tone_mapped_rec2020[pixel * 3 + 2] - blue * scale) < 2.0e-6F);
    }
    EXPECT(pano_gpu_output_convert_tone_mapped_rec2020_to_linear_srgb(
               histogram_output, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 192> converted_linear_srgb {};
    EXPECT(pano_gpu_test_read_output_linear_srgb(
               histogram_output, converted_linear_srgb.data(), sizeof(converted_linear_srgb),
               error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < band_histogram_coverage.size(); ++pixel)
    {
        const float red = tone_mapped_rec2020[pixel * 3];
        const float green = tone_mapped_rec2020[pixel * 3 + 1];
        const float blue = tone_mapped_rec2020[pixel * 3 + 2];
        const float expected_red = red * 1.660491F - green * 0.587641F - blue * 0.072850F;
        const float expected_green =
            red * -0.124550F + green * 1.132900F - blue * 0.008349F;
        const float expected_blue =
            red * -0.018151F - green * 0.100579F + blue * 1.118730F;
        EXPECT(std::fabs(converted_linear_srgb[pixel * 3] - expected_red) < 2.0e-6F);
        EXPECT(std::fabs(converted_linear_srgb[pixel * 3 + 1] - expected_green) < 2.0e-6F);
        EXPECT(std::fabs(converted_linear_srgb[pixel * 3 + 2] - expected_blue) < 2.0e-6F);
    }
    EXPECT(pano_gpu_output_apply_auto_contrast_converted_srgb(
               histogram_output, 0, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_quantize_normalized_srgb8(
               histogram_output, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    band_srgb8.fill(0);
    EXPECT(pano_gpu_test_read_output_srgb8(
               histogram_output, band_srgb8.data(), sizeof(band_srgb8), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t index = 0; index < converted_linear_srgb.size(); ++index)
    {
        const float positive = std::max(converted_linear_srgb[index], 0.0F);
        const float encoded = positive <= 0.0031308F
            ? positive * 12.92F
            : 1.055F * std::pow(positive, 1.0F / 2.4F) - 0.055F;
        const int expected = static_cast<int>(
            std::nearbyint(std::clamp(encoded, 0.0F, 1.0F) * 255.0F));
        EXPECT(std::abs(static_cast<int>(band_srgb8[index]) - expected) <= 1);
    }
    EXPECT(pano_gpu_output_copy_linear_float(
               histogram_output, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 192> band_float_output {};
    EXPECT(pano_gpu_test_read_output_float(
               histogram_output, band_float_output.data(), sizeof(band_float_output), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(std::memcmp(
               band_float_output.data(), band_histogram_color.data(),
               sizeof(band_float_output)) == 0);
    pano_gpu_output_download_request download_request {};
    download_request.size = sizeof(download_request);
    download_request.abi_version = PANO_GPU_ABI_VERSION;
    download_request.output_width = 2;
    download_request.row_start = 32;
    download_request.row_count = 32;
    download_request.data = band_srgb8.data();
    download_request.data_bytes = sizeof(band_srgb8);
    ++download_request.output_width;
    EXPECT(pano_gpu_output_download_srgb8(
               histogram_output, &download_request, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    --download_request.output_width;
    --download_request.data_bytes;
    EXPECT(pano_gpu_output_download_srgb8(
               histogram_output, &download_request, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    ++download_request.data_bytes;
    pano_gpu_cancellation_token *download_token = nullptr;
    EXPECT(pano_gpu_cancellation_token_create(
               &download_token, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    pano_gpu_cancellation_token_cancel(download_token);
    band_srgb8.fill(0xA5);
    EXPECT(pano_gpu_output_download_srgb8(
               histogram_output, &download_request, download_token, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_CANCELLED);
    EXPECT(std::all_of(
        band_srgb8.begin(), band_srgb8.end(), [](const uint8_t value) { return value == 0xA5; }));
    pano_gpu_cancellation_token_destroy(&download_token);
    pano_gpu_test_fail_next_download_allocation();
    EXPECT(pano_gpu_output_download_srgb8(
               histogram_output, &download_request, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
    EXPECT(std::string(error.data()).find("download readback") != std::string::npos);
    EXPECT(std::all_of(
        band_srgb8.begin(), band_srgb8.end(), [](const uint8_t value) { return value == 0xA5; }));
    pano_gpu_test_fail_next_download_submission();
    EXPECT(pano_gpu_output_download_srgb8(
               histogram_output, &download_request, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
    EXPECT(std::string(error.data()).find("submit D3D12 output download") != std::string::npos);
    EXPECT(std::all_of(
        band_srgb8.begin(), band_srgb8.end(), [](const uint8_t value) { return value == 0xA5; }));
    pano_gpu_test_fail_next_download_fence_wait();
    EXPECT(pano_gpu_output_download_srgb8(
               histogram_output, &download_request, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
    EXPECT(std::string(error.data()).find("download fence wait") != std::string::npos);
    EXPECT(std::all_of(
        band_srgb8.begin(), band_srgb8.end(), [](const uint8_t value) { return value == 0xA5; }));
    pano_gpu_test_fail_next_download_map();
    EXPECT(pano_gpu_output_download_srgb8(
               histogram_output, &download_request, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
    EXPECT(std::string(error.data()).find("map D3D12 output download") != std::string::npos);
    EXPECT(std::all_of(
        band_srgb8.begin(), band_srgb8.end(), [](const uint8_t value) { return value == 0xA5; }));
    pano_gpu_test_fail_next_fence_wait();
    const auto timeout_started = std::chrono::steady_clock::now();
    EXPECT(pano_gpu_output_download_srgb8(
               histogram_output, &download_request, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
    const auto timeout_elapsed = std::chrono::steady_clock::now() - timeout_started;
    EXPECT(timeout_elapsed < std::chrono::seconds(5));
    EXPECT(std::string(error.data()).find("output download timed out") != std::string::npos);
    EXPECT(std::all_of(
        band_srgb8.begin(), band_srgb8.end(), [](const uint8_t value) { return value == 0xA5; }));
    EXPECT(pano_gpu_cancellation_token_create(
               &download_token, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    pano_gpu_test_cancel_after_next_output_download_wait();
    EXPECT(pano_gpu_output_download_srgb8(
               histogram_output, &download_request, download_token, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_CANCELLED);
    EXPECT(std::all_of(
        band_srgb8.begin(), band_srgb8.end(), [](const uint8_t value) { return value == 0xA5; }));
    pano_gpu_cancellation_token_destroy(&download_token);
    EXPECT(pano_gpu_output_download_srgb8(
               histogram_output, &download_request, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t index = 0; index < converted_linear_srgb.size(); ++index)
    {
        const float positive = std::max(converted_linear_srgb[index], 0.0F);
        const float encoded = positive <= 0.0031308F
            ? positive * 12.92F
            : 1.055F * std::pow(positive, 1.0F / 2.4F) - 0.055F;
        const int expected = static_cast<int>(
            std::nearbyint(std::clamp(encoded, 0.0F, 1.0F) * 255.0F));
        EXPECT(std::abs(static_cast<int>(band_srgb8[index]) - expected) <= 1);
    }
    download_request.data = band_float_output.data();
    download_request.data_bytes = sizeof(band_float_output);
    EXPECT(pano_gpu_output_download_float(
               histogram_output, &download_request, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(std::memcmp(
               band_float_output.data(), band_histogram_color.data(),
               sizeof(band_float_output)) == 0);
    std::array<uint8_t, 64> band_downloaded_coverage {};
    download_request.data = band_downloaded_coverage.data();
    download_request.data_bytes = sizeof(band_downloaded_coverage);
    EXPECT(pano_gpu_output_download_coverage(
               histogram_output, &download_request, nullptr, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(std::memcmp(
               band_downloaded_coverage.data(), band_histogram_coverage.data(),
               sizeof(band_downloaded_coverage)) == 0);
    pano_gpu_output_transfer_diagnostics transfer_diagnostics {};
    transfer_diagnostics.size = sizeof(transfer_diagnostics);
    transfer_diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_output_query_transfer_diagnostics(
               histogram_output, &transfer_diagnostics, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(transfer_diagnostics.readback_bytes == 64 * 1024);
    EXPECT(transfer_diagnostics.download_count == 3);
    EXPECT(transfer_diagnostics.downloaded_bytes ==
           sizeof(band_srgb8) + sizeof(band_float_output) + sizeof(band_downloaded_coverage));
    band_histogram_color[0] = std::numeric_limits<float>::quiet_NaN();
    band_histogram_color[1] = std::numeric_limits<float>::infinity();
    band_histogram_color[2] = -std::numeric_limits<float>::infinity();
    EXPECT(pano_gpu_test_upload_output_histogram_band(
               histogram_output, band_histogram_color.data(), sizeof(band_histogram_color),
               band_histogram_coverage.data(), sizeof(band_histogram_coverage), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_copy_linear_float(
               histogram_output, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_read_output_float(
               histogram_output, band_float_output.data(), sizeof(band_float_output), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(std::memcmp(
               band_float_output.data(), band_histogram_color.data(),
               sizeof(band_float_output)) == 0);
    EXPECT(pano_gpu_output_query_histogram_diagnostics(
               histogram_output, &histogram_diagnostics, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(histogram_diagnostics.clear_count == 1);
    EXPECT(histogram_diagnostics.accumulated_band_count == 2);
    EXPECT(histogram_diagnostics.accumulated_pixels == 128);
    pano_gpu_output_destroy(&histogram_output);
    histogram_output_options.output_width = 1;
    histogram_output_options.output_height = 1;
    histogram_output_options.output_band_rows = 0;
    EXPECT(pano_gpu_output_create_empty(
               session, &histogram_output_options, &histogram_output, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_linear(
               histogram_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_coverage(
               histogram_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_prepare_auto_contrast_histogram(
               histogram_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const std::array<float, 3> empty_color {};
    const std::array<uint8_t, 1> empty_coverage {};
    EXPECT(pano_gpu_test_upload_output_histogram_band(
               histogram_output, empty_color.data(), sizeof(empty_color), empty_coverage.data(),
               sizeof(empty_coverage), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_accumulate_auto_contrast_histogram_srgb(
               histogram_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    selected_levels = {};
    selected_levels.size = sizeof(selected_levels);
    selected_levels.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_output_select_auto_contrast_levels(
               histogram_output, &selected_levels, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(selected_levels.black == 0.0F);
    EXPECT(selected_levels.white == 1.0F);
    pano_gpu_output_destroy(&histogram_output);
#endif
    pano_gpu_exposure_graph_diagnostics graph_diagnostics {};
    graph_diagnostics.size = sizeof(graph_diagnostics);
    graph_diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_prepare_exposure_graph(
               session, 0, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_enumerate_exposure_pairs(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_copy_exposure_pair_reports(
               session, nullptr, 0, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_query_exposure_graph(
               session, &graph_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(graph_diagnostics.pair_capacity == 0);
    EXPECT(graph_diagnostics.pair_report_count == 0);
    EXPECT(graph_diagnostics.equation_count == 0);
    EXPECT(graph_diagnostics.solve_equation_count == 0);
    EXPECT(pano_gpu_session_build_exposure_solve_graph(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_copy_exposure_solve_equations(
               session, nullptr, 0, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    pano_gpu_exposure_solve_result single_solve {};
    single_solve.size = sizeof(single_solve);
    single_solve.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_solve_exposure_graph(
               session, &single_solve, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 1> single_gain {};
    EXPECT(pano_gpu_session_copy_exposure_log_gains(
               session, single_gain.data(), sizeof(single_gain), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(single_solve.anchor_frame_index == 0);
    EXPECT(single_solve.edge_count == 0);
    EXPECT(single_solve.frame_count == 1);
    EXPECT(single_gain[0] == 0.0F);
    EXPECT(pano_gpu_session_prepare_exposure_graph(
               session, 1, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    pano_gpu_session_clear_exposure_graph(session);
    pano_gpu_session_clear_exposure_graph(session);
    EXPECT(session != nullptr);
    pano_gpu_projection_request projection_request {};
    projection_request.size = sizeof(projection_request);
    projection_request.abi_version = PANO_GPU_ABI_VERSION;
    projection_request.output_width = 64;
    projection_request.output_height = 32;
    projection_request.row_count = 16;
    projection_request.latitude_span_degrees = 180.0F;
    projection_request.horizontal_fov_degrees = 90.0F;
    projection_request.vertical_fov_degrees = 60.0F;
    projection_request.world_to_camera[0] = 1.0F;
    projection_request.world_to_camera[4] = 1.0F;
    projection_request.world_to_camera[8] = 1.0F;
    pano_gpu_projection_result_layout projection_layout {};
    projection_layout.size = sizeof(projection_layout);
    projection_layout.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_test_validate_projection_request(
               session, &projection_request, &projection_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(projection_layout.pixel_count == 64 * 16);
    EXPECT(projection_layout.world_ray_bytes == 64 * 16 * 3 * sizeof(float));
    EXPECT(projection_layout.camera_ray_bytes == projection_layout.world_ray_bytes);
    EXPECT(projection_layout.projected_coordinate_bytes == 64 * 16 * 2 * sizeof(float));
    EXPECT(projection_layout.validity_bytes == 64 * 16);
    const pano_gpu_result ray_dispatch_result = pano_gpu_test_dispatch_rays(
        session, &projection_request, nullptr, 0, nullptr, 0, nullptr, 0, error.data(),
        static_cast<uint32_t>(error.size()));
    if (!expect(ray_dispatch_result == PANO_GPU_SUCCESS, "pano_gpu_test_dispatch_rays(...) == PANO_GPU_SUCCESS", __LINE__))
    {
        std::fprintf(stderr, "D3D12 ray dispatch detail: %s\n", error.data());
        return 1;
    }
    projection_request.output_width = 4;
    projection_request.output_height = 2;
    projection_request.row_count = 2;
    projection_request.latitude_span_degrees = 60.0F;
    EXPECT(pano_gpu_test_validate_projection_request(
               session, &projection_request, &projection_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 24> world_rays {};
    EXPECT(pano_gpu_test_dispatch_rays(
               session, &projection_request, nullptr, projection_layout.world_ray_bytes, nullptr, 0, nullptr, 0, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    EXPECT(pano_gpu_test_dispatch_rays(
               session, &projection_request, world_rays.data(), projection_layout.world_ray_bytes, nullptr, 0, nullptr, 0, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (uint32_t y = 0; y < projection_request.row_count; ++y)
    {
        for (uint32_t x = 0; x < projection_request.output_width; ++x)
        {
            const float longitude =
                ((static_cast<float>(x) + 0.5F) / projection_request.output_width - 0.5F) * 6.283185307179586F;
            const float latitude = (0.5F - (static_cast<float>(y) + 0.5F) / projection_request.output_height) *
                projection_request.latitude_span_degrees * 0.0174532925199433F;
            const float cosine = std::cos(latitude);
            const size_t offset = (static_cast<size_t>(y) * projection_request.output_width + x) * 3;
            EXPECT(std::fabs(world_rays[offset] - cosine * std::sin(longitude)) < 1.0e-5F);
            EXPECT(std::fabs(world_rays[offset + 1] - std::sin(latitude)) < 1.0e-5F);
            EXPECT(std::fabs(world_rays[offset + 2] - cosine * std::cos(longitude)) < 1.0e-5F);
        }
    }
    std::array<float, 16> projected_coordinates {};
    std::array<uint8_t, 8> validity {};
    EXPECT(pano_gpu_test_dispatch_rays(
               session, &projection_request, nullptr, 0, projected_coordinates.data(),
               projection_layout.projected_coordinate_bytes, validity.data(), projection_layout.validity_bytes, error.data(),
               static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    const float focal_x = 2.0F;
    const float focal_y = 4.0F / (2.0F * std::tan(30.0F * 0.0174532925199433F));
    for (uint32_t y = 0; y < projection_request.row_count; ++y)
    {
        for (uint32_t x = 0; x < projection_request.output_width; ++x)
        {
            const float longitude =
                ((static_cast<float>(x) + 0.5F) / projection_request.output_width - 0.5F) * 6.283185307179586F;
            const float latitude = (0.5F - (static_cast<float>(y) + 0.5F) / projection_request.output_height) *
                projection_request.latitude_span_degrees * 0.0174532925199433F;
            const float cosine = std::cos(latitude);
            const float ray_x = cosine * std::sin(longitude);
            const float ray_y = std::sin(latitude);
            const float ray_z = cosine * std::cos(longitude);
            const float safe_z = std::fabs(ray_z) > 1.0e-8F ? ray_z : 1.0F;
            const size_t offset = (static_cast<size_t>(y) * projection_request.output_width + x) * 2;
            const float expected_x = 1.5F + focal_x * ray_x / safe_z;
            const float expected_y = 1.5F - focal_y * ray_y / safe_z;
            EXPECT(std::fabs(projected_coordinates[offset] - std::clamp(expected_x, 0.0F, 3.0F)) < 1.0e-4F);
            EXPECT(std::fabs(projected_coordinates[offset + 1] - std::clamp(expected_y, 0.0F, 3.0F)) < 1.0e-4F);
            EXPECT(validity[y * projection_request.output_width + x] ==
                   static_cast<uint8_t>(ray_z > 0.0F && expected_x >= -0.5F && expected_x <= 3.5F &&
                                        expected_y >= -0.5F && expected_y <= 3.5F));
        }
    }
    projection_request.output_width = 1;
    projection_request.output_height = 1;
    projection_request.row_count = 1;
    EXPECT(pano_gpu_test_validate_projection_request(
               session, &projection_request, &projection_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 2> center_coordinate {};
    EXPECT(pano_gpu_test_dispatch_rays(
               session, &projection_request, nullptr, 0, center_coordinate.data(),
               projection_layout.projected_coordinate_bytes, validity.data(), projection_layout.validity_bytes, error.data(),
               static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(std::fabs(center_coordinate[0] - 1.5F) < 1.0e-5F);
    EXPECT(std::fabs(center_coordinate[1] - 1.5F) < 1.0e-5F);
    EXPECT(validity[0] == 1);
    EXPECT(pano_gpu_test_dispatch_rays(
               session, &projection_request, nullptr, 0, nullptr, projection_layout.projected_coordinate_bytes,
               nullptr, 0, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    EXPECT(pano_gpu_test_dispatch_rays(
               session, &projection_request, nullptr, 0, nullptr, 0, nullptr, projection_layout.validity_bytes,
               error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    projection_request.output_width = 4;
    projection_request.output_height = 2;
    projection_request.row_count = 2;
    EXPECT(pano_gpu_test_validate_projection_request(
               session, &projection_request, &projection_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    const std::array<float, 9> y_axis_rotation {0.0F, 0.0F, -1.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 0.0F};
    std::memcpy(projection_request.world_to_camera, y_axis_rotation.data(), sizeof(projection_request.world_to_camera));
    EXPECT(pano_gpu_test_dispatch_rays(
               session, &projection_request, world_rays.data(), projection_layout.world_ray_bytes, nullptr, 0, nullptr, 0, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (uint32_t y = 0; y < projection_request.row_count; ++y)
    {
        for (uint32_t x = 0; x < projection_request.output_width; ++x)
        {
            const float longitude =
                ((static_cast<float>(x) + 0.5F) / projection_request.output_width - 0.5F) * 6.283185307179586F;
            const float latitude = (0.5F - (static_cast<float>(y) + 0.5F) / projection_request.output_height) *
                projection_request.latitude_span_degrees * 0.0174532925199433F;
            const float cosine = std::cos(latitude);
            const float world_x = cosine * std::sin(longitude);
            const float world_y = std::sin(latitude);
            const float world_z = cosine * std::cos(longitude);
            const size_t offset = (static_cast<size_t>(y) * projection_request.output_width + x) * 3;
            EXPECT(std::fabs(world_rays[offset] - world_z) < 1.0e-5F);
            EXPECT(std::fabs(world_rays[offset + 1] - world_y) < 1.0e-5F);
            EXPECT(std::fabs(world_rays[offset + 2] + world_x) < 1.0e-5F);
        }
    }
    projection_request.world_to_camera[0] = 1.0F;
    projection_request.world_to_camera[1] = 0.0F;
    projection_request.world_to_camera[2] = 0.0F;
    projection_request.world_to_camera[3] = 0.0F;
    projection_request.world_to_camera[4] = 1.0F;
    projection_request.world_to_camera[5] = 0.0F;
    projection_request.world_to_camera[6] = 0.0F;
    projection_request.world_to_camera[7] = 0.0F;
    projection_request.world_to_camera[8] = 1.0F;
    projection_request.output_width = 64;
    projection_request.output_height = 32;
    projection_request.row_count = 16;
    projection_request.row_start = 17;
    projection_request.row_count = 16;
    EXPECT(pano_gpu_test_validate_projection_request(
               session, &projection_request, &projection_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    EXPECT(projection_layout.pixel_count == 0);
    projection_request.row_start = 0;
    projection_request.world_to_camera[0] = std::numeric_limits<float>::infinity();
    EXPECT(pano_gpu_test_validate_projection_request(
               session, &projection_request, &projection_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    projection_request.world_to_camera[0] = 1.0F;
    pano_gpu_output_create_options output_options {};
    output_options.size = sizeof(output_options);
    output_options.abi_version = PANO_GPU_ABI_VERSION;
    output_options.output_width = 64;
    output_options.output_height = 32;
    output_options.output_sample_bytes = 1;
    output_options.descriptor_count = 5;
    output_options.output_workspace_bytes = 64ULL * 32 * 13;
    pano_gpu_output *failed_output = reinterpret_cast<pano_gpu_output *>(1);
    pano_gpu_test_fail_next_output_allocation();
    EXPECT(pano_gpu_output_create_empty(
               session, &output_options, &failed_output, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_OUT_OF_MEMORY);
    EXPECT(failed_output == nullptr);
    EXPECT(pano_gpu_query_diagnostics(&diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(diagnostics.live_session_count == 1);
    EXPECT(diagnostics.live_output_count == 0);
    EXPECT(pano_gpu_validate_output_create_options(
               session, &output_options, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_output *resident_output = nullptr;
    EXPECT(pano_gpu_output_create_empty(
               session, &output_options, &resident_output, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_query_diagnostics(&diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(diagnostics.live_output_count == 1);
    pano_gpu_output_diagnostics output_diagnostics {};
    output_diagnostics.size = sizeof(output_diagnostics);
    output_diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_output_query_diagnostics(
               resident_output, &output_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(output_diagnostics.planned_linear_bytes == 64 * 1024);
    EXPECT(output_diagnostics.linear_bytes == 0);
    EXPECT(output_diagnostics.is_banded == 0);
    EXPECT(output_diagnostics.output_band_rows == 0);
    EXPECT(output_diagnostics.band_row_start == 0);
    EXPECT(output_diagnostics.band_row_count == 0);
    EXPECT(output_diagnostics.planned_coverage_bytes == 64 * 1024);
    EXPECT(output_diagnostics.coverage_bytes == 0);
#if defined(_WIN32)
    EXPECT(pano_gpu_output_allocate_linear(
               resident_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_linear(
               resident_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    EXPECT(pano_gpu_output_query_diagnostics(
               resident_output, &output_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(output_diagnostics.linear_bytes == 64 * 1024);
    EXPECT(pano_gpu_output_allocate_coverage(
               resident_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_coverage(
               resident_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    EXPECT(pano_gpu_output_query_diagnostics(
               resident_output, &output_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(output_diagnostics.coverage_bytes == 64 * 1024);
#else
    EXPECT(pano_gpu_output_allocate_linear(
               resident_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
    EXPECT(pano_gpu_output_allocate_coverage(
               resident_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
#endif
    pano_gpu_output_destroy(&resident_output);
    EXPECT(resident_output == nullptr);
    EXPECT(pano_gpu_query_diagnostics(&diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(diagnostics.live_output_count == 0);
    output_options.output_band_rows = 32;
    EXPECT(pano_gpu_validate_output_create_options(
               session, &output_options, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_output *banded_output = nullptr;
    EXPECT(pano_gpu_output_create_empty(
               session, &output_options, &banded_output, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_query_diagnostics(&diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(diagnostics.live_output_count == 1);
    EXPECT(pano_gpu_output_query_diagnostics(
               banded_output, &output_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(output_diagnostics.is_banded == 1);
    EXPECT(output_diagnostics.output_band_rows == 32);
    EXPECT(output_diagnostics.band_row_start == 0);
    EXPECT(output_diagnostics.band_row_count == 32);
    EXPECT(output_diagnostics.linear_bytes == 0);
    EXPECT(output_diagnostics.coverage_bytes == 0);
    pano_gpu_output_destroy(&banded_output);
    EXPECT(banded_output == nullptr);
    EXPECT(pano_gpu_query_diagnostics(&diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(diagnostics.live_output_count == 0);
    pano_gpu_output_create_options large_banded_options = output_options;
    large_banded_options.output_width = 8192;
    large_banded_options.output_height = 64;
    large_banded_options.output_band_rows = 32;
    large_banded_options.output_workspace_bytes = 8192ULL * 64 * 13;
    EXPECT(pano_gpu_output_create_empty(
               session, &large_banded_options, &banded_output, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_query_diagnostics(&diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(diagnostics.live_output_count == 1);
    EXPECT(pano_gpu_output_query_diagnostics(
               banded_output, &output_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(output_diagnostics.band_row_start == 0);
    EXPECT(output_diagnostics.band_row_count == 32);
    EXPECT(output_diagnostics.planned_linear_bytes == 8192ULL * 32 * 3 * sizeof(float));
    EXPECT(output_diagnostics.planned_coverage_bytes == 8192ULL * 32);
#if defined(_WIN32)
    EXPECT(pano_gpu_output_allocate_linear(
               banded_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_coverage(
               banded_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_query_diagnostics(
               banded_output, &output_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(output_diagnostics.linear_bytes == 8192ULL * 32 * 3 * sizeof(float));
    EXPECT(output_diagnostics.coverage_bytes == 8192ULL * 32);
#else
    EXPECT(pano_gpu_output_allocate_linear(
               banded_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
    EXPECT(pano_gpu_output_allocate_coverage(
               banded_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
#endif
    pano_gpu_output_destroy(&banded_output);
    EXPECT(banded_output == nullptr);
    EXPECT(pano_gpu_query_diagnostics(&diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(diagnostics.live_output_count == 0);
    output_options.size -= 1;
    EXPECT(pano_gpu_validate_output_create_options(
               session, &output_options, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    output_options.size = sizeof(output_options);
    output_options.descriptor_count -= 1;
    EXPECT(pano_gpu_validate_output_create_options(
               session, &output_options, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    output_options.descriptor_count = 5;
    output_options.output_band_rows = 31;
    EXPECT(pano_gpu_validate_output_create_options(
               session, &output_options, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    output_options.output_band_rows = 0;
    EXPECT(pano_gpu_validate_source_upload(
               session, &source_upload, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    pano_gpu_uint8_sample_request sample_request {};
    sample_request.size = sizeof(sample_request);
    sample_request.abi_version = PANO_GPU_ABI_VERSION;
    sample_request.coordinate_count = 5;
    std::array<float, 10> sample_coordinates {
        0.0F, 0.0F, 3.0F, 3.0F, 1.5F, 1.5F, -0.25F, 2.0F, 3.25F, -0.25F};
    pano_gpu_uint8_sample_result_layout sample_layout {};
    sample_layout.size = sizeof(sample_layout);
    sample_layout.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_test_validate_uint8_sample_request(
               session, &sample_request, sample_coordinates.data(), sizeof(sample_coordinates), &sample_layout,
               error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    source_upload.frame_index = 1;
    EXPECT(pano_gpu_validate_source_upload(
               session, &source_upload, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    source_upload.frame_index = 0;
    source_upload.data_bytes -= 1;
    EXPECT(pano_gpu_validate_source_upload(
               session, &source_upload, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    source_upload.data_bytes = source_data.size();
    source_upload.source_sample_type = PANO_GPU_SAMPLE_UINT16;
    EXPECT(pano_gpu_validate_source_upload(
               session, &source_upload, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    source_upload.source_sample_type = PANO_GPU_SAMPLE_UINT8;
    pano_gpu_test_fail_next_source_allocation();
    EXPECT(pano_gpu_session_allocate_source(session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_OUT_OF_MEMORY);
    EXPECT(pano_gpu_test_session_source_bytes(session) == 0);
    EXPECT(pano_gpu_session_allocate_source(session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_session_source_bytes(session) == 64ULL * 1024);
    EXPECT(pano_gpu_session_allocate_source(session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    pano_gpu_test_fail_next_upload_slot_allocation();
    EXPECT(pano_gpu_session_allocate_upload_slot(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_OUT_OF_MEMORY);
    EXPECT(pano_gpu_test_session_upload_slot_bytes(session) == 0);
    EXPECT(pano_gpu_session_allocate_upload_slot(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_session_upload_slot_bytes(session) == 64ULL * 1024);
    EXPECT(pano_gpu_session_allocate_upload_slot(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    pano_gpu_test_fail_next_second_upload_slot_allocation();
    EXPECT(pano_gpu_session_allocate_second_upload_slot(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_OUT_OF_MEMORY);
    EXPECT(pano_gpu_test_session_second_upload_slot_bytes(session) == 0);
    EXPECT(pano_gpu_session_allocate_second_upload_slot(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_session_second_upload_slot_bytes(session) == 64ULL * 1024);
    EXPECT(pano_gpu_session_allocate_second_upload_slot(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    EXPECT(pano_gpu_session_finish_uploads(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_upload_frame_zero(
               session, &source_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_finish_uploads(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_validate_uint8_sample_request(
               session, &sample_request, sample_coordinates.data(), sizeof(sample_coordinates), &sample_layout,
               error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(sample_layout.coordinate_bytes == sizeof(sample_coordinates));
    EXPECT(sample_layout.sampled_rgb_bytes == 5 * 3 * sizeof(float));
    std::array<float, 15> sampled_rgb {};
    EXPECT(pano_gpu_test_sample_uint8(
               session, &sample_request, sample_coordinates.data(), sizeof(sample_coordinates), sampled_rgb.data(),
               sample_layout.sampled_rgb_bytes - 1, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    EXPECT(pano_gpu_test_sample_uint8(
               session, &sample_request, sample_coordinates.data(), sizeof(sample_coordinates), sampled_rgb.data(),
               sample_layout.sampled_rgb_bytes, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (uint32_t sample_index = 0; sample_index < sample_request.coordinate_count; ++sample_index)
    {
        const float x = std::clamp(sample_coordinates[2 * sample_index], 0.0F, 3.0F);
        const float y = std::clamp(sample_coordinates[2 * sample_index + 1], 0.0F, 3.0F);
        const uint32_t x0 = static_cast<uint32_t>(std::floor(x));
        const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
        const uint32_t x1 = std::min(x0 + 1, 3U);
        const uint32_t y1 = std::min(y0 + 1, 3U);
        const float x_fraction = x - x0;
        const float y_fraction = y - y0;
        const size_t result_offset = static_cast<size_t>(sample_index) * 3;
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const auto source_value = [&](const uint32_t source_x, const uint32_t source_y) {
                return source_data[(static_cast<size_t>(source_y) * 4 + source_x) * 3 + channel] / 255.0F;
            };
            const float top = source_value(x0, y0) * (1.0F - x_fraction) + source_value(x1, y0) * x_fraction;
            const float bottom = source_value(x0, y1) * (1.0F - x_fraction) + source_value(x1, y1) * x_fraction;
            EXPECT(std::fabs(sampled_rgb[result_offset + channel] -
                             (top * (1.0F - y_fraction) + bottom * y_fraction)) < 1.0e-6F);
        }
    }
    sample_request.frame_index = 1;
    EXPECT(pano_gpu_test_validate_uint8_sample_request(
               session, &sample_request, sample_coordinates.data(), sizeof(sample_coordinates), &sample_layout,
               error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    sample_request.frame_index = 0;
    EXPECT(pano_gpu_test_validate_uint8_sample_request(
               session, &sample_request, sample_coordinates.data(), sizeof(sample_coordinates) - 1, &sample_layout,
               error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    sample_coordinates[0] = std::numeric_limits<float>::infinity();
    EXPECT(pano_gpu_test_validate_uint8_sample_request(
               session, &sample_request, sample_coordinates.data(), sizeof(sample_coordinates), &sample_layout,
               error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    sample_coordinates[0] = 0.0F;
    std::array<uint8_t, 48> read_source_data {};
    EXPECT(pano_gpu_test_read_session_frame_zero(
               session, read_source_data.data(), read_source_data.size(), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(std::memcmp(source_data.data(), read_source_data.data(), source_data.size()) == 0);
    source_data.front() ^= 0xffU;
    EXPECT(pano_gpu_session_upload_frame_zero(
               session, &source_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_finish_uploads(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_read_session_frame_zero(
               session, read_source_data.data(), read_source_data.size(), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(std::memcmp(source_data.data(), read_source_data.data(), source_data.size()) == 0);
    pano_gpu_test_fail_next_rotation_allocation();
    EXPECT(pano_gpu_session_allocate_rotations(session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_OUT_OF_MEMORY);
    EXPECT(pano_gpu_test_session_rotation_bytes(session) == 0);
    EXPECT(pano_gpu_session_allocate_rotations(session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_session_rotation_bytes(session) == 64ULL * 1024);
    EXPECT(pano_gpu_session_allocate_rotations(session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    EXPECT(pano_gpu_session_upload_rotations(
               session, rotation.data(), sizeof(rotation), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_upload_rotations(
               session, rotation.data(), sizeof(rotation), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    std::array<float, 9> read_rotation {};
    EXPECT(pano_gpu_test_read_session_rotations(
               session, read_rotation.data(), sizeof(read_rotation), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(std::memcmp(rotation.data(), read_rotation.data(), sizeof(rotation)) == 0);
    EXPECT(pano_gpu_session_allocate_encoding_metadata(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_session_encoding_metadata_bytes(session) == 0);
    pano_gpu_session_diagnostics session_diagnostics {};
    session_diagnostics.size = sizeof(session_diagnostics);
    session_diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_query_diagnostics(
               session, &session_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(session_diagnostics.planned_source_bytes == 64ULL * 1024);
    EXPECT(session_diagnostics.source_bytes == 64ULL * 1024);
    EXPECT(session_diagnostics.planned_rotation_bytes == 64ULL * 1024);
    EXPECT(session_diagnostics.rotation_bytes == 64ULL * 1024);
    EXPECT(session_diagnostics.planned_encoding_metadata_bytes == 0);
    EXPECT(session_diagnostics.encoding_metadata_bytes == 0);
    EXPECT(session_diagnostics.upload_count == 2);
    EXPECT(session_diagnostics.uploaded_bytes == 2 * source_data.size());
    EXPECT(session_diagnostics.last_completed_upload_fence != 0);
    EXPECT(pano_gpu_query_diagnostics(&diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(diagnostics.live_session_count == 1);
    pano_gpu_session_destroy(&session);
    EXPECT(session == nullptr);
    pano_gpu_session_destroy(&session);
    EXPECT(pano_gpu_query_diagnostics(&diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(diagnostics.live_session_count == 0);
    pano_gpu_session_create_options uint16_options = session_options;
    uint16_options.source_sample_type = PANO_GPU_SAMPLE_UINT16;
    uint16_options.transfer_function = PANO_GPU_TRANSFER_LINEAR;
    uint16_options.source_row_stride_bytes = 24;
    EXPECT(pano_gpu_session_create(
               device, &uint16_options, &session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    pano_gpu_uint16_sample_request uint16_sample_request {};
    uint16_sample_request.size = sizeof(uint16_sample_request);
    uint16_sample_request.abi_version = PANO_GPU_ABI_VERSION;
    uint16_sample_request.coordinate_count = 5;
    std::array<float, 10> uint16_sample_coordinates {
        0.0F, 0.0F, 3.0F, 3.0F, 1.5F, 1.5F, -0.25F, 2.0F, 3.25F, -0.25F};
    pano_gpu_uint16_sample_result_layout uint16_sample_layout {};
    uint16_sample_layout.size = sizeof(uint16_sample_layout);
    uint16_sample_layout.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_test_validate_uint16_sample_request(
               session, &uint16_sample_request, uint16_sample_coordinates.data(), sizeof(uint16_sample_coordinates),
               &uint16_sample_layout, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    EXPECT(pano_gpu_test_validate_uint8_sample_request(
               session, &uint16_sample_request, uint16_sample_coordinates.data(), sizeof(uint16_sample_coordinates),
               &uint16_sample_layout, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    std::array<uint16_t, 48> uint16_source_data {};
    for (size_t index = 0; index < uint16_source_data.size(); ++index)
        uint16_source_data[index] = static_cast<uint16_t>(index * 997U + 13U);
    pano_gpu_source_upload uint16_upload {};
    uint16_upload.size = sizeof(uint16_upload);
    uint16_upload.abi_version = PANO_GPU_ABI_VERSION;
    uint16_upload.source_sample_type = PANO_GPU_SAMPLE_UINT16;
    uint16_upload.source_row_stride_bytes = 24;
    uint16_upload.data = uint16_source_data.data();
    uint16_upload.data_bytes = sizeof(uint16_source_data);
    EXPECT(pano_gpu_session_allocate_source(session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_upload_slot(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_upload_frame_zero(
               session, &uint16_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_finish_uploads(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_exposure_proxy_request uint16_proxy_request {};
    uint16_proxy_request.size = sizeof(uint16_proxy_request);
    uint16_proxy_request.abi_version = PANO_GPU_ABI_VERSION;
    uint16_proxy_request.frame_count = 1;
    uint16_proxy_request.source_width = 4;
    uint16_proxy_request.source_height = 4;
    pano_gpu_exposure_proxy_layout uint16_proxy_layout {};
    uint16_proxy_layout.size = sizeof(uint16_proxy_layout);
    uint16_proxy_layout.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_test_plan_exposure_proxies(
               session, &uint16_proxy_request, &uint16_proxy_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 48> uint16_proxy {};
    EXPECT(pano_gpu_test_dispatch_uint16_exposure_proxies(
               session, &uint16_proxy_request, uint16_proxy.data(), sizeof(uint16_proxy), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t index = 0; index < uint16_proxy.size(); ++index)
        EXPECT(std::fabs(uint16_proxy[index] - uint16_source_data[index] / 65535.0F) < 1.0e-6F);
    EXPECT(pano_gpu_test_validate_uint16_sample_request(
               session, &uint16_sample_request, uint16_sample_coordinates.data(), sizeof(uint16_sample_coordinates),
               &uint16_sample_layout, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(uint16_sample_layout.coordinate_bytes == sizeof(uint16_sample_coordinates));
    EXPECT(uint16_sample_layout.sampled_rgb_bytes == 5 * 3 * sizeof(float));
    std::array<float, 15> uint16_sampled_rgb {};
    EXPECT(pano_gpu_test_sample_uint16(
               session, &uint16_sample_request, uint16_sample_coordinates.data(), sizeof(uint16_sample_coordinates),
               uint16_sampled_rgb.data(), uint16_sample_layout.sampled_rgb_bytes, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_sample_uint8(
               session, &uint16_sample_request, uint16_sample_coordinates.data(), sizeof(uint16_sample_coordinates),
               uint16_sampled_rgb.data(), uint16_sample_layout.sampled_rgb_bytes, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    for (uint32_t sample_index = 0; sample_index < uint16_sample_request.coordinate_count; ++sample_index)
    {
        const float x = std::clamp(uint16_sample_coordinates[2 * sample_index], 0.0F, 3.0F);
        const float y = std::clamp(uint16_sample_coordinates[2 * sample_index + 1], 0.0F, 3.0F);
        const uint32_t x0 = static_cast<uint32_t>(std::floor(x));
        const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
        const uint32_t x1 = std::min(x0 + 1, 3U);
        const uint32_t y1 = std::min(y0 + 1, 3U);
        const float x_fraction = x - x0;
        const float y_fraction = y - y0;
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const auto source_value = [&](const uint32_t source_x, const uint32_t source_y) {
                return uint16_source_data[(static_cast<size_t>(source_y) * 4 + source_x) * 3 + channel] / 65535.0F;
            };
            const float top = source_value(x0, y0) * (1.0F - x_fraction) + source_value(x1, y0) * x_fraction;
            const float bottom = source_value(x0, y1) * (1.0F - x_fraction) + source_value(x1, y1) * x_fraction;
            EXPECT(std::fabs(uint16_sampled_rgb[3 * sample_index + channel] -
                             (top * (1.0F - y_fraction) + bottom * y_fraction)) < 1.0e-6F);
        }
    }
    pano_gpu_session_destroy(&session);
    pano_gpu_session_create_options float32_options = session_options;
    float32_options.source_sample_type = PANO_GPU_SAMPLE_FLOAT32;
    float32_options.transfer_function = PANO_GPU_TRANSFER_LINEAR;
    float32_options.source_row_stride_bytes = 48;
    EXPECT(pano_gpu_session_create(
               device, &float32_options, &session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    pano_gpu_float32_sample_request float32_sample_request {};
    float32_sample_request.size = sizeof(float32_sample_request);
    float32_sample_request.abi_version = PANO_GPU_ABI_VERSION;
    float32_sample_request.coordinate_count = 5;
    pano_gpu_float32_sample_result_layout float32_sample_layout {};
    float32_sample_layout.size = sizeof(float32_sample_layout);
    float32_sample_layout.abi_version = PANO_GPU_ABI_VERSION;
    pano_gpu_one_frame_composite_request composite_request {};
    composite_request.size = sizeof(composite_request);
    composite_request.abi_version = PANO_GPU_ABI_VERSION;
    composite_request.source_sample_type = PANO_GPU_SAMPLE_FLOAT32;
    composite_request.output_width = 8;
    composite_request.output_height = 4;
    composite_request.row_start = 1;
    composite_request.row_count = 2;
    composite_request.latitude_span_degrees = 180.0F;
    composite_request.horizontal_fov_degrees = 90.0F;
    composite_request.vertical_fov_degrees = 90.0F;
    composite_request.world_to_camera[0] = 1.0F;
    composite_request.world_to_camera[4] = 1.0F;
    composite_request.world_to_camera[8] = 1.0F;
    pano_gpu_one_frame_composite_result_layout composite_layout {};
    composite_layout.size = sizeof(composite_layout);
    composite_layout.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_test_validate_one_frame_composite_request(
               session, &composite_request, &composite_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    EXPECT(pano_gpu_test_validate_float32_sample_request(
               session, &float32_sample_request, uint16_sample_coordinates.data(), sizeof(uint16_sample_coordinates),
               &float32_sample_layout, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    EXPECT(pano_gpu_test_validate_uint16_sample_request(
               session, &float32_sample_request, uint16_sample_coordinates.data(), sizeof(uint16_sample_coordinates),
               &float32_sample_layout, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    std::array<float, 48> float32_source_data {};
    for (size_t index = 0; index < float32_source_data.size(); ++index)
        float32_source_data[index] = static_cast<float>(index) * 0.125F;
    float32_source_data[0] = std::numeric_limits<float>::quiet_NaN();
    float32_source_data[1] = std::numeric_limits<float>::infinity();
    float32_source_data[2] = -std::numeric_limits<float>::infinity();
    pano_gpu_source_upload float32_upload {};
    float32_upload.size = sizeof(float32_upload);
    float32_upload.abi_version = PANO_GPU_ABI_VERSION;
    float32_upload.source_sample_type = PANO_GPU_SAMPLE_FLOAT32;
    float32_upload.source_row_stride_bytes = 48;
    float32_upload.data = float32_source_data.data();
    float32_upload.data_bytes = sizeof(float32_source_data);
    EXPECT(pano_gpu_session_allocate_source(session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_upload_slot(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_upload_frame_zero(
               session, &float32_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_finish_uploads(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_exposure_proxy_request float32_proxy_request {};
    float32_proxy_request.size = sizeof(float32_proxy_request);
    float32_proxy_request.abi_version = PANO_GPU_ABI_VERSION;
    float32_proxy_request.frame_count = 1;
    float32_proxy_request.source_width = 4;
    float32_proxy_request.source_height = 4;
    pano_gpu_exposure_proxy_layout float32_proxy_layout {};
    float32_proxy_layout.size = sizeof(float32_proxy_layout);
    float32_proxy_layout.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_test_plan_exposure_proxies(
               session, &float32_proxy_request, &float32_proxy_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 48> float32_proxy {};
    EXPECT(pano_gpu_test_dispatch_float32_exposure_proxies(
               session, &float32_proxy_request, float32_proxy.data(), sizeof(float32_proxy), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(std::isnan(float32_proxy[0]));
    EXPECT(std::isinf(float32_proxy[1]) && float32_proxy[1] > 0.0F);
    EXPECT(std::isinf(float32_proxy[2]) && float32_proxy[2] < 0.0F);
    for (size_t index = 3; index < float32_proxy.size(); ++index)
        EXPECT(float32_proxy[index] == float32_source_data[index]);
    composite_request.source_sample_type = PANO_GPU_SAMPLE_UINT8;
    EXPECT(pano_gpu_test_validate_one_frame_composite_request(
               session, &composite_request, &composite_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    composite_request.source_sample_type = PANO_GPU_SAMPLE_FLOAT32;
    composite_request.row_start = composite_request.output_height;
    EXPECT(pano_gpu_test_validate_one_frame_composite_request(
               session, &composite_request, &composite_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    composite_request.row_start = 1;
    composite_request.vertical_fov_degrees = std::numeric_limits<float>::infinity();
    EXPECT(pano_gpu_test_validate_one_frame_composite_request(
               session, &composite_request, &composite_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    composite_request.vertical_fov_degrees = 90.0F;
    composite_request.rectilinear_output = 2;
    EXPECT(pano_gpu_test_validate_one_frame_composite_request(
               session, &composite_request, &composite_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    composite_request.rectilinear_output = 1;
    composite_request.output_vertical_fov_degrees = 0.0F;
    EXPECT(pano_gpu_test_validate_one_frame_composite_request(
               session, &composite_request, &composite_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    composite_request.output_vertical_fov_degrees = 180.0F;
    EXPECT(pano_gpu_test_validate_one_frame_composite_request(
               session, &composite_request, &composite_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    composite_request.output_vertical_fov_degrees = 90.0F;
    EXPECT(pano_gpu_test_validate_one_frame_composite_request(
               session, &composite_request, &composite_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    composite_request.rectilinear_output = 0;
    composite_request.output_vertical_fov_degrees = 0.0F;
    EXPECT(composite_layout.pixel_count == 16);
    EXPECT(composite_layout.linear_rgb_bytes == 16 * 3 * sizeof(float));
    EXPECT(composite_layout.coverage_bytes == 16);
    EXPECT(composite_layout.candidate_edge_distance_bytes == 16 * sizeof(float));
    std::array<float, 32> composite_coordinates {};
    std::array<uint8_t, 16> composite_validity {};
    EXPECT(pano_gpu_test_dispatch_one_frame_projection(
               session, &composite_request, composite_coordinates.data(), sizeof(composite_coordinates),
               composite_validity.data(), sizeof(composite_validity), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    const float composite_focal = 2.0F;
    for (uint32_t row = 0; row < composite_request.row_count; ++row)
    {
        for (uint32_t column = 0; column < composite_request.output_width; ++column)
        {
            const float longitude =
                ((static_cast<float>(column) + 0.5F) / composite_request.output_width - 0.5F) * 6.283185307179586F;
            const float latitude =
                (0.5F - (composite_request.row_start + static_cast<float>(row) + 0.5F) / composite_request.output_height) *
                composite_request.latitude_span_degrees * 0.0174532925199433F;
            const float cosine = std::cos(latitude);
            const float camera_x = cosine * std::sin(longitude);
            const float camera_y = std::sin(latitude);
            const float camera_z = cosine * std::cos(longitude);
            const float safe_z = std::fabs(camera_z) > 1.0e-8F ? camera_z : 1.0F;
            const float expected_x = 1.5F + composite_focal * camera_x / safe_z;
            const float expected_y = 1.5F - composite_focal * camera_y / safe_z;
            const size_t index = static_cast<size_t>(row) * composite_request.output_width + column;
            EXPECT(std::fabs(composite_coordinates[2 * index] - std::clamp(expected_x, 0.0F, 3.0F)) < 1.0e-4F);
            EXPECT(std::fabs(composite_coordinates[2 * index + 1] - std::clamp(expected_y, 0.0F, 3.0F)) < 1.0e-4F);
            EXPECT(composite_validity[index] ==
                   static_cast<uint8_t>(camera_z > 0.0F && expected_x >= -0.5F && expected_x <= 3.5F &&
                                        expected_y >= -0.5F && expected_y <= 3.5F));
        }
    }
    EXPECT(pano_gpu_test_validate_float32_sample_request(
               session, &float32_sample_request, uint16_sample_coordinates.data(), sizeof(uint16_sample_coordinates),
               &float32_sample_layout, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(float32_sample_layout.coordinate_bytes == sizeof(uint16_sample_coordinates));
    EXPECT(float32_sample_layout.sampled_rgb_bytes == 5 * 3 * sizeof(float));
    std::array<float, 2> float32_nonfinite_coordinates {0.0F, 0.0F};
    float32_sample_request.coordinate_count = 1;
    std::array<float, 3> float32_nonfinite_rgb {};
    EXPECT(pano_gpu_test_sample_float32(
               session, &float32_sample_request, float32_nonfinite_coordinates.data(),
               sizeof(float32_nonfinite_coordinates), float32_nonfinite_rgb.data(), sizeof(float32_nonfinite_rgb),
               error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(std::isnan(float32_nonfinite_rgb[0]));
    EXPECT(std::isnan(float32_nonfinite_rgb[1]));
    EXPECT(std::isnan(float32_nonfinite_rgb[2]));
    pano_gpu_session_destroy(&session);
    EXPECT(pano_gpu_session_create(
               device, &float32_options, &session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    for (size_t index = 0; index < float32_source_data.size(); ++index)
        float32_source_data[index] = static_cast<float>(index) * 0.125F - 1.0F;
    float32_upload.data = float32_source_data.data();
    EXPECT(pano_gpu_session_allocate_source(session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_upload_slot(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_upload_frame_zero(
               session, &float32_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_finish_uploads(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<float, 6> float32_center_coordinates {0.0F, 0.0F, 2.0F, 2.0F, 2.0F, 1.0F};
    float32_sample_request.coordinate_count = 3;
    std::array<float, 9> float32_sampled_rgb {};
    EXPECT(pano_gpu_test_sample_float32(
               session, &float32_sample_request, float32_center_coordinates.data(), sizeof(float32_center_coordinates),
               float32_sampled_rgb.data(), sizeof(float32_sampled_rgb), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (uint32_t sample_index = 0; sample_index < float32_sample_request.coordinate_count; ++sample_index)
    {
        const uint32_t source_x = static_cast<uint32_t>(float32_center_coordinates[2 * sample_index]);
        const uint32_t source_y = static_cast<uint32_t>(float32_center_coordinates[2 * sample_index + 1]);
        for (uint32_t channel = 0; channel < 3; ++channel)
            EXPECT(float32_sampled_rgb[3 * sample_index + channel] ==
                   float32_source_data[(source_y * 4 + source_x) * 3 + channel]);
    }
    std::array<float, 2> float32_half_pixel_coordinates {1.5F, 1.5F};
    float32_sample_request.coordinate_count = 1;
    std::array<float, 3> float32_half_pixel_rgb {};
    EXPECT(pano_gpu_test_sample_float32(
               session, &float32_sample_request, float32_half_pixel_coordinates.data(), sizeof(float32_half_pixel_coordinates),
               float32_half_pixel_rgb.data(), sizeof(float32_half_pixel_rgb), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (uint32_t channel = 0; channel < 3; ++channel)
    {
        const auto source_value = [&](const uint32_t source_x, const uint32_t source_y) {
            return float32_source_data[(source_y * 4 + source_x) * 3 + channel];
        };
        const float expected = (source_value(1, 1) + source_value(2, 1) + source_value(1, 2) + source_value(2, 2)) * 0.25F;
        EXPECT(std::fabs(float32_half_pixel_rgb[channel] - expected) < 1.0e-6F);
    }
    float32_sample_request.coordinate_count = 5;
    std::array<float, 15> float32_clipped_rgb {};
    EXPECT(pano_gpu_test_sample_float32(
               session, &float32_sample_request, uint16_sample_coordinates.data(), sizeof(uint16_sample_coordinates),
               float32_clipped_rgb.data(), sizeof(float32_clipped_rgb), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (uint32_t sample_index = 0; sample_index < float32_sample_request.coordinate_count; ++sample_index)
    {
        const float x = std::clamp(uint16_sample_coordinates[2 * sample_index], 0.0F, 3.0F);
        const float y = std::clamp(uint16_sample_coordinates[2 * sample_index + 1], 0.0F, 3.0F);
        const uint32_t x0 = static_cast<uint32_t>(std::floor(x));
        const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
        const uint32_t x1 = std::min(x0 + 1, 3U);
        const uint32_t y1 = std::min(y0 + 1, 3U);
        const float x_fraction = x - x0;
        const float y_fraction = y - y0;
        for (uint32_t channel = 0; channel < 3; ++channel)
        {
            const auto source_value = [&](const uint32_t source_x, const uint32_t source_y) {
                return float32_source_data[(source_y * 4 + source_x) * 3 + channel];
            };
            const float top = source_value(x0, y0) * (1.0F - x_fraction) + source_value(x1, y0) * x_fraction;
            const float bottom = source_value(x0, y1) * (1.0F - x_fraction) + source_value(x1, y1) * x_fraction;
            EXPECT(std::fabs(float32_clipped_rgb[3 * sample_index + channel] -
                             (top * (1.0F - y_fraction) + bottom * y_fraction)) < 1.0e-6F);
        }
    }
    pano_gpu_session_destroy(&session);
    pano_gpu_session_create_options uint8_candidate_options = session_options;
    uint8_candidate_options.source_sample_type = PANO_GPU_SAMPLE_UINT8;
    uint8_candidate_options.source_row_stride_bytes = 12;
    EXPECT(pano_gpu_session_create(
               device, &uint8_candidate_options, &session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<uint8_t, 48> uint8_candidate_source {};
    for (size_t index = 0; index < uint8_candidate_source.size(); ++index)
        uint8_candidate_source[index] = static_cast<uint8_t>(index * 17U + 3U);
    pano_gpu_source_upload uint8_candidate_upload {};
    uint8_candidate_upload.size = sizeof(uint8_candidate_upload);
    uint8_candidate_upload.abi_version = PANO_GPU_ABI_VERSION;
    uint8_candidate_upload.source_sample_type = PANO_GPU_SAMPLE_UINT8;
    uint8_candidate_upload.source_row_stride_bytes = 12;
    uint8_candidate_upload.data = uint8_candidate_source.data();
    uint8_candidate_upload.data_bytes = sizeof(uint8_candidate_source);
    EXPECT(pano_gpu_session_allocate_source(session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_upload_slot(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_upload_frame_zero(
               session, &uint8_candidate_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_finish_uploads(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    composite_request.source_sample_type = PANO_GPU_SAMPLE_UINT8;
    std::array<float, 48> uint8_candidates {};
    std::array<uint8_t, 16> uint8_candidate_validity {};
    std::array<float, 16> uint8_candidate_edge_distances {};
    EXPECT(pano_gpu_test_dispatch_one_frame_uint8_candidates(
               session, &composite_request, uint8_candidates.data(), sizeof(uint8_candidates),
               uint8_candidate_validity.data(), sizeof(uint8_candidate_validity), uint8_candidate_edge_distances.data(),
               sizeof(uint8_candidate_edge_distances), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (uint32_t row = 0; row < composite_request.row_count; ++row)
    {
        for (uint32_t column = 0; column < composite_request.output_width; ++column)
        {
            const float longitude =
                ((static_cast<float>(column) + 0.5F) / composite_request.output_width - 0.5F) * 6.283185307179586F;
            const float latitude =
                (0.5F - (composite_request.row_start + static_cast<float>(row) + 0.5F) / composite_request.output_height) *
                composite_request.latitude_span_degrees * 0.0174532925199433F;
            const float cosine = std::cos(latitude);
            const float camera_x = cosine * std::sin(longitude);
            const float camera_y = std::sin(latitude);
            const float camera_z = cosine * std::cos(longitude);
            const float safe_z = std::fabs(camera_z) > 1.0e-8F ? camera_z : 1.0F;
            const float projected_x = 1.5F + composite_focal * camera_x / safe_z;
            const float projected_y = 1.5F - composite_focal * camera_y / safe_z;
            const float x = std::clamp(projected_x, 0.0F, 3.0F);
            const float y = std::clamp(projected_y, 0.0F, 3.0F);
            const uint32_t x0 = static_cast<uint32_t>(std::floor(x));
            const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
            const uint32_t x1 = std::min(x0 + 1, 3U);
            const uint32_t y1 = std::min(y0 + 1, 3U);
            const float x_fraction = x - x0;
            const float y_fraction = y - y0;
            const size_t pixel = static_cast<size_t>(row) * composite_request.output_width + column;
            EXPECT(uint8_candidate_validity[pixel] ==
                   static_cast<uint8_t>(camera_z > 0.0F && projected_x >= -0.5F && projected_x <= 3.5F &&
                                        projected_y >= -0.5F && projected_y <= 3.5F));
            const float expected_edge_distance = std::min(std::min(x, y), std::min(3.0F - x, 3.0F - y));
            EXPECT(std::fabs(uint8_candidate_edge_distances[pixel] - expected_edge_distance) < 1.0e-5F);
            for (uint32_t channel = 0; channel < 3; ++channel)
            {
                const auto source_value = [&](const uint32_t source_x, const uint32_t source_y) {
                    const float encoded =
                        uint8_candidate_source[(source_y * 4 + source_x) * 3 + channel] / 255.0F;
                    return encoded <= 0.04045F
                        ? encoded / 12.92F
                        : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
                };
                const float top = source_value(x0, y0) * (1.0F - x_fraction) + source_value(x1, y0) * x_fraction;
                const float bottom = source_value(x0, y1) * (1.0F - x_fraction) + source_value(x1, y1) * x_fraction;
                EXPECT(std::fabs(uint8_candidates[3 * pixel + channel] -
                                 (top * (1.0F - y_fraction) + bottom * y_fraction)) < 1.0e-5F);
            }
        }
    }
    std::array<float, 48> uint8_prior_rgb {};
    std::array<float, 16> uint8_prior_weight {};
    std::array<float, 48> uint8_selected_rgb {};
    std::array<float, 16> uint8_selected_weight {};
    std::array<uint8_t, 16> uint8_selected_coverage {};
    EXPECT(pano_gpu_test_dispatch_one_frame_uint8_hard_selection(
               session, &composite_request, uint8_prior_rgb.data(), sizeof(uint8_prior_rgb), uint8_prior_weight.data(),
               sizeof(uint8_prior_weight), uint8_selected_rgb.data(), sizeof(uint8_selected_rgb),
               uint8_selected_weight.data(), sizeof(uint8_selected_weight), uint8_selected_coverage.data(),
               sizeof(uint8_selected_coverage), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < uint8_candidate_validity.size(); ++pixel)
    {
        const float expected_weight = uint8_candidate_validity[pixel] != 0
            ? std::max(uint8_candidate_edge_distances[pixel], 1.0e-6F)
            : 0.0F;
        EXPECT(std::fabs(uint8_selected_weight[pixel] - expected_weight) < 1.0e-6F);
        EXPECT(uint8_selected_coverage[pixel] == static_cast<uint8_t>(expected_weight > 0.0F));
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const float expected_rgb = uint8_candidate_validity[pixel] != 0
                ? uint8_candidates[3 * pixel + channel]
                : 0.0F;
            EXPECT(std::fabs(uint8_selected_rgb[3 * pixel + channel] - expected_rgb) < 1.0e-5F);
        }
    }
    pano_gpu_session_destroy(&session);
    EXPECT(pano_gpu_session_create(
               device, &uint16_options, &session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_source(session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_upload_slot(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_upload_frame_zero(
               session, &uint16_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_finish_uploads(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    composite_request.source_sample_type = PANO_GPU_SAMPLE_UINT16;
    std::array<float, 48> uint16_candidates {};
    std::array<uint8_t, 16> uint16_candidate_validity {};
    std::array<float, 16> uint16_candidate_edge_distances {};
    EXPECT(pano_gpu_test_dispatch_one_frame_uint16_candidates(
               session, &composite_request, uint16_candidates.data(), sizeof(uint16_candidates),
               uint16_candidate_validity.data(), sizeof(uint16_candidate_validity), uint16_candidate_edge_distances.data(),
               sizeof(uint16_candidate_edge_distances), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    composite_request.rectilinear_output = 1;
    composite_request.output_vertical_fov_degrees = 90.0F;
    std::array<float, 48> uint16_rectilinear_candidates {};
    std::array<uint8_t, 16> uint16_rectilinear_validity {};
    std::array<float, 16> uint16_rectilinear_edge_distances {};
    EXPECT(pano_gpu_test_dispatch_one_frame_uint16_candidates(
               session, &composite_request, uint16_rectilinear_candidates.data(),
               sizeof(uint16_rectilinear_candidates), uint16_rectilinear_validity.data(),
               sizeof(uint16_rectilinear_validity), uint16_rectilinear_edge_distances.data(),
               sizeof(uint16_rectilinear_edge_distances), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(uint16_rectilinear_candidates != uint16_candidates);
    EXPECT(uint16_rectilinear_validity != uint16_candidate_validity);
    composite_request.rectilinear_output = 0;
    composite_request.output_vertical_fov_degrees = 0.0F;
    for (uint32_t row = 0; row < composite_request.row_count; ++row)
    {
        for (uint32_t column = 0; column < composite_request.output_width; ++column)
        {
            const float longitude =
                ((static_cast<float>(column) + 0.5F) / composite_request.output_width - 0.5F) * 6.283185307179586F;
            const float latitude =
                (0.5F - (composite_request.row_start + static_cast<float>(row) + 0.5F) / composite_request.output_height) *
                composite_request.latitude_span_degrees * 0.0174532925199433F;
            const float cosine = std::cos(latitude);
            const float camera_x = cosine * std::sin(longitude);
            const float camera_y = std::sin(latitude);
            const float camera_z = cosine * std::cos(longitude);
            const float safe_z = std::fabs(camera_z) > 1.0e-8F ? camera_z : 1.0F;
            const float projected_x = 1.5F + composite_focal * camera_x / safe_z;
            const float projected_y = 1.5F - composite_focal * camera_y / safe_z;
            const float x = std::clamp(projected_x, 0.0F, 3.0F);
            const float y = std::clamp(projected_y, 0.0F, 3.0F);
            const uint32_t x0 = static_cast<uint32_t>(std::floor(x));
            const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
            const uint32_t x1 = std::min(x0 + 1, 3U);
            const uint32_t y1 = std::min(y0 + 1, 3U);
            const float x_fraction = x - x0;
            const float y_fraction = y - y0;
            const size_t pixel = static_cast<size_t>(row) * composite_request.output_width + column;
            EXPECT(uint16_candidate_validity[pixel] ==
                   static_cast<uint8_t>(camera_z > 0.0F && projected_x >= -0.5F && projected_x <= 3.5F &&
                                        projected_y >= -0.5F && projected_y <= 3.5F));
            const float expected_edge_distance = std::min(std::min(x, y), std::min(3.0F - x, 3.0F - y));
            EXPECT(std::fabs(uint16_candidate_edge_distances[pixel] - expected_edge_distance) < 1.0e-5F);
            for (uint32_t channel = 0; channel < 3; ++channel)
            {
                const auto source_value = [&](const uint32_t source_x, const uint32_t source_y) {
                    return uint16_source_data[(source_y * 4 + source_x) * 3 + channel] / 65535.0F;
                };
                const float top = source_value(x0, y0) * (1.0F - x_fraction) + source_value(x1, y0) * x_fraction;
                const float bottom = source_value(x0, y1) * (1.0F - x_fraction) + source_value(x1, y1) * x_fraction;
                EXPECT(std::fabs(uint16_candidates[3 * pixel + channel] -
                                 (top * (1.0F - y_fraction) + bottom * y_fraction)) < 1.0e-5F);
            }
        }
    }
    std::array<float, 48> uint16_prior_rgb {};
    std::array<float, 16> uint16_prior_weight {};
    std::array<float, 48> uint16_selected_rgb {};
    std::array<float, 16> uint16_selected_weight {};
    std::array<uint8_t, 16> uint16_selected_coverage {};
    EXPECT(pano_gpu_test_dispatch_one_frame_uint16_hard_selection(
               session, &composite_request, uint16_prior_rgb.data(), sizeof(uint16_prior_rgb), uint16_prior_weight.data(),
               sizeof(uint16_prior_weight), uint16_selected_rgb.data(), sizeof(uint16_selected_rgb),
               uint16_selected_weight.data(), sizeof(uint16_selected_weight), uint16_selected_coverage.data(),
               sizeof(uint16_selected_coverage), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < uint16_candidate_validity.size(); ++pixel)
    {
        const float expected_weight = uint16_candidate_validity[pixel] != 0
            ? std::max(uint16_candidate_edge_distances[pixel], 1.0e-6F)
            : 0.0F;
        EXPECT(std::fabs(uint16_selected_weight[pixel] - expected_weight) < 1.0e-6F);
        EXPECT(uint16_selected_coverage[pixel] == static_cast<uint8_t>(expected_weight > 0.0F));
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const float expected_rgb = uint16_candidate_validity[pixel] != 0
                ? uint16_candidates[3 * pixel + channel]
                : 0.0F;
            EXPECT(std::fabs(uint16_selected_rgb[3 * pixel + channel] - expected_rgb) < 1.0e-5F);
        }
    }
    pano_gpu_session_destroy(&session);
    EXPECT(pano_gpu_session_create(
               device, &float32_options, &session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    float32_upload.data = float32_source_data.data();
    EXPECT(pano_gpu_session_allocate_source(session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_upload_slot(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_upload_frame_zero(
               session, &float32_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_finish_uploads(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    composite_request.source_sample_type = PANO_GPU_SAMPLE_FLOAT32;
    std::array<float, 48> float32_candidates {};
    std::array<uint8_t, 16> float32_candidate_validity {};
    std::array<float, 16> float32_candidate_edge_distances {};
    EXPECT(pano_gpu_test_dispatch_one_frame_float32_candidates(
               session, &composite_request, float32_candidates.data(), sizeof(float32_candidates),
               float32_candidate_validity.data(), sizeof(float32_candidate_validity), float32_candidate_edge_distances.data(),
               sizeof(float32_candidate_edge_distances), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (uint32_t row = 0; row < composite_request.row_count; ++row)
    {
        for (uint32_t column = 0; column < composite_request.output_width; ++column)
        {
            const float longitude =
                ((static_cast<float>(column) + 0.5F) / composite_request.output_width - 0.5F) * 6.283185307179586F;
            const float latitude =
                (0.5F - (composite_request.row_start + static_cast<float>(row) + 0.5F) / composite_request.output_height) *
                composite_request.latitude_span_degrees * 0.0174532925199433F;
            const float cosine = std::cos(latitude);
            const float camera_x = cosine * std::sin(longitude);
            const float camera_y = std::sin(latitude);
            const float camera_z = cosine * std::cos(longitude);
            const float safe_z = std::fabs(camera_z) > 1.0e-8F ? camera_z : 1.0F;
            const float projected_x = 1.5F + composite_focal * camera_x / safe_z;
            const float projected_y = 1.5F - composite_focal * camera_y / safe_z;
            const float x = std::clamp(projected_x, 0.0F, 3.0F);
            const float y = std::clamp(projected_y, 0.0F, 3.0F);
            const uint32_t x0 = static_cast<uint32_t>(std::floor(x));
            const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
            const uint32_t x1 = std::min(x0 + 1, 3U);
            const uint32_t y1 = std::min(y0 + 1, 3U);
            const float x_fraction = x - x0;
            const float y_fraction = y - y0;
            const size_t pixel = static_cast<size_t>(row) * composite_request.output_width + column;
            EXPECT(float32_candidate_validity[pixel] ==
                   static_cast<uint8_t>(camera_z > 0.0F && projected_x >= -0.5F && projected_x <= 3.5F &&
                                        projected_y >= -0.5F && projected_y <= 3.5F));
            const float expected_edge_distance = std::min(std::min(x, y), std::min(3.0F - x, 3.0F - y));
            EXPECT(std::fabs(float32_candidate_edge_distances[pixel] - expected_edge_distance) < 1.0e-5F);
            for (uint32_t channel = 0; channel < 3; ++channel)
            {
                const auto source_value = [&](const uint32_t source_x, const uint32_t source_y) {
                    return float32_source_data[(source_y * 4 + source_x) * 3 + channel];
                };
                const float top = source_value(x0, y0) * (1.0F - x_fraction) + source_value(x1, y0) * x_fraction;
                const float bottom = source_value(x0, y1) * (1.0F - x_fraction) + source_value(x1, y1) * x_fraction;
                EXPECT(std::fabs(float32_candidates[3 * pixel + channel] -
                                 (top * (1.0F - y_fraction) + bottom * y_fraction)) < 1.0e-5F);
            }
        }
    }
    std::array<float, 48> float32_prior_rgb {};
    std::array<float, 16> float32_prior_weight {};
    std::array<float, 48> float32_selected_rgb {};
    std::array<float, 16> float32_selected_weight {};
    std::array<uint8_t, 16> float32_selected_coverage {};
    EXPECT(pano_gpu_test_dispatch_one_frame_float32_hard_selection(
               session, &composite_request, float32_prior_rgb.data(), sizeof(float32_prior_rgb),
               float32_prior_weight.data(), sizeof(float32_prior_weight), float32_selected_rgb.data(),
               sizeof(float32_selected_rgb), float32_selected_weight.data(), sizeof(float32_selected_weight),
               float32_selected_coverage.data(), sizeof(float32_selected_coverage), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < float32_candidate_validity.size(); ++pixel)
    {
        const float expected_weight = float32_candidate_validity[pixel] != 0
            ? std::max(float32_candidate_edge_distances[pixel], 1.0e-6F)
            : 0.0F;
        EXPECT(std::fabs(float32_selected_weight[pixel] - expected_weight) < 1.0e-6F);
        EXPECT(float32_selected_coverage[pixel] == static_cast<uint8_t>(expected_weight > 0.0F));
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const float expected_rgb = float32_candidate_validity[pixel] != 0
                ? float32_candidates[3 * pixel + channel]
                : 0.0F;
            EXPECT(std::fabs(float32_selected_rgb[3 * pixel + channel] - expected_rgb) < 1.0e-5F);
        }
    }
    pano_gpu_session_destroy(&session);
    EXPECT(pano_gpu_session_create(
               device, &session_options, &session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    pano_gpu_hard_selection_request hard_selection_request {};
    hard_selection_request.size = sizeof(hard_selection_request);
    hard_selection_request.abi_version = PANO_GPU_ABI_VERSION;
    hard_selection_request.pixel_count = 4;
    std::array<float, 12> hard_candidate_rgb {};
    std::array<uint8_t, 4> hard_candidate_validity {1, 0, 1, 1};
    std::array<float, 4> hard_candidate_edge_distance {1.0F, 0.0F, 0.5F, 2.0F};
    std::array<float, 12> hard_prior_rgb {};
    std::array<float, 4> hard_prior_weight {0.0F, 0.0F, 1.0F, 3.0F};
    pano_gpu_hard_selection_result_layout hard_selection_layout {};
    hard_selection_layout.size = sizeof(hard_selection_layout);
    hard_selection_layout.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_test_validate_hard_selection_request(
               session, &hard_selection_request, hard_candidate_rgb.data(), sizeof(hard_candidate_rgb),
               hard_candidate_validity.data(), sizeof(hard_candidate_validity), hard_candidate_edge_distance.data(),
               sizeof(hard_candidate_edge_distance), hard_prior_rgb.data(), sizeof(hard_prior_rgb), hard_prior_weight.data(),
               sizeof(hard_prior_weight), &hard_selection_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(hard_selection_layout.selected_rgb_bytes == sizeof(hard_candidate_rgb));
    EXPECT(hard_selection_layout.selected_weight_bytes == sizeof(hard_candidate_edge_distance));
    EXPECT(hard_selection_layout.coverage_bytes == hard_candidate_validity.size());
    hard_candidate_validity[1] = 2;
    EXPECT(pano_gpu_test_validate_hard_selection_request(
               session, &hard_selection_request, hard_candidate_rgb.data(), sizeof(hard_candidate_rgb),
               hard_candidate_validity.data(), sizeof(hard_candidate_validity), hard_candidate_edge_distance.data(),
               sizeof(hard_candidate_edge_distance), hard_prior_rgb.data(), sizeof(hard_prior_rgb), hard_prior_weight.data(),
               sizeof(hard_prior_weight), &hard_selection_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    hard_candidate_validity[1] = 0;
    hard_candidate_edge_distance[0] = std::numeric_limits<float>::infinity();
    EXPECT(pano_gpu_test_validate_hard_selection_request(
               session, &hard_selection_request, hard_candidate_rgb.data(), sizeof(hard_candidate_rgb),
               hard_candidate_validity.data(), sizeof(hard_candidate_validity), hard_candidate_edge_distance.data(),
               sizeof(hard_candidate_edge_distance), hard_prior_rgb.data(), sizeof(hard_prior_rgb), hard_prior_weight.data(),
               sizeof(hard_prior_weight), &hard_selection_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    hard_candidate_edge_distance[0] = 1.0F;
    hard_prior_weight[0] = -1.0F;
    EXPECT(pano_gpu_test_validate_hard_selection_request(
               session, &hard_selection_request, hard_candidate_rgb.data(), sizeof(hard_candidate_rgb),
               hard_candidate_validity.data(), sizeof(hard_candidate_validity), hard_candidate_edge_distance.data(),
               sizeof(hard_candidate_edge_distance), hard_prior_rgb.data(), sizeof(hard_prior_rgb), hard_prior_weight.data(),
               sizeof(hard_prior_weight), &hard_selection_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    hard_candidate_rgb = {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F, 0.0F};
    hard_prior_rgb = {0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F, 0.7F, 0.8F, 0.9F, 0.2F, 0.3F, 0.4F};
    hard_candidate_validity = {1, 0, 1, 1};
    hard_candidate_edge_distance = {1.0F, 0.0F, 0.5F, 2.0F};
    hard_prior_weight = {0.0F, 0.0F, 0.5F, 3.0F};
    pano_gpu_feather_accumulation_request feather_request {};
    feather_request.size = sizeof(feather_request);
    feather_request.abi_version = PANO_GPU_ABI_VERSION;
    feather_request.pixel_count = 4;
    feather_request.source_width = 4;
    feather_request.source_height = 4;
    pano_gpu_feather_accumulation_result_layout feather_layout {};
    feather_layout.size = sizeof(feather_layout);
    feather_layout.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_test_validate_feather_accumulation_request(
               session, &feather_request, hard_candidate_rgb.data(), sizeof(hard_candidate_rgb),
               hard_candidate_validity.data(), sizeof(hard_candidate_validity), hard_candidate_edge_distance.data(),
               sizeof(hard_candidate_edge_distance), hard_prior_rgb.data(), sizeof(hard_prior_rgb),
               hard_prior_weight.data(), sizeof(hard_prior_weight), &feather_layout, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(feather_layout.accumulator_rgb_bytes == sizeof(hard_candidate_rgb));
    EXPECT(feather_layout.accumulator_weight_bytes == sizeof(hard_candidate_edge_distance));
    hard_candidate_validity[0] = 2;
    EXPECT(pano_gpu_test_validate_feather_accumulation_request(
               session, &feather_request, hard_candidate_rgb.data(), sizeof(hard_candidate_rgb),
               hard_candidate_validity.data(), sizeof(hard_candidate_validity), hard_candidate_edge_distance.data(),
               sizeof(hard_candidate_edge_distance), hard_prior_rgb.data(), sizeof(hard_prior_rgb),
               hard_prior_weight.data(), sizeof(hard_prior_weight), &feather_layout, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    hard_candidate_validity[0] = 1;
    hard_candidate_edge_distance[0] = std::numeric_limits<float>::infinity();
    EXPECT(pano_gpu_test_validate_feather_accumulation_request(
               session, &feather_request, hard_candidate_rgb.data(), sizeof(hard_candidate_rgb),
               hard_candidate_validity.data(), sizeof(hard_candidate_validity), hard_candidate_edge_distance.data(),
               sizeof(hard_candidate_edge_distance), hard_prior_rgb.data(), sizeof(hard_prior_rgb),
               hard_prior_weight.data(), sizeof(hard_prior_weight), &feather_layout, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    hard_candidate_edge_distance[0] = 1.0F;
    feather_request.source_width = 0;
    EXPECT(pano_gpu_test_validate_feather_accumulation_request(
               session, &feather_request, hard_candidate_rgb.data(), sizeof(hard_candidate_rgb),
               hard_candidate_validity.data(), sizeof(hard_candidate_validity), hard_candidate_edge_distance.data(),
               sizeof(hard_candidate_edge_distance), hard_prior_rgb.data(), sizeof(hard_prior_rgb),
               hard_prior_weight.data(), sizeof(hard_prior_weight), &feather_layout, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    feather_request.source_width = 4;
    std::array<float, 4> feather_weight {};
    EXPECT(pano_gpu_test_dispatch_feather_weights(
               session, &feather_request, hard_candidate_validity.data(), sizeof(hard_candidate_validity),
               hard_candidate_edge_distance.data(), sizeof(hard_candidate_edge_distance), feather_weight.data(),
               sizeof(feather_weight), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const std::array<float, 4> expected_feather_weight {1.0F, 0.0F, 0.5F, 2.0F};
    for (size_t index = 0; index < feather_weight.size(); ++index)
        EXPECT(std::fabs(feather_weight[index] - expected_feather_weight[index]) < 1.0e-6F);
    feather_request.source_width = 1;
    feather_request.source_height = 1;
    hard_candidate_validity = {1, 1, 0, 1};
    hard_candidate_edge_distance = {0.0F, 0.5F, 3.0F, 2.0F};
    EXPECT(pano_gpu_test_dispatch_feather_weights(
               session, &feather_request, hard_candidate_validity.data(), sizeof(hard_candidate_validity),
               hard_candidate_edge_distance.data(), sizeof(hard_candidate_edge_distance), feather_weight.data(),
               sizeof(feather_weight), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const std::array<float, 4> expected_minimum_feather_weight {1.0e-6F, 0.5F, 0.0F, 2.0F};
    for (size_t index = 0; index < feather_weight.size(); ++index)
        EXPECT(std::fabs(feather_weight[index] - expected_minimum_feather_weight[index]) < 1.0e-6F);
    feather_request.source_width = 4;
    feather_request.source_height = 4;
    hard_candidate_validity = {1, 0, 1, 1};
    hard_candidate_edge_distance = {1.0F, 0.0F, 0.5F, 2.0F};
    pano_gpu_exposure_request exposure_request {};
    exposure_request.size = sizeof(exposure_request);
    exposure_request.abi_version = PANO_GPU_ABI_VERSION;
    exposure_request.frame_count = 1;
    exposure_request.output_width = 8;
    exposure_request.output_height = 4;
    exposure_request.local_field_width = 2;
    exposure_request.local_field_height = 1;
    const std::array<float, 1> global_gain {1.25F};
    std::array<float, 2> local_exposure_field {0.0F, -0.25F};
    pano_gpu_exposure_result_layout exposure_layout {};
    exposure_layout.size = sizeof(exposure_layout);
    exposure_layout.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_test_validate_exposure_request(
               session, &exposure_request, global_gain.data(), sizeof(global_gain), local_exposure_field.data(),
               sizeof(local_exposure_field), &exposure_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(exposure_layout.global_gain_bytes == sizeof(global_gain));
    EXPECT(exposure_layout.local_field_bytes == sizeof(local_exposure_field));
    const std::array<float, 1> invalid_global_gain {std::numeric_limits<float>::infinity()};
    EXPECT(pano_gpu_test_validate_exposure_request(
               session, &exposure_request, invalid_global_gain.data(), sizeof(invalid_global_gain),
               local_exposure_field.data(), sizeof(local_exposure_field), &exposure_layout, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    exposure_request.frame_count = 2;
    EXPECT(pano_gpu_test_validate_exposure_request(
               session, &exposure_request, global_gain.data(), sizeof(global_gain), local_exposure_field.data(),
               sizeof(local_exposure_field), &exposure_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    exposure_request.frame_count = 1;
    local_exposure_field[0] = std::numeric_limits<float>::infinity();
    EXPECT(pano_gpu_test_validate_exposure_request(
               session, &exposure_request, global_gain.data(), sizeof(global_gain), local_exposure_field.data(),
               sizeof(local_exposure_field), &exposure_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    local_exposure_field[0] = 0.0F;
    exposure_request.local_field_width = 1;
    EXPECT(pano_gpu_test_validate_exposure_request(
               session, &exposure_request, global_gain.data(), sizeof(global_gain), local_exposure_field.data(),
               sizeof(local_exposure_field), &exposure_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    exposure_request.local_field_width = 2;
    pano_gpu_one_frame_composite_request local_field_request = composite_request;
    local_field_request.output_width = 4;
    local_field_request.output_height = 4;
    local_field_request.row_start = 0;
    local_field_request.row_count = 4;
    std::array<float, 1> built_local_field {};
    EXPECT(pano_gpu_test_dispatch_one_frame_equirect_local_exposure(
               session, &local_field_request, 1.25F, built_local_field.data(), sizeof(built_local_field), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(std::fabs(built_local_field[0] - std::log(1.25F)) < 1.0e-6F);
    local_field_request.rectilinear_output = 1;
    local_field_request.output_vertical_fov_degrees = 90.0F;
    EXPECT(pano_gpu_test_dispatch_one_frame_rectilinear_local_exposure(
               session, &local_field_request, 1.25F, built_local_field.data(), sizeof(built_local_field), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(std::fabs(built_local_field[0] - std::log(1.25F)) < 1.0e-6F);
    std::array<float, 12> gained_rgb {};
    EXPECT(pano_gpu_test_dispatch_global_gain(
               session, 4, 1.0F, hard_candidate_rgb.data(), sizeof(hard_candidate_rgb), gained_rgb.data(),
               sizeof(gained_rgb), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(gained_rgb == hard_candidate_rgb);
    EXPECT(pano_gpu_test_dispatch_global_gain(
               session, 4, 2.5F, hard_candidate_rgb.data(), sizeof(hard_candidate_rgb), gained_rgb.data(),
               sizeof(gained_rgb), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t index = 0; index < gained_rgb.size(); ++index)
        EXPECT(std::fabs(gained_rgb[index] - hard_candidate_rgb[index] * 2.5F) < 1.0e-6F);
    std::array<float, 96> local_candidate_rgb {};
    local_candidate_rgb.fill(1.0F);
    const std::array<float, 2> sampled_local_field {0.0F, std::log(4.0F)};
    std::array<float, 96> locally_adjusted_rgb {};
    EXPECT(pano_gpu_test_dispatch_local_exposure(
               session, 8, 4, 0, 4, local_candidate_rgb.data(), sizeof(local_candidate_rgb),
               sampled_local_field.data(), sizeof(sampled_local_field), locally_adjusted_rgb.data(),
               sizeof(locally_adjusted_rgb), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(std::fabs(locally_adjusted_rgb[0] - 1.0F) < 1.0e-6F);
    const float interior_gain = std::exp(0.625F * std::log(4.0F));
    EXPECT(std::fabs(locally_adjusted_rgb[3 * 4] - interior_gain) < 1.0e-5F);
    const std::array<float, 2> invalid_sampled_local_field {std::numeric_limits<float>::infinity(), 0.0F};
    EXPECT(pano_gpu_test_dispatch_local_exposure(
               session, 8, 4, 0, 4, local_candidate_rgb.data(), sizeof(local_candidate_rgb),
               invalid_sampled_local_field.data(), sizeof(invalid_sampled_local_field), locally_adjusted_rgb.data(),
               sizeof(locally_adjusted_rgb), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    const std::array<float, 4> candidate_feather_weight {1.0F, 0.0F, 0.5F, 2.0F};
    std::array<float, 12> accumulated_rgb {};
    std::array<float, 4> accumulated_weight {};
    EXPECT(pano_gpu_test_dispatch_feather_accumulation(
               session, &feather_request, hard_candidate_rgb.data(), sizeof(hard_candidate_rgb),
               candidate_feather_weight.data(), sizeof(candidate_feather_weight), hard_prior_rgb.data(),
               sizeof(hard_prior_rgb), hard_prior_weight.data(), sizeof(hard_prior_weight), accumulated_rgb.data(),
               sizeof(accumulated_rgb), accumulated_weight.data(), sizeof(accumulated_weight), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const std::array<float, 12> expected_accumulated_rgb {
        1.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F, 0.7F, 0.8F, 1.4F, 2.2F, 2.3F, 0.4F};
    const std::array<float, 4> expected_accumulated_weight {1.0F, 0.0F, 1.0F, 5.0F};
    for (size_t index = 0; index < accumulated_rgb.size(); ++index)
        EXPECT(std::fabs(accumulated_rgb[index] - expected_accumulated_rgb[index]) < 1.0e-6F);
    for (size_t index = 0; index < accumulated_weight.size(); ++index)
        EXPECT(std::fabs(accumulated_weight[index] - expected_accumulated_weight[index]) < 1.0e-6F);
    const std::array<float, 12> second_candidate_rgb {
        0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.5F, 0.5F, 0.5F, 2.0F, 0.0F, 0.0F};
    const std::array<float, 4> first_candidate_weight {1.0F, 0.0F, 0.5F, 2.0F};
    const std::array<float, 4> second_candidate_weight {0.5F, 2.0F, 1.0F, 0.0F};
    EXPECT(pano_gpu_test_dispatch_two_frame_feather_accumulation(
               session, &feather_request, hard_candidate_rgb.data(), sizeof(hard_candidate_rgb),
               first_candidate_weight.data(), sizeof(first_candidate_weight), second_candidate_rgb.data(),
               sizeof(second_candidate_rgb), second_candidate_weight.data(), sizeof(second_candidate_weight),
               accumulated_rgb.data(), sizeof(accumulated_rgb), accumulated_weight.data(), sizeof(accumulated_weight),
               error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const std::array<float, 12> expected_two_frame_accumulated_rgb {
        1.0F, 0.5F, 0.0F, 2.0F, 0.0F, 2.0F, 0.5F, 0.5F, 1.0F, 2.0F, 2.0F, 0.0F};
    const std::array<float, 4> expected_two_frame_accumulated_weight {1.5F, 2.0F, 1.5F, 2.0F};
    for (size_t index = 0; index < accumulated_rgb.size(); ++index)
        EXPECT(std::fabs(accumulated_rgb[index] - expected_two_frame_accumulated_rgb[index]) < 1.0e-6F);
    for (size_t index = 0; index < accumulated_weight.size(); ++index)
        EXPECT(std::fabs(accumulated_weight[index] - expected_two_frame_accumulated_weight[index]) < 1.0e-6F);
    const std::array<float, 12> third_candidate_rgb {
        0.0F, 0.0F, 1.0F, 0.25F, 0.25F, 0.25F, 2.0F, 0.0F, 2.0F, 0.0F, 1.0F, 1.0F};
    const std::array<float, 4> third_candidate_weight {0.25F, 0.5F, 0.0F, 3.0F};
    EXPECT(pano_gpu_test_dispatch_three_frame_feather_accumulation(
               session, &feather_request, hard_candidate_rgb.data(), sizeof(hard_candidate_rgb),
               first_candidate_weight.data(), sizeof(first_candidate_weight), second_candidate_rgb.data(),
               sizeof(second_candidate_rgb), second_candidate_weight.data(), sizeof(second_candidate_weight),
               third_candidate_rgb.data(), sizeof(third_candidate_rgb), third_candidate_weight.data(),
               sizeof(third_candidate_weight), accumulated_rgb.data(), sizeof(accumulated_rgb), accumulated_weight.data(),
               sizeof(accumulated_weight), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const std::array<float, 12> expected_three_frame_accumulated_rgb {
        1.0F, 0.5F, 0.25F, 2.125F, 0.125F, 2.125F, 0.5F, 0.5F, 1.0F, 2.0F, 5.0F, 3.0F};
    const std::array<float, 4> expected_three_frame_accumulated_weight {1.75F, 2.5F, 1.5F, 5.0F};
    for (size_t index = 0; index < accumulated_rgb.size(); ++index)
        EXPECT(std::fabs(accumulated_rgb[index] - expected_three_frame_accumulated_rgb[index]) < 1.0e-6F);
    for (size_t index = 0; index < accumulated_weight.size(); ++index)
        EXPECT(std::fabs(accumulated_weight[index] - expected_three_frame_accumulated_weight[index]) < 1.0e-6F);
    std::array<float, 12> ordered_first_rgb {};
    std::array<float, 12> ordered_second_rgb {};
    std::array<float, 12> ordered_third_rgb {};
    ordered_first_rgb[0] = 1.0e20F;
    ordered_second_rgb[0] = -1.0e20F;
    ordered_third_rgb[0] = 1.0F;
    const std::array<float, 4> ordered_feather_weight {1.0F, 0.0F, 0.0F, 0.0F};
    EXPECT(pano_gpu_test_dispatch_three_frame_feather_accumulation(
               session, &feather_request, ordered_first_rgb.data(), sizeof(ordered_first_rgb),
               ordered_feather_weight.data(), sizeof(ordered_feather_weight), ordered_second_rgb.data(), sizeof(ordered_second_rgb),
               ordered_feather_weight.data(), sizeof(ordered_feather_weight), ordered_third_rgb.data(), sizeof(ordered_third_rgb),
               ordered_feather_weight.data(), sizeof(ordered_feather_weight), accumulated_rgb.data(), sizeof(accumulated_rgb),
               accumulated_weight.data(), sizeof(accumulated_weight), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(accumulated_rgb[0] == 1.0F);
    EXPECT(accumulated_weight[0] == 3.0F);
    const void *one_frame_rgb[] = {hard_candidate_rgb.data()};
    const uint64_t one_frame_rgb_bytes[] = {sizeof(hard_candidate_rgb)};
    const void *one_frame_weight[] = {first_candidate_weight.data()};
    const uint64_t one_frame_weight_bytes[] = {sizeof(first_candidate_weight)};
    EXPECT(pano_gpu_test_dispatch_feather_accumulation_chain(
               session, &feather_request, one_frame_rgb, one_frame_rgb_bytes, one_frame_weight,
               one_frame_weight_bytes, 1, accumulated_rgb.data(), sizeof(accumulated_rgb),
               accumulated_weight.data(), sizeof(accumulated_weight), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < accumulated_weight.size(); ++pixel)
    {
        EXPECT(std::fabs(accumulated_weight[pixel] - first_candidate_weight[pixel]) < 1.0e-6F);
        for (size_t channel = 0; channel < 3; ++channel)
            EXPECT(std::fabs(accumulated_rgb[3 * pixel + channel] -
                             hard_candidate_rgb[3 * pixel + channel] * first_candidate_weight[pixel]) < 1.0e-6F);
    }
    std::array<float, 12> ordered_fourth_rgb {};
    ordered_fourth_rgb[0] = 2.0F;
    const void *feather_four_frame_rgb[] = {
        ordered_first_rgb.data(), ordered_second_rgb.data(), ordered_third_rgb.data(), ordered_fourth_rgb.data()};
    const uint64_t feather_four_frame_rgb_bytes[] = {
        sizeof(ordered_first_rgb), sizeof(ordered_second_rgb), sizeof(ordered_third_rgb), sizeof(ordered_fourth_rgb)};
    const void *feather_four_frame_weight[] = {
        ordered_feather_weight.data(), ordered_feather_weight.data(), ordered_feather_weight.data(),
        ordered_feather_weight.data()};
    const uint64_t feather_four_frame_weight_bytes[] = {
        sizeof(ordered_feather_weight), sizeof(ordered_feather_weight), sizeof(ordered_feather_weight),
        sizeof(ordered_feather_weight)};
    EXPECT(pano_gpu_test_dispatch_feather_accumulation_chain(
               session, &feather_request, feather_four_frame_rgb, feather_four_frame_rgb_bytes,
               feather_four_frame_weight, feather_four_frame_weight_bytes, 4, accumulated_rgb.data(),
               sizeof(accumulated_rgb),
               accumulated_weight.data(), sizeof(accumulated_weight), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(accumulated_rgb[0] == 3.0F);
    EXPECT(accumulated_weight[0] == 4.0F);
    const std::array<float, 12> normalization_input_rgb {
        2.0F, 4.0F, 6.0F, 7.0F, 8.0F, 9.0F, 3.0F, 6.0F, 9.0F, 1.0F, 2.0F, 3.0F};
    const std::array<float, 4> normalization_input_weight {2.0F, 0.0F, 0.5F, 3.0F};
    EXPECT(pano_gpu_test_dispatch_feather_normalize(
               session, &feather_request, normalization_input_rgb.data(), sizeof(normalization_input_rgb),
               normalization_input_weight.data(), sizeof(normalization_input_weight), accumulated_rgb.data(),
               sizeof(accumulated_rgb), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const std::array<float, 12> expected_normalized_rgb {
        1.0F, 2.0F, 3.0F, 7.0F, 8.0F, 9.0F, 6.0F, 12.0F, 18.0F, 1.0F / 3.0F, 2.0F / 3.0F, 1.0F};
    for (size_t index = 0; index < accumulated_rgb.size(); ++index)
        EXPECT(std::fabs(accumulated_rgb[index] - expected_normalized_rgb[index]) < 1.0e-6F);
    std::array<float, 12> feather_incomplete_marked_rgb {};
    EXPECT(pano_gpu_test_dispatch_mark_incomplete(
               session, 4, accumulated_rgb.data(), sizeof(accumulated_rgb), normalization_input_weight.data(),
               sizeof(normalization_input_weight), feather_incomplete_marked_rgb.data(),
               sizeof(feather_incomplete_marked_rgb), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    const std::array<float, 12> expected_feather_incomplete_marked_rgb {
        1.0F, 2.0F, 3.0F, 1.0F, 0.0F, 1.0F, 6.0F, 12.0F, 18.0F, 1.0F / 3.0F, 2.0F / 3.0F, 1.0F};
    for (size_t index = 0; index < feather_incomplete_marked_rgb.size(); ++index)
        EXPECT(std::fabs(feather_incomplete_marked_rgb[index] - expected_feather_incomplete_marked_rgb[index]) < 1.0e-6F);
    constexpr uint32_t band_width = 8;
    constexpr uint32_t band_rows = 32;
    constexpr uint32_t full_band_rows = 64;
    constexpr uint32_t band_pixels = band_width * band_rows;
    constexpr uint32_t full_band_pixels = band_width * full_band_rows;
    std::vector<float> full_first_rgb(3 * full_band_pixels);
    std::vector<float> full_second_rgb(3 * full_band_pixels);
    std::vector<float> full_first_weight(full_band_pixels);
    std::vector<float> full_second_weight(full_band_pixels);
    for (uint32_t pixel = 0; pixel < full_band_pixels; ++pixel)
    {
        full_first_rgb[3 * pixel] = static_cast<float>(pixel % 11) * 0.1F;
        full_first_rgb[3 * pixel + 1] = static_cast<float>(pixel % 7) * 0.2F;
        full_first_rgb[3 * pixel + 2] = static_cast<float>(pixel % 5) * 0.3F;
        full_second_rgb[3 * pixel] = static_cast<float>(pixel % 13) * 0.15F;
        full_second_rgb[3 * pixel + 1] = static_cast<float>(pixel % 3) * 0.25F;
        full_second_rgb[3 * pixel + 2] = static_cast<float>(pixel % 9) * 0.05F;
        full_first_weight[pixel] = pixel % 17 == 0 ? 0.0F : 0.25F + static_cast<float>(pixel % 4) * 0.25F;
        full_second_weight[pixel] = pixel % 19 == 0 ? 0.0F : 0.5F + static_cast<float>(pixel % 3) * 0.25F;
    }
    pano_gpu_feather_accumulation_request resident_feather_request = feather_request;
    resident_feather_request.pixel_count = full_band_pixels;
    resident_feather_request.source_width = band_width;
    resident_feather_request.source_height = full_band_rows;
    std::vector<float> resident_accumulated_rgb(3 * full_band_pixels);
    std::vector<float> resident_accumulated_weight(full_band_pixels);
    std::vector<float> resident_normalized_rgb(3 * full_band_pixels);
    EXPECT(pano_gpu_test_dispatch_two_frame_feather_accumulation(
               session, &resident_feather_request, full_first_rgb.data(), full_first_rgb.size() * sizeof(float),
               full_first_weight.data(), full_first_weight.size() * sizeof(float), full_second_rgb.data(),
               full_second_rgb.size() * sizeof(float), full_second_weight.data(), full_second_weight.size() * sizeof(float),
               resident_accumulated_rgb.data(), resident_accumulated_rgb.size() * sizeof(float),
               resident_accumulated_weight.data(), resident_accumulated_weight.size() * sizeof(float), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_dispatch_feather_normalize(
               session, &resident_feather_request, resident_accumulated_rgb.data(),
               resident_accumulated_rgb.size() * sizeof(float), resident_accumulated_weight.data(),
               resident_accumulated_weight.size() * sizeof(float), resident_normalized_rgb.data(),
               resident_normalized_rgb.size() * sizeof(float), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    const void *resident_feather_rgb[] = {full_first_rgb.data(), full_second_rgb.data()};
    const uint64_t resident_feather_rgb_bytes[] = {
        full_first_rgb.size() * sizeof(float), full_second_rgb.size() * sizeof(float)};
    const void *resident_feather_weight[] = {full_first_weight.data(), full_second_weight.data()};
    const uint64_t resident_feather_weight_bytes[] = {
        full_first_weight.size() * sizeof(float), full_second_weight.size() * sizeof(float)};
    pano_gpu_output_create_options feather_output_options {};
    feather_output_options.size = sizeof(feather_output_options);
    feather_output_options.abi_version = PANO_GPU_ABI_VERSION;
    feather_output_options.output_width = band_width;
    feather_output_options.output_height = full_band_rows;
    feather_output_options.output_sample_bytes = 1;
    feather_output_options.descriptor_count = session_options.frame_count + 4;
    feather_output_options.output_workspace_bytes = 13ULL * full_band_pixels;
    pano_gpu_output *resident_feather_output = nullptr;
    EXPECT(pano_gpu_output_create_empty(
               session, &feather_output_options, &resident_feather_output, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
#if defined(_WIN32)
    EXPECT(pano_gpu_output_allocate_linear(
               resident_feather_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_coverage(
               resident_feather_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const pano_gpu_result resident_feather_output_result =
        pano_gpu_test_dispatch_output_feather_accumulation(
            resident_feather_output, &resident_feather_request, resident_feather_rgb,
            resident_feather_rgb_bytes, resident_feather_weight, resident_feather_weight_bytes, 2,
            error.data(), static_cast<uint32_t>(error.size()));
    if (!expect(
            resident_feather_output_result == PANO_GPU_SUCCESS,
            "resident feather output dispatch succeeds", __LINE__))
    {
        std::fprintf(stderr, "D3D12 resident feather output detail: %s\n", error.data());
        return 1;
    }
    std::vector<float> stored_resident_feather_rgb(3 * full_band_pixels);
    std::vector<uint8_t> stored_resident_feather_coverage(full_band_pixels);
    EXPECT(pano_gpu_test_read_output_band(
               resident_feather_output, stored_resident_feather_rgb.data(),
               stored_resident_feather_rgb.size() * sizeof(float), stored_resident_feather_coverage.data(),
               stored_resident_feather_coverage.size(), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    for (uint32_t pixel = 0; pixel < full_band_pixels; ++pixel)
    {
        EXPECT(stored_resident_feather_coverage[pixel] ==
               static_cast<uint8_t>(resident_accumulated_weight[pixel] > 0.0F));
        for (size_t channel = 0; channel < 3; ++channel)
            EXPECT(std::fabs(stored_resident_feather_rgb[3 * pixel + channel] -
                             resident_normalized_rgb[3 * pixel + channel]) < 1.0e-6F);
    }
#endif
    pano_gpu_output_destroy(&resident_feather_output);
    pano_gpu_feather_accumulation_request band_feather_request = resident_feather_request;
    band_feather_request.pixel_count = band_pixels;
    band_feather_request.source_height = band_rows;
    feather_output_options.output_band_rows = band_rows;
    pano_gpu_output *band_feather_output = nullptr;
    EXPECT(pano_gpu_output_create_empty(
               session, &feather_output_options, &band_feather_output, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
#if defined(_WIN32)
    EXPECT(pano_gpu_output_allocate_linear(
               band_feather_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_coverage(
               band_feather_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
#endif
    for (uint32_t band = 0; band < 2; ++band)
    {
        const size_t offset = static_cast<size_t>(band) * band_pixels;
        std::vector<float> band_accumulated_rgb(3 * band_pixels);
        std::vector<float> band_accumulated_weight(band_pixels);
        std::vector<float> band_normalized_rgb(3 * band_pixels);
        EXPECT(pano_gpu_test_dispatch_two_frame_feather_accumulation(
                   session, &band_feather_request, full_first_rgb.data() + 3 * offset,
                   3 * band_pixels * sizeof(float), full_first_weight.data() + offset, band_pixels * sizeof(float),
                   full_second_rgb.data() + 3 * offset, 3 * band_pixels * sizeof(float),
                   full_second_weight.data() + offset, band_pixels * sizeof(float), band_accumulated_rgb.data(),
                   band_accumulated_rgb.size() * sizeof(float), band_accumulated_weight.data(),
                   band_accumulated_weight.size() * sizeof(float), error.data(), static_cast<uint32_t>(error.size())) ==
               PANO_GPU_SUCCESS);
        EXPECT(pano_gpu_test_dispatch_feather_normalize(
                   session, &band_feather_request, band_accumulated_rgb.data(),
                   band_accumulated_rgb.size() * sizeof(float), band_accumulated_weight.data(),
                   band_accumulated_weight.size() * sizeof(float), band_normalized_rgb.data(),
                   band_normalized_rgb.size() * sizeof(float), error.data(), static_cast<uint32_t>(error.size())) ==
               PANO_GPU_SUCCESS);
#if defined(_WIN32)
        const void *band_feather_rgb[] = {
            full_first_rgb.data() + 3 * offset, full_second_rgb.data() + 3 * offset};
        const uint64_t band_feather_rgb_bytes[] = {
            3ULL * band_pixels * sizeof(float), 3ULL * band_pixels * sizeof(float)};
        const void *band_feather_weight[] = {
            full_first_weight.data() + offset, full_second_weight.data() + offset};
        const uint64_t band_feather_weight_bytes[] = {
            1ULL * band_pixels * sizeof(float), 1ULL * band_pixels * sizeof(float)};
        EXPECT(pano_gpu_test_set_output_band(
                   band_feather_output, band * band_rows, band_rows, error.data(),
                   static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
        EXPECT(pano_gpu_test_dispatch_output_feather_accumulation(
                   band_feather_output, &band_feather_request, band_feather_rgb,
                   band_feather_rgb_bytes, band_feather_weight, band_feather_weight_bytes, 2,
                   error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
        std::vector<float> stored_band_feather_rgb(3 * band_pixels);
        std::vector<uint8_t> stored_band_feather_coverage(band_pixels);
        EXPECT(pano_gpu_test_read_output_band(
                   band_feather_output, stored_band_feather_rgb.data(),
                   stored_band_feather_rgb.size() * sizeof(float), stored_band_feather_coverage.data(),
                   stored_band_feather_coverage.size(), error.data(), static_cast<uint32_t>(error.size())) ==
               PANO_GPU_SUCCESS);
#endif
        for (uint32_t pixel = 0; pixel < band_pixels; ++pixel)
        {
#if defined(_WIN32)
            EXPECT(stored_band_feather_coverage[pixel] ==
                   static_cast<uint8_t>(band_accumulated_weight[pixel] > 0.0F));
#endif
            for (size_t channel = 0; channel < 3; ++channel)
            {
                EXPECT(std::fabs(band_normalized_rgb[3 * pixel + channel] -
                                 resident_normalized_rgb[3 * (offset + pixel) + channel]) < 1.0e-6F);
#if defined(_WIN32)
                EXPECT(std::fabs(stored_band_feather_rgb[3 * pixel + channel] -
                                 band_normalized_rgb[3 * pixel + channel]) < 1.0e-6F);
#endif
            }
        }
    }
    pano_gpu_output_destroy(&band_feather_output);
    std::array<float, 12> hard_selected_rgb {};
    std::array<float, 4> hard_selected_weight {};
    std::array<uint8_t, 4> hard_coverage {};
    EXPECT(pano_gpu_test_dispatch_hard_selection(
               session, &hard_selection_request, hard_candidate_rgb.data(), sizeof(hard_candidate_rgb),
               hard_candidate_validity.data(), sizeof(hard_candidate_validity), hard_candidate_edge_distance.data(),
               sizeof(hard_candidate_edge_distance), hard_prior_rgb.data(), sizeof(hard_prior_rgb), hard_prior_weight.data(),
               sizeof(hard_prior_weight), hard_selected_rgb.data(), sizeof(hard_selected_rgb), hard_selected_weight.data(),
               sizeof(hard_selected_weight), hard_coverage.data(), sizeof(hard_coverage), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const std::array<float, 12> expected_hard_rgb {
        1.0F, 0.0F, 0.0F, 0.4F, 0.5F, 0.6F, 0.7F, 0.8F, 0.9F, 0.2F, 0.3F, 0.4F};
    const std::array<float, 4> expected_hard_weight {1.0F, 0.0F, 0.5F, 3.0F};
    const std::array<uint8_t, 4> expected_hard_coverage {1, 0, 1, 1};
    EXPECT(hard_selected_rgb == expected_hard_rgb);
    EXPECT(hard_selected_weight == expected_hard_weight);
    EXPECT(hard_coverage == expected_hard_coverage);
    std::array<float, 12> incomplete_marked_rgb {};
    EXPECT(pano_gpu_test_dispatch_mark_incomplete(
               session, 4, hard_selected_rgb.data(), sizeof(hard_selected_rgb), hard_selected_weight.data(),
               sizeof(hard_selected_weight), incomplete_marked_rgb.data(), sizeof(incomplete_marked_rgb), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const std::array<float, 12> expected_incomplete_marked_rgb {
        1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.7F, 0.8F, 0.9F, 0.2F, 0.3F, 0.4F};
    EXPECT(incomplete_marked_rgb == expected_incomplete_marked_rgb);
    EXPECT(hard_coverage == expected_hard_coverage);
    pano_gpu_session_destroy(&session);
    const auto uint8_proxy_matches_area_oracle = [&](const uint32_t source_width, const uint32_t source_height,
                                                       const uint32_t source_row_stride_bytes) {
        pano_gpu_session_create_options proxy_options = session_options;
        proxy_options.transfer_function = PANO_GPU_TRANSFER_LINEAR;
        proxy_options.source_width = source_width;
        proxy_options.source_height = source_height;
        proxy_options.source_row_stride_bytes = source_row_stride_bytes;
        std::vector<uint8_t> source(static_cast<size_t>(source_height) * source_row_stride_bytes, 0xcdU);
        for (uint32_t y = 0; y < source_height; ++y)
            for (uint32_t x = 0; x < source_width * 3; ++x)
                source[static_cast<size_t>(y) * source_row_stride_bytes + x] =
                    static_cast<uint8_t>((17U * y + 11U * x + 3U) % 256U);
        pano_gpu_session *proxy_session = nullptr;
        if (pano_gpu_session_create(
                device, &proxy_options, &proxy_session, error.data(), static_cast<uint32_t>(error.size())) !=
                PANO_GPU_SUCCESS ||
            pano_gpu_session_allocate_source(proxy_session, error.data(), static_cast<uint32_t>(error.size())) !=
                PANO_GPU_SUCCESS ||
            pano_gpu_session_allocate_upload_slot(proxy_session, error.data(), static_cast<uint32_t>(error.size())) !=
                PANO_GPU_SUCCESS)
            return false;
        pano_gpu_source_upload proxy_upload {};
        proxy_upload.size = sizeof(proxy_upload);
        proxy_upload.abi_version = PANO_GPU_ABI_VERSION;
        proxy_upload.source_sample_type = PANO_GPU_SAMPLE_UINT8;
        proxy_upload.source_row_stride_bytes = source_row_stride_bytes;
        proxy_upload.data = source.data();
        proxy_upload.data_bytes = source.size();
        pano_gpu_exposure_proxy_request proxy_request {};
        proxy_request.size = sizeof(proxy_request);
        proxy_request.abi_version = PANO_GPU_ABI_VERSION;
        proxy_request.frame_count = 1;
        proxy_request.source_width = source_width;
        proxy_request.source_height = source_height;
        pano_gpu_exposure_proxy_layout proxy_layout {};
        proxy_layout.size = sizeof(proxy_layout);
        proxy_layout.abi_version = PANO_GPU_ABI_VERSION;
        const bool ready = pano_gpu_session_upload_frame_zero(
                                 proxy_session, &proxy_upload, error.data(), static_cast<uint32_t>(error.size())) ==
                PANO_GPU_SUCCESS &&
            pano_gpu_session_finish_uploads(
                proxy_session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS &&
            pano_gpu_test_plan_exposure_proxies(
                proxy_session, &proxy_request, &proxy_layout, error.data(), static_cast<uint32_t>(error.size())) ==
                PANO_GPU_SUCCESS;
        std::vector<float> proxy(proxy_layout.proxy_total_bytes / sizeof(float));
        const bool dispatched = ready && pano_gpu_test_dispatch_uint8_exposure_proxies(
            proxy_session, &proxy_request, proxy.data(), proxy_layout.proxy_total_bytes, error.data(),
            static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS;
        const bool retained = ready && pano_gpu_session_build_exposure_proxies(
            proxy_session, &proxy_request, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS &&
            pano_gpu_test_session_exposure_proxy_bytes(proxy_session) == proxy_layout.proxy_total_bytes &&
            pano_gpu_session_build_exposure_proxies(
                proxy_session, &proxy_request, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT;
        pano_gpu_exposure_pair_request pair_request {};
        pair_request.size = sizeof(pair_request);
        pair_request.abi_version = PANO_GPU_ABI_VERSION;
        pair_request.first_frame_index = 0;
        pair_request.second_frame_index = 1;
        pair_request.sample_width = 2;
        pair_request.sample_height = 3;
        pair_request.latitude_span_degrees = 180.0F;
        pair_request.horizontal_fov_degrees = 90.0F;
        pair_request.vertical_fov_degrees = 60.0F;
        std::array<float, 24> pair_coordinates {};
        std::array<uint8_t, 6> pair_overlap {};
        pano_gpu_exposure_pair_layout pair_layout {};
        pair_layout.size = sizeof(pair_layout);
        pair_layout.abi_version = PANO_GPU_ABI_VERSION;
        const bool pair_contract = pano_gpu_test_validate_exposure_pair_request(
            proxy_session, &pair_request, pair_coordinates.data(), sizeof(pair_coordinates), pair_overlap.data(),
            sizeof(pair_overlap), &pair_layout, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT;
        pair_request.second_frame_index = 0;
        const bool pair_rejects_same_frame = pano_gpu_test_validate_exposure_pair_request(
            proxy_session, &pair_request, pair_coordinates.data(), sizeof(pair_coordinates), pair_overlap.data(),
            sizeof(pair_overlap), &pair_layout, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT;
        const auto expected = [&](const uint32_t proxy_x, const uint32_t proxy_y, const uint32_t channel) {
            const float left = proxy_x * static_cast<float>(source_width) / proxy_layout.proxy_width;
            const float right = (proxy_x + 1) * static_cast<float>(source_width) / proxy_layout.proxy_width;
            const float top = proxy_y * static_cast<float>(source_height) / proxy_layout.proxy_height;
            const float bottom = (proxy_y + 1) * static_cast<float>(source_height) / proxy_layout.proxy_height;
            float sum = 0.0F;
            float total = 0.0F;
            for (uint32_t y = static_cast<uint32_t>(std::floor(top)); y < std::min(static_cast<uint32_t>(std::ceil(bottom)), source_height); ++y)
                for (uint32_t x = static_cast<uint32_t>(std::floor(left)); x < std::min(static_cast<uint32_t>(std::ceil(right)), source_width); ++x)
                {
                    const float weight = (std::min(right, x + 1.0F) - std::max(left, static_cast<float>(x))) *
                        (std::min(bottom, y + 1.0F) - std::max(top, static_cast<float>(y)));
                    sum += weight * source[static_cast<size_t>(y) * source_row_stride_bytes + 3 * x + channel] / 255.0F;
                    total += weight;
                }
            return sum / total;
        };
        bool matches = dispatched && retained && pair_contract && pair_rejects_same_frame;
        for (const uint32_t proxy_x : {0U, proxy_layout.proxy_width / 2, proxy_layout.proxy_width - 1})
            for (const uint32_t proxy_y : {0U, proxy_layout.proxy_height - 1})
                for (uint32_t channel = 0; channel < 3; ++channel)
                    matches = matches && std::fabs(
                        proxy[(static_cast<size_t>(proxy_y) * proxy_layout.proxy_width + proxy_x) * 3 + channel] -
                        expected(proxy_x, proxy_y, channel)) < 2.0e-5F;
        pano_gpu_session_destroy(&proxy_session);
        return matches;
    };
    EXPECT(uint8_proxy_matches_area_oracle(257, 5, 772));
    EXPECT(uint8_proxy_matches_area_oracle(258, 5, 776));
    std::array<float, 27> pair_rotations {};
    for (uint32_t frame = 0; frame < 3; ++frame)
    {
        pair_rotations[frame * 9] = 1.0F;
        pair_rotations[frame * 9 + 4] = 1.0F;
        pair_rotations[frame * 9 + 8] = 1.0F;
    }
    pano_gpu_session_create_options pair_options = session_options;
    pair_options.frame_count = 3;
    pair_options.rotations = pair_rotations.data();
    pair_options.rotations_bytes = sizeof(pair_rotations);
    EXPECT(pano_gpu_session_create(
               device, &pair_options, &session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<uint8_t, 48> pair_second_source = source_data;
    pano_gpu_source_upload pair_upload = source_upload;
    pair_upload.frame_index = 0;
    pair_upload.data = source_data.data();
    EXPECT(pano_gpu_session_allocate_source(session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_rotations(session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_upload_rotations(
               session, pair_rotations.data(), sizeof(pair_rotations), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_upload_slot(session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_second_upload_slot(session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_upload_frame_zero(session, &pair_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pair_upload.frame_index = 1;
    pair_upload.data = pair_second_source.data();
    EXPECT(pano_gpu_session_upload_frame(session, &pair_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pair_upload.frame_index = 2;
    EXPECT(pano_gpu_session_upload_frame(session, &pair_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_finish_uploads(session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_exposure_proxy_request retained_pair_proxy_request {};
    retained_pair_proxy_request.size = sizeof(retained_pair_proxy_request);
    retained_pair_proxy_request.abi_version = PANO_GPU_ABI_VERSION;
    retained_pair_proxy_request.frame_count = 3;
    retained_pair_proxy_request.source_width = 4;
    retained_pair_proxy_request.source_height = 4;
    EXPECT(pano_gpu_session_build_exposure_proxies(
               session, &retained_pair_proxy_request, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_exposure_pair_request valid_pair_request {};
    valid_pair_request.size = sizeof(valid_pair_request);
    valid_pair_request.abi_version = PANO_GPU_ABI_VERSION;
    valid_pair_request.second_frame_index = 1;
    valid_pair_request.sample_width = 8;
    valid_pair_request.sample_height = 4;
    valid_pair_request.latitude_span_degrees = 180.0F;
    valid_pair_request.horizontal_fov_degrees = 90.0F;
    valid_pair_request.vertical_fov_degrees = 60.0F;
    std::array<float, 128> valid_pair_coordinates {};
    std::array<uint8_t, 32> valid_pair_overlap {};
    pano_gpu_exposure_pair_layout valid_pair_layout {};
    valid_pair_layout.size = sizeof(valid_pair_layout);
    valid_pair_layout.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_test_validate_exposure_pair_request(
               session, &valid_pair_request, valid_pair_coordinates.data(), sizeof(valid_pair_coordinates),
               valid_pair_overlap.data(), sizeof(valid_pair_overlap), &valid_pair_layout, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(valid_pair_layout.sample_count == 32);
    EXPECT(valid_pair_layout.paired_coordinate_bytes == sizeof(valid_pair_coordinates));
    EXPECT(valid_pair_layout.overlap_bytes == sizeof(valid_pair_overlap));
    pano_gpu_exposure_pair_layout rejected_pair_layout {};
    rejected_pair_layout.size = sizeof(rejected_pair_layout);
    rejected_pair_layout.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_test_validate_exposure_pair_request(
               session, &valid_pair_request, valid_pair_coordinates.data(), sizeof(valid_pair_coordinates) - 1,
               valid_pair_overlap.data(), sizeof(valid_pair_overlap), &rejected_pair_layout, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    EXPECT(pano_gpu_test_dispatch_exposure_pair_projection(
               session, &valid_pair_request, valid_pair_coordinates.data(), sizeof(valid_pair_coordinates),
               valid_pair_overlap.data(), sizeof(valid_pair_overlap), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const auto expected_pair_projection = [&](const uint32_t x, const uint32_t y) {
        const float longitude = ((static_cast<float>(x) + 0.5F) / valid_pair_request.sample_width - 0.5F) *
            6.283185307179586F;
        const float latitude = (0.5F - (static_cast<float>(y) + 0.5F) / valid_pair_request.sample_height) *
            valid_pair_request.latitude_span_degrees * 0.0174532925199433F;
        const float cosine = std::cos(latitude);
        const float camera_x = cosine * std::sin(longitude);
        const float camera_y = std::sin(latitude);
        const float camera_z = cosine * std::cos(longitude);
        const float focal_x = 4.0F / (2.0F * std::tan(valid_pair_request.horizontal_fov_degrees * 0.00872664625997165F));
        const float focal_y = 4.0F / (2.0F * std::tan(valid_pair_request.vertical_fov_degrees * 0.00872664625997165F));
        const float projected_x = 1.5F + focal_x * camera_x / camera_z;
        const float projected_y = 1.5F - focal_y * camera_y / camera_z;
        const bool visible = camera_z > 0.0F && projected_x >= -0.5F && projected_x <= 3.5F &&
            projected_y >= -0.5F && projected_y <= 3.5F;
        return std::array<float, 3> {
            std::clamp(projected_x, 0.0F, 3.0F), std::clamp(projected_y, 0.0F, 3.0F), visible ? 1.0F : 0.0F};
    };
    uint32_t visible_pair_samples = 0;
    for (uint32_t y = 0; y < valid_pair_request.sample_height; ++y)
        for (uint32_t x = 0; x < valid_pair_request.sample_width; ++x)
        {
            const uint32_t index = y * valid_pair_request.sample_width + x;
            const auto expected = expected_pair_projection(x, y);
            EXPECT(std::fabs(valid_pair_coordinates[index * 4] - expected[0]) < 1.0e-5F);
            EXPECT(std::fabs(valid_pair_coordinates[index * 4 + 1] - expected[1]) < 1.0e-5F);
            EXPECT(std::fabs(valid_pair_coordinates[index * 4 + 2] - expected[0]) < 1.0e-5F);
            EXPECT(std::fabs(valid_pair_coordinates[index * 4 + 3] - expected[1]) < 1.0e-5F);
            EXPECT(valid_pair_overlap[index] == static_cast<uint8_t>(expected[2]));
            visible_pair_samples += valid_pair_overlap[index];
        }
    EXPECT(visible_pair_samples > 0 && visible_pair_samples < valid_pair_overlap.size());
    const std::array<uint8_t, 32> saved_pair_overlap = valid_pair_overlap;
    std::array<float, 192> sampled_pair_values {};
    EXPECT(pano_gpu_test_dispatch_exposure_pair_samples(
               session, &valid_pair_request, valid_pair_coordinates.data(), sizeof(valid_pair_coordinates),
               valid_pair_overlap.data(), sizeof(valid_pair_overlap), sampled_pair_values.data(),
               sizeof(sampled_pair_values), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const auto sample_pair_source = [](const auto &source, const float coordinate_x, const float coordinate_y,
                                        const uint32_t channel) {
        const uint32_t x0 = static_cast<uint32_t>(std::floor(coordinate_x));
        const uint32_t y0 = static_cast<uint32_t>(std::floor(coordinate_y));
        const uint32_t x1 = std::min(x0 + 1, 3U);
        const uint32_t y1 = std::min(y0 + 1, 3U);
        const float wx = coordinate_x - x0;
        const float wy = coordinate_y - y0;
        const auto value = [&](const uint32_t sample_x, const uint32_t sample_y) {
            const float encoded = source[(sample_y * 4 + sample_x) * 3 + channel] / 255.0F;
            return encoded <= 0.04045F ? encoded / 12.92F :
                std::pow((encoded + 0.055F) / 1.055F, 2.4F);
        };
        return (1.0F - wx) * (1.0F - wy) * value(x0, y0) + wx * (1.0F - wy) * value(x1, y0) +
            (1.0F - wx) * wy * value(x0, y1) + wx * wy * value(x1, y1);
    };
    for (uint32_t index = 0; index < valid_pair_layout.sample_count; ++index)
        for (uint32_t channel = 0; channel < 3; ++channel)
        {
            EXPECT(std::fabs(sampled_pair_values[index * 6 + channel] - sample_pair_source(
                source_data, valid_pair_coordinates[index * 4], valid_pair_coordinates[index * 4 + 1], channel)) <
                1.0e-5F);
            EXPECT(std::fabs(sampled_pair_values[index * 6 + 3 + channel] - sample_pair_source(
                pair_second_source, valid_pair_coordinates[index * 4 + 2], valid_pair_coordinates[index * 4 + 3], channel)) <
                1.0e-5F);
        }
    EXPECT(valid_pair_overlap == saved_pair_overlap);
    EXPECT(pano_gpu_session_prepare_exposure_pair_scratch(
               session, &valid_pair_request, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_dispatch_exposure_pair_projection_samples(
               session, &valid_pair_request, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 128> resident_coordinates {};
    std::array<uint8_t, 32> resident_overlap {};
    std::array<float, 192> resident_samples {};
    EXPECT(pano_gpu_test_read_resident_exposure_pair_projection_samples(
               session, resident_coordinates.data(), sizeof(resident_coordinates), resident_overlap.data(),
               sizeof(resident_overlap), resident_samples.data(), sizeof(resident_samples), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t index = 0; index < resident_coordinates.size(); ++index)
        EXPECT(std::fabs(resident_coordinates[index] - valid_pair_coordinates[index]) < 1.0e-6F);
    EXPECT(resident_overlap == saved_pair_overlap);
    for (size_t index = 0; index < resident_samples.size(); ++index)
        EXPECT(std::fabs(resident_samples[index] - sampled_pair_values[index]) < 1.0e-6F);
    EXPECT(pano_gpu_session_dispatch_exposure_pair_classification(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<float, 64> resident_luminance {};
    std::array<uint8_t, 32> resident_accepted {};
    EXPECT(pano_gpu_test_read_resident_exposure_pair_classification(
               session, resident_luminance.data(), sizeof(resident_luminance), resident_accepted.data(),
               sizeof(resident_accepted), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 64> staged_luminance {};
    std::array<uint8_t, 32> staged_accepted {};
    EXPECT(pano_gpu_test_classify_exposure_pair_samples(
               session, &valid_pair_request, sampled_pair_values.data(), sizeof(sampled_pair_values),
               saved_pair_overlap.data(), sizeof(saved_pair_overlap), staged_luminance.data(),
               sizeof(staged_luminance), staged_accepted.data(), sizeof(staged_accepted), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t index = 0; index < resident_luminance.size(); ++index)
        EXPECT(std::fabs(resident_luminance[index] - staged_luminance[index]) < 1.0e-6F);
    EXPECT(resident_accepted == staged_accepted);
    std::array<float, 64> staged_gradients {};
    EXPECT(pano_gpu_test_dispatch_exposure_pair_gradients(
               session, &valid_pair_request, staged_luminance.data(), sizeof(staged_luminance),
               staged_gradients.data(), sizeof(staged_gradients), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<float, 2> staged_gradient_limits {};
    EXPECT(pano_gpu_test_compute_exposure_pair_gradient_limits(
               session, &valid_pair_request, staged_gradients.data(), sizeof(staged_gradients),
               staged_gradient_limits.data(), sizeof(staged_gradient_limits), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_dispatch_exposure_pair_gradient_limits(
               session, valid_pair_request.sample_width, valid_pair_request.sample_height, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<float, 64> resident_gradients {};
    std::array<float, 2> resident_gradient_limits {};
    EXPECT(pano_gpu_test_read_resident_exposure_pair_gradient_limits(
               session, resident_gradients.data(), sizeof(resident_gradients),
               resident_gradient_limits.data(), sizeof(resident_gradient_limits), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t index = 0; index < resident_gradients.size(); ++index)
        EXPECT(std::fabs(resident_gradients[index] - staged_gradients[index]) < 1.0e-6F);
    EXPECT(std::fabs(resident_gradient_limits[0] - staged_gradient_limits[0]) < 1.0e-6F);
    EXPECT(std::fabs(resident_gradient_limits[1] - staged_gradient_limits[1]) < 1.0e-6F);
    std::array<uint8_t, 32> staged_filtered = staged_accepted;
    EXPECT(pano_gpu_test_filter_exposure_pair_acceptance(
               session, &valid_pair_request, staged_gradients.data(), sizeof(staged_gradients),
               staged_gradient_limits.data(), sizeof(staged_gradient_limits), staged_filtered.data(),
               sizeof(staged_filtered), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 32> staged_ratios {};
    EXPECT(pano_gpu_test_build_exposure_pair_ratios(
               session, &valid_pair_request, staged_luminance.data(), sizeof(staged_luminance),
               staged_filtered.data(), sizeof(staged_filtered), staged_ratios.data(),
               sizeof(staged_ratios), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_dispatch_exposure_pair_filter_ratios(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<uint8_t, 32> resident_filtered {};
    std::array<float, 32> resident_ratios {};
    EXPECT(pano_gpu_test_read_resident_exposure_pair_filter_ratios(
               session, resident_filtered.data(), sizeof(resident_filtered), resident_ratios.data(),
               sizeof(resident_ratios), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(resident_filtered == staged_filtered);
    for (size_t index = 0; index < resident_ratios.size(); ++index)
        EXPECT(std::fabs(resident_ratios[index] - staged_ratios[index]) < 1.0e-6F);
    EXPECT(pano_gpu_session_dispatch_exposure_pair_trim(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<float, 32> resident_sorted_ratios {};
    std::array<float, 2> resident_trim_bounds {};
    std::array<uint8_t, 32> resident_trimmed {};
    EXPECT(pano_gpu_test_read_resident_exposure_pair_trim(
               session, resident_sorted_ratios.data(), sizeof(resident_sorted_ratios),
               resident_trim_bounds.data(), sizeof(resident_trim_bounds), resident_trimmed.data(),
               sizeof(resident_trimmed), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 32> expected_resident_sorted {};
    expected_resident_sorted.fill(std::numeric_limits<float>::infinity());
    size_t resident_valid_count = 0;
    for (size_t index = 0; index < staged_filtered.size(); ++index)
        if (staged_filtered[index] != 0)
            expected_resident_sorted[resident_valid_count++] = staged_ratios[index];
    EXPECT(resident_valid_count > 0);
    std::sort(expected_resident_sorted.begin(), expected_resident_sorted.end());
    for (size_t index = 0; index < expected_resident_sorted.size(); ++index)
        if (std::isinf(expected_resident_sorted[index]))
            EXPECT(std::isinf(resident_sorted_ratios[index]));
        else
            EXPECT(std::fabs(resident_sorted_ratios[index] - expected_resident_sorted[index]) < 1.0e-6F);
    const auto resident_quantile = [&](const double fraction) {
        const double position = static_cast<double>(resident_valid_count - 1) * fraction;
        const size_t lower = static_cast<size_t>(std::floor(position));
        const size_t upper = static_cast<size_t>(std::ceil(position));
        const float weight = static_cast<float>(position - static_cast<double>(lower));
        return expected_resident_sorted[lower] * (1.0F - weight) +
            expected_resident_sorted[upper] * weight;
    };
    EXPECT(std::fabs(resident_trim_bounds[0] - resident_quantile(0.1)) < 1.0e-6F);
    EXPECT(std::fabs(resident_trim_bounds[1] - resident_quantile(0.9)) < 1.0e-6F);
    for (size_t index = 0; index < resident_trimmed.size(); ++index)
    {
        const bool expected = staged_filtered[index] != 0 &&
            staged_ratios[index] >= resident_trim_bounds[0] &&
            staged_ratios[index] <= resident_trim_bounds[1];
        EXPECT(resident_trimmed[index] == static_cast<uint8_t>(expected));
    }
    std::vector<float> resident_inlier_ratios;
    for (size_t index = 0; index < resident_trimmed.size(); ++index)
        if (resident_trimmed[index] != 0)
            resident_inlier_ratios.push_back(staged_ratios[index]);
    std::sort(resident_inlier_ratios.begin(), resident_inlier_ratios.end());
    const auto median_of = [](const std::vector<float> &values) {
        const size_t span = values.size() - 1;
        return 0.5F * (values[span / 2] + values[(span + 1) / 2]);
    };
    const float expected_resident_difference = median_of(resident_inlier_ratios);
    std::vector<float> resident_deviations;
    for (const float value : resident_inlier_ratios)
        resident_deviations.push_back(std::fabs(value - expected_resident_difference));
    std::sort(resident_deviations.begin(), resident_deviations.end());
    const float expected_resident_mad = median_of(resident_deviations);
    uint32_t expected_resident_reason = PANO_GPU_EXPOSURE_PAIR_ACCEPTED;
    if (resident_valid_count < 24)
        expected_resident_reason = PANO_GPU_EXPOSURE_PAIR_INSUFFICIENT_VALID;
    else if (resident_inlier_ratios.size() < 12)
        expected_resident_reason = PANO_GPU_EXPOSURE_PAIR_INSUFFICIENT_INLIERS;
    else if (expected_resident_mad > 0.5F)
        expected_resident_reason = PANO_GPU_EXPOSURE_PAIR_EXCESSIVE_DISPERSION;
    const float expected_resident_weight = expected_resident_reason == PANO_GPU_EXPOSURE_PAIR_ACCEPTED
        ? std::sqrt(static_cast<float>(resident_inlier_ratios.size())) / (1.0F + expected_resident_mad)
        : 0.0F;
    pano_gpu_exposure_pair_reduction resident_reduction {};
    resident_reduction.size = sizeof(resident_reduction);
    resident_reduction.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_reduce_exposure_pair(
               session, &resident_reduction, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(resident_reduction.rejection_reason == expected_resident_reason);
    EXPECT(resident_reduction.valid_count == resident_valid_count);
    EXPECT(resident_reduction.inlier_count == resident_inlier_ratios.size());
    EXPECT(std::fabs(resident_reduction.difference - expected_resident_difference) < 1.0e-6F);
    EXPECT(std::fabs(resident_reduction.mad - expected_resident_mad) < 1.0e-6F);
    EXPECT(std::fabs(resident_reduction.weight - expected_resident_weight) < 1.0e-5F);
    EXPECT(resident_reduction.downloaded_bytes == 32);
    pano_gpu_exposure_pair_request graph_pair_request = valid_pair_request;
    graph_pair_request.sample_width = 64;
    graph_pair_request.sample_height = 32;
    EXPECT(pano_gpu_session_prepare_exposure_pair_scratch(
               session, &graph_pair_request, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_dispatch_exposure_pair_projection_samples(
               session, &graph_pair_request, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_dispatch_exposure_pair_classification(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_dispatch_exposure_pair_gradient_limits(
               session, graph_pair_request.sample_width, graph_pair_request.sample_height, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_dispatch_exposure_pair_filter_ratios(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_dispatch_exposure_pair_trim(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_exposure_pair_reduction direct_graph_reduction {};
    direct_graph_reduction.size = sizeof(direct_graph_reduction);
    direct_graph_reduction.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_reduce_exposure_pair(
               session, &direct_graph_reduction, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(direct_graph_reduction.rejection_reason == PANO_GPU_EXPOSURE_PAIR_ACCEPTED);
    EXPECT(pano_gpu_session_prepare_exposure_graph(
               session, 3, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_enumerate_exposure_pairs(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_reduce_exposure_graph(
               session, &graph_pair_request, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    pano_gpu_exposure_graph_diagnostics reduced_graph_diagnostics {};
    reduced_graph_diagnostics.size = sizeof(reduced_graph_diagnostics);
    reduced_graph_diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_query_exposure_graph(
               session, &reduced_graph_diagnostics, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(reduced_graph_diagnostics.pair_report_count == 3);
    EXPECT(reduced_graph_diagnostics.equation_count == 3);
    std::array<pano_gpu_exposure_pair_report, 3> reduced_reports {};
    EXPECT(pano_gpu_session_copy_exposure_pair_reports(
               session, reduced_reports.data(), sizeof(reduced_reports), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const std::array<std::array<uint32_t, 2>, 3> graph_pairs {{{0, 1}, {0, 2}, {1, 2}}};
    for (size_t index = 0; index < graph_pairs.size(); ++index)
    {
        EXPECT(reduced_reports[index].left_frame_index == graph_pairs[index][0]);
        EXPECT(reduced_reports[index].right_frame_index == graph_pairs[index][1]);
        EXPECT(reduced_reports[index].rejection_reason == direct_graph_reduction.rejection_reason);
        EXPECT(reduced_reports[index].valid_count == direct_graph_reduction.valid_count);
        EXPECT(reduced_reports[index].inlier_count == direct_graph_reduction.inlier_count);
        EXPECT(reduced_reports[index].geometric_count >= 24);
        EXPECT(reduced_reports[index].geometric_count == reduced_reports[0].geometric_count);
    }
    std::array<pano_gpu_exposure_equation, 3> reduced_equations {};
    EXPECT(pano_gpu_session_copy_exposure_equations(
               session, reduced_equations.data(), sizeof(reduced_equations), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t index = 0; index < graph_pairs.size(); ++index)
    {
        EXPECT(reduced_equations[index].left_frame_index == graph_pairs[index][0]);
        EXPECT(reduced_equations[index].right_frame_index == graph_pairs[index][1]);
        EXPECT(std::fabs(reduced_equations[index].difference - direct_graph_reduction.difference) < 1.0e-6);
        EXPECT(std::fabs(reduced_equations[index].weight - direct_graph_reduction.weight) < 1.0e-6);
    }
    pano_gpu_session_clear_exposure_graph(session);
    EXPECT(pano_gpu_session_prepare_exposure_graph(
               session, 3, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_enumerate_exposure_pairs(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    struct ExposureProgress
    {
        uint32_t completed = 0;
        uint32_t total = 0;
        uint32_t calls = 0;
    } exposure_progress;
    const auto record_exposure_progress = [](void *const user_data, const uint32_t completed,
                                             const uint32_t total) -> int {
        auto &progress = *static_cast<ExposureProgress *>(user_data);
        if (completed <= progress.completed || total != 3)
            return 0;
        progress.completed = completed;
        progress.total = total;
        ++progress.calls;
        return 1;
    };
    EXPECT(pano_gpu_session_reduce_reference_exposure_graph_progress(
               session, &graph_pair_request, record_exposure_progress, &exposure_progress,
               error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(exposure_progress.completed == 3 && exposure_progress.total == 3 &&
           exposure_progress.calls == 3);
    reduced_graph_diagnostics = {};
    reduced_graph_diagnostics.size = sizeof(reduced_graph_diagnostics);
    reduced_graph_diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_query_exposure_graph(
               session, &reduced_graph_diagnostics, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(reduced_graph_diagnostics.pair_report_count == 3);
    EXPECT(reduced_graph_diagnostics.equation_count == 3);
    EXPECT(pano_gpu_session_copy_exposure_equations(
               session, reduced_equations.data(), sizeof(reduced_equations), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t index = 0; index < graph_pairs.size(); ++index)
    {
        EXPECT(reduced_equations[index].left_frame_index == graph_pairs[index][0]);
        EXPECT(reduced_equations[index].right_frame_index == graph_pairs[index][1]);
        EXPECT(std::isfinite(reduced_equations[index].difference));
        EXPECT(reduced_equations[index].weight > 0.0);
    }
    std::array<float, 192> classification_samples = sampled_pair_values;
    std::array<uint8_t, 32> classification_overlap = saved_pair_overlap;
    classification_samples.fill(0.5F);
    classification_overlap.fill(1);
    classification_samples[6] = 1.0e-6F;
    classification_samples[7] = 1.0e-6F;
    classification_samples[8] = 1.0e-6F;
    classification_samples[12] = std::numeric_limits<float>::quiet_NaN();
    classification_samples[18] = std::numeric_limits<float>::infinity();
    classification_overlap[4] = 0;
    classification_samples[30] = 1.0F;
    classification_samples[31] = 1.0F;
    classification_samples[32] = 1.0F;
    std::array<float, 64> classified_luminance {};
    std::array<uint8_t, 32> classified_accepted {};
    EXPECT(pano_gpu_test_classify_exposure_pair_samples(
               session, &valid_pair_request, classification_samples.data(), sizeof(classification_samples),
               classification_overlap.data(), sizeof(classification_overlap), classified_luminance.data(),
               sizeof(classified_luminance), classified_accepted.data(), sizeof(classified_accepted), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (uint32_t index = 0; index < valid_pair_layout.sample_count; ++index)
    {
        const auto luminance = [&](const uint32_t offset) {
            return classification_samples[index * 6 + offset] * 0.2126F +
                classification_samples[index * 6 + offset + 1] * 0.7152F +
                classification_samples[index * 6 + offset + 2] * 0.0722F;
        };
        const float first_luminance = luminance(0);
        const float second_luminance = luminance(3);
        const uint8_t expected_accepted = classification_overlap[index] != 0 &&
                std::isfinite(first_luminance) && std::isfinite(second_luminance) &&
                first_luminance > 1.0e-5F && second_luminance > 1.0e-5F
                && !(std::any_of(classification_samples.begin() + index * 6,
                        classification_samples.begin() + index * 6 + 6,
                        [](const float value) { return value >= 0.995F; }))
            ? 1
            : 0;
        if (std::isfinite(first_luminance))
            EXPECT(std::fabs(classified_luminance[index * 2] - first_luminance) < 1.0e-5F);
        else
            EXPECT(!std::isfinite(classified_luminance[index * 2]));
        EXPECT(std::fabs(classified_luminance[index * 2 + 1] - second_luminance) < 1.0e-5F);
        EXPECT(classified_accepted[index] == expected_accepted);
    }
    EXPECT(classified_accepted[0] == 1);
    EXPECT(classified_accepted[1] == 0);
    EXPECT(classified_accepted[2] == 0);
    EXPECT(classified_accepted[3] == 0);
    EXPECT(classified_accepted[4] == 0);
    EXPECT(classified_accepted[5] == 0);
    pano_gpu_session_destroy(&session);
    pano_gpu_session_create_options linear_pair_options = pair_options;
    linear_pair_options.transfer_function = PANO_GPU_TRANSFER_LINEAR;
    EXPECT(pano_gpu_session_create(
               device, &linear_pair_options, &session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_source(session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_rotations(session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_upload_rotations(
               session, pair_rotations.data(), sizeof(pair_rotations), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_upload_slot(session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_second_upload_slot(session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pair_upload.frame_index = 0;
    pair_upload.data = source_data.data();
    EXPECT(pano_gpu_session_upload_frame_zero(session, &pair_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pair_upload.frame_index = 1;
    pair_upload.data = pair_second_source.data();
    EXPECT(pano_gpu_session_upload_frame(session, &pair_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pair_upload.frame_index = 2;
    EXPECT(pano_gpu_session_upload_frame(session, &pair_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_finish_uploads(session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_build_exposure_proxies(
               session, &retained_pair_proxy_request, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    classification_samples.fill(1.0F);
    classification_overlap.fill(1);
    classified_accepted.fill(0);
    EXPECT(pano_gpu_test_classify_exposure_pair_samples(
               session, &valid_pair_request, classification_samples.data(), sizeof(classification_samples),
               classification_overlap.data(), sizeof(classification_overlap), classified_luminance.data(),
               sizeof(classified_luminance), classified_accepted.data(), sizeof(classified_accepted), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(std::all_of(classified_accepted.begin(), classified_accepted.end(),
               [](const uint8_t value) { return value == 1; }));
    const auto expected_pair_gradient = [&](const std::array<float, 64> &luminance, const uint32_t index,
                                            const uint32_t channel) {
        const uint32_t x = index % valid_pair_request.sample_width;
        const uint32_t y = index / valid_pair_request.sample_width;
        const uint32_t left_x = x == 0 ? std::min(1U, valid_pair_request.sample_width - 1) : x - 1;
        const uint32_t right_x = x == valid_pair_request.sample_width - 1 ? std::max(valid_pair_request.sample_width - 2, 0U) : x + 1;
        const uint32_t top_y = y == 0 ? std::min(1U, valid_pair_request.sample_height - 1) : y - 1;
        const uint32_t bottom_y = y == valid_pair_request.sample_height - 1 ? std::max(valid_pair_request.sample_height - 2, 0U) : y + 1;
        const auto logged = [&](const uint32_t sample_x, const uint32_t sample_y) { return std::log(std::max(luminance[(sample_y * valid_pair_request.sample_width + sample_x) * 2 + channel], 1.0e-5F)); };
        const float top_left = logged(left_x, top_y), top = logged(x, top_y), top_right = logged(right_x, top_y);
        const float left = logged(left_x, y), right = logged(right_x, y);
        const float bottom_left = logged(left_x, bottom_y), bottom = logged(x, bottom_y), bottom_right = logged(right_x, bottom_y);
        const float horizontal = -top_left + top_right - 2.0F * left + 2.0F * right - bottom_left + bottom_right;
        const float vertical = -top_left - 2.0F * top - top_right + bottom_left + 2.0F * bottom + bottom_right;
        return std::sqrt(horizontal * horizontal + vertical * vertical);
    };
    const auto check_pair_gradients = [&](const std::array<float, 64> &luminance) {
        std::array<float, 64> gradients {};
        if (pano_gpu_test_dispatch_exposure_pair_gradients(session, &valid_pair_request, luminance.data(), sizeof(luminance), gradients.data(), sizeof(gradients), error.data(), static_cast<uint32_t>(error.size())) != PANO_GPU_SUCCESS)
            return false;
        for (uint32_t index = 0; index < valid_pair_layout.sample_count; ++index)
            for (uint32_t channel = 0; channel < 2; ++channel)
                if (std::fabs(gradients[index * 2 + channel] - expected_pair_gradient(luminance, index, channel)) >= 1.0e-5F)
                    return false;
        return true;
    };
    std::array<float, 64> flat_luminance {};
    flat_luminance.fill(0.5F);
    EXPECT(check_pair_gradients(flat_luminance));
    std::array<float, 64> textured_luminance {};
    std::array<float, 64> edge_luminance {};
    for (uint32_t index = 0; index < valid_pair_layout.sample_count; ++index)
    {
        const uint32_t x = index % valid_pair_request.sample_width;
        const uint32_t y = index / valid_pair_request.sample_width;
        textured_luminance[index * 2] = 0.1F + 0.07F * x + 0.03F * y;
        textured_luminance[index * 2 + 1] = 0.2F + 0.05F * x + 0.04F * y;
        edge_luminance[index * 2] = x < valid_pair_request.sample_width / 2 ? 0.1F : 0.8F;
        edge_luminance[index * 2 + 1] = y < valid_pair_request.sample_height / 2 ? 0.2F : 0.9F;
    }
    EXPECT(check_pair_gradients(textured_luminance));
    EXPECT(check_pair_gradients(edge_luminance));
    std::array<float, 64> filter_gradients {};
    std::array<uint8_t, 32> filter_accepted {};
    filter_accepted.fill(1);
    for (uint32_t index = 0; index < valid_pair_layout.sample_count; ++index)
    {
        filter_gradients[index * 2] = index % 3 == 0 ? 0.4F : 0.2F;
        filter_gradients[index * 2 + 1] = index % 5 == 0 ? 0.5F : 0.3F;
    }
    filter_accepted[1] = 0;
    filter_gradients[4] = std::numeric_limits<float>::quiet_NaN();
    const auto finite_p90 = [&](const uint32_t channel) {
        std::vector<float> values;
        for (uint32_t index = 0; index < valid_pair_layout.sample_count; ++index)
            if (std::isfinite(filter_gradients[index * 2 + channel]))
                values.push_back(filter_gradients[index * 2 + channel]);
        std::sort(values.begin(), values.end());
        const double position = static_cast<double>(values.size() - 1) * 0.9;
        const size_t lower = static_cast<size_t>(std::floor(position));
        const size_t upper = static_cast<size_t>(std::ceil(position));
        const float fraction = static_cast<float>(position - static_cast<double>(lower));
        return values[lower] * (1.0F - fraction) + values[upper] * fraction;
    };
    std::array<float, 2> gradient_limits {};
    EXPECT(pano_gpu_test_compute_exposure_pair_gradient_limits(
               session, &valid_pair_request, filter_gradients.data(), sizeof(filter_gradients),
               gradient_limits.data(), sizeof(gradient_limits), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(std::fabs(gradient_limits[0] - finite_p90(0)) < 1.0e-6F);
    EXPECT(std::fabs(gradient_limits[1] - finite_p90(1)) < 1.0e-6F);
    EXPECT(pano_gpu_test_filter_exposure_pair_acceptance(
               session, &valid_pair_request, filter_gradients.data(), sizeof(filter_gradients), gradient_limits.data(),
               sizeof(gradient_limits), filter_accepted.data(), sizeof(filter_accepted), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (uint32_t index = 0; index < valid_pair_layout.sample_count; ++index)
    {
        const bool expected = index != 1 && std::isfinite(filter_gradients[index * 2]) &&
            std::isfinite(filter_gradients[index * 2 + 1]) && filter_gradients[index * 2] <= gradient_limits[0] &&
            filter_gradients[index * 2 + 1] <= gradient_limits[1];
        EXPECT(filter_accepted[index] == (expected ? 1 : 0));
    }
    std::array<float, 64> ratio_luminance {};
    std::array<uint8_t, 32> ratio_accepted {};
    std::array<float, 32> log_ratios {};
    for (uint32_t index = 0; index < valid_pair_layout.sample_count; ++index)
    {
        ratio_luminance[index * 2] = 0.25F + 0.01F * static_cast<float>(index);
        ratio_luminance[index * 2 + 1] = 0.35F + 0.013F * static_cast<float>((index * 11) % 32);
        ratio_accepted[index] = index % 3 != 0 ? 1 : 0;
    }
    EXPECT(pano_gpu_test_build_exposure_pair_ratios(
               session, &valid_pair_request, ratio_luminance.data(), sizeof(ratio_luminance), ratio_accepted.data(),
               sizeof(ratio_accepted), log_ratios.data(), sizeof(log_ratios), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (uint32_t index = 0; index < valid_pair_layout.sample_count; ++index)
    {
        if (ratio_accepted[index] != 0)
            EXPECT(std::fabs(log_ratios[index] - std::log(ratio_luminance[index * 2] / ratio_luminance[index * 2 + 1])) < 1.0e-5F);
    }
    std::array<float, 32> sortable_ratios {};
    EXPECT(pano_gpu_test_prepare_exposure_pair_sort(
               session, &valid_pair_request, sortable_ratios.data(), sizeof(sortable_ratios), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (uint32_t index = 0; index < valid_pair_layout.sample_count; ++index)
    {
        if (ratio_accepted[index] != 0)
            EXPECT(std::fabs(sortable_ratios[index] - log_ratios[index]) < 1.0e-6F);
        else
            EXPECT(std::isinf(sortable_ratios[index]) && sortable_ratios[index] > 0.0F);
    }
    std::array<float, 32> expected_sorted_ratios = sortable_ratios;
    std::sort(expected_sorted_ratios.begin(), expected_sorted_ratios.end());
    std::array<float, 32> sorted_ratios {};
    EXPECT(pano_gpu_test_sort_exposure_pair(
               session, sorted_ratios.data(), sizeof(sorted_ratios), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (uint32_t index = 0; index < sorted_ratios.size(); ++index)
    {
        if (std::isinf(expected_sorted_ratios[index]))
            EXPECT(std::isinf(sorted_ratios[index]) && sorted_ratios[index] > 0.0F);
        else
            EXPECT(std::fabs(sorted_ratios[index] - expected_sorted_ratios[index]) < 1.0e-6F);
    }
    const auto expected_trim_bounds = [&](const std::array<uint8_t, 32> &accepted_mask) {
        std::array<float, 32> values {};
        values.fill(std::numeric_limits<float>::infinity());
        size_t count = 0;
        for (uint32_t index = 0; index < valid_pair_layout.sample_count; ++index)
            if (accepted_mask[index] != 0)
                values[count++] = log_ratios[index];
        std::sort(values.begin(), values.end());
        const auto quantile = [&](const double fraction) {
            const double position = static_cast<double>(count - 1) * fraction;
            const size_t lower = static_cast<size_t>(std::floor(position));
            const size_t upper = static_cast<size_t>(std::ceil(position));
            const float weight = static_cast<float>(position - static_cast<double>(lower));
            return values[lower] * (1.0F - weight) + values[upper] * weight;
        };
        return std::array<float, 2> {quantile(0.1), quantile(0.9)};
    };
    std::array<float, 2> trim_bounds {};
    EXPECT(pano_gpu_test_extract_exposure_pair_trim_bounds(
               session, trim_bounds.data(), sizeof(trim_bounds), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<float, 2> expected_bounds = expected_trim_bounds(ratio_accepted);
    EXPECT(std::fabs(trim_bounds[0] - expected_bounds[0]) < 1.0e-6F);
    EXPECT(std::fabs(trim_bounds[1] - expected_bounds[1]) < 1.0e-6F);
    const auto check_trimmed = [&](const std::array<uint8_t, 32> &accepted_mask,
                                   const std::array<float, 2> &bounds) {
        std::array<uint8_t, 32> trimmed_mask {};
        if (pano_gpu_test_trim_exposure_pair(
                session, trimmed_mask.data(), sizeof(trimmed_mask), error.data(),
                static_cast<uint32_t>(error.size())) != PANO_GPU_SUCCESS)
            return false;
        for (uint32_t index = 0; index < valid_pair_layout.sample_count; ++index)
        {
            const bool expected = accepted_mask[index] != 0 && log_ratios[index] >= bounds[0] &&
                log_ratios[index] <= bounds[1];
            if (trimmed_mask[index] != static_cast<uint8_t>(expected))
                return false;
        }
        return true;
    };
    EXPECT(check_trimmed(ratio_accepted, expected_bounds));
    pano_gpu_exposure_pair_reduction reduction {};
    reduction.size = sizeof(reduction);
    reduction.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_test_reduce_exposure_pair(
               session, &reduction, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(reduction.rejection_reason == PANO_GPU_EXPOSURE_PAIR_INSUFFICIENT_VALID);
    EXPECT(reduction.valid_count == 21);
    EXPECT(reduction.weight == 0.0F);
    EXPECT(reduction.downloaded_bytes == 32);
    ratio_accepted.fill(1);
    EXPECT(pano_gpu_test_build_exposure_pair_ratios(
               session, &valid_pair_request, ratio_luminance.data(), sizeof(ratio_luminance), ratio_accepted.data(),
               sizeof(ratio_accepted), log_ratios.data(), sizeof(log_ratios), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_prepare_exposure_pair_sort(
               session, &valid_pair_request, sortable_ratios.data(), sizeof(sortable_ratios), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_sort_exposure_pair(
               session, sorted_ratios.data(), sizeof(sorted_ratios), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_extract_exposure_pair_trim_bounds(
               session, trim_bounds.data(), sizeof(trim_bounds), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    expected_bounds = expected_trim_bounds(ratio_accepted);
    EXPECT(check_trimmed(ratio_accepted, expected_bounds));
    std::vector<float> expected_inliers;
    for (uint32_t index = 0; index < valid_pair_layout.sample_count; ++index)
        if (log_ratios[index] >= expected_bounds[0] && log_ratios[index] <= expected_bounds[1])
            expected_inliers.push_back(log_ratios[index]);
    std::sort(expected_inliers.begin(), expected_inliers.end());
    const auto median = [](const std::vector<float> &values) {
        const size_t lower = (values.size() - 1) / 2;
        const size_t upper = values.size() / 2;
        return 0.5F * (values[lower] + values[upper]);
    };
    const float expected_difference = median(expected_inliers);
    std::vector<float> expected_deviations;
    for (const float value : expected_inliers)
        expected_deviations.push_back(std::fabs(value - expected_difference));
    std::sort(expected_deviations.begin(), expected_deviations.end());
    const float expected_mad = median(expected_deviations);
    const float expected_reduction_weight =
        std::sqrt(static_cast<float>(expected_inliers.size())) / (1.0F + expected_mad);
    reduction = {};
    reduction.size = sizeof(reduction);
    reduction.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_test_reduce_exposure_pair(
               session, &reduction, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(reduction.rejection_reason == PANO_GPU_EXPOSURE_PAIR_ACCEPTED);
    EXPECT(reduction.valid_count == 32);
    EXPECT(reduction.inlier_count == expected_inliers.size());
    EXPECT(std::fabs(reduction.difference - expected_difference) < 1.0e-6F);
    EXPECT(std::fabs(reduction.mad - expected_mad) < 1.0e-6F);
    EXPECT(std::fabs(reduction.weight - expected_reduction_weight) < 1.0e-5F);
    EXPECT(reduction.downloaded_bytes == 32);
    ratio_accepted[1] = 0;
    EXPECT(pano_gpu_test_build_exposure_pair_ratios(
               session, &valid_pair_request, ratio_luminance.data(), sizeof(ratio_luminance), ratio_accepted.data(),
               sizeof(ratio_accepted), log_ratios.data(), sizeof(log_ratios), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_prepare_exposure_pair_sort(
               session, &valid_pair_request, sortable_ratios.data(), sizeof(sortable_ratios), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_sort_exposure_pair(
               session, sorted_ratios.data(), sizeof(sorted_ratios), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_extract_exposure_pair_trim_bounds(
               session, trim_bounds.data(), sizeof(trim_bounds), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    expected_bounds = expected_trim_bounds(ratio_accepted);
    EXPECT(std::fabs(trim_bounds[0] - expected_bounds[0]) < 1.0e-6F);
    EXPECT(std::fabs(trim_bounds[1] - expected_bounds[1]) < 1.0e-6F);
    EXPECT(check_trimmed(ratio_accepted, expected_bounds));
    pano_gpu_session_destroy(&session);
    std::array<uint8_t, 5> encoding_metadata {1, 2, 3, 4, 5};
    pano_gpu_session_create_options encoding_options = session_options;
    encoding_options.encoding_metadata = encoding_metadata.data();
    encoding_options.encoding_metadata_bytes = encoding_metadata.size();
    EXPECT(pano_gpu_session_create(
               device, &encoding_options, &session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    pano_gpu_test_fail_next_encoding_metadata_allocation();
    EXPECT(pano_gpu_session_allocate_encoding_metadata(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_OUT_OF_MEMORY);
    EXPECT(pano_gpu_test_session_encoding_metadata_bytes(session) == 0);
    EXPECT(pano_gpu_session_allocate_encoding_metadata(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_session_encoding_metadata_bytes(session) == 64ULL * 1024);
    EXPECT(pano_gpu_session_allocate_encoding_metadata(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    pano_gpu_test_fail_next_encoding_metadata_upload();
    EXPECT(pano_gpu_session_upload_encoding_metadata(
               session, encoding_metadata.data(), encoding_metadata.size(), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
    EXPECT(pano_gpu_session_upload_encoding_metadata(
               session, encoding_metadata.data(), encoding_metadata.size(), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_upload_encoding_metadata(
               session, encoding_metadata.data(), encoding_metadata.size(), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    std::array<uint8_t, 5> read_encoding_metadata {};
    EXPECT(pano_gpu_test_read_session_encoding_metadata(
               session, read_encoding_metadata.data(), read_encoding_metadata.size(), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(std::memcmp(
               encoding_metadata.data(), read_encoding_metadata.data(), encoding_metadata.size()) == 0);
    session_diagnostics.size = sizeof(session_diagnostics);
    session_diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_query_diagnostics(
               session, &session_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(session_diagnostics.planned_encoding_metadata_bytes == 64ULL * 1024);
    EXPECT(session_diagnostics.encoding_metadata_bytes == 64ULL * 1024);
    pano_gpu_session_destroy(&session);
    std::array<float, 18> multi_rotation {};
    pano_gpu_session_create_options multi_options = session_options;
    multi_options.frame_count = 2;
    multi_options.source_width = 1000;
    multi_options.source_height = 100;
    multi_options.source_row_stride_bytes = 3000;
    multi_options.rotations = multi_rotation.data();
    multi_options.rotations_bytes = sizeof(multi_rotation);
    EXPECT(pano_gpu_session_create(
               device, &multi_options, &session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_prepare_exposure_graph(
               session, 1, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    graph_diagnostics = {};
    graph_diagnostics.size = sizeof(graph_diagnostics);
    graph_diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_query_exposure_graph(
               session, &graph_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(graph_diagnostics.pair_capacity == 1);
    EXPECT(pano_gpu_session_enumerate_exposure_pairs(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<pano_gpu_exposure_pair_report, 1> two_frame_reports {};
    EXPECT(pano_gpu_session_copy_exposure_pair_reports(
               session, two_frame_reports.data(), sizeof(two_frame_reports), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(two_frame_reports[0].left_frame_index == 0);
    EXPECT(two_frame_reports[0].right_frame_index == 1);
    EXPECT(two_frame_reports[0].rejection_reason == PANO_GPU_EXPOSURE_PAIR_PENDING);
    pano_gpu_test_fail_next_allocation();
    EXPECT(pano_gpu_session_prepare_exposure_graph(
               session, 0, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_OUT_OF_MEMORY);
    EXPECT(pano_gpu_session_query_exposure_graph(
               session, &graph_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(graph_diagnostics.pair_capacity == 1);
    EXPECT(pano_gpu_session_prepare_exposure_graph(
               session, 0, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_query_exposure_graph(
               session, &graph_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(graph_diagnostics.pair_capacity == 0);
    pano_gpu_exposure_proxy_request proxy_request {};
    proxy_request.size = sizeof(proxy_request);
    proxy_request.abi_version = PANO_GPU_ABI_VERSION;
    proxy_request.frame_count = multi_options.frame_count;
    proxy_request.source_width = multi_options.source_width;
    proxy_request.source_height = multi_options.source_height;
    pano_gpu_exposure_proxy_layout proxy_layout {};
    proxy_layout.size = sizeof(proxy_layout);
    proxy_layout.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_test_plan_exposure_proxies(
               session, &proxy_request, &proxy_layout, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(proxy_layout.proxy_width == 256);
    EXPECT(proxy_layout.proxy_height == 26);
    EXPECT(proxy_layout.proxy_frame_offset_bytes == 256ULL * 26 * 3 * sizeof(float));
    EXPECT(proxy_layout.proxy_frame_bytes == proxy_layout.proxy_frame_offset_bytes);
    EXPECT(proxy_layout.proxy_total_bytes == 2 * proxy_layout.proxy_frame_bytes);
    EXPECT(pano_gpu_session_allocate_source(session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_session_source_bytes(session) == 10ULL * 64 * 1024);
    pano_gpu_session_destroy(&session);
    multi_options.source_width = 1001;
    multi_options.source_height = 101;
    multi_options.source_row_stride_bytes = 3003;
    EXPECT(pano_gpu_session_create(
               device, &multi_options, &session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    proxy_request.source_width = multi_options.source_width;
    proxy_request.source_height = multi_options.source_height;
    EXPECT(pano_gpu_test_plan_exposure_proxies(
               session, &proxy_request, &proxy_layout, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(proxy_layout.proxy_width == 256);
    EXPECT(proxy_layout.proxy_height == 26);
    EXPECT(proxy_layout.proxy_total_bytes == 2 * 256ULL * 26 * 3 * sizeof(float));
    pano_gpu_session_destroy(&session);
    std::array<float, 27> three_frame_rotation {};
    pano_gpu_session_create_options three_frame_options = session_options;
    three_frame_options.frame_count = 3;
    three_frame_options.rotations = three_frame_rotation.data();
    three_frame_options.rotations_bytes = sizeof(three_frame_rotation);
    EXPECT(pano_gpu_session_create(
               device, &three_frame_options, &session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_prepare_exposure_graph(
               session, 3, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_enumerate_exposure_pairs(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<pano_gpu_exposure_pair_report, 3> three_frame_reports {};
    EXPECT(pano_gpu_session_copy_exposure_pair_reports(
               session, three_frame_reports.data(), sizeof(three_frame_reports), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const std::array<std::array<uint32_t, 2>, 3> expected_pairs {{{0, 1}, {0, 2}, {1, 2}}};
    for (size_t index = 0; index < expected_pairs.size(); ++index)
    {
        EXPECT(three_frame_reports[index].left_frame_index == expected_pairs[index][0]);
        EXPECT(three_frame_reports[index].right_frame_index == expected_pairs[index][1]);
        EXPECT(three_frame_reports[index].rejection_reason == PANO_GPU_EXPOSURE_PAIR_PENDING);
        EXPECT(three_frame_reports[index].valid_count == 0);
        EXPECT(three_frame_reports[index].inlier_count == 0);
        EXPECT(three_frame_reports[index].geometric_count == 0);
    }
    const std::array<pano_gpu_exposure_pair_report, 3> bridge_reports {{
        {0, 1, PANO_GPU_EXPOSURE_PAIR_ACCEPTED, 24, 12, 24},
        {0, 2, PANO_GPU_EXPOSURE_PAIR_INSUFFICIENT_VALID, 0, 0, 24},
        {1, 2, PANO_GPU_EXPOSURE_PAIR_INSUFFICIENT_VALID, 0, 0, 0},
    }};
    const std::array<pano_gpu_exposure_equation, 1> measured_equations {{{0, 1, 0.25, 2.0}}};
    EXPECT(pano_gpu_test_replace_exposure_graph(
               session, bridge_reports.data(), static_cast<uint32_t>(bridge_reports.size()),
               measured_equations.data(), static_cast<uint32_t>(measured_equations.size()),
               error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_build_exposure_solve_graph(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<pano_gpu_exposure_equation, 2> solve_equations {};
    EXPECT(pano_gpu_session_copy_exposure_solve_equations(
               session, solve_equations.data(), sizeof(solve_equations), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(solve_equations[0].left_frame_index == 0);
    EXPECT(solve_equations[0].right_frame_index == 1);
    EXPECT(solve_equations[0].difference == 0.25);
    EXPECT(solve_equations[0].weight == 2.0);
    EXPECT(solve_equations[1].left_frame_index == 0);
    EXPECT(solve_equations[1].right_frame_index == 2);
    EXPECT(solve_equations[1].difference == 0.0);
    EXPECT(solve_equations[1].weight == 1.0);
    graph_diagnostics = {};
    graph_diagnostics.size = sizeof(graph_diagnostics);
    graph_diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_query_exposure_graph(
               session, &graph_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(graph_diagnostics.equation_count == 1);
    EXPECT(graph_diagnostics.solve_equation_count == 2);
    pano_gpu_exposure_solve_result solve_result {};
    solve_result.size = sizeof(solve_result);
    solve_result.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_solve_exposure_graph(
               session, &solve_result, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 3> solved_gains {};
    EXPECT(pano_gpu_session_copy_exposure_log_gains(
               session, solved_gains.data(), sizeof(solved_gains), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(solve_result.anchor_frame_index == 0);
    EXPECT(solve_result.edge_count == 2);
    EXPECT(std::fabs(solved_gains[0]) < 1.0e-7F);
    EXPECT(std::fabs(solved_gains[1] - 0.25F) < 1.0e-6F);
    EXPECT(std::fabs(solved_gains[2]) < 1.0e-7F);
    auto disconnected_reports = bridge_reports;
    disconnected_reports[1].geometric_count = 0;
    EXPECT(pano_gpu_test_replace_exposure_graph(
               session, disconnected_reports.data(),
               static_cast<uint32_t>(disconnected_reports.size()), measured_equations.data(),
               static_cast<uint32_t>(measured_equations.size()), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_build_exposure_solve_graph(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_copy_exposure_solve_equations(
               session, solve_equations.data(), sizeof(pano_gpu_exposure_equation), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const std::array<pano_gpu_exposure_pair_report, 3> weighted_reports {{
        {0, 1, PANO_GPU_EXPOSURE_PAIR_ACCEPTED, 24, 12, 24},
        {0, 2, PANO_GPU_EXPOSURE_PAIR_ACCEPTED, 24, 12, 24},
        {1, 2, PANO_GPU_EXPOSURE_PAIR_ACCEPTED, 24, 12, 24},
    }};
    const std::array<pano_gpu_exposure_equation, 3> weighted_equations {{
        {0, 1, 0.2, 1.0}, {0, 2, 0.0, 1.0}, {1, 2, 0.4, 3.0}}};
    EXPECT(pano_gpu_test_replace_exposure_graph(
               session, weighted_reports.data(), static_cast<uint32_t>(weighted_reports.size()),
               weighted_equations.data(), static_cast<uint32_t>(weighted_equations.size()),
               error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_build_exposure_solve_graph(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_solve_exposure_graph(
               session, &solve_result, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_copy_exposure_log_gains(
               session, solved_gains.data(), sizeof(solved_gains), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(solve_result.edge_count == 3);
    EXPECT(solve_result.anchor_frame_index == 0);
    EXPECT(std::fabs(solved_gains[0]) < 1.0e-7F);
    EXPECT(std::fabs(solved_gains[1] + 0.057142857F) < 1.0e-6F);
    EXPECT(std::fabs(solved_gains[2] - 0.257142857F) < 1.0e-6F);
    pano_gpu_exposure_pair_request resident_request {};
    resident_request.size = sizeof(resident_request);
    resident_request.abi_version = PANO_GPU_ABI_VERSION;
    resident_request.first_frame_index = 0;
    resident_request.second_frame_index = 1;
    resident_request.sample_width = 40;
    resident_request.sample_height = 1;
    resident_request.latitude_span_degrees = 180.0F;
    resident_request.horizontal_fov_degrees = 90.0F;
    resident_request.vertical_fov_degrees = 60.0F;
    EXPECT(pano_gpu_session_prepare_exposure_pair_scratch(
               session, &resident_request, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    pano_gpu_exposure_pair_scratch_diagnostics scratch_diagnostics {};
    scratch_diagnostics.size = sizeof(scratch_diagnostics);
    scratch_diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_query_exposure_pair_scratch(
               session, &scratch_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(scratch_diagnostics.sample_count == 40);
    EXPECT(scratch_diagnostics.sortable_capacity == 64);
    EXPECT(scratch_diagnostics.device_bytes == 68ULL * 40 + 16ULL * 64 + 80);
    EXPECT(scratch_diagnostics.readback_bytes == 0);
    EXPECT(scratch_diagnostics.resource_count == 15);
    EXPECT(scratch_diagnostics.reserved == 0);
    pano_gpu_test_fail_next_allocation();
    resident_request.sample_width = 32;
    EXPECT(pano_gpu_session_prepare_exposure_pair_scratch(
               session, &resident_request, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_OUT_OF_MEMORY);
    EXPECT(pano_gpu_session_query_exposure_pair_scratch(
               session, &scratch_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(scratch_diagnostics.sample_count == 40);
    EXPECT(pano_gpu_session_prepare_exposure_pair_scratch(
               session, &resident_request, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_query_exposure_pair_scratch(
               session, &scratch_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(scratch_diagnostics.sample_count == 32);
    EXPECT(scratch_diagnostics.sortable_capacity == 32);
    EXPECT(scratch_diagnostics.device_bytes == 68ULL * 32 + 16ULL * 32 + 80);
    pano_gpu_session_clear_exposure_pair_scratch(session);
    pano_gpu_session_clear_exposure_pair_scratch(session);
    EXPECT(pano_gpu_session_query_exposure_pair_scratch(
               session, &scratch_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(scratch_diagnostics.sample_count == 0);
    EXPECT(scratch_diagnostics.device_bytes == 0);
    EXPECT(scratch_diagnostics.resource_count == 0);
    EXPECT(pano_gpu_session_allocate_source(session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_upload_slot(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_second_upload_slot(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<uint8_t, 48> second_source_data {};
    std::array<uint8_t, 48> third_source_data {};
    for (size_t index = 0; index < second_source_data.size(); ++index)
    {
        second_source_data[index] = static_cast<uint8_t>(255U - index * 3U);
        third_source_data[index] = static_cast<uint8_t>(index * 5U + 7U);
    }
    pano_gpu_source_upload second_upload = source_upload;
    pano_gpu_cancellation_token *active_upload_token = nullptr;
    EXPECT(pano_gpu_cancellation_token_create(
               &active_upload_token, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_test_fail_next_fence_signal();
    EXPECT(pano_gpu_session_upload_frame_cancellable(
               session, &second_upload, active_upload_token, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
    EXPECT(pano_gpu_session_upload_frame_cancellable(
               session, &second_upload, active_upload_token, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_cancellation_token_destroy(&active_upload_token);
    const uint64_t first_upload_fence = pano_gpu_test_session_first_upload_slot_fence(session);
    EXPECT(first_upload_fence != 0);
    second_upload.frame_index = 1;
    second_upload.data = second_source_data.data();
    pano_gpu_cancellation_token *cancelled_upload_token = nullptr;
    EXPECT(pano_gpu_cancellation_token_create(
               &cancelled_upload_token, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_cancellation_token_cancel(cancelled_upload_token);
    EXPECT(pano_gpu_session_upload_frame_cancellable(
               session, &second_upload, cancelled_upload_token, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_CANCELLED);
    pano_gpu_session_diagnostics cancelled_upload_diagnostics {};
    cancelled_upload_diagnostics.size = sizeof(cancelled_upload_diagnostics);
    cancelled_upload_diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_query_diagnostics(
               session, &cancelled_upload_diagnostics, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(cancelled_upload_diagnostics.upload_count == 1);
    EXPECT(cancelled_upload_diagnostics.uploaded_bytes == source_data.size());
    pano_gpu_cancellation_token_destroy(&cancelled_upload_token);
    EXPECT(pano_gpu_session_upload_frame(
               session, &second_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const uint64_t second_upload_fence = pano_gpu_test_session_second_upload_slot_fence(session);
    EXPECT(second_upload_fence != 0);
    second_upload.frame_index = 2;
    second_upload.data = third_source_data.data();
    pano_gpu_cancellation_token *post_wait_upload_token = nullptr;
    EXPECT(pano_gpu_cancellation_token_create(
               &post_wait_upload_token, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_test_cancel_after_next_upload_slot_wait();
    EXPECT(pano_gpu_session_upload_frame_cancellable(
               session, &second_upload, post_wait_upload_token, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_CANCELLED);
    EXPECT(pano_gpu_test_session_first_upload_slot_fence(session) == first_upload_fence);
    EXPECT(pano_gpu_test_read_session_frame(
               session, 0, read_source_data.data(), read_source_data.size(), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(std::memcmp(source_data.data(), read_source_data.data(), source_data.size()) == 0);
    pano_gpu_cancellation_token_destroy(&post_wait_upload_token);
    EXPECT(pano_gpu_session_upload_frame(
               session, &second_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_session_upload_slot_bytes(session) == 64ULL * 1024);
    EXPECT(pano_gpu_test_session_first_upload_slot_fence(session) > first_upload_fence);
    EXPECT(pano_gpu_test_session_second_upload_slot_fence(session) == second_upload_fence);
    pano_gpu_cancellation_token *finish_token = nullptr;
    EXPECT(pano_gpu_cancellation_token_create(
               &finish_token, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_test_cancel_after_next_upload_finish_wait();
    EXPECT(pano_gpu_session_finish_uploads_cancellable(
               session, finish_token, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_CANCELLED);
    pano_gpu_cancellation_token_destroy(&finish_token);
    EXPECT(pano_gpu_session_finish_uploads(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_session_diagnostics finished_upload_diagnostics {};
    finished_upload_diagnostics.size = sizeof(finished_upload_diagnostics);
    finished_upload_diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_query_diagnostics(
               session, &finished_upload_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(finished_upload_diagnostics.last_completed_upload_fence >=
           std::max(
               pano_gpu_test_session_first_upload_slot_fence(session),
               pano_gpu_test_session_second_upload_slot_fence(session)));
    EXPECT(pano_gpu_test_read_session_frame(
               session, 0, read_source_data.data(), read_source_data.size(), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(std::memcmp(source_data.data(), read_source_data.data(), source_data.size()) == 0);
    EXPECT(pano_gpu_test_read_session_frame(
               session, 1, read_source_data.data(), read_source_data.size(), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(std::memcmp(second_source_data.data(), read_source_data.data(), second_source_data.size()) == 0);
    EXPECT(pano_gpu_test_read_session_frame(
               session, 2, read_source_data.data(), read_source_data.size(), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(std::memcmp(third_source_data.data(), read_source_data.data(), third_source_data.size()) == 0);
    pano_gpu_one_frame_composite_request ordered_frame_requests[3] {};
    for (uint32_t index = 0; index < 3; ++index)
    {
        ordered_frame_requests[index] = composite_request;
        ordered_frame_requests[index].frame_index = index;
        ordered_frame_requests[index].source_sample_type = PANO_GPU_SAMPLE_UINT8;
    }
    pano_gpu_ordered_hard_composite_request ordered_request {};
    ordered_request.size = sizeof(ordered_request);
    ordered_request.abi_version = PANO_GPU_ABI_VERSION;
    ordered_request.frame_request_count = 2;
    ordered_request.frame_requests = ordered_frame_requests;
    pano_gpu_ordered_hard_composite_result_layout ordered_layout {};
    ordered_layout.size = sizeof(ordered_layout);
    ordered_layout.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_test_validate_ordered_hard_composite_request(
               session, &ordered_request, &ordered_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(ordered_layout.pixel_count == 16);
    EXPECT(ordered_layout.selected_rgb_bytes == 16 * 3 * sizeof(float));
    EXPECT(ordered_layout.selected_weight_bytes == 16 * sizeof(float));
    EXPECT(ordered_layout.coverage_bytes == 16);
    EXPECT(pano_gpu_test_validate_two_frame_uint8_hard_composite_request(
               session, &ordered_request, &ordered_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    ordered_request.frame_request_count = 1;
    EXPECT(pano_gpu_test_validate_ordered_hard_composite_request(
               session, &ordered_request, &ordered_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    ordered_request.frame_request_count = 2;
    EXPECT(pano_gpu_test_validate_two_frame_uint8_hard_composite_request(
               session, &ordered_request, &ordered_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    ordered_frame_requests[1].source_sample_type = PANO_GPU_SAMPLE_UINT16;
    EXPECT(pano_gpu_test_validate_two_frame_uint8_hard_composite_request(
               session, &ordered_request, &ordered_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    ordered_frame_requests[1].source_sample_type = PANO_GPU_SAMPLE_UINT8;
    std::swap(ordered_frame_requests[0], ordered_frame_requests[1]);
    EXPECT(pano_gpu_test_validate_ordered_hard_composite_request(
               session, &ordered_request, &ordered_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    std::swap(ordered_frame_requests[0], ordered_frame_requests[1]);
    ordered_frame_requests[1].row_start += 1;
    EXPECT(pano_gpu_test_validate_ordered_hard_composite_request(
               session, &ordered_request, &ordered_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    ordered_frame_requests[1].row_start -= 1;
    ordered_frame_requests[1].frame_index = 3;
    EXPECT(pano_gpu_test_validate_ordered_hard_composite_request(
               session, &ordered_request, &ordered_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    ordered_frame_requests[1].frame_index = 1;
    std::array<float, 48> ordered_first_candidates {};
    std::array<float, 48> ordered_second_candidates {};
    std::array<uint8_t, 16> ordered_first_validity {};
    std::array<uint8_t, 16> ordered_second_validity {};
    std::array<float, 16> ordered_first_edge {};
    std::array<float, 16> ordered_second_edge {};
    EXPECT(pano_gpu_test_dispatch_one_frame_uint8_candidates(
               session, &ordered_frame_requests[0], ordered_first_candidates.data(), sizeof(ordered_first_candidates),
               ordered_first_validity.data(), sizeof(ordered_first_validity), ordered_first_edge.data(),
               sizeof(ordered_first_edge), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_dispatch_one_frame_uint8_candidates(
               session, &ordered_frame_requests[1], ordered_second_candidates.data(), sizeof(ordered_second_candidates),
               ordered_second_validity.data(), sizeof(ordered_second_validity), ordered_second_edge.data(),
               sizeof(ordered_second_edge), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<float, 48> ordered_rgb {};
    std::array<float, 16> ordered_weight {};
    std::array<uint8_t, 16> ordered_coverage {};
    EXPECT(pano_gpu_test_dispatch_two_frame_uint8_hard_composite(
               session, &ordered_request, ordered_rgb.data(), sizeof(ordered_rgb), ordered_weight.data(),
               sizeof(ordered_weight), ordered_coverage.data(), sizeof(ordered_coverage), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < ordered_coverage.size(); ++pixel)
    {
        const float first_weight = ordered_first_validity[pixel] != 0 ? std::max(ordered_first_edge[pixel], 1.0e-6F) : 0.0F;
        const float second_weight = ordered_second_validity[pixel] != 0 ? std::max(ordered_second_edge[pixel], 1.0e-6F) : 0.0F;
        const bool select_second = second_weight > first_weight;
        const float expected_weight = select_second ? second_weight : first_weight;
        EXPECT(std::fabs(ordered_weight[pixel] - expected_weight) < 1.0e-6F);
        EXPECT(ordered_coverage[pixel] == static_cast<uint8_t>(expected_weight > 0.0F));
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const float expected_rgb = expected_weight == 0.0F ? 0.0F :
                (select_second ? ordered_second_candidates[3 * pixel + channel]
                               : ordered_first_candidates[3 * pixel + channel]);
            EXPECT(std::fabs(ordered_rgb[3 * pixel + channel] - expected_rgb) < 1.0e-5F);
        }
    }
    const std::array<float, 2> ordered_global_gains {0.5F, 2.0F};
    EXPECT(pano_gpu_test_dispatch_two_frame_uint8_hard_composite_with_gains(
               session, &ordered_request, ordered_global_gains.data(), sizeof(ordered_global_gains), ordered_rgb.data(),
               sizeof(ordered_rgb), ordered_weight.data(), sizeof(ordered_weight), ordered_coverage.data(),
               sizeof(ordered_coverage), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < ordered_coverage.size(); ++pixel)
    {
        const float first_weight = ordered_first_validity[pixel] != 0 ? std::max(ordered_first_edge[pixel], 1.0e-6F) : 0.0F;
        const float second_weight = ordered_second_validity[pixel] != 0 ? std::max(ordered_second_edge[pixel], 1.0e-6F) : 0.0F;
        const bool select_second = second_weight > first_weight;
        const float expected_weight = select_second ? second_weight : first_weight;
        EXPECT(std::fabs(ordered_weight[pixel] - expected_weight) < 1.0e-6F);
        EXPECT(ordered_coverage[pixel] == static_cast<uint8_t>(expected_weight > 0.0F));
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const float expected_rgb = expected_weight == 0.0F ? 0.0F :
                (select_second ? ordered_second_candidates[3 * pixel + channel] * ordered_global_gains[1]
                               : ordered_first_candidates[3 * pixel + channel] * ordered_global_gains[0]);
            EXPECT(std::fabs(ordered_rgb[3 * pixel + channel] - expected_rgb) < 1.0e-5F);
        }
    }
    const std::array<float, 4> ordered_local_fields {0.0F, 0.0F, 0.0F, std::log(2.0F)};
    EXPECT(pano_gpu_test_dispatch_two_frame_uint8_hard_composite_with_exposure(
               session, &ordered_request, ordered_global_gains.data(), sizeof(ordered_global_gains),
               ordered_local_fields.data(), sizeof(ordered_local_fields), ordered_rgb.data(), sizeof(ordered_rgb),
               ordered_weight.data(), sizeof(ordered_weight), ordered_coverage.data(), sizeof(ordered_coverage),
               error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < ordered_coverage.size(); ++pixel)
    {
        const float first_weight = ordered_first_validity[pixel] != 0 ? std::max(ordered_first_edge[pixel], 1.0e-6F) : 0.0F;
        const float second_weight = ordered_second_validity[pixel] != 0 ? std::max(ordered_second_edge[pixel], 1.0e-6F) : 0.0F;
        const bool select_second = second_weight > first_weight;
        const float field_x = std::clamp(static_cast<float>(pixel % 8) * 0.25F - 0.375F, 0.0F, 1.0F);
        const float gain = select_second ? 2.0F * std::exp(field_x * std::log(2.0F)) : 0.5F;
        const float selected_weight = select_second ? second_weight : first_weight;
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const float expected_rgb = selected_weight == 0.0F ? 0.0F :
                (select_second ? ordered_second_candidates[3 * pixel + channel]
                               : ordered_first_candidates[3 * pixel + channel]) * gain;
            EXPECT(std::fabs(ordered_rgb[3 * pixel + channel] - expected_rgb) < 1.0e-5F);
        }
    }
    ordered_request.frame_request_count = 3;
    EXPECT(pano_gpu_test_validate_three_frame_uint8_hard_composite_request(
               session, &ordered_request, &ordered_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 48> ordered_third_candidates {};
    std::array<uint8_t, 16> ordered_third_validity {};
    std::array<float, 16> ordered_third_edge {};
    EXPECT(pano_gpu_test_dispatch_one_frame_uint8_candidates(
               session, &ordered_frame_requests[2], ordered_third_candidates.data(), sizeof(ordered_third_candidates),
               ordered_third_validity.data(), sizeof(ordered_third_validity), ordered_third_edge.data(),
               sizeof(ordered_third_edge), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<float, 48> ordered_three_rgb {};
    std::array<float, 16> ordered_three_weight {};
    std::array<uint8_t, 16> ordered_three_coverage {};
    EXPECT(pano_gpu_test_dispatch_three_frame_uint8_hard_composite(
               session, &ordered_request, ordered_three_rgb.data(), sizeof(ordered_three_rgb),
               ordered_three_weight.data(), sizeof(ordered_three_weight), ordered_three_coverage.data(),
               sizeof(ordered_three_coverage), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < ordered_three_coverage.size(); ++pixel)
    {
        const float first_weight = ordered_first_validity[pixel] != 0 ? std::max(ordered_first_edge[pixel], 1.0e-6F) : 0.0F;
        const float second_weight = ordered_second_validity[pixel] != 0 ? std::max(ordered_second_edge[pixel], 1.0e-6F) : 0.0F;
        const bool select_second = second_weight > first_weight;
        const float prior_weight = select_second ? second_weight : first_weight;
        const float third_weight = ordered_third_validity[pixel] != 0 ? std::max(ordered_third_edge[pixel], 1.0e-6F) : 0.0F;
        const bool select_third = third_weight > prior_weight;
        const float expected_weight = select_third ? third_weight : prior_weight;
        EXPECT(std::fabs(ordered_three_weight[pixel] - expected_weight) < 1.0e-6F);
        EXPECT(ordered_three_coverage[pixel] == static_cast<uint8_t>(expected_weight > 0.0F));
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const float expected_rgb = expected_weight == 0.0F ? 0.0F :
                (select_third ? ordered_third_candidates[3 * pixel + channel] :
                 (select_second ? ordered_second_candidates[3 * pixel + channel]
                                : ordered_first_candidates[3 * pixel + channel]));
            EXPECT(std::fabs(ordered_three_rgb[3 * pixel + channel] - expected_rgb) < 1.0e-5F);
        }
    }
    EXPECT(pano_gpu_session_prepare_exposure_graph(
               session, 3, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    const std::array<pano_gpu_exposure_pair_report, 3> composition_gain_reports {{
        {0, 1, PANO_GPU_EXPOSURE_PAIR_ACCEPTED, 24, 12, 24},
        {0, 2, PANO_GPU_EXPOSURE_PAIR_ACCEPTED, 24, 12, 24},
        {1, 2, PANO_GPU_EXPOSURE_PAIR_ACCEPTED, 24, 12, 24},
    }};
    const double log_two = std::log(2.0);
    const std::array<pano_gpu_exposure_equation, 3> composition_gain_equations {{
        {0, 1, log_two, 1.0}, {0, 2, 2.0 * log_two, 1.0}, {1, 2, log_two, 1.0}}};
    EXPECT(pano_gpu_test_replace_exposure_graph(
               session, composition_gain_reports.data(),
               static_cast<uint32_t>(composition_gain_reports.size()),
               composition_gain_equations.data(),
               static_cast<uint32_t>(composition_gain_equations.size()), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_build_exposure_solve_graph(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_exposure_solve_result composition_gain_result {};
    composition_gain_result.size = sizeof(composition_gain_result);
    composition_gain_result.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_solve_exposure_graph(
               session, &composition_gain_result, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_upload_exposure_gains(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_upload_exposure_gains(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    pano_gpu_exposure_report retained_report {};
    retained_report.size = sizeof(retained_report);
    retained_report.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_query_exposure_report(
               session, &retained_report, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(retained_report.gains_uploaded == 1);
    EXPECT(retained_report.solve_count > 0);
    EXPECT(retained_report.gain_upload_count > 0);
    EXPECT(pano_gpu_test_dispatch_three_frame_uint8_hard_composite_with_session_gains(
               session, &ordered_request, ordered_three_rgb.data(), sizeof(ordered_three_rgb),
               ordered_three_weight.data(), sizeof(ordered_three_weight), ordered_three_coverage.data(),
               sizeof(ordered_three_coverage), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_dispatch_three_frame_uint8_hard_composite_with_session_gains(
               session, &ordered_request, ordered_three_rgb.data(), sizeof(ordered_three_rgb),
               ordered_three_weight.data(), sizeof(ordered_three_weight), ordered_three_coverage.data(),
               sizeof(ordered_three_coverage), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    pano_gpu_exposure_report reused_report {};
    reused_report.size = sizeof(reused_report);
    reused_report.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_query_exposure_report(
               session, &reused_report, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(reused_report.anchor_frame_index == retained_report.anchor_frame_index);
    EXPECT(reused_report.edge_count == retained_report.edge_count);
    EXPECT(reused_report.solve_count == retained_report.solve_count);
    EXPECT(reused_report.gain_upload_count == retained_report.gain_upload_count);
    const std::array<float, 3> composition_gains {0.5F, 1.0F, 2.0F};
#if defined(_WIN32)
    pano_gpu_one_frame_composite_request retained_output_frames[3] {};
    for (uint32_t frame = 0; frame < 3; ++frame)
    {
        retained_output_frames[frame] = ordered_frame_requests[frame];
        retained_output_frames[frame].row_start = 0;
        retained_output_frames[frame].row_count = retained_output_frames[frame].output_height;
    }
    pano_gpu_ordered_hard_composite_request retained_output_request = ordered_request;
    retained_output_request.frame_requests = retained_output_frames;
    pano_gpu_output_create_options retained_output_options {};
    retained_output_options.size = sizeof(retained_output_options);
    retained_output_options.abi_version = PANO_GPU_ABI_VERSION;
    retained_output_options.output_width = 8;
    retained_output_options.output_height = 4;
    retained_output_options.output_sample_bytes = 1;
    retained_output_options.descriptor_count = 7;
    retained_output_options.output_workspace_bytes = 8ULL * 4 * 13;
    pano_gpu_output *retained_gain_output = nullptr;
    EXPECT(pano_gpu_output_create_empty(
               session, &retained_output_options, &retained_gain_output, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_linear(
               retained_gain_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_coverage(
               retained_gain_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_composite_inputs retained_gain_inputs {};
    retained_gain_inputs.size = sizeof(retained_gain_inputs);
    retained_gain_inputs.abi_version = PANO_GPU_ABI_VERSION;
    retained_gain_inputs.use_session_exposure_gains = 1;
    pano_gpu_composite_inputs explicit_gain_inputs = retained_gain_inputs;
    explicit_gain_inputs.use_session_exposure_gains = 0;
    explicit_gain_inputs.global_gains = composition_gains.data();
    explicit_gain_inputs.global_gain_bytes = sizeof(composition_gains);
    for (const bool feather : {false, true})
    {
        std::array<float, 96> retained_rgb {};
        std::array<float, 96> explicit_rgb {};
        std::array<uint8_t, 32> retained_coverage {};
        std::array<uint8_t, 32> explicit_coverage {};
        EXPECT((feather
                    ? pano_gpu_output_compose_feather_with_inputs(
                          retained_gain_output, &retained_output_request, &retained_gain_inputs,
                          error.data(), static_cast<uint32_t>(error.size()))
                    : pano_gpu_output_compose_hard_with_inputs(
                          retained_gain_output, &retained_output_request, &retained_gain_inputs,
                          error.data(), static_cast<uint32_t>(error.size()))) == PANO_GPU_SUCCESS);
        EXPECT(pano_gpu_test_read_output_band(
                   retained_gain_output, retained_rgb.data(), sizeof(retained_rgb), retained_coverage.data(),
                   sizeof(retained_coverage), error.data(), static_cast<uint32_t>(error.size())) ==
               PANO_GPU_SUCCESS);
        EXPECT((feather
                    ? pano_gpu_output_compose_feather_with_inputs(
                          retained_gain_output, &retained_output_request, &explicit_gain_inputs,
                          error.data(), static_cast<uint32_t>(error.size()))
                    : pano_gpu_output_compose_hard_with_inputs(
                          retained_gain_output, &retained_output_request, &explicit_gain_inputs,
                          error.data(), static_cast<uint32_t>(error.size()))) == PANO_GPU_SUCCESS);
        EXPECT(pano_gpu_test_read_output_band(
                   retained_gain_output, explicit_rgb.data(), sizeof(explicit_rgb), explicit_coverage.data(),
                   sizeof(explicit_coverage), error.data(), static_cast<uint32_t>(error.size())) ==
               PANO_GPU_SUCCESS);
        EXPECT(retained_coverage == explicit_coverage);
        for (size_t index = 0; index < retained_rgb.size(); ++index)
            EXPECT(std::fabs(retained_rgb[index] - explicit_rgb[index]) < 1.0e-5F);
    }
    pano_gpu_output_destroy(&retained_gain_output);
#endif
    for (size_t pixel = 0; pixel < ordered_three_coverage.size(); ++pixel)
    {
        const float weights[3] {
            ordered_first_validity[pixel] != 0 ? std::max(ordered_first_edge[pixel], 1.0e-6F) : 0.0F,
            ordered_second_validity[pixel] != 0 ? std::max(ordered_second_edge[pixel], 1.0e-6F) : 0.0F,
            ordered_third_validity[pixel] != 0 ? std::max(ordered_third_edge[pixel], 1.0e-6F) : 0.0F};
        uint32_t selected = weights[1] > weights[0] ? 1U : 0U;
        if (weights[2] > weights[selected])
            selected = 2;
        const std::array<const float *, 3> candidates {
            ordered_first_candidates.data(), ordered_second_candidates.data(),
            ordered_third_candidates.data()};
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const float expected = weights[selected] == 0.0F
                ? 0.0F
                : candidates[selected][3 * pixel + channel] * composition_gains[selected];
            EXPECT(std::fabs(ordered_three_rgb[3 * pixel + channel] - expected) < 1.0e-5F);
        }
    }
    const uint64_t retained_source_bytes = pano_gpu_test_session_source_bytes(session);
    const uint64_t retained_proxy_bytes = pano_gpu_test_session_exposure_proxy_bytes(session);
    EXPECT(pano_gpu_session_invalidate_exposure(
               session, PANO_GPU_EXPOSURE_INVALIDATE_MANUAL_GAINS, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_invalidate_exposure(
               session, PANO_GPU_EXPOSURE_INVALIDATE_MANUAL_GAINS, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_exposure_report manual_invalidated_report {};
    manual_invalidated_report.size = sizeof(manual_invalidated_report);
    manual_invalidated_report.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_query_exposure_report(
               session, &manual_invalidated_report, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(manual_invalidated_report.gains_uploaded == 0);
    EXPECT(manual_invalidated_report.solve_count == retained_report.solve_count);
    EXPECT(pano_gpu_test_dispatch_three_frame_uint8_hard_composite_with_session_gains(
               session, &ordered_request, ordered_three_rgb.data(), sizeof(ordered_three_rgb),
               ordered_three_weight.data(), sizeof(ordered_three_weight), ordered_three_coverage.data(),
               sizeof(ordered_three_coverage), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    EXPECT(pano_gpu_session_upload_exposure_gains(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_invalidate_exposure(
               session, PANO_GPU_EXPOSURE_INVALIDATE_GEOMETRY, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_invalidate_exposure(
               session, PANO_GPU_EXPOSURE_INVALIDATE_GEOMETRY, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_query_exposure_report(
               session, &manual_invalidated_report, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    graph_diagnostics = {};
    graph_diagnostics.size = sizeof(graph_diagnostics);
    graph_diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_query_exposure_graph(
               session, &graph_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(graph_diagnostics.pair_capacity == 0);
    EXPECT(graph_diagnostics.solve_equation_count == 0);
    EXPECT(pano_gpu_test_session_source_bytes(session) == retained_source_bytes);
    EXPECT(pano_gpu_test_session_exposure_proxy_bytes(session) == retained_proxy_bytes);
    EXPECT(pano_gpu_session_invalidate_exposure(
               session, 0, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    pano_gpu_one_frame_composite_request output_frame_requests[3] {};
    for (uint32_t index = 0; index < 3; ++index)
    {
        output_frame_requests[index] = ordered_frame_requests[index];
        output_frame_requests[index].row_start = 0;
        output_frame_requests[index].row_count = output_frame_requests[index].output_height;
    }
    pano_gpu_ordered_hard_composite_request output_ordered_request = ordered_request;
    output_ordered_request.frame_requests = output_frame_requests;
    pano_gpu_output_create_options ordered_output_options {};
    ordered_output_options.size = sizeof(ordered_output_options);
    ordered_output_options.abi_version = PANO_GPU_ABI_VERSION;
    ordered_output_options.output_width = ordered_frame_requests[0].output_width;
    ordered_output_options.output_height = ordered_frame_requests[0].output_height;
    ordered_output_options.output_sample_bytes = 1;
    ordered_output_options.descriptor_count = 7;
    ordered_output_options.output_workspace_bytes = 8ULL * 4 * 13;
    pano_gpu_output *ordered_output = nullptr;
    EXPECT(pano_gpu_output_create_empty(
               session, &ordered_output_options, &ordered_output, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_validate_output_hard_composite_request(
               ordered_output, &output_ordered_request, &ordered_layout, error.data(), static_cast<uint32_t>(error.size())) !=
           PANO_GPU_SUCCESS);
#if defined(_WIN32)
    EXPECT(pano_gpu_output_allocate_linear(
               ordered_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_coverage(
               ordered_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_validate_output_hard_composite_request(
               ordered_output, &output_ordered_request, &ordered_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 96> expected_output_rgb {};
    std::array<float, 32> expected_output_weight {};
    std::array<uint8_t, 32> expected_output_coverage {};
    const pano_gpu_result expected_output_result = pano_gpu_test_dispatch_three_frame_uint8_hard_composite(
               session, &output_ordered_request, expected_output_rgb.data(), sizeof(expected_output_rgb),
               expected_output_weight.data(), sizeof(expected_output_weight), expected_output_coverage.data(),
               sizeof(expected_output_coverage), error.data(), static_cast<uint32_t>(error.size()));
    EXPECT(expected_output_result == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_compose_hard(
               ordered_output, &output_ordered_request, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<float, 96> stored_output_rgb {};
    std::array<uint8_t, 32> stored_output_coverage {};
    EXPECT(pano_gpu_test_read_output_band(
               ordered_output, stored_output_rgb.data(), sizeof(stored_output_rgb), stored_output_coverage.data(),
               sizeof(stored_output_coverage), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < stored_output_coverage.size(); ++pixel)
    {
        EXPECT(stored_output_coverage[pixel] == expected_output_coverage[pixel]);
        for (size_t channel = 0; channel < 3; ++channel)
            EXPECT(std::fabs(stored_output_rgb[3 * pixel + channel] - expected_output_rgb[3 * pixel + channel]) < 1.0e-5F);
    }
    EXPECT(pano_gpu_output_compose_feather(
               ordered_output, &output_ordered_request, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_read_output_band(
               ordered_output, stored_output_rgb.data(), sizeof(stored_output_rgb), stored_output_coverage.data(),
               sizeof(stored_output_coverage), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<std::array<float, 96>, 3> resident_candidate_rgb {};
    std::array<std::array<uint8_t, 32>, 3> resident_candidate_validity {};
    std::array<std::array<float, 32>, 3> resident_candidate_edge {};
    for (size_t frame = 0; frame < resident_candidate_rgb.size(); ++frame)
        EXPECT(pano_gpu_test_dispatch_one_frame_uint8_candidates(
                   session, &output_frame_requests[frame], resident_candidate_rgb[frame].data(),
                   sizeof(resident_candidate_rgb[frame]), resident_candidate_validity[frame].data(),
                   sizeof(resident_candidate_validity[frame]), resident_candidate_edge[frame].data(),
                   sizeof(resident_candidate_edge[frame]), error.data(), static_cast<uint32_t>(error.size())) ==
               PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < stored_output_coverage.size(); ++pixel)
    {
        float expected_weight = 0.0F;
        std::array<float, 3> expected_rgb {};
        for (size_t frame = 0; frame < resident_candidate_rgb.size(); ++frame)
        {
            const float weight = resident_candidate_validity[frame][pixel] != 0
                ? std::max(resident_candidate_edge[frame][pixel], 1.0e-6F)
                : 0.0F;
            expected_weight += weight;
            for (size_t channel = 0; channel < 3; ++channel)
                expected_rgb[channel] += resident_candidate_rgb[frame][3 * pixel + channel] * weight;
        }
        EXPECT(stored_output_coverage[pixel] == static_cast<uint8_t>(expected_weight > 0.0F));
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const float normalized = expected_weight > 0.0F
                ? expected_rgb[channel] / expected_weight
                : expected_rgb[channel];
            EXPECT(std::fabs(stored_output_rgb[3 * pixel + channel] - normalized) < 1.0e-5F);
        }
    }
    const auto run_composite_inputs = [&](const bool feather, const pano_gpu_composite_inputs &inputs,
                                          std::array<float, 96> *const rgb,
                                          std::array<uint8_t, 32> *const coverage_values) {
        const pano_gpu_result result = feather
            ? pano_gpu_output_compose_feather_with_inputs(
                  ordered_output, &output_ordered_request, &inputs, error.data(),
                  static_cast<uint32_t>(error.size()))
            : pano_gpu_output_compose_hard_with_inputs(
                  ordered_output, &output_ordered_request, &inputs, error.data(),
                  static_cast<uint32_t>(error.size()));
        if (result != PANO_GPU_SUCCESS)
            return result;
        return pano_gpu_test_read_output_band(
            ordered_output, rgb->data(), sizeof(*rgb), coverage_values->data(), sizeof(*coverage_values),
            error.data(), static_cast<uint32_t>(error.size()));
    };
    pano_gpu_composite_inputs identity_inputs {};
    identity_inputs.size = sizeof(identity_inputs);
    identity_inputs.abi_version = PANO_GPU_ABI_VERSION;
    const std::array<float, 3> doubled_gains {2.0F, 2.0F, 2.0F};
    pano_gpu_composite_inputs doubled_inputs = identity_inputs;
    doubled_inputs.global_gains = doubled_gains.data();
    doubled_inputs.global_gain_bytes = sizeof(doubled_gains);
    std::array<float, 6> doubled_local_fields {};
    doubled_local_fields.fill(std::log(2.0F));
    pano_gpu_composite_inputs local_inputs = identity_inputs;
    local_inputs.local_fields = doubled_local_fields.data();
    local_inputs.local_field_bytes = sizeof(doubled_local_fields);
    pano_gpu_composite_inputs incomplete_inputs = identity_inputs;
    incomplete_inputs.mark_incomplete = 1;
    for (const bool feather : {false, true})
    {
        std::array<float, 96> baseline_rgb {};
        std::array<uint8_t, 32> baseline_coverage {};
        EXPECT(run_composite_inputs(feather, identity_inputs, &baseline_rgb, &baseline_coverage) ==
               PANO_GPU_SUCCESS);
        for (const pano_gpu_composite_inputs *const doubled : {&doubled_inputs, &local_inputs})
        {
            std::array<float, 96> doubled_rgb {};
            std::array<uint8_t, 32> doubled_coverage {};
            EXPECT(run_composite_inputs(feather, *doubled, &doubled_rgb, &doubled_coverage) ==
                   PANO_GPU_SUCCESS);
            EXPECT(doubled_coverage == baseline_coverage);
            for (size_t index = 0; index < doubled_rgb.size(); ++index)
                EXPECT(std::fabs(doubled_rgb[index] - 2.0F * baseline_rgb[index]) < 2.0e-5F);
        }
        std::array<float, 96> incomplete_rgb {};
        std::array<uint8_t, 32> incomplete_coverage {};
        EXPECT(run_composite_inputs(feather, incomplete_inputs, &incomplete_rgb, &incomplete_coverage) ==
               PANO_GPU_SUCCESS);
        EXPECT(incomplete_coverage == baseline_coverage);
        for (size_t pixel = 0; pixel < incomplete_coverage.size(); ++pixel)
            for (size_t channel = 0; channel < 3; ++channel)
            {
                const float expected = incomplete_coverage[pixel] == 0
                    ? (channel == 0 || channel == 2 ? 1.0F : 0.0F)
                    : baseline_rgb[3 * pixel + channel];
                EXPECT(std::fabs(incomplete_rgb[3 * pixel + channel] - expected) < 1.0e-5F);
            }
    }
    pano_gpu_test_fail_next_composite_before_dispatch();
    EXPECT(pano_gpu_output_compose_hard_with_inputs(
               ordered_output, &output_ordered_request, &identity_inputs, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
    EXPECT(std::string(error.data()).find("before first D3D12 numerical dispatch") !=
           std::string::npos);
    EXPECT(pano_gpu_output_compose_hard_with_inputs(
               ordered_output, &output_ordered_request, &identity_inputs, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_test_fail_next_composite_after_dispatch();
    EXPECT(pano_gpu_output_compose_hard_with_inputs(
               ordered_output, &output_ordered_request, &identity_inputs, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
    EXPECT(std::string(error.data()).find("after first D3D12 numerical dispatch") !=
           std::string::npos);
    EXPECT(pano_gpu_output_compose_hard_with_inputs(
               ordered_output, &output_ordered_request, &identity_inputs, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_test_fail_next_device_removed_before_dispatch();
    EXPECT(pano_gpu_output_compose_hard_with_inputs(
               ordered_output, &output_ordered_request, &identity_inputs, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
    EXPECT(std::string(error.data()).find("before first numerical dispatch") != std::string::npos);
    EXPECT(std::string(error.data()).find("HRESULT 0x887a0005") != std::string::npos);
    EXPECT(std::string(error.data()).find("device reason 0x887a0007") != std::string::npos);
    EXPECT(pano_gpu_output_compose_hard_with_inputs(
               ordered_output, &output_ordered_request, &identity_inputs, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_test_fail_next_device_removed_after_dispatch();
    EXPECT(pano_gpu_output_compose_hard_with_inputs(
               ordered_output, &output_ordered_request, &identity_inputs, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
    EXPECT(std::string(error.data()).find("after first numerical dispatch") != std::string::npos);
    EXPECT(std::string(error.data()).find("HRESULT 0x887a0005") != std::string::npos);
    EXPECT(std::string(error.data()).find("device reason 0x887a0007") != std::string::npos);
    EXPECT(pano_gpu_output_compose_hard_with_inputs(
               ordered_output, &output_ordered_request, &identity_inputs, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_composite_inputs invalid_inputs = identity_inputs;
    invalid_inputs.use_session_exposure_gains = 1;
    invalid_inputs.global_gains = doubled_gains.data();
    invalid_inputs.global_gain_bytes = sizeof(doubled_gains);
    EXPECT(pano_gpu_output_compose_hard_with_inputs(
               ordered_output, &output_ordered_request, &invalid_inputs, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_INVALID_ARGUMENT);
    output_frame_requests[1].row_count -= 1;
    EXPECT(pano_gpu_test_validate_output_hard_composite_request(
               ordered_output, &output_ordered_request, &ordered_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    output_frame_requests[1].row_count += 1;
    pano_gpu_one_frame_composite_request later_band_frames[3] {};
    for (uint32_t index = 0; index < 3; ++index)
    {
        later_band_frames[index] = output_frame_requests[index];
        later_band_frames[index].output_height = 64;
        later_band_frames[index].row_start = 32;
        later_band_frames[index].row_count = 32;
    }
    pano_gpu_ordered_hard_composite_request later_band_request = ordered_request;
    later_band_request.frame_requests = later_band_frames;
    pano_gpu_output_create_options later_band_options = ordered_output_options;
    later_band_options.output_height = 64;
    later_band_options.output_band_rows = 32;
    later_band_options.output_workspace_bytes = 8ULL * 32 * 13;
    pano_gpu_output *later_band_output = nullptr;
    EXPECT(pano_gpu_output_create_empty(
               session, &later_band_options, &later_band_output, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_linear(
               later_band_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_coverage(
               later_band_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<float, 8 * 32 * 3> expected_later_band_rgb {};
    std::array<float, 8 * 32> expected_later_band_weight {};
    std::array<uint8_t, 8 * 32> expected_later_band_coverage {};
    EXPECT(pano_gpu_test_dispatch_three_frame_uint8_hard_composite(
               session, &later_band_request, expected_later_band_rgb.data(), sizeof(expected_later_band_rgb),
               expected_later_band_weight.data(), sizeof(expected_later_band_weight), expected_later_band_coverage.data(),
               sizeof(expected_later_band_coverage), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_compose_hard(
               later_band_output, &later_band_request, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<float, 8 * 32 * 3> stored_later_band_rgb {};
    std::array<uint8_t, 8 * 32> stored_later_band_coverage {};
    EXPECT(pano_gpu_test_read_output_band(
               later_band_output, stored_later_band_rgb.data(), sizeof(stored_later_band_rgb),
               stored_later_band_coverage.data(), sizeof(stored_later_band_coverage), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < stored_later_band_coverage.size(); ++pixel)
    {
        EXPECT(stored_later_band_coverage[pixel] == expected_later_band_coverage[pixel]);
        for (size_t channel = 0; channel < 3; ++channel)
            EXPECT(std::fabs(stored_later_band_rgb[3 * pixel + channel] -
                             expected_later_band_rgb[3 * pixel + channel]) < 1.0e-5F);
    }
    pano_gpu_one_frame_composite_request resident_band_frames[3] {};
    for (uint32_t index = 0; index < 3; ++index)
    {
        resident_band_frames[index] = later_band_frames[index];
        resident_band_frames[index].row_start = 0;
        resident_band_frames[index].row_count = 64;
    }
    pano_gpu_ordered_hard_composite_request resident_band_request = later_band_request;
    resident_band_request.frame_requests = resident_band_frames;
    std::array<float, 8 * 64 * 3> expected_resident_band_rgb {};
    std::array<float, 8 * 64> expected_resident_band_weight {};
    std::array<uint8_t, 8 * 64> expected_resident_band_coverage {};
    EXPECT(pano_gpu_test_dispatch_three_frame_uint8_hard_composite(
               session, &resident_band_request, expected_resident_band_rgb.data(), sizeof(expected_resident_band_rgb),
               expected_resident_band_weight.data(), sizeof(expected_resident_band_weight),
               expected_resident_band_coverage.data(), sizeof(expected_resident_band_coverage), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (uint32_t index = 0; index < 3; ++index)
    {
        later_band_frames[index].row_start = 0;
        later_band_frames[index].row_count = 32;
    }
    EXPECT(pano_gpu_output_compose_hard(
               later_band_output, &later_band_request, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<float, 8 * 32 * 3> stored_first_band_rgb {};
    std::array<uint8_t, 8 * 32> stored_first_band_coverage {};
    EXPECT(pano_gpu_test_read_output_band(
               later_band_output, stored_first_band_rgb.data(), sizeof(stored_first_band_rgb),
               stored_first_band_coverage.data(), sizeof(stored_first_band_coverage), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < stored_first_band_coverage.size(); ++pixel)
    {
        EXPECT(stored_first_band_coverage[pixel] == expected_resident_band_coverage[pixel]);
        EXPECT(stored_later_band_coverage[pixel] == expected_resident_band_coverage[pixel + 8 * 32]);
        for (size_t channel = 0; channel < 3; ++channel)
        {
            EXPECT(std::fabs(stored_first_band_rgb[3 * pixel + channel] -
                             expected_resident_band_rgb[3 * pixel + channel]) < 1.0e-5F);
            EXPECT(std::fabs(stored_later_band_rgb[3 * pixel + channel] -
                             expected_resident_band_rgb[3 * (pixel + 8 * 32) + channel]) < 1.0e-5F);
        }
    }
    pano_gpu_output_create_options resident_feather_options = later_band_options;
    resident_feather_options.output_band_rows = 0;
    resident_feather_options.output_workspace_bytes = 8ULL * 64 * 13;
    pano_gpu_output *resident_feather_composite = nullptr;
    EXPECT(pano_gpu_output_create_empty(
               session, &resident_feather_options, &resident_feather_composite, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_linear(
               resident_feather_composite, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_coverage(
               resident_feather_composite, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_compose_feather(
               resident_feather_composite, &resident_band_request, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<float, 8 * 64 * 3> resident_composite_feather_rgb {};
    std::array<uint8_t, 8 * 64> resident_composite_feather_coverage {};
    EXPECT(pano_gpu_test_read_output_band(
               resident_feather_composite, resident_composite_feather_rgb.data(),
               sizeof(resident_composite_feather_rgb), resident_composite_feather_coverage.data(),
               sizeof(resident_composite_feather_coverage), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_compose_feather(
               later_band_output, &later_band_request, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_read_output_band(
               later_band_output, stored_first_band_rgb.data(), sizeof(stored_first_band_rgb),
               stored_first_band_coverage.data(), sizeof(stored_first_band_coverage), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (uint32_t index = 0; index < 3; ++index)
    {
        later_band_frames[index].row_start = 32;
        later_band_frames[index].row_count = 32;
    }
    EXPECT(pano_gpu_test_set_output_band(
               later_band_output, 32, 32, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_compose_feather(
               later_band_output, &later_band_request, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_read_output_band(
               later_band_output, stored_later_band_rgb.data(), sizeof(stored_later_band_rgb),
               stored_later_band_coverage.data(), sizeof(stored_later_band_coverage), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < stored_first_band_coverage.size(); ++pixel)
    {
        EXPECT(stored_first_band_coverage[pixel] == resident_composite_feather_coverage[pixel]);
        EXPECT(stored_later_band_coverage[pixel] == resident_composite_feather_coverage[pixel + 8 * 32]);
        for (size_t channel = 0; channel < 3; ++channel)
        {
            EXPECT(std::fabs(stored_first_band_rgb[3 * pixel + channel] -
                             resident_composite_feather_rgb[3 * pixel + channel]) < 1.0e-5F);
            EXPECT(std::fabs(stored_later_band_rgb[3 * pixel + channel] -
                             resident_composite_feather_rgb[3 * (pixel + 8 * 32) + channel]) < 1.0e-5F);
        }
    }
    pano_gpu_output_destroy(&resident_feather_composite);
    pano_gpu_output_destroy(&later_band_output);
#endif
    pano_gpu_output_destroy(&ordered_output);
    pano_gpu_session_destroy(&session);
    pano_gpu_session_create_options uint16_multi_options = uint16_options;
    uint16_multi_options.frame_count = 3;
    uint16_multi_options.rotations = three_frame_rotation.data();
    uint16_multi_options.rotations_bytes = sizeof(three_frame_rotation);
    EXPECT(pano_gpu_session_create(
               device, &uint16_multi_options, &session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_source(session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_upload_slot(session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_second_upload_slot(session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<uint16_t, 48> second_uint16_source_data {};
    std::array<uint16_t, 48> third_uint16_source_data {};
    for (size_t index = 0; index < second_uint16_source_data.size(); ++index)
    {
        second_uint16_source_data[index] = static_cast<uint16_t>(65535U - index * 619U);
        third_uint16_source_data[index] = static_cast<uint16_t>(index * 431U + 97U);
    }
    pano_gpu_source_upload second_uint16_upload = uint16_upload;
    second_uint16_upload.frame_index = 1;
    second_uint16_upload.data = second_uint16_source_data.data();
    pano_gpu_source_upload third_uint16_upload = uint16_upload;
    third_uint16_upload.frame_index = 2;
    third_uint16_upload.data = third_uint16_source_data.data();
    EXPECT(pano_gpu_session_upload_frame_zero(
               session, &uint16_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_upload_frame(
               session, &second_uint16_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_upload_frame(
               session, &third_uint16_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_finish_uploads(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_one_frame_composite_request uint16_ordered_frames[3] {};
    for (uint32_t index = 0; index < 3; ++index)
    {
        uint16_ordered_frames[index] = composite_request;
        uint16_ordered_frames[index].frame_index = index;
        uint16_ordered_frames[index].source_sample_type = PANO_GPU_SAMPLE_UINT16;
    }
    pano_gpu_ordered_hard_composite_request uint16_ordered_request = ordered_request;
    uint16_ordered_request.frame_request_count = 3;
    uint16_ordered_request.frame_requests = uint16_ordered_frames;
    for (uint32_t frame = 0; frame < 3; ++frame)
    {
        uint16_ordered_frames[frame].row_start = 0;
        uint16_ordered_frames[frame].row_count = uint16_ordered_frames[frame].output_height;
    }
    std::array<std::array<float, 96>, 3> uint16_feather_candidates {};
    std::array<std::array<uint8_t, 32>, 3> uint16_feather_validity {};
    std::array<std::array<float, 32>, 3> uint16_feather_edges {};
    for (uint32_t frame = 0; frame < 3; ++frame)
        EXPECT(pano_gpu_test_dispatch_one_frame_uint16_candidates(
                   session, &uint16_ordered_frames[frame], uint16_feather_candidates[frame].data(),
                   sizeof(uint16_feather_candidates[frame]), uint16_feather_validity[frame].data(),
                   sizeof(uint16_feather_validity[frame]), uint16_feather_edges[frame].data(),
                   sizeof(uint16_feather_edges[frame]), error.data(), static_cast<uint32_t>(error.size())) ==
               PANO_GPU_SUCCESS);
    pano_gpu_output_create_options uint16_feather_output_options {};
    uint16_feather_output_options.size = sizeof(uint16_feather_output_options);
    uint16_feather_output_options.abi_version = PANO_GPU_ABI_VERSION;
    uint16_feather_output_options.output_width = 8;
    uint16_feather_output_options.output_height = 4;
    uint16_feather_output_options.output_sample_bytes = 2;
    uint16_feather_output_options.descriptor_count = 7;
    uint16_feather_output_options.output_workspace_bytes = 8ULL * 4 * 13;
    pano_gpu_output *uint16_feather_output = nullptr;
    EXPECT(pano_gpu_output_create_empty(
               session, &uint16_feather_output_options, &uint16_feather_output, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
#if defined(_WIN32)
    EXPECT(pano_gpu_output_allocate_linear(
               uint16_feather_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_coverage(
               uint16_feather_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_compose_feather(
               uint16_feather_output, &uint16_ordered_request, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<float, 96> uint16_feather_rgb {};
    std::array<uint8_t, 32> uint16_feather_coverage {};
    EXPECT(pano_gpu_test_read_output_band(
               uint16_feather_output, uint16_feather_rgb.data(), sizeof(uint16_feather_rgb),
               uint16_feather_coverage.data(), sizeof(uint16_feather_coverage), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < uint16_feather_coverage.size(); ++pixel)
    {
        float expected_weight = 0.0F;
        std::array<float, 3> expected_rgb {};
        for (uint32_t frame = 0; frame < 3; ++frame)
        {
            const float weight = uint16_feather_validity[frame][pixel] != 0
                ? std::max(uint16_feather_edges[frame][pixel], 1.0e-6F)
                : 0.0F;
            expected_weight += weight;
            for (size_t channel = 0; channel < 3; ++channel)
                expected_rgb[channel] += uint16_feather_candidates[frame][3 * pixel + channel] * weight;
        }
        EXPECT(uint16_feather_coverage[pixel] == static_cast<uint8_t>(expected_weight > 0.0F));
        for (size_t channel = 0; channel < 3; ++channel)
            EXPECT(std::fabs(uint16_feather_rgb[3 * pixel + channel] -
                             (expected_weight > 0.0F ? expected_rgb[channel] / expected_weight
                                                     : expected_rgb[channel])) < 1.0e-5F);
    }
#endif
    pano_gpu_output_destroy(&uint16_feather_output);
    for (uint32_t frame = 0; frame < 3; ++frame)
    {
        uint16_ordered_frames[frame].row_start = composite_request.row_start;
        uint16_ordered_frames[frame].row_count = composite_request.row_count;
    }
    uint16_ordered_request.frame_request_count = 2;
    uint16_ordered_request.frame_requests = uint16_ordered_frames;
    EXPECT(pano_gpu_test_validate_two_frame_uint16_hard_composite_request(
               session, &uint16_ordered_request, &ordered_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 48> ordered_uint16_first_candidates {};
    std::array<float, 48> ordered_uint16_second_candidates {};
    std::array<uint8_t, 16> ordered_uint16_first_validity {};
    std::array<uint8_t, 16> ordered_uint16_second_validity {};
    std::array<float, 16> ordered_uint16_first_edge {};
    std::array<float, 16> ordered_uint16_second_edge {};
    EXPECT(pano_gpu_test_dispatch_one_frame_uint16_candidates(
               session, &uint16_ordered_frames[0], ordered_uint16_first_candidates.data(),
               sizeof(ordered_uint16_first_candidates), ordered_uint16_first_validity.data(),
               sizeof(ordered_uint16_first_validity), ordered_uint16_first_edge.data(), sizeof(ordered_uint16_first_edge),
               error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_dispatch_one_frame_uint16_candidates(
               session, &uint16_ordered_frames[1], ordered_uint16_second_candidates.data(),
               sizeof(ordered_uint16_second_candidates), ordered_uint16_second_validity.data(),
               sizeof(ordered_uint16_second_validity), ordered_uint16_second_edge.data(), sizeof(ordered_uint16_second_edge),
               error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<float, 48> ordered_uint16_rgb {};
    std::array<float, 16> ordered_uint16_weight {};
    std::array<uint8_t, 16> ordered_uint16_coverage {};
    EXPECT(pano_gpu_test_dispatch_two_frame_uint16_hard_composite(
               session, &uint16_ordered_request, ordered_uint16_rgb.data(), sizeof(ordered_uint16_rgb),
               ordered_uint16_weight.data(), sizeof(ordered_uint16_weight), ordered_uint16_coverage.data(),
               sizeof(ordered_uint16_coverage), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < ordered_uint16_coverage.size(); ++pixel)
    {
        const float first_weight = ordered_uint16_first_validity[pixel] != 0
            ? std::max(ordered_uint16_first_edge[pixel], 1.0e-6F)
            : 0.0F;
        const float second_weight = ordered_uint16_second_validity[pixel] != 0
            ? std::max(ordered_uint16_second_edge[pixel], 1.0e-6F)
            : 0.0F;
        const bool select_second = second_weight > first_weight;
        const float expected_weight = select_second ? second_weight : first_weight;
        EXPECT(std::fabs(ordered_uint16_weight[pixel] - expected_weight) < 1.0e-6F);
        EXPECT(ordered_uint16_coverage[pixel] == static_cast<uint8_t>(expected_weight > 0.0F));
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const float expected_rgb = expected_weight == 0.0F ? 0.0F :
                (select_second ? ordered_uint16_second_candidates[3 * pixel + channel]
                               : ordered_uint16_first_candidates[3 * pixel + channel]);
            EXPECT(std::fabs(ordered_uint16_rgb[3 * pixel + channel] - expected_rgb) < 1.0e-5F);
        }
    }
    uint16_ordered_request.frame_request_count = 3;
    EXPECT(pano_gpu_test_validate_three_frame_uint16_hard_composite_request(
               session, &uint16_ordered_request, &ordered_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 48> ordered_uint16_third_candidates {};
    std::array<uint8_t, 16> ordered_uint16_third_validity {};
    std::array<float, 16> ordered_uint16_third_edge {};
    EXPECT(pano_gpu_test_dispatch_one_frame_uint16_candidates(
               session, &uint16_ordered_frames[2], ordered_uint16_third_candidates.data(),
               sizeof(ordered_uint16_third_candidates), ordered_uint16_third_validity.data(),
               sizeof(ordered_uint16_third_validity), ordered_uint16_third_edge.data(), sizeof(ordered_uint16_third_edge),
               error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<float, 48> ordered_uint16_three_rgb {};
    std::array<float, 16> ordered_uint16_three_weight {};
    std::array<uint8_t, 16> ordered_uint16_three_coverage {};
    EXPECT(pano_gpu_test_dispatch_three_frame_uint16_hard_composite(
               session, &uint16_ordered_request, ordered_uint16_three_rgb.data(), sizeof(ordered_uint16_three_rgb),
               ordered_uint16_three_weight.data(), sizeof(ordered_uint16_three_weight), ordered_uint16_three_coverage.data(),
               sizeof(ordered_uint16_three_coverage), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < ordered_uint16_three_coverage.size(); ++pixel)
    {
        const float first_weight = ordered_uint16_first_validity[pixel] != 0
            ? std::max(ordered_uint16_first_edge[pixel], 1.0e-6F)
            : 0.0F;
        const float second_weight = ordered_uint16_second_validity[pixel] != 0
            ? std::max(ordered_uint16_second_edge[pixel], 1.0e-6F)
            : 0.0F;
        const bool select_second = second_weight > first_weight;
        const float prior_weight = select_second ? second_weight : first_weight;
        const float third_weight = ordered_uint16_third_validity[pixel] != 0
            ? std::max(ordered_uint16_third_edge[pixel], 1.0e-6F)
            : 0.0F;
        const bool select_third = third_weight > prior_weight;
        const float expected_weight = select_third ? third_weight : prior_weight;
        EXPECT(std::fabs(ordered_uint16_three_weight[pixel] - expected_weight) < 1.0e-6F);
        EXPECT(ordered_uint16_three_coverage[pixel] == static_cast<uint8_t>(expected_weight > 0.0F));
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const float expected_rgb = expected_weight == 0.0F ? 0.0F :
                (select_third ? ordered_uint16_third_candidates[3 * pixel + channel] :
                 (select_second ? ordered_uint16_second_candidates[3 * pixel + channel]
                                : ordered_uint16_first_candidates[3 * pixel + channel]));
            EXPECT(std::fabs(ordered_uint16_three_rgb[3 * pixel + channel] - expected_rgb) < 1.0e-5F);
        }
    }
    uint16_ordered_request.frame_request_count = 2;
    uint16_ordered_frames[1].source_sample_type = PANO_GPU_SAMPLE_UINT8;
    EXPECT(pano_gpu_test_validate_two_frame_uint16_hard_composite_request(
               session, &uint16_ordered_request, &ordered_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    pano_gpu_session_destroy(&session);
    pano_gpu_session_create_options float32_multi_options = float32_options;
    float32_multi_options.frame_count = 3;
    float32_multi_options.rotations = three_frame_rotation.data();
    float32_multi_options.rotations_bytes = sizeof(three_frame_rotation);
    EXPECT(pano_gpu_session_create(
               device, &float32_multi_options, &session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_source(session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_upload_slot(session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_second_upload_slot(session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 48> first_float32_multi_source {};
    std::array<float, 48> second_float32_multi_source {};
    std::array<float, 48> third_float32_multi_source {};
    for (size_t index = 0; index < first_float32_multi_source.size(); ++index)
    {
        first_float32_multi_source[index] = static_cast<float>(index) * 0.03125F - 0.5F;
        second_float32_multi_source[index] = 1.0F - static_cast<float>(index) * 0.015625F;
        third_float32_multi_source[index] = static_cast<float>(index) * 0.0234375F - 0.25F;
    }
    pano_gpu_source_upload first_float32_multi_upload = float32_upload;
    first_float32_multi_upload.data = first_float32_multi_source.data();
    pano_gpu_source_upload second_float32_multi_upload = first_float32_multi_upload;
    second_float32_multi_upload.frame_index = 1;
    second_float32_multi_upload.data = second_float32_multi_source.data();
    pano_gpu_source_upload third_float32_multi_upload = first_float32_multi_upload;
    third_float32_multi_upload.frame_index = 2;
    third_float32_multi_upload.data = third_float32_multi_source.data();
    EXPECT(pano_gpu_session_upload_frame_zero(
               session, &first_float32_multi_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_upload_frame(
               session, &second_float32_multi_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_upload_frame(
               session, &third_float32_multi_upload, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_finish_uploads(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_one_frame_composite_request float32_ordered_frames[3] {};
    for (uint32_t index = 0; index < 3; ++index)
    {
        float32_ordered_frames[index] = composite_request;
        float32_ordered_frames[index].frame_index = index;
        float32_ordered_frames[index].source_sample_type = PANO_GPU_SAMPLE_FLOAT32;
    }
    pano_gpu_ordered_hard_composite_request float32_ordered_request = ordered_request;
    float32_ordered_request.frame_request_count = 3;
    float32_ordered_request.frame_requests = float32_ordered_frames;
    for (uint32_t frame = 0; frame < 3; ++frame)
    {
        float32_ordered_frames[frame].row_start = 0;
        float32_ordered_frames[frame].row_count = float32_ordered_frames[frame].output_height;
    }
    std::array<std::array<float, 96>, 3> float32_feather_candidates {};
    std::array<std::array<uint8_t, 32>, 3> float32_feather_validity {};
    std::array<std::array<float, 32>, 3> float32_feather_edges {};
    for (uint32_t frame = 0; frame < 3; ++frame)
        EXPECT(pano_gpu_test_dispatch_one_frame_float32_candidates(
                   session, &float32_ordered_frames[frame], float32_feather_candidates[frame].data(),
                   sizeof(float32_feather_candidates[frame]), float32_feather_validity[frame].data(),
                   sizeof(float32_feather_validity[frame]), float32_feather_edges[frame].data(),
                   sizeof(float32_feather_edges[frame]), error.data(), static_cast<uint32_t>(error.size())) ==
               PANO_GPU_SUCCESS);
    pano_gpu_output_create_options float32_feather_output_options {};
    float32_feather_output_options.size = sizeof(float32_feather_output_options);
    float32_feather_output_options.abi_version = PANO_GPU_ABI_VERSION;
    float32_feather_output_options.output_width = 8;
    float32_feather_output_options.output_height = 4;
    float32_feather_output_options.output_sample_bytes = 4;
    float32_feather_output_options.descriptor_count = 7;
    float32_feather_output_options.output_workspace_bytes = 8ULL * 4 * 13;
    pano_gpu_output *float32_feather_output = nullptr;
    EXPECT(pano_gpu_output_create_empty(
               session, &float32_feather_output_options, &float32_feather_output, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
#if defined(_WIN32)
    EXPECT(pano_gpu_output_allocate_linear(
               float32_feather_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_coverage(
               float32_feather_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_compose_feather(
               float32_feather_output, &float32_ordered_request, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<float, 96> float32_feather_rgb {};
    std::array<uint8_t, 32> float32_feather_coverage {};
    EXPECT(pano_gpu_test_read_output_band(
               float32_feather_output, float32_feather_rgb.data(), sizeof(float32_feather_rgb),
               float32_feather_coverage.data(), sizeof(float32_feather_coverage), error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < float32_feather_coverage.size(); ++pixel)
    {
        float expected_weight = 0.0F;
        std::array<float, 3> expected_rgb {};
        for (uint32_t frame = 0; frame < 3; ++frame)
        {
            const float weight = float32_feather_validity[frame][pixel] != 0
                ? std::max(float32_feather_edges[frame][pixel], 1.0e-6F)
                : 0.0F;
            expected_weight += weight;
            for (size_t channel = 0; channel < 3; ++channel)
                expected_rgb[channel] += float32_feather_candidates[frame][3 * pixel + channel] * weight;
        }
        EXPECT(float32_feather_coverage[pixel] == static_cast<uint8_t>(expected_weight > 0.0F));
        for (size_t channel = 0; channel < 3; ++channel)
            EXPECT(std::fabs(float32_feather_rgb[3 * pixel + channel] -
                             (expected_weight > 0.0F ? expected_rgb[channel] / expected_weight
                                                     : expected_rgb[channel])) < 1.0e-5F);
    }
#endif
    pano_gpu_output_destroy(&float32_feather_output);
    for (uint32_t frame = 0; frame < 3; ++frame)
    {
        float32_ordered_frames[frame].row_start = composite_request.row_start;
        float32_ordered_frames[frame].row_count = composite_request.row_count;
    }
    float32_ordered_request.frame_request_count = 2;
    float32_ordered_request.frame_requests = float32_ordered_frames;
    EXPECT(pano_gpu_test_validate_two_frame_float32_hard_composite_request(
               session, &float32_ordered_request, &ordered_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 48> ordered_float32_first_candidates {};
    std::array<float, 48> ordered_float32_second_candidates {};
    std::array<uint8_t, 16> ordered_float32_first_validity {};
    std::array<uint8_t, 16> ordered_float32_second_validity {};
    std::array<float, 16> ordered_float32_first_edge {};
    std::array<float, 16> ordered_float32_second_edge {};
    EXPECT(pano_gpu_test_dispatch_one_frame_float32_candidates(
               session, &float32_ordered_frames[0], ordered_float32_first_candidates.data(),
               sizeof(ordered_float32_first_candidates), ordered_float32_first_validity.data(),
               sizeof(ordered_float32_first_validity), ordered_float32_first_edge.data(),
               sizeof(ordered_float32_first_edge), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_dispatch_one_frame_float32_candidates(
               session, &float32_ordered_frames[1], ordered_float32_second_candidates.data(),
               sizeof(ordered_float32_second_candidates), ordered_float32_second_validity.data(),
               sizeof(ordered_float32_second_validity), ordered_float32_second_edge.data(),
               sizeof(ordered_float32_second_edge), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<float, 48> ordered_float32_rgb {};
    std::array<float, 16> ordered_float32_weight {};
    std::array<uint8_t, 16> ordered_float32_coverage {};
    EXPECT(pano_gpu_test_dispatch_two_frame_float32_hard_composite(
               session, &float32_ordered_request, ordered_float32_rgb.data(), sizeof(ordered_float32_rgb),
               ordered_float32_weight.data(), sizeof(ordered_float32_weight), ordered_float32_coverage.data(),
               sizeof(ordered_float32_coverage), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < ordered_float32_coverage.size(); ++pixel)
    {
        const float first_weight = ordered_float32_first_validity[pixel] != 0
            ? std::max(ordered_float32_first_edge[pixel], 1.0e-6F)
            : 0.0F;
        const float second_weight = ordered_float32_second_validity[pixel] != 0
            ? std::max(ordered_float32_second_edge[pixel], 1.0e-6F)
            : 0.0F;
        const bool select_second = second_weight > first_weight;
        const float expected_weight = select_second ? second_weight : first_weight;
        EXPECT(std::fabs(ordered_float32_weight[pixel] - expected_weight) < 1.0e-6F);
        EXPECT(ordered_float32_coverage[pixel] == static_cast<uint8_t>(expected_weight > 0.0F));
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const float expected_rgb = expected_weight == 0.0F ? 0.0F :
                (select_second ? ordered_float32_second_candidates[3 * pixel + channel]
                               : ordered_float32_first_candidates[3 * pixel + channel]);
            EXPECT(std::fabs(ordered_float32_rgb[3 * pixel + channel] - expected_rgb) < 1.0e-5F);
        }
    }
    float32_ordered_request.frame_request_count = 3;
    EXPECT(pano_gpu_test_validate_three_frame_float32_hard_composite_request(
               session, &float32_ordered_request, &ordered_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 48> ordered_float32_third_candidates {};
    std::array<uint8_t, 16> ordered_float32_third_validity {};
    std::array<float, 16> ordered_float32_third_edge {};
    EXPECT(pano_gpu_test_dispatch_one_frame_float32_candidates(
               session, &float32_ordered_frames[2], ordered_float32_third_candidates.data(),
               sizeof(ordered_float32_third_candidates), ordered_float32_third_validity.data(),
               sizeof(ordered_float32_third_validity), ordered_float32_third_edge.data(),
               sizeof(ordered_float32_third_edge), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<float, 48> ordered_float32_three_rgb {};
    std::array<float, 16> ordered_float32_three_weight {};
    std::array<uint8_t, 16> ordered_float32_three_coverage {};
    EXPECT(pano_gpu_test_dispatch_three_frame_float32_hard_composite(
               session, &float32_ordered_request, ordered_float32_three_rgb.data(), sizeof(ordered_float32_three_rgb),
               ordered_float32_three_weight.data(), sizeof(ordered_float32_three_weight), ordered_float32_three_coverage.data(),
               sizeof(ordered_float32_three_coverage), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < ordered_float32_three_coverage.size(); ++pixel)
    {
        const float first_weight = ordered_float32_first_validity[pixel] != 0
            ? std::max(ordered_float32_first_edge[pixel], 1.0e-6F)
            : 0.0F;
        const float second_weight = ordered_float32_second_validity[pixel] != 0
            ? std::max(ordered_float32_second_edge[pixel], 1.0e-6F)
            : 0.0F;
        const bool select_second = second_weight > first_weight;
        const float prior_weight = select_second ? second_weight : first_weight;
        const float third_weight = ordered_float32_third_validity[pixel] != 0
            ? std::max(ordered_float32_third_edge[pixel], 1.0e-6F)
            : 0.0F;
        const bool select_third = third_weight > prior_weight;
        const float expected_weight = select_third ? third_weight : prior_weight;
        EXPECT(std::fabs(ordered_float32_three_weight[pixel] - expected_weight) < 1.0e-6F);
        EXPECT(ordered_float32_three_coverage[pixel] == static_cast<uint8_t>(expected_weight > 0.0F));
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const float expected_rgb = expected_weight == 0.0F ? 0.0F :
                (select_third ? ordered_float32_third_candidates[3 * pixel + channel] :
                 (select_second ? ordered_float32_second_candidates[3 * pixel + channel]
                                : ordered_float32_first_candidates[3 * pixel + channel]));
            EXPECT(std::fabs(ordered_float32_three_rgb[3 * pixel + channel] - expected_rgb) < 1.0e-5F);
        }
    }
    float32_ordered_request.frame_request_count = 2;
    float32_ordered_frames[1].source_sample_type = PANO_GPU_SAMPLE_UINT16;
    EXPECT(pano_gpu_test_validate_two_frame_float32_hard_composite_request(
               session, &float32_ordered_request, &ordered_layout, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    pano_gpu_session_destroy(&session);
    std::array<float, 36> four_frame_rotations {};
    for (uint32_t frame = 0; frame < 4; ++frame)
        for (uint32_t diagonal = 0; diagonal < 3; ++diagonal)
            four_frame_rotations[frame * 9 + diagonal * 4] = 1.0F;
    pano_gpu_session_create_options four_frame_options = session_options;
    four_frame_options.frame_count = 4;
    four_frame_options.rotations = four_frame_rotations.data();
    four_frame_options.rotations_bytes = sizeof(four_frame_rotations);
    EXPECT(pano_gpu_session_create(
               device, &four_frame_options, &session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_source(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_upload_slot(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_session_allocate_second_upload_slot(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    std::array<std::array<uint8_t, 48>, 4> four_frame_sources {};
    for (uint32_t frame = 0; frame < 4; ++frame)
        for (size_t index = 0; index < four_frame_sources[frame].size(); ++index)
            four_frame_sources[frame][index] = static_cast<uint8_t>(17U * frame + 3U * index + 1U);
    pano_gpu_source_upload four_frame_upload = source_upload;
    for (uint32_t frame = 0; frame < 4; ++frame)
    {
        four_frame_upload.frame_index = frame;
        four_frame_upload.data = four_frame_sources[frame].data();
        EXPECT((frame == 0
                    ? pano_gpu_session_upload_frame_zero(
                          session, &four_frame_upload, error.data(), static_cast<uint32_t>(error.size()))
                    : pano_gpu_session_upload_frame(
                          session, &four_frame_upload, error.data(), static_cast<uint32_t>(error.size()))) ==
               PANO_GPU_SUCCESS);
    }
    EXPECT(pano_gpu_session_finish_uploads(
               session, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_one_frame_composite_request four_frame_requests[4] {};
    std::array<std::array<float, 96>, 4> four_frame_candidates {};
    std::array<std::array<uint8_t, 32>, 4> four_frame_validity {};
    std::array<std::array<float, 32>, 4> four_frame_edges {};
    for (uint32_t frame = 0; frame < 4; ++frame)
    {
        four_frame_requests[frame] = composite_request;
        four_frame_requests[frame].frame_index = frame;
        four_frame_requests[frame].source_sample_type = PANO_GPU_SAMPLE_UINT8;
        four_frame_requests[frame].row_start = 0;
        four_frame_requests[frame].row_count = four_frame_requests[frame].output_height;
        EXPECT(pano_gpu_test_dispatch_one_frame_uint8_candidates(
                   session, &four_frame_requests[frame], four_frame_candidates[frame].data(),
                   sizeof(four_frame_candidates[frame]), four_frame_validity[frame].data(),
                   sizeof(four_frame_validity[frame]), four_frame_edges[frame].data(),
                   sizeof(four_frame_edges[frame]), error.data(), static_cast<uint32_t>(error.size())) ==
               PANO_GPU_SUCCESS);
    }
    pano_gpu_ordered_hard_composite_request four_frame_request = ordered_request;
    four_frame_request.frame_request_count = 4;
    four_frame_request.frame_requests = four_frame_requests;
    pano_gpu_output_create_options four_frame_output_options {};
    four_frame_output_options.size = sizeof(four_frame_output_options);
    four_frame_output_options.abi_version = PANO_GPU_ABI_VERSION;
    four_frame_output_options.output_width = 8;
    four_frame_output_options.output_height = 4;
    four_frame_output_options.output_sample_bytes = 1;
    four_frame_output_options.descriptor_count = 8;
    four_frame_output_options.output_workspace_bytes = 8ULL * 4 * 13;
    pano_gpu_output *four_frame_output = nullptr;
    EXPECT(pano_gpu_output_create_empty(
               session, &four_frame_output_options, &four_frame_output, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
#if defined(_WIN32)
    EXPECT(pano_gpu_output_allocate_linear(
               four_frame_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_coverage(
               four_frame_output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_compose_hard(
               four_frame_output, &four_frame_request, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    std::array<float, 96> four_frame_rgb {};
    std::array<uint8_t, 32> four_frame_coverage {};
    EXPECT(pano_gpu_test_read_output_band(
               four_frame_output, four_frame_rgb.data(), sizeof(four_frame_rgb), four_frame_coverage.data(),
               sizeof(four_frame_coverage), error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < four_frame_coverage.size(); ++pixel)
    {
        uint32_t selected_frame = 0;
        float selected_weight = four_frame_validity[0][pixel] != 0
            ? std::max(four_frame_edges[0][pixel], 1.0e-6F)
            : 0.0F;
        for (uint32_t frame = 1; frame < 4; ++frame)
        {
            const float candidate_weight = four_frame_validity[frame][pixel] != 0
                ? std::max(four_frame_edges[frame][pixel], 1.0e-6F)
                : 0.0F;
            if (candidate_weight > selected_weight)
            {
                selected_frame = frame;
                selected_weight = candidate_weight;
            }
        }
        EXPECT(four_frame_coverage[pixel] == static_cast<uint8_t>(selected_weight > 0.0F));
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const float expected = selected_weight == 0.0F
                ? 0.0F
                : four_frame_candidates[selected_frame][3 * pixel + channel];
            EXPECT(std::fabs(four_frame_rgb[3 * pixel + channel] - expected) < 1.0e-5F);
        }
    }
    EXPECT(pano_gpu_output_compose_feather(
               four_frame_output, &four_frame_request, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_test_read_output_band(
               four_frame_output, four_frame_rgb.data(), sizeof(four_frame_rgb), four_frame_coverage.data(),
               sizeof(four_frame_coverage), error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    for (size_t pixel = 0; pixel < four_frame_coverage.size(); ++pixel)
    {
        float expected_weight = 0.0F;
        std::array<float, 3> expected_rgb {};
        for (uint32_t frame = 0; frame < 4; ++frame)
        {
            const float weight = four_frame_validity[frame][pixel] != 0
                ? std::max(four_frame_edges[frame][pixel], 1.0e-6F)
                : 0.0F;
            expected_weight += weight;
            for (size_t channel = 0; channel < 3; ++channel)
                expected_rgb[channel] += four_frame_candidates[frame][3 * pixel + channel] * weight;
        }
        EXPECT(four_frame_coverage[pixel] == static_cast<uint8_t>(expected_weight > 0.0F));
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const float normalized = expected_weight > 0.0F
                ? expected_rgb[channel] / expected_weight
                : expected_rgb[channel];
            EXPECT(std::fabs(four_frame_rgb[3 * pixel + channel] - normalized) < 1.0e-5F);
        }
    }
#endif
    pano_gpu_output_destroy(&four_frame_output);
    pano_gpu_session_destroy(&session);
    session = reinterpret_cast<pano_gpu_session *>(1);
    pano_gpu_test_fail_next_session_allocation();
    EXPECT(pano_gpu_session_create(
               device, &session_options, &session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_OUT_OF_MEMORY);
    EXPECT(session == nullptr);
    EXPECT(pano_gpu_session_create(
               device, &session_options, &session, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    pano_gpu_output *output = nullptr;
    EXPECT(pano_gpu_output_create_empty(
               session, &output_options, &output, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(output != nullptr);
    EXPECT(pano_gpu_output_allocate_linear(
               output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    EXPECT(pano_gpu_output_allocate_coverage(
               output, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS);
    pano_gpu_session_destroy(&session);
    EXPECT(session == nullptr);
    pano_gpu_device_destroy(&device);
    EXPECT(device == nullptr);
    EXPECT(pano_gpu_query_diagnostics(&diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(diagnostics.live_device_count == 1);
    EXPECT(diagnostics.live_queue_count == 1);
    EXPECT(diagnostics.live_fence_count == 1);
    EXPECT(diagnostics.live_session_count == 1);
    EXPECT(diagnostics.live_output_count == 1);
    pano_gpu_output_destroy(&output);
    EXPECT(output == nullptr);
    pano_gpu_output_destroy(&output);
    EXPECT(pano_gpu_query_diagnostics(&diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(diagnostics.live_device_count == 0);
    EXPECT(diagnostics.live_queue_count == 0);
    EXPECT(diagnostics.live_fence_count == 0);
    EXPECT(diagnostics.live_session_count == 0);
    EXPECT(diagnostics.live_output_count == 0);
    EXPECT(pano_gpu_device_create(&options, &device, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    pano_gpu_test_fail_next_pipeline_creation();
    EXPECT(pano_gpu_device_dispatch_self_test(
               device, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
    pano_gpu_test_fail_next_resource_creation();
    EXPECT(pano_gpu_device_dispatch_self_test(
               device, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
    pano_gpu_test_fail_next_descriptor_creation();
    EXPECT(pano_gpu_device_dispatch_self_test(
               device, error.data(), static_cast<uint32_t>(error.size())) == PANO_GPU_UNAVAILABLE);
    EXPECT(pano_gpu_query_diagnostics(&diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(diagnostics.live_device_count == 1);
    EXPECT(diagnostics.live_queue_count == 1);
    EXPECT(diagnostics.live_fence_count == 1);
    const pano_gpu_result direct_dispatch_result =
        pano_gpu_device_dispatch_self_test(device, error.data(), static_cast<uint32_t>(error.size()));
    if (!expect(
            direct_dispatch_result == PANO_GPU_SUCCESS,
            "pano_gpu_device_dispatch_self_test(...) == PANO_GPU_SUCCESS", __LINE__))
    {
        std::fprintf(stderr, "D3D12 direct self-test detail: %s\n", error.data());
        return 1;
    }
    pano_gpu_device_destroy(&device);
    EXPECT(device == nullptr);
    pano_gpu_device_destroy(&device);
    EXPECT(pano_gpu_device_query_diagnostics(
               device, &device_diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_INVALID_ARGUMENT);
    EXPECT(device_diagnostics.adapter.luid == 0);
    EXPECT(device_diagnostics.usable_local_bytes == 0);
    EXPECT(pano_gpu_query_diagnostics(&diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(diagnostics.live_device_count == 0);
    EXPECT(diagnostics.live_queue_count == 0);
    EXPECT(diagnostics.live_fence_count == 0);
    const pano_gpu_result dispatch_result =
        pano_gpu_dispatch_self_test(1, error.data(), static_cast<uint32_t>(error.size()));
    if (!expect(dispatch_result == PANO_GPU_SUCCESS, "pano_gpu_dispatch_self_test(...) == PANO_GPU_SUCCESS", __LINE__))
    {
        std::fprintf(stderr, "D3D12 self-test detail: %s\n", error.data());
        return 1;
    }
    const pano_gpu_result hardware_dispatch_result =
        pano_gpu_dispatch_self_test(0, error.data(), static_cast<uint32_t>(error.size()));
    EXPECT(hardware_dispatch_result == PANO_GPU_SUCCESS ||
           hardware_dispatch_result == PANO_GPU_UNAVAILABLE);
    EXPECT(pano_gpu_query_diagnostics(&diagnostics, error.data(), static_cast<uint32_t>(error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(diagnostics.live_device_count == 0);
    EXPECT(diagnostics.live_queue_count == 0);
    EXPECT(diagnostics.live_fence_count == 0);
#endif

    pano_gpu_device_destroy(nullptr);
    pano_gpu_session_destroy(nullptr);
    pano_gpu_output_destroy(nullptr);
    pano_gpu_preview_destroy(nullptr);
    pano_gpu_cancellation_token_destroy(nullptr);
    return 0;
}
