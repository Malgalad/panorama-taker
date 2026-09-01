#include "pano_gpu.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "pano_gpu_fill_shader.h"
#include "pano_gpu_ray_shader.h"
#include "pano_gpu_uint8_sample_shader.h"
#include "pano_gpu_uint16_sample_shader.h"
#include "pano_gpu_float32_sample_shader.h"
#include "pano_gpu_uint8_candidate_shader.h"
#include "pano_gpu_uint16_candidate_shader.h"
#include "pano_gpu_float32_candidate_shader.h"
#include "pano_gpu_hard_selection_shader.h"
#include "pano_gpu_feather_weight_shader.h"
#include "pano_gpu_feather_accumulate_shader.h"
#include "pano_gpu_feather_normalize_shader.h"
#include "pano_gpu_feather_normalize_output_shader.h"
#include "pano_gpu_global_gain_shader.h"
#include "pano_gpu_equirect_local_exposure_shader.h"
#include "pano_gpu_local_exposure_shader.h"
#include "pano_gpu_mark_incomplete_shader.h"
#include "pano_gpu_mark_incomplete_output_shader.h"
#include "pano_gpu_convert_linear_srgb_shader.h"
#include "pano_gpu_auto_contrast_histogram_srgb_shader.h"
#include "pano_gpu_select_auto_contrast_levels_shader.h"
#include "pano_gpu_apply_auto_contrast_srgb_shader.h"
#include "pano_gpu_quantize_srgb8_shader.h"
#include "pano_gpu_tone_map_rec2020_shader.h"
#include "pano_gpu_rec2020_linear_srgb_shader.h"
#include "pano_gpu_preview_base_shader.h"
#include "pano_gpu_preview_present_shader.h"
#include "pano_gpu_preview_present_overlay_shader.h"
#include "pano_gpu_preview_overlay_shader.h"
#include "pano_gpu_exposure_proxy_uint8_shader.h"
#include "pano_gpu_exposure_proxy_uint16_shader.h"
#include "pano_gpu_exposure_proxy_float32_shader.h"
#include "pano_gpu_exposure_pair_projection_shader.h"
#include "pano_gpu_exposure_pair_samples_shader.h"
#include "pano_gpu_exposure_pair_classify_shader.h"
#include "pano_gpu_exposure_pair_classify_resident_shader.h"
#include "pano_gpu_exposure_pair_gradient_shader.h"
#include "pano_gpu_exposure_pair_filter_shader.h"
#include "pano_gpu_exposure_pair_filter_resident_shader.h"
#include "pano_gpu_exposure_pair_ratio_shader.h"
#include "pano_gpu_exposure_pair_sort_prepare_shader.h"
#include "pano_gpu_exposure_pair_sort_shader.h"
#include "pano_gpu_exposure_pair_bounds_shader.h"
#include "pano_gpu_exposure_pair_trim_shader.h"
#include "pano_gpu_exposure_gradient_sort_prepare_shader.h"
#include "pano_gpu_exposure_gradient_sort_shader.h"
#include "pano_gpu_exposure_gradient_bounds_shader.h"
#include "pano_gpu_exposure_pair_reduce_summary_shader.h"
#include "pano_gpu_exposure_pair_reduce_deviations_shader.h"
#include "pano_gpu_exposure_pair_reduce_result_shader.h"
#endif

namespace
{
std::atomic<uint32_t> live_device_count {0};
std::atomic<uint32_t> live_queue_count {0};
std::atomic<uint32_t> live_fence_count {0};
std::atomic<uint32_t> live_session_count {0};
std::atomic<uint32_t> live_output_count {0};
std::atomic<uint32_t> live_preview_count {0};
std::atomic<uint32_t> live_preview_surface_count {0};
#if defined(PANO_GPU_TEST_HOOKS)
std::atomic<bool> fail_next_preview_surface_device_removed {false};
#endif
#if defined(PANO_GPU_TEST_HOOKS)
std::atomic<bool> fail_next_fence_wait {false};
#endif
} // namespace

struct pano_gpu_cancellation_token
{
    std::atomic<bool> cancelled {false};
};

struct pano_gpu_device_core
{
    pano_gpu_adapter_info adapter_info {};
#if defined(_WIN32)
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<ID3D12Device> d3d_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    std::atomic<uint64_t> next_fence_value {0};
    bool counts_registered {false};

    ~pano_gpu_device_core()
    {
        if (!counts_registered)
            return;
        if (fence)
            live_fence_count.fetch_sub(1, std::memory_order_relaxed);
        if (queue)
            live_queue_count.fetch_sub(1, std::memory_order_relaxed);
        if (d3d_device)
            live_device_count.fetch_sub(1, std::memory_order_relaxed);
    }
#endif
};

struct pano_gpu_device
{
    std::shared_ptr<pano_gpu_device_core> core;
};

struct pano_gpu_session
{
    std::atomic<uint32_t> reference_count {1};
    std::shared_ptr<pano_gpu_device_core> device_core;
    uint32_t frame_count {0};
    uint32_t source_sample_type {0};
    uint32_t transfer_function {0};
    uint32_t source_width {0};
    uint32_t source_height {0};
    uint32_t source_row_stride_bytes {0};
    uint64_t source_frame_bytes {0};
    uint64_t planned_source_bytes {0};
    uint64_t source_bytes {0};
    uint64_t requested_rotation_bytes {0};
    uint64_t planned_rotation_bytes {0};
    uint64_t rotation_bytes {0};
    uint64_t requested_encoding_metadata_bytes {0};
    uint64_t planned_encoding_metadata_bytes {0};
    uint64_t encoding_metadata_bytes {0};
    uint64_t upload_slot_bytes {0};
    uint64_t second_upload_slot_bytes {0};
    uint64_t first_upload_slot_fence {0};
    uint64_t second_upload_slot_fence {0};
    uint32_t upload_count {0};
    uint64_t uploaded_bytes {0};
    uint64_t last_completed_upload_fence {0};
    std::vector<uint64_t> frame_upload_fences;
    bool source_is_shader_readable {false};
    uint64_t exposure_proxy_bytes {0};
    bool exposure_proxies_retained {false};
    uint64_t exposure_pair_scratch_sample_count {0};
    uint64_t exposure_pair_sortable_capacity {0};
    bool exposure_pair_scratch_retained {false};
    bool exposure_pair_sort_prepared {false};
    bool exposure_pair_sorted {false};
    bool exposure_pair_bounds_ready {false};
    bool exposure_pair_trimmed {false};
    uint32_t exposure_pair_capacity {0};
    uint64_t resident_pair_sample_count {0};
    uint64_t resident_pair_sortable_capacity {0};
    uint64_t resident_pair_device_bytes {0};
    bool resident_pair_projected_sampled {false};
    bool resident_pair_classified {false};
    bool resident_pair_gradient_limits_ready {false};
    bool resident_pair_ratios_ready {false};
    bool resident_pair_trimmed {false};
    bool resident_pair_reduced {false};
    uint32_t resident_pair_geometric_count {0};
    std::vector<pano_gpu_exposure_equation> exposure_equations;
    std::vector<pano_gpu_exposure_equation> exposure_solve_equations;
    bool exposure_solve_graph_ready {false};
    std::vector<float> exposure_log_gains;
    uint32_t exposure_anchor_frame_index {0};
    uint32_t exposure_solve_edge_count {0};
    bool exposure_solved {false};
    std::vector<float> exposure_global_gains;
    bool exposure_gains_uploaded {false};
    uint32_t exposure_solve_count {0};
    uint32_t exposure_gain_upload_count {0};
    std::vector<pano_gpu_exposure_pair_report> exposure_pair_reports;
    bool rotations_uploaded {false};
    bool encoding_metadata_uploaded {false};
#if defined(_WIN32)
    Microsoft::WRL::ComPtr<ID3D12Resource> source;
    Microsoft::WRL::ComPtr<ID3D12Resource> exposure_proxies;
    Microsoft::WRL::ComPtr<ID3D12Resource> exposure_pair_accepted;
    Microsoft::WRL::ComPtr<ID3D12Resource> exposure_pair_log_ratios;
    Microsoft::WRL::ComPtr<ID3D12Resource> exposure_pair_sortable_ratios;
    Microsoft::WRL::ComPtr<ID3D12Resource> exposure_pair_trim_bounds;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 15> resident_pair_scratch;
    Microsoft::WRL::ComPtr<ID3D12Resource> rotations;
    Microsoft::WRL::ComPtr<ID3D12Resource> encoding_metadata;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_slot;
    void *mapped_upload_slot {nullptr};
    Microsoft::WRL::ComPtr<ID3D12Resource> second_upload_slot;
    void *mapped_second_upload_slot {nullptr};
#endif
    bool count_registered {false};

    ~pano_gpu_session()
    {
#if defined(_WIN32)
        if (upload_slot && mapped_upload_slot != nullptr)
            upload_slot->Unmap(0, nullptr);
        if (second_upload_slot && mapped_second_upload_slot != nullptr)
            second_upload_slot->Unmap(0, nullptr);
#endif
        if (count_registered)
            live_session_count.fetch_sub(1, std::memory_order_relaxed);
    }
};

void release_session(pano_gpu_session *session);

pano_gpu_result pano_gpu_test_plan_exposure_proxies(
    const pano_gpu_session *session, const pano_gpu_exposure_proxy_request *request,
    pano_gpu_exposure_proxy_layout *layout, char *error_buffer, uint32_t error_buffer_size) noexcept;
pano_gpu_result pano_gpu_test_validate_exposure_pair_request(
    const pano_gpu_session *session, const pano_gpu_exposure_pair_request *request,
    const void *paired_coordinates, uint64_t paired_coordinate_bytes, const void *overlap,
    uint64_t overlap_bytes, pano_gpu_exposure_pair_layout *layout, char *error_buffer,
    uint32_t error_buffer_size) noexcept;

struct pano_gpu_output
{
    pano_gpu_session *session {nullptr};
    uint32_t output_width {0};
    uint32_t output_height {0};
    uint32_t output_band_rows {0};
    uint32_t band_row_start {0};
    uint32_t band_row_count {0};
    uint64_t planned_linear_bytes {0};
    uint64_t linear_bytes {0};
    uint64_t planned_coverage_bytes {0};
    uint64_t coverage_bytes {0};
    uint64_t histogram_bytes {0};
    uint64_t histogram_accumulated_pixels {0};
    uint32_t histogram_clear_count {0};
    uint32_t histogram_accumulated_band_count {0};
    float auto_contrast_black {0.0F};
    float auto_contrast_white {0.0F};
    bool auto_contrast_levels_ready {false};
    uint64_t normalized_srgb_bytes {0};
    uint64_t quantized_srgb_bytes {0};
    uint64_t tone_mapped_rec2020_bytes {0};
    uint64_t converted_linear_srgb_bytes {0};
    uint64_t float_output_bytes {0};
    uint64_t download_readback_bytes {0};
    uint64_t download_count {0};
    uint64_t downloaded_bytes {0};
    bool count_registered {false};
#if defined(_WIN32)
    Microsoft::WRL::ComPtr<ID3D12Resource> linear;
    Microsoft::WRL::ComPtr<ID3D12Resource> coverage;
    Microsoft::WRL::ComPtr<ID3D12Resource> histogram;
    Microsoft::WRL::ComPtr<ID3D12Resource> auto_contrast_levels;
    Microsoft::WRL::ComPtr<ID3D12Resource> normalized_srgb;
    Microsoft::WRL::ComPtr<ID3D12Resource> quantized_srgb;
    Microsoft::WRL::ComPtr<ID3D12Resource> tone_mapped_rec2020;
    Microsoft::WRL::ComPtr<ID3D12Resource> converted_linear_srgb;
    Microsoft::WRL::ComPtr<ID3D12Resource> float_output;
    Microsoft::WRL::ComPtr<ID3D12Resource> download_readback;
#endif
    ~pano_gpu_output()
    {
#if defined(_WIN32)
        coverage.Reset();
        histogram.Reset();
        auto_contrast_levels.Reset();
        normalized_srgb.Reset();
        quantized_srgb.Reset();
        tone_mapped_rec2020.Reset();
        converted_linear_srgb.Reset();
        float_output.Reset();
        download_readback.Reset();
        linear.Reset();
#endif
        if (count_registered)
            live_output_count.fetch_sub(1, std::memory_order_relaxed);
        release_session(session);
    }
};

struct pano_gpu_preview
{
    pano_gpu_session *session {nullptr};
    uint32_t frame_count {0};
    uint32_t preview_width {0};
    uint32_t preview_height {0};
    uint32_t overview_width {0};
    uint32_t overview_height {0};
    uint32_t mask_width {0};
    uint32_t mask_height {0};
    uint64_t preview_rgb8_bytes {0};
    uint64_t overview_rgb8_bytes {0};
    uint64_t compact_mask_bytes {0};
    bool count_registered {false};
    bool hovered_frames_ready {false};
    std::atomic<uint64_t> latest_generation {0};
    std::atomic<bool> rendering {false};
#if defined(_WIN32)
    Microsoft::WRL::ComPtr<ID3D12Resource> preview_rgb8;
    Microsoft::WRL::ComPtr<ID3D12Resource> overview_rgb8;
    Microsoft::WRL::ComPtr<ID3D12Resource> compact_masks;
    Microsoft::WRL::ComPtr<ID3D12Resource> viewport_rgb8;
    Microsoft::WRL::ComPtr<ID3D12Resource> viewport_readback;
    Microsoft::WRL::ComPtr<ID3D12Resource> hovered_upload;
    Microsoft::WRL::ComPtr<ID3D12Resource> hovered_frames;
#endif
    ~pano_gpu_preview()
    {
#if defined(_WIN32)
        compact_masks.Reset();
        viewport_readback.Reset();
        viewport_rgb8.Reset();
        hovered_frames.Reset();
        hovered_upload.Reset();
        overview_rgb8.Reset();
        preview_rgb8.Reset();
#endif
        if (count_registered)
            live_preview_count.fetch_sub(1, std::memory_order_relaxed);
        release_session(session);
    }
};

struct pano_gpu_preview_surface
{
    std::shared_ptr<pano_gpu_device_core> device_core;
    uint32_t width {0};
    uint32_t height {0};
    uint64_t present_count {0};
    uint64_t resize_count {0};
    bool occluded {false};
    bool device_lost {false};
    bool count_registered {false};
    std::atomic<bool> presenting {false};
#if defined(_WIN32)
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> buffers;
    Microsoft::WRL::ComPtr<ID3D12Resource> present_texture;
#endif
    ~pano_gpu_preview_surface()
    {
#if defined(_WIN32)
        for (auto &buffer : buffers)
            buffer.Reset();
        present_texture.Reset();
        rtv_heap.Reset();
        swap_chain.Reset();
#endif
        if (count_registered)
            live_preview_surface_count.fetch_sub(1, std::memory_order_relaxed);
    }
};

void release_session(pano_gpu_session *const session)
{
    if (session != nullptr && session->reference_count.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete session;
}


namespace
{
void write_error(char *error_buffer, const uint32_t error_buffer_size, const char *message)
{
    if (error_buffer == nullptr || error_buffer_size == 0)
        return;
    const size_t message_size = std::strlen(message);
    const size_t copy_size = message_size < error_buffer_size - 1 ? message_size : error_buffer_size - 1;
    std::memcpy(error_buffer, message, copy_size);
    error_buffer[copy_size] = '\0';
}

#if defined(_WIN32)
struct upload_slot_selection
{
    ID3D12Resource *resource {nullptr};
    void *mapped {nullptr};
    uint64_t *last_fence {nullptr};
};

void write_hresult_error(
    char *const error_buffer, const uint32_t error_buffer_size, const char *const operation,
    const HRESULT result)
{
    char message[128] {};
    std::snprintf(message, sizeof(message), "%s (HRESULT 0x%08lx)", operation,
                  static_cast<unsigned long>(result));
    write_error(error_buffer, error_buffer_size, message);
}

void write_device_error(
    char *const error_buffer, const uint32_t error_buffer_size, const char *const operation,
    const HRESULT result, ID3D12Device *const device)
{
    char message[192] {};
    const HRESULT removal_reason = device == nullptr ? E_FAIL : device->GetDeviceRemovedReason();
    std::snprintf(
        message, sizeof(message), "%s (HRESULT 0x%08lx; device reason 0x%08lx)", operation,
        static_cast<unsigned long>(result), static_cast<unsigned long>(removal_reason));
    write_error(error_buffer, error_buffer_size, message);
}

pano_gpu_result wait_for_fence(
    pano_gpu_device_core *const device_core, const uint64_t fence_value, char *const error_buffer,
    const uint32_t error_buffer_size, const char *const operation)
{
    if (device_core->fence->GetCompletedValue() < fence_value)
    {
        HANDLE event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event_handle == nullptr ||
            FAILED(device_core->fence->SetEventOnCompletion(fence_value, event_handle)) ||
            WaitForSingleObject(event_handle, 10000) != WAIT_OBJECT_0)
        {
            if (event_handle != nullptr)
                CloseHandle(event_handle);
            write_error(error_buffer, error_buffer_size, operation);
            return PANO_GPU_UNAVAILABLE;
        }
        CloseHandle(event_handle);
    }
#if defined(PANO_GPU_TEST_HOOKS)
    if (fail_next_fence_wait.exchange(false, std::memory_order_relaxed))
    {
        write_error(error_buffer, error_buffer_size, operation);
        return PANO_GPU_UNAVAILABLE;
    }
#endif
    return PANO_GPU_SUCCESS;
}
#endif

bool valid_probe_arguments(
    const pano_gpu_probe_options *options, pano_gpu_adapter_info *adapter, char *error_buffer,
    const uint32_t error_buffer_size)
{
    if (options == nullptr || adapter == nullptr || options->size != sizeof(*options) ||
        options->abi_version != PANO_GPU_ABI_VERSION || adapter->size != sizeof(*adapter) ||
        adapter->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 probe structure");
        return false;
    }
    return true;
}

bool valid_diagnostics_arguments(
    const pano_gpu_diagnostics *const diagnostics, char *const error_buffer,
    const uint32_t error_buffer_size)
{
    if (diagnostics == nullptr || diagnostics->size != sizeof(*diagnostics) ||
        diagnostics->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 diagnostics structure");
        return false;
    }
    return true;
}

bool valid_device_diagnostics_arguments(
    const pano_gpu_device_diagnostics *const diagnostics, char *const error_buffer,
    const uint32_t error_buffer_size)
{
    if (diagnostics == nullptr || diagnostics->size != sizeof(*diagnostics) ||
        diagnostics->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 device diagnostics structure");
        return false;
    }
    return true;
}

bool valid_session_diagnostics_arguments(
    const pano_gpu_session_diagnostics *const diagnostics, char *const error_buffer,
    const uint32_t error_buffer_size)
{
    if (diagnostics == nullptr || diagnostics->size != sizeof(*diagnostics) ||
        diagnostics->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 session diagnostics structure");
        return false;
    }
    return true;
}

#if defined(_WIN32)
bool admit_adapter(IDXGIAdapter1 *const selected, pano_gpu_adapter_info *const adapter)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(selected, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        return false;
    D3D12_FEATURE_DATA_ROOT_SIGNATURE root_signature {D3D_ROOT_SIGNATURE_VERSION_1_0};
    if (FAILED(device->CheckFeatureSupport(
            D3D12_FEATURE_ROOT_SIGNATURE, &root_signature, sizeof(root_signature))) ||
        root_signature.HighestVersion < D3D_ROOT_SIGNATURE_VERSION_1_0)
        return false;
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model {D3D_SHADER_MODEL_5_1};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader_model, sizeof(shader_model))) ||
        shader_model.HighestShaderModel < D3D_SHADER_MODEL_5_1)
        return false;
    DXGI_ADAPTER_DESC1 description {};
    if (FAILED(selected->GetDesc1(&description)))
        return false;
    ComPtr<IDXGIAdapter3> adapter3;
    DXGI_QUERY_VIDEO_MEMORY_INFO memory {};
    if (FAILED(selected->QueryInterface(IID_PPV_ARGS(&adapter3))) ||
        FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)) ||
        memory.CurrentUsage > memory.Budget)
        return false;

    pano_gpu_adapter_info admitted {};
    admitted.size = sizeof(admitted);
    admitted.abi_version = PANO_GPU_ABI_VERSION;
    admitted.vendor_id = description.VendorId;
    admitted.device_id = description.DeviceId;
    admitted.luid = (static_cast<uint64_t>(description.AdapterLuid.HighPart) << 32) |
        description.AdapterLuid.LowPart;
    admitted.dedicated_bytes = description.DedicatedVideoMemory;
    admitted.local_budget_bytes = memory.Budget;
    admitted.local_usage_bytes = memory.CurrentUsage;
    if (WideCharToMultiByte(
            CP_UTF8, 0, description.Description, -1, admitted.name, sizeof(admitted.name), nullptr,
            nullptr) == 0)
        admitted.name[0] = '\0';
    *adapter = admitted;
    return true;
}

bool select_admitted_adapter(
    const pano_gpu_probe_options *const options, Microsoft::WRL::ComPtr<IDXGIAdapter1> *const selected,
    pano_gpu_adapter_info *const adapter, char *const error_buffer, const uint32_t error_buffer_size)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create DXGI factory");
        return false;
    }
    if (options->allow_warp != 0)
    {
        ComPtr<IDXGIFactory4> factory4;
        if (FAILED(factory.As(&factory4)) ||
            FAILED(factory4->EnumWarpAdapter(
                __uuidof(IDXGIAdapter1), reinterpret_cast<void **>(selected->ReleaseAndGetAddressOf()))) ||
            !admit_adapter(selected->Get(), adapter))
        {
            write_error(error_buffer, error_buffer_size, "no compatible WARP D3D12 adapter found");
            return false;
        }
        return true;
    }

    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(factory.As(&factory6)))
    {
        for (UINT index = 0;; ++index)
        {
            const HRESULT result = factory6->EnumAdapterByGpuPreference(
                index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1),
                reinterpret_cast<void **>(selected->ReleaseAndGetAddressOf()));
            if (result == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(result))
            {
                write_error(error_buffer, error_buffer_size, "cannot enumerate high-performance DXGI adapters");
                return false;
            }
            DXGI_ADAPTER_DESC1 description {};
            if (SUCCEEDED(selected->Get()->GetDesc1(&description)) &&
                (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                admit_adapter(selected->Get(), adapter))
                return true;
            selected->Reset();
        }
    }
    for (UINT index = 0;; ++index)
    {
        const HRESULT result = factory->EnumAdapters1(index, selected->ReleaseAndGetAddressOf());
        if (result == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(result))
        {
            write_error(error_buffer, error_buffer_size, "cannot enumerate DXGI adapters");
            return false;
        }
        DXGI_ADAPTER_DESC1 description {};
        if (SUCCEEDED(selected->Get()->GetDesc1(&description)) &&
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            admit_adapter(selected->Get(), adapter))
            return true;
        selected->Reset();
    }
    write_error(error_buffer, error_buffer_size, "no compatible hardware D3D12 adapter found");
    return false;
}
#endif

constexpr uint64_t mib = 1024 * 1024;
constexpr uint64_t resource_alignment = 64 * 1024;
constexpr uint64_t gpu_reserve_bytes = 384 * mib;

#if defined(PANO_GPU_TEST_HOOKS)
std::atomic<bool> fail_next_allocation {false};
std::atomic<bool> fail_next_device_creation {false};
std::atomic<bool> fail_next_pipeline_creation {false};
std::atomic<bool> fail_next_descriptor_creation {false};
std::atomic<bool> fail_next_resource_creation {false};
std::atomic<bool> fail_next_composite_before_dispatch {false};
std::atomic<bool> fail_next_composite_after_dispatch {false};
std::atomic<bool> fail_next_device_removed_before_dispatch {false};
std::atomic<bool> fail_next_device_removed_after_dispatch {false};
std::atomic<bool> fail_next_download_allocation {false};
std::atomic<bool> fail_next_download_submission {false};
std::atomic<bool> fail_next_download_fence_wait {false};
std::atomic<bool> fail_next_download_map {false};
std::atomic<bool> fail_next_session_allocation {false};
std::atomic<bool> fail_next_output_allocation {false};
std::atomic<bool> fail_next_preview_allocation {false};
std::atomic<bool> fail_next_source_allocation {false};
std::atomic<bool> fail_next_rotation_allocation {false};
std::atomic<bool> fail_next_encoding_metadata_allocation {false};
std::atomic<bool> fail_next_upload_slot_allocation {false};
std::atomic<bool> fail_next_second_upload_slot_allocation {false};
std::atomic<bool> fail_next_encoding_metadata_upload {false};
std::atomic<bool> fail_next_fence_signal {false};
std::atomic<bool> cancel_after_next_upload_slot_wait {false};
std::atomic<bool> cancel_after_next_upload_finish_wait {false};
std::atomic<bool> cancel_after_next_output_download_wait {false};
std::atomic<bool> stale_after_next_preview_wait {false};
#endif

bool checked_add(const uint64_t left, const uint64_t right, uint64_t *const result)
{
    if (left > std::numeric_limits<uint64_t>::max() - right)
        return false;
    *result = left + right;
    return true;
}

bool checked_multiply(const uint64_t left, const uint64_t right, uint64_t *const result)
{
    if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left)
        return false;
    *result = left * right;
    return true;
}

bool align_resource(const uint64_t value, uint64_t *const result)
{
    const uint64_t padding = resource_alignment - 1;
    if (value > std::numeric_limits<uint64_t>::max() - padding)
        return false;
    *result = (value + padding) & ~padding;
    return true;
}
} // namespace

uint32_t pano_gpu_abi_version(void) noexcept
{
    return PANO_GPU_ABI_VERSION;
}

pano_gpu_result pano_gpu_probe(char *error_buffer, const uint32_t error_buffer_size) noexcept
{
    write_error(error_buffer, error_buffer_size, "D3D12 backend is not implemented in this build");
    return PANO_GPU_UNAVAILABLE;
}

pano_gpu_result pano_gpu_probe_adapter(
    const pano_gpu_probe_options *const options, pano_gpu_adapter_info *const adapter,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (!valid_probe_arguments(options, adapter, error_buffer, error_buffer_size))
        return PANO_GPU_INVALID_ARGUMENT;
    std::memset(adapter, 0, sizeof(*adapter));
    adapter->size = sizeof(*adapter);
    adapter->abi_version = PANO_GPU_ABI_VERSION;
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIAdapter1> selected;
    return select_admitted_adapter(options, &selected, adapter, error_buffer, error_buffer_size)
        ? PANO_GPU_SUCCESS
        : PANO_GPU_UNAVAILABLE;
#endif
}

pano_gpu_result pano_gpu_device_dispatch_self_test(
    pano_gpu_device *const handle, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
#if !defined(_WIN32)
    (void)handle;
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    using Microsoft::WRL::ComPtr;
    if (handle == nullptr || !handle->core || !handle->core->d3d_device || !handle->core->queue ||
        !handle->core->fence)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 device handle is invalid");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    ID3D12Device *const device = handle->core->d3d_device.Get();
    ID3D12CommandQueue *const queue = handle->core->queue.Get();
    D3D12_DESCRIPTOR_RANGE range {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameter {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable = {1, &range};
    D3D12_ROOT_SIGNATURE_DESC root_description {1, &parameter, 0, nullptr,
                                                D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)))
    {
        write_error(error_buffer, error_buffer_size, "cannot serialize D3D12 root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    if (FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root_signature))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root_signature.Get();
    pipeline_description.CS = {pano_gpu_fill_shader, sizeof(pano_gpu_fill_shader)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    const bool fail_pipeline =
#if defined(PANO_GPU_TEST_HOOKS)
        fail_next_pipeline_creation.exchange(false, std::memory_order_relaxed);
#else
        false;
#endif
    if (fail_pipeline ||
        FAILED(device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 compute pipeline");
        return PANO_GPU_UNAVAILABLE;
    }
    constexpr UINT element_count = 16;
    constexpr UINT64 buffer_bytes = element_count * sizeof(uint32_t);
    D3D12_HEAP_PROPERTIES default_heap {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES upload_heap {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = buffer_bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_RESOURCE_DESC output_buffer = buffer;
    output_buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D12Resource> output;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    const bool fail_resource =
#if defined(PANO_GPU_TEST_HOOKS)
        fail_next_resource_creation.exchange(false, std::memory_order_relaxed);
#else
        false;
#endif
    if (fail_resource || FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &output_buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&output))) ||
        FAILED(device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&upload))) ||
        FAILED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 self-test resources");
        return PANO_GPU_UNAVAILABLE;
    }
    void *mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 upload resource");
        return PANO_GPU_UNAVAILABLE;
    }
    std::memset(mapped, 0, static_cast<size_t>(buffer_bytes));
    upload->Unmap(0, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 1;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    const bool fail_descriptor =
#if defined(PANO_GPU_TEST_HOOKS)
        fail_next_descriptor_creation.exchange(false, std::memory_order_relaxed);
#else
        false;
#endif
    if (fail_descriptor ||
        FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&descriptor_heap))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 descriptor heap");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.Buffer.NumElements = element_count;
    uav.Buffer.StructureByteStride = sizeof(uint32_t);
    device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, descriptor_heap->GetCPUDescriptorHandleForHeapStart());
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    const HRESULT allocator_result =
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(allocator_result))
    {
        write_device_error(
            error_buffer, error_buffer_size, "cannot create D3D12 command allocator", allocator_result,
            device);
        return PANO_GPU_UNAVAILABLE;
    }
    if (FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 command list");
        return PANO_GPU_UNAVAILABLE;
    }
    list->CopyResource(output.Get(), upload.Get());
    D3D12_RESOURCE_BARRIER to_uav {};
    to_uav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_uav.Transition.pResource = output.Get();
    to_uav.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_uav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    to_uav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &to_uav);
    ID3D12DescriptorHeap *heaps[] = {descriptor_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_signature.Get());
    list->SetComputeRootDescriptorTable(0, descriptor_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER to_copy {};
    to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy.Transition.pResource = output.Get();
    to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &to_copy);
    list->CopyResource(readback.Get(), output.Get());
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value = handle->core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(queue->Signal(handle->core->fence.Get(), fence_value)))
    {
        write_error(error_buffer, error_buffer_size, "cannot signal D3D12 self-test fence");
        return PANO_GPU_UNAVAILABLE;
    }
    HANDLE event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_handle == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 self-test fence event");
        return PANO_GPU_UNAVAILABLE;
    }
    const HRESULT wait_result = handle->core->fence->SetEventOnCompletion(fence_value, event_handle);
    const DWORD wait_status = SUCCEEDED(wait_result) ? WaitForSingleObject(event_handle, 10000) : WAIT_FAILED;
    CloseHandle(event_handle);
    if (wait_status != WAIT_OBJECT_0)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 self-test fence timed out");
        return PANO_GPU_UNAVAILABLE;
    }
    const D3D12_RANGE read_range {0, static_cast<SIZE_T>(buffer_bytes)};
    if (FAILED(readback->Map(0, &read_range, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 readback resource");
        return PANO_GPU_UNAVAILABLE;
    }
    const auto *values = static_cast<const uint32_t *>(mapped);
    bool valid = true;
    for (UINT index = 0; index < element_count; ++index)
        valid = valid && values[index] == index * 3 + 1;
    readback->Unmap(0, nullptr);
    if (!valid)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 self-test returned unexpected bytes");
        return PANO_GPU_UNAVAILABLE;
    }
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_dispatch_self_test(
    const uint32_t allow_warp, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    pano_gpu_probe_options options {};
    options.size = sizeof(options);
    options.abi_version = PANO_GPU_ABI_VERSION;
    options.allow_warp = allow_warp != 0 ? 1U : 0U;
    pano_gpu_device *device = nullptr;
    const pano_gpu_result create_result =
        pano_gpu_device_create(&options, &device, error_buffer, error_buffer_size);
    if (create_result != PANO_GPU_SUCCESS)
        return create_result;
    const pano_gpu_result dispatch_result =
        pano_gpu_device_dispatch_self_test(device, error_buffer, error_buffer_size);
    pano_gpu_device_destroy(&device);
    return dispatch_result;
}

pano_gpu_result pano_gpu_plan_memory(
    const pano_gpu_memory_request *const request, pano_gpu_memory_plan *const plan,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (request == nullptr || plan == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || plan->size != sizeof(*plan) ||
        plan->abi_version != PANO_GPU_ABI_VERSION || request->frame_count == 0 ||
        request->source_width == 0 || request->source_height == 0 || request->output_width == 0 ||
        request->output_height == 0 || request->source_sample_bytes == 0 ||
        request->output_sample_bytes == 0 || request->free_bytes > request->total_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 memory-plan request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    uint64_t pixels = 0;
    uint64_t source_bytes = 0;
    uint64_t output_pixels = 0;
    if (!checked_multiply(request->frame_count, request->source_width, &pixels) ||
        !checked_multiply(pixels, request->source_height, &pixels) || !checked_multiply(pixels, 3, &pixels) ||
        !checked_multiply(pixels, request->source_sample_bytes, &source_bytes) ||
        !align_resource(source_bytes, &source_bytes) ||
        !checked_multiply(request->output_width, request->output_height, &output_pixels) ||
        output_pixels > std::numeric_limits<uint32_t>::max())
    {
        write_error(error_buffer, error_buffer_size, "D3D12 memory-plan dimensions overflow");
        return PANO_GPU_UNAVAILABLE;
    }
    const uint64_t reserve = std::max(gpu_reserve_bytes, request->total_bytes * 15 / 100);
    const uint64_t system_available = request->free_bytes > reserve ? request->free_bytes - reserve : 0;
    const uint64_t available = request->requested_budget_bytes == 0 ? system_available :
        std::min(system_available, request->requested_budget_bytes);
    uint64_t minimum_session_bytes = 0;
    uint64_t source_frame_bytes = 0;
    uint64_t minimum_upload_bytes = 0;
    uint64_t minimum_output_bytes_per_pixel = 3 * sizeof(float) + sizeof(uint8_t);
    if ((request->needs_sdr_conversion != 0 &&
         !checked_add(
             minimum_output_bytes_per_pixel, 3ULL * request->output_sample_bytes,
             &minimum_output_bytes_per_pixel)) ||
        !checked_multiply(request->frame_count, 9 * sizeof(float), &minimum_session_bytes) ||
        !align_resource(minimum_session_bytes, &minimum_session_bytes) ||
        !checked_multiply(request->source_width, request->source_height, &source_frame_bytes) ||
        !checked_multiply(source_frame_bytes, 3 * request->source_sample_bytes, &source_frame_bytes) ||
        !align_resource(source_frame_bytes, &source_frame_bytes) ||
        !checked_multiply(source_frame_bytes, 2, &minimum_upload_bytes))
    {
        write_error(error_buffer, error_buffer_size, "D3D12 allocation-accounting minimum overflows");
        return PANO_GPU_UNAVAILABLE;
    }
    if (request->session_workspace_bytes < minimum_session_bytes ||
        request->output_workspace_bytes_per_pixel < minimum_output_bytes_per_pixel ||
        request->upload_bytes < minimum_upload_bytes || request->readback_bytes_per_pixel == 0 ||
        request->descriptor_count < request->frame_count + 4 || request->reserved != 0)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 allocation accounting");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    uint64_t session_bytes = 0;
    uint64_t upload_bytes = 0;
    uint64_t preview_bytes = 0;
    if (!align_resource(request->session_workspace_bytes, &session_bytes) ||
        !align_resource(request->upload_bytes, &upload_bytes) ||
        !align_resource(request->preview_cache_bytes, &preview_bytes))
    {
        write_error(error_buffer, error_buffer_size, "D3D12 fixed allocation accounting overflows");
        return PANO_GPU_UNAVAILABLE;
    }
    const auto workspace_for_rows = [request](
                                        const uint32_t rows, uint64_t *const workspace,
                                        uint64_t *const readback) {
        uint64_t row_pixels = 0;
        uint64_t variable_workspace = 0;
        uint64_t variable_readback = 0;
        return checked_multiply(rows, request->output_width, &row_pixels) &&
               checked_multiply(
                   row_pixels, request->output_workspace_bytes_per_pixel, &variable_workspace) &&
               checked_add(variable_workspace, request->output_workspace_fixed_bytes, workspace) &&
               align_resource(*workspace, workspace) &&
               checked_multiply(row_pixels, request->readback_bytes_per_pixel, &variable_readback) &&
               checked_add(variable_readback, request->readback_fixed_bytes, readback) &&
               align_resource(*readback, readback);
    };
    uint64_t workspace = 0;
    uint64_t readback = 0;
    if (!workspace_for_rows(request->output_height, &workspace, &readback))
    {
        write_error(error_buffer, error_buffer_size, "D3D12 output workspace overflows");
        return PANO_GPU_UNAVAILABLE;
    }
    uint64_t required = 0;
    if (!checked_add(source_bytes, session_bytes, &required) ||
        !checked_add(required, upload_bytes, &required) ||
        !checked_add(required, preview_bytes, &required) ||
        !checked_add(required, workspace, &required) || !checked_add(required, readback, &required))
    {
        write_error(error_buffer, error_buffer_size, "D3D12 memory-plan total overflows");
        return PANO_GPU_UNAVAILABLE;
    }
    uint32_t rows = 0;
    if (required > available)
    {
        for (uint32_t candidate = (std::min(request->output_height, 1024U) / 32U) * 32U;
             candidate >= 32;)
        {
            if (workspace_for_rows(candidate, &workspace, &readback) &&
                checked_add(source_bytes, session_bytes, &required) &&
                checked_add(required, upload_bytes, &required) &&
                checked_add(required, preview_bytes, &required) &&
                checked_add(required, workspace, &required) && checked_add(required, readback, &required) &&
                required <= available)
            {
                rows = candidate;
                break;
            }
            if (candidate == 32)
                break;
            candidate -= 32;
        }
        if (rows == 0)
        {
            write_error(error_buffer, error_buffer_size, "insufficient D3D12 memory for sources and a 32-row output band");
            return PANO_GPU_UNAVAILABLE;
        }
    }
    plan->output_band_rows = rows;
    plan->descriptor_count = request->descriptor_count;
    plan->source_bytes = source_bytes;
    plan->session_workspace_bytes = session_bytes;
    plan->output_workspace_bytes = workspace;
    plan->upload_bytes = upload_bytes;
    plan->readback_bytes = readback;
    plan->reserve_bytes = reserve;
    plan->required_bytes = required;
    plan->available_bytes = available;
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_query_diagnostics(
    pano_gpu_diagnostics *const diagnostics, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (!valid_diagnostics_arguments(diagnostics, error_buffer, error_buffer_size))
        return PANO_GPU_INVALID_ARGUMENT;
    diagnostics->live_device_count = live_device_count.load(std::memory_order_relaxed);
    diagnostics->live_queue_count = live_queue_count.load(std::memory_order_relaxed);
    diagnostics->live_fence_count = live_fence_count.load(std::memory_order_relaxed);
    diagnostics->live_session_count = live_session_count.load(std::memory_order_relaxed);
    diagnostics->live_output_count = live_output_count.load(std::memory_order_relaxed);
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_device_create(
    const pano_gpu_probe_options *const options, pano_gpu_device **const device,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (options == nullptr || device == nullptr || options->size != sizeof(*options) ||
        options->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 device-create structure");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    *device = nullptr;
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> d3d_device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    D3D12_COMMAND_QUEUE_DESC queue_description {};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    pano_gpu_adapter_info ignored_adapter {};
    ignored_adapter.size = sizeof(ignored_adapter);
    ignored_adapter.abi_version = PANO_GPU_ABI_VERSION;
    const bool fail_device =
#if defined(PANO_GPU_TEST_HOOKS)
        fail_next_device_creation.exchange(false, std::memory_order_relaxed);
#else
        false;
#endif
    if (fail_device ||
        !select_admitted_adapter(options, &adapter, &ignored_adapter, error_buffer, error_buffer_size) ||
        FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d_device))) ||
        FAILED(d3d_device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue))) ||
        FAILED(d3d_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 device resources");
        return PANO_GPU_UNAVAILABLE;
    }
    try
    {
        auto core = std::make_shared<pano_gpu_device_core>();
        core->adapter = adapter;
        core->adapter_info = ignored_adapter;
        core->d3d_device = d3d_device;
        core->queue = queue;
        core->fence = fence;
        pano_gpu_device *const created = new pano_gpu_device;
        created->core = std::move(core);
        live_device_count.fetch_add(1, std::memory_order_relaxed);
        live_queue_count.fetch_add(1, std::memory_order_relaxed);
        live_fence_count.fetch_add(1, std::memory_order_relaxed);
        created->core->counts_registered = true;
        *device = created;
        return PANO_GPU_SUCCESS;
    }
    catch (const std::bad_alloc &)
    {
        write_error(error_buffer, error_buffer_size, "cannot allocate D3D12 device handle");
        return PANO_GPU_OUT_OF_MEMORY;
    }
    catch (...)
    {
        write_error(error_buffer, error_buffer_size, "unexpected D3D12 device creation failure");
        return PANO_GPU_UNAVAILABLE;
    }
#endif
}

pano_gpu_result pano_gpu_device_query_diagnostics(
    const pano_gpu_device *const device, pano_gpu_device_diagnostics *const diagnostics,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (!valid_device_diagnostics_arguments(diagnostics, error_buffer, error_buffer_size))
        return PANO_GPU_INVALID_ARGUMENT;
    std::memset(diagnostics, 0, sizeof(*diagnostics));
    diagnostics->size = sizeof(*diagnostics);
    diagnostics->abi_version = PANO_GPU_ABI_VERSION;
    if (device == nullptr || !device->core)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 device handle is null");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    diagnostics->adapter = device->core->adapter_info;
    if (diagnostics->adapter.local_usage_bytes > diagnostics->adapter.local_budget_bytes)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 device memory budget is invalid");
        return PANO_GPU_UNAVAILABLE;
    }
    diagnostics->usable_local_bytes =
        diagnostics->adapter.local_budget_bytes - diagnostics->adapter.local_usage_bytes;
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_validate_session_create_options(
    const pano_gpu_device *const device, const pano_gpu_session_create_options *const options,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (device == nullptr || !device->core || options == nullptr || options->size != sizeof(*options) ||
        options->abi_version != PANO_GPU_ABI_VERSION || options->frame_count == 0 ||
        options->source_width == 0 || options->source_height == 0 ||
        options->device_luid != device->core->adapter_info.luid ||
        (options->rotations == nullptr) != (options->rotations_bytes == 0) ||
        (options->encoding_metadata == nullptr) != (options->encoding_metadata_bytes == 0))
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 session-create structure");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    uint64_t sample_bytes = 0;
    switch (options->source_sample_type)
    {
    case PANO_GPU_SAMPLE_UINT8:
        sample_bytes = sizeof(uint8_t);
        break;
    case PANO_GPU_SAMPLE_UINT16:
        sample_bytes = sizeof(uint16_t);
        break;
    case PANO_GPU_SAMPLE_FLOAT32:
        sample_bytes = sizeof(float);
        break;
    default:
        write_error(error_buffer, error_buffer_size, "unsupported D3D12 source sample type");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    if (options->transfer_function != PANO_GPU_TRANSFER_SRGB &&
        options->transfer_function != PANO_GPU_TRANSFER_PQ &&
        options->transfer_function != PANO_GPU_TRANSFER_LINEAR)
    {
        write_error(error_buffer, error_buffer_size, "unsupported D3D12 source transfer function");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    uint64_t minimum_stride = 0;
    uint64_t rotation_bytes = 0;
    if (!checked_multiply(options->source_width, 3 * sample_bytes, &minimum_stride) ||
        minimum_stride > std::numeric_limits<uint32_t>::max() ||
        options->source_row_stride_bytes < minimum_stride ||
        !checked_multiply(options->frame_count, 9 * sizeof(float), &rotation_bytes) ||
        (options->rotations != nullptr && options->rotations_bytes != rotation_bytes))
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 session source stride or rotation metadata");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_session_create(
    pano_gpu_device *const device, const pano_gpu_session_create_options *const options,
    pano_gpu_session **const session, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 session out-handle is null");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    *session = nullptr;
    const pano_gpu_result validation_result =
        pano_gpu_validate_session_create_options(device, options, error_buffer, error_buffer_size);
    if (validation_result != PANO_GPU_SUCCESS)
        return validation_result;
    uint64_t planned_source_bytes = 0;
    uint64_t planned_rotation_bytes = 0;
    uint64_t planned_encoding_metadata_bytes = 0;
    if (!checked_multiply(options->frame_count, options->source_height, &planned_source_bytes) ||
        !checked_multiply(planned_source_bytes, options->source_row_stride_bytes, &planned_source_bytes) ||
        !align_resource(planned_source_bytes, &planned_source_bytes) ||
        (options->rotations_bytes != 0 && !align_resource(options->rotations_bytes, &planned_rotation_bytes)) ||
        (options->encoding_metadata_bytes != 0 &&
         !align_resource(options->encoding_metadata_bytes, &planned_encoding_metadata_bytes)))
    {
        write_error(error_buffer, error_buffer_size, "D3D12 session allocation plan overflows");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    try
    {
#if defined(PANO_GPU_TEST_HOOKS)
        if (fail_next_session_allocation.exchange(false, std::memory_order_relaxed))
            throw std::bad_alloc {};
#endif
        std::unique_ptr<pano_gpu_session> created {new pano_gpu_session};
        created->device_core = device->core;
        created->frame_count = options->frame_count;
        created->source_sample_type = options->source_sample_type;
        created->transfer_function = options->transfer_function;
        created->source_width = options->source_width;
        created->source_height = options->source_height;
        created->source_row_stride_bytes = options->source_row_stride_bytes;
        created->source_frame_bytes =
            static_cast<uint64_t>(created->source_height) * created->source_row_stride_bytes;
        created->frame_upload_fences.resize(created->frame_count);
        created->planned_source_bytes = planned_source_bytes;
        created->requested_rotation_bytes = options->rotations_bytes;
        created->planned_rotation_bytes = planned_rotation_bytes;
        created->requested_encoding_metadata_bytes = options->encoding_metadata_bytes;
        created->planned_encoding_metadata_bytes = planned_encoding_metadata_bytes;
        live_session_count.fetch_add(1, std::memory_order_relaxed);
        created->count_registered = true;
        *session = created.release();
        return PANO_GPU_SUCCESS;
    }
    catch (const std::bad_alloc &)
    {
        write_error(error_buffer, error_buffer_size, "cannot allocate D3D12 session handle");
        return PANO_GPU_OUT_OF_MEMORY;
    }
    catch (...)
    {
        write_error(error_buffer, error_buffer_size, "unexpected D3D12 session creation failure");
        return PANO_GPU_UNAVAILABLE;
    }
}

pano_gpu_result pano_gpu_session_query_diagnostics(
    const pano_gpu_session *const session, pano_gpu_session_diagnostics *const diagnostics,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (!valid_session_diagnostics_arguments(diagnostics, error_buffer, error_buffer_size))
        return PANO_GPU_INVALID_ARGUMENT;
    std::memset(diagnostics, 0, sizeof(*diagnostics));
    diagnostics->size = sizeof(*diagnostics);
    diagnostics->abi_version = PANO_GPU_ABI_VERSION;
    if (session == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 session handle is null");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    diagnostics->planned_source_bytes = session->planned_source_bytes;
    diagnostics->source_bytes = session->source_bytes;
    diagnostics->planned_rotation_bytes = session->planned_rotation_bytes;
    diagnostics->rotation_bytes = session->rotation_bytes;
    diagnostics->planned_encoding_metadata_bytes = session->planned_encoding_metadata_bytes;
    diagnostics->encoding_metadata_bytes = session->encoding_metadata_bytes;
    diagnostics->upload_count = session->upload_count;
    diagnostics->uploaded_bytes = session->uploaded_bytes;
    diagnostics->last_completed_upload_fence = session->last_completed_upload_fence;
#if defined(_WIN32)
    if (session->device_core && session->device_core->fence)
    {
        const uint64_t completed_fence = session->device_core->fence->GetCompletedValue();
        if (session->first_upload_slot_fence <= completed_fence)
            diagnostics->last_completed_upload_fence = std::max(
                diagnostics->last_completed_upload_fence, session->first_upload_slot_fence);
        if (session->second_upload_slot_fence <= completed_fence)
            diagnostics->last_completed_upload_fence = std::max(
                diagnostics->last_completed_upload_fence, session->second_upload_slot_fence);
    }
#endif
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_exposure_pair_count(
    const uint32_t frame_count, uint32_t *const pair_count, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (pair_count == nullptr)
        return PANO_GPU_INVALID_ARGUMENT;
    *pair_count = 0;
    uint64_t count = 0;
    if (frame_count == 0 || !checked_multiply(frame_count, frame_count - 1ULL, &count) ||
        (count /= 2) > std::numeric_limits<uint32_t>::max())
    {
        write_error(error_buffer, error_buffer_size, "D3D12 exposure pair count overflows");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    *pair_count = static_cast<uint32_t>(count);
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_plan_auto_contrast_histogram(
    const pano_gpu_histogram_request *const request, pano_gpu_histogram_layout *const layout,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (layout == nullptr || layout->size != sizeof(*layout) ||
        layout->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 histogram layout");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    layout->bin_count = 0;
    layout->counter_bytes = 0;
    layout->histogram_bytes = 0;
    layout->maximum_population = 0;
    uint64_t population = 0;
    if (request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->output_width == 0 ||
        request->output_height == 0 ||
        !checked_multiply(request->output_width, request->output_height, &population) ||
        population > std::numeric_limits<uint32_t>::max())
    {
        write_error(error_buffer, error_buffer_size, "D3D12 histogram population exceeds uint32");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    layout->bin_count = 4096;
    layout->counter_bytes = sizeof(uint32_t);
    layout->histogram_bytes = 4096ULL * sizeof(uint32_t);
    layout->maximum_population = population;
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_session_prepare_exposure_graph(
    pano_gpu_session *const session, const uint32_t pair_capacity, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    uint64_t maximum_pairs = 0;
    if (session == nullptr ||
        !checked_multiply(session->frame_count, session->frame_count - 1ULL, &maximum_pairs))
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-graph session");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    maximum_pairs /= 2;
    if (pair_capacity > maximum_pairs)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 exposure-graph capacity exceeds frame pairs");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    try
    {
#if defined(PANO_GPU_TEST_HOOKS)
        if (fail_next_allocation.exchange(false, std::memory_order_relaxed))
            throw std::bad_alloc {};
#endif
        std::vector<pano_gpu_exposure_equation> equations;
        std::vector<pano_gpu_exposure_pair_report> reports;
        equations.reserve(pair_capacity);
        reports.reserve(pair_capacity);
        session->exposure_equations.swap(equations);
        session->exposure_solve_equations.clear();
        session->exposure_solve_graph_ready = false;
        session->exposure_log_gains.clear();
        session->exposure_solved = false;
        session->exposure_global_gains.clear();
        session->exposure_gains_uploaded = false;
        session->exposure_pair_reports.swap(reports);
        session->exposure_pair_capacity = pair_capacity;
        return PANO_GPU_SUCCESS;
    }
    catch (const std::bad_alloc &)
    {
        write_error(error_buffer, error_buffer_size, "cannot allocate D3D12 exposure-graph storage");
        return PANO_GPU_OUT_OF_MEMORY;
    }
    catch (...)
    {
        write_error(error_buffer, error_buffer_size, "unexpected D3D12 exposure-graph allocation failure");
        return PANO_GPU_UNAVAILABLE;
    }
}

pano_gpu_result pano_gpu_session_enumerate_exposure_pairs(
    pano_gpu_session *const session, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    uint32_t pair_count = 0;
    if (session == nullptr ||
        pano_gpu_exposure_pair_count(
            session->frame_count, &pair_count, error_buffer, error_buffer_size) != PANO_GPU_SUCCESS ||
        session->exposure_pair_capacity != pair_count)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-pair enumeration state");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    try
    {
        std::vector<pano_gpu_exposure_pair_report> reports;
        reports.reserve(pair_count);
        for (uint32_t left = 0; left < session->frame_count; ++left)
            for (uint32_t right = left + 1; right < session->frame_count; ++right)
                reports.push_back(pano_gpu_exposure_pair_report {
                    left, right, PANO_GPU_EXPOSURE_PAIR_PENDING, 0, 0, 0});
        session->exposure_pair_reports.swap(reports);
        session->exposure_equations.clear();
        session->exposure_solve_equations.clear();
        session->exposure_solve_graph_ready = false;
        session->exposure_log_gains.clear();
        session->exposure_solved = false;
        session->exposure_global_gains.clear();
        session->exposure_gains_uploaded = false;
        return PANO_GPU_SUCCESS;
    }
    catch (const std::bad_alloc &)
    {
        write_error(error_buffer, error_buffer_size, "cannot enumerate D3D12 exposure pairs");
        return PANO_GPU_OUT_OF_MEMORY;
    }
    catch (...)
    {
        write_error(error_buffer, error_buffer_size, "unexpected D3D12 exposure-pair enumeration failure");
        return PANO_GPU_UNAVAILABLE;
    }
}

pano_gpu_result pano_gpu_session_copy_exposure_pair_reports(
    const pano_gpu_session *const session, pano_gpu_exposure_pair_report *const reports,
    const uint64_t report_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    uint64_t expected_bytes = 0;
    if (session == nullptr ||
        !checked_multiply(
            session->exposure_pair_reports.size(), sizeof(pano_gpu_exposure_pair_report),
            &expected_bytes) ||
        report_bytes != expected_bytes || (expected_bytes != 0 && reports == nullptr))
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-pair report copy");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    if (expected_bytes != 0)
        std::memcpy(reports, session->exposure_pair_reports.data(), static_cast<size_t>(expected_bytes));
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_session_copy_exposure_equations(
    const pano_gpu_session *const session, pano_gpu_exposure_equation *const equations,
    const uint64_t equation_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    uint64_t expected_bytes = 0;
    if (session == nullptr ||
        !checked_multiply(
            session->exposure_equations.size(), sizeof(pano_gpu_exposure_equation),
            &expected_bytes) ||
        equation_bytes != expected_bytes || (expected_bytes != 0 && equations == nullptr))
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-equation copy");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    if (expected_bytes != 0)
        std::memcpy(equations, session->exposure_equations.data(), static_cast<size_t>(expected_bytes));
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_session_build_exposure_solve_graph(
    pano_gpu_session *const session, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || session->exposure_pair_reports.size() != session->exposure_pair_capacity)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure solve-graph state");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    for (const auto &report : session->exposure_pair_reports)
        if (report.rejection_reason == PANO_GPU_EXPOSURE_PAIR_PENDING)
        {
            write_error(error_buffer, error_buffer_size, "D3D12 exposure reports are still pending");
            return PANO_GPU_INVALID_ARGUMENT;
        }
    try
    {
        const size_t frame_count = session->frame_count;
        if (frame_count != 0 && frame_count > std::numeric_limits<size_t>::max() / frame_count)
            throw std::bad_alloc {};
        const size_t matrix_size = frame_count * frame_count;
        std::vector<uint8_t> adjacency(matrix_size, 0);
        std::vector<uint8_t> geometric(matrix_size, 0);
        std::vector<uint8_t> bridges(matrix_size, 0);
        for (size_t frame = 0; frame < frame_count; ++frame)
            adjacency[frame * frame_count + frame] = 1;
        for (const auto &equation : session->exposure_equations)
        {
            if (equation.left_frame_index >= frame_count || equation.right_frame_index >= frame_count ||
                equation.left_frame_index == equation.right_frame_index || equation.weight <= 0.0 ||
                !std::isfinite(equation.weight) || !std::isfinite(equation.difference))
            {
                write_error(error_buffer, error_buffer_size, "invalid measured D3D12 exposure equation");
                return PANO_GPU_INVALID_ARGUMENT;
            }
            adjacency[equation.left_frame_index * frame_count + equation.right_frame_index] = 1;
            adjacency[equation.right_frame_index * frame_count + equation.left_frame_index] = 1;
        }
        for (const auto &report : session->exposure_pair_reports)
            if (report.geometric_count >= 24)
            {
                geometric[report.left_frame_index * frame_count + report.right_frame_index] = 1;
                geometric[report.right_frame_index * frame_count + report.left_frame_index] = 1;
            }
        for (size_t iteration = 0; iteration < frame_count; ++iteration)
        {
            std::vector<uint8_t> reachability = adjacency;
            for (size_t middle = 0; middle < frame_count; ++middle)
                for (size_t row = 0; row < frame_count; ++row)
                    if (reachability[row * frame_count + middle] != 0)
                        for (size_t column = 0; column < frame_count; ++column)
                            reachability[row * frame_count + column] |=
                                reachability[middle * frame_count + column];
            std::vector<uint8_t> additions(matrix_size, 0);
            for (size_t row = 0; row < frame_count; ++row)
                for (size_t column = 0; column < frame_count; ++column)
                    if (geometric[row * frame_count + column] != 0 &&
                        reachability[row * frame_count + column] == 0)
                    {
                        additions[row * frame_count + column] = 1;
                        additions[column * frame_count + row] = 1;
                        break;
                    }
            for (size_t index = 0; index < matrix_size; ++index)
            {
                bridges[index] |= additions[index];
                adjacency[index] |= additions[index];
            }
        }
        std::vector<pano_gpu_exposure_equation> solve_equations = session->exposure_equations;
        for (uint32_t left = 0; left < session->frame_count; ++left)
            for (uint32_t right = left + 1; right < session->frame_count; ++right)
                if (bridges[static_cast<size_t>(left) * frame_count + right] != 0)
                    solve_equations.push_back(pano_gpu_exposure_equation {left, right, 0.0, 1.0});
        session->exposure_solve_equations.swap(solve_equations);
        session->exposure_solve_graph_ready = true;
        session->exposure_log_gains.clear();
        session->exposure_solved = false;
        session->exposure_global_gains.clear();
        session->exposure_gains_uploaded = false;
        return PANO_GPU_SUCCESS;
    }
    catch (const std::bad_alloc &)
    {
        write_error(error_buffer, error_buffer_size, "cannot allocate D3D12 exposure solve graph");
        return PANO_GPU_OUT_OF_MEMORY;
    }
    catch (...)
    {
        write_error(error_buffer, error_buffer_size, "unexpected D3D12 exposure solve-graph failure");
        return PANO_GPU_UNAVAILABLE;
    }
}

pano_gpu_result pano_gpu_session_copy_exposure_solve_equations(
    const pano_gpu_session *const session, pano_gpu_exposure_equation *const equations,
    const uint64_t equation_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    uint64_t expected_bytes = 0;
    if (session == nullptr || !session->exposure_solve_graph_ready ||
        !checked_multiply(
            session->exposure_solve_equations.size(), sizeof(pano_gpu_exposure_equation),
            &expected_bytes) ||
        equation_bytes != expected_bytes || (expected_bytes != 0 && equations == nullptr))
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure solve-equation copy");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    if (expected_bytes != 0)
        std::memcpy(
            equations, session->exposure_solve_equations.data(), static_cast<size_t>(expected_bytes));
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_session_solve_exposure_graph(
    pano_gpu_session *const session, pano_gpu_exposure_solve_result *const result,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || !session->exposure_solve_graph_ready || result == nullptr ||
        result->size != sizeof(*result) || result->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure solve request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    result->anchor_frame_index = 0;
    result->edge_count = 0;
    result->frame_count = 0;
    result->reserved = 0;
    try
    {
        const size_t count = session->frame_count;
        if (count == 0 || count > std::numeric_limits<size_t>::max() / count)
            throw std::bad_alloc {};
        std::vector<double> system(count * count, 0.0);
        std::vector<double> values(count, 0.0);
        for (const auto &edge : session->exposure_solve_equations)
        {
            const size_t left = edge.left_frame_index;
            const size_t right = edge.right_frame_index;
            system[left * count + left] += edge.weight;
            system[right * count + right] += edge.weight;
            system[left * count + right] -= edge.weight;
            system[right * count + left] -= edge.weight;
            values[left] -= edge.weight * edge.difference;
            values[right] += edge.weight * edge.difference;
        }
        system[0] += 1.0;
        for (size_t column = 0; column < count; ++column)
        {
            size_t pivot_row = column;
            double pivot_size = std::fabs(system[column * count + column]);
            for (size_t row = column + 1; row < count; ++row)
                if (const double candidate = std::fabs(system[row * count + column]);
                    candidate > pivot_size)
                {
                    pivot_size = candidate;
                    pivot_row = row;
                }
            if (pivot_size <= 1.0e-12)
                continue;
            if (pivot_row != column)
            {
                for (size_t entry = column; entry < count; ++entry)
                    std::swap(system[column * count + entry], system[pivot_row * count + entry]);
                std::swap(values[column], values[pivot_row]);
            }
            const double pivot = system[column * count + column];
            for (size_t entry = column; entry < count; ++entry)
                system[column * count + entry] /= pivot;
            values[column] /= pivot;
            for (size_t row = 0; row < count; ++row)
            {
                if (row == column)
                    continue;
                const double factor = system[row * count + column];
                if (factor == 0.0)
                    continue;
                for (size_t entry = column; entry < count; ++entry)
                    system[row * count + entry] -= factor * system[column * count + entry];
                values[row] -= factor * values[column];
            }
        }
        std::vector<double> solution(count, 0.0);
        for (size_t row = 0; row < count; ++row)
            if (std::fabs(system[row * count + row]) > 1.0e-12)
                solution[row] = values[row];
        std::vector<double> ordered = solution;
        std::sort(ordered.begin(), ordered.end());
        const double median = count % 2 == 0
            ? 0.5 * (ordered[count / 2 - 1] + ordered[count / 2])
            : ordered[count / 2];
        for (double &value : solution)
            value -= median;
        ordered = solution;
        std::sort(ordered.begin(), ordered.end());
        const double centered_median = count % 2 == 0
            ? 0.5 * (ordered[count / 2 - 1] + ordered[count / 2])
            : ordered[count / 2];
        uint32_t anchor = 0;
        for (uint32_t frame = 1; frame < session->frame_count; ++frame)
            if (std::fabs(solution[frame] - centered_median) <
                std::fabs(solution[anchor] - centered_median))
                anchor = frame;
        const double limit = std::log(2.0);
        std::vector<float> gains;
        gains.reserve(count);
        for (const double value : solution)
            gains.push_back(static_cast<float>(std::clamp(value, -limit, limit)));
        session->exposure_log_gains.swap(gains);
        session->exposure_anchor_frame_index = anchor;
        session->exposure_solve_edge_count =
            static_cast<uint32_t>(session->exposure_solve_equations.size());
        session->exposure_solved = true;
        ++session->exposure_solve_count;
        result->anchor_frame_index = anchor;
        result->edge_count = session->exposure_solve_edge_count;
        result->frame_count = session->frame_count;
        return PANO_GPU_SUCCESS;
    }
    catch (const std::bad_alloc &)
    {
        write_error(error_buffer, error_buffer_size, "cannot allocate D3D12 exposure solve storage");
        return PANO_GPU_OUT_OF_MEMORY;
    }
    catch (...)
    {
        write_error(error_buffer, error_buffer_size, "unexpected D3D12 exposure solve failure");
        return PANO_GPU_UNAVAILABLE;
    }
}

pano_gpu_result pano_gpu_session_copy_exposure_log_gains(
    const pano_gpu_session *const session, float *const log_gains,
    const uint64_t log_gain_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    uint64_t expected_bytes = 0;
    if (session == nullptr || !session->exposure_solved ||
        !checked_multiply(session->exposure_log_gains.size(), sizeof(float), &expected_bytes) ||
        log_gain_bytes != expected_bytes || (expected_bytes != 0 && log_gains == nullptr))
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure gain copy");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    if (expected_bytes != 0)
        std::memcpy(log_gains, session->exposure_log_gains.data(), static_cast<size_t>(expected_bytes));
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_session_upload_exposure_gains(
    pano_gpu_session *const session, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || !session->exposure_solved || session->exposure_gains_uploaded ||
        session->exposure_log_gains.size() != session->frame_count)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-gain upload state");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    try
    {
        std::vector<float> gains;
        gains.reserve(session->frame_count);
        for (const float log_gain : session->exposure_log_gains)
        {
            const float gain = std::exp(log_gain);
            if (!std::isfinite(gain) || gain <= 0.0F)
            {
                write_error(error_buffer, error_buffer_size, "invalid solved D3D12 exposure gain");
                return PANO_GPU_INVALID_ARGUMENT;
            }
            gains.push_back(gain);
        }
        session->exposure_global_gains.swap(gains);
        session->exposure_gains_uploaded = true;
        ++session->exposure_gain_upload_count;
        return PANO_GPU_SUCCESS;
    }
    catch (const std::bad_alloc &)
    {
        write_error(error_buffer, error_buffer_size, "cannot upload D3D12 exposure gains");
        return PANO_GPU_OUT_OF_MEMORY;
    }
    catch (...)
    {
        write_error(error_buffer, error_buffer_size, "unexpected D3D12 exposure-gain upload failure");
        return PANO_GPU_UNAVAILABLE;
    }
}

pano_gpu_result pano_gpu_session_query_exposure_report(
    const pano_gpu_session *const session, pano_gpu_exposure_report *const report,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || !session->exposure_solved || report == nullptr ||
        report->size != sizeof(*report) || report->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid retained D3D12 exposure report");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    report->anchor_frame_index = session->exposure_anchor_frame_index;
    report->edge_count = session->exposure_solve_edge_count;
    report->frame_count = session->frame_count;
    report->gains_uploaded = session->exposure_gains_uploaded ? 1U : 0U;
    report->solve_count = session->exposure_solve_count;
    report->gain_upload_count = session->exposure_gain_upload_count;
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_session_invalidate_exposure(
    pano_gpu_session *const session, const uint32_t reason, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr ||
        (reason != PANO_GPU_EXPOSURE_INVALIDATE_MANUAL_GAINS &&
         reason != PANO_GPU_EXPOSURE_INVALIDATE_GEOMETRY))
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure invalidation reason");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    session->exposure_global_gains.clear();
    session->exposure_gains_uploaded = false;
    if (reason == PANO_GPU_EXPOSURE_INVALIDATE_GEOMETRY)
    {
        pano_gpu_session_clear_exposure_pair_scratch(session);
        pano_gpu_session_clear_exposure_graph(session);
    }
    return PANO_GPU_SUCCESS;
}

static pano_gpu_result dispatch_reference_exposure_pair_ratios(
    pano_gpu_session *const session, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || !session->resident_pair_classified ||
        session->resident_pair_ratios_ready || session->resident_pair_sample_count == 0)
    {
        write_error(error_buffer, error_buffer_size,
                    "resident reference exposure-pair inputs are not ready");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 1};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    const D3D12_ROOT_SIGNATURE_DESC root_description {
        2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root))))
    {
        write_error(error_buffer, error_buffer_size,
                    "cannot create resident reference exposure-pair root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root.Get();
    pipeline_description.CS = {
        pano_gpu_exposure_pair_ratio_shader, sizeof(pano_gpu_exposure_pair_ratio_shader)};
    if (FAILED(device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline))))
    {
        write_error(error_buffer, error_buffer_size,
                    "cannot create resident reference exposure-pair pipeline");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 3;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&heap))) ||
        FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(),
            IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size,
                    "cannot create resident reference exposure-pair dispatch");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT count = static_cast<UINT>(session->resident_pair_sample_count);
    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto descriptor = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC luminance_srv {};
    luminance_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    luminance_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    luminance_srv.Format = DXGI_FORMAT_UNKNOWN;
    luminance_srv.Buffer.NumElements = count;
    luminance_srv.Buffer.StructureByteStride = 2 * sizeof(float);
    device->CreateShaderResourceView(
        session->resident_pair_scratch[4].Get(), &luminance_srv, descriptor);
    descriptor.ptr += increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC accepted_srv {};
    accepted_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    accepted_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    accepted_srv.Format = DXGI_FORMAT_R32_UINT;
    accepted_srv.Buffer.NumElements = count;
    device->CreateShaderResourceView(
        session->resident_pair_scratch[1].Get(), &accepted_srv, descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ratio_uav {};
    ratio_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    ratio_uav.Format = DXGI_FORMAT_R32_FLOAT;
    ratio_uav.Buffer.NumElements = count;
    device->CreateUnorderedAccessView(
        session->resident_pair_scratch[9].Get(), nullptr, &ratio_uav, descriptor);

    std::array<D3D12_RESOURCE_BARRIER, 2> copy_transitions {};
    copy_transitions[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    copy_transitions[0].Transition = {
        session->resident_pair_scratch[5].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    copy_transitions[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    copy_transitions[1].Transition = {
        session->resident_pair_scratch[1].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST};
    list->ResourceBarrier(2, copy_transitions.data());
    list->CopyResource(
        session->resident_pair_scratch[1].Get(), session->resident_pair_scratch[5].Get());
    for (auto &transition : copy_transitions)
    {
        transition.Transition.StateBefore = transition.Transition.StateAfter;
        transition.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    list->ResourceBarrier(2, copy_transitions.data());
    D3D12_RESOURCE_BARRIER luminance_transition {};
    luminance_transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    luminance_transition.Transition = {
        session->resident_pair_scratch[4].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &luminance_transition);
    const uint32_t constants[1] {count};
    ID3D12DescriptorHeap *heaps[] {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRoot32BitConstants(0, 1, constants, 0);
    list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((count + 63) / 64, 1, 1);
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size,
                    "cannot close resident reference exposure-pair dispatch");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(
            session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(
            session->device_core.get(), fence_value, error_buffer, error_buffer_size,
            "resident reference exposure-pair dispatch timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    session->resident_pair_ratios_ready = true;
    return PANO_GPU_SUCCESS;
#endif
}

static pano_gpu_result reduce_exposure_graph(
    pano_gpu_session *const session, const pano_gpu_exposure_pair_request *const request_template,
    const bool filter_gradients, pano_gpu_progress_callback progress,
    void *const progress_user_data, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || request_template == nullptr ||
        request_template->size != sizeof(*request_template) ||
        request_template->abi_version != PANO_GPU_ABI_VERSION ||
        request_template->sample_width == 0 || request_template->sample_height == 0 ||
        request_template->reserved != 0 ||
        session->exposure_pair_reports.size() != session->exposure_pair_capacity)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-graph reduction request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    for (const auto &report : session->exposure_pair_reports)
        if (report.rejection_reason != PANO_GPU_EXPOSURE_PAIR_PENDING)
        {
            write_error(error_buffer, error_buffer_size, "D3D12 exposure graph is not pending");
            return PANO_GPU_INVALID_ARGUMENT;
        }
    try
    {
        std::vector<pano_gpu_exposure_pair_report> reports = session->exposure_pair_reports;
        std::vector<pano_gpu_exposure_equation> equations;
        equations.reserve(reports.size());
        uint32_t completed = 0;
        for (auto &report : reports)
        {
            pano_gpu_exposure_pair_request request = *request_template;
            request.first_frame_index = report.left_frame_index;
            request.second_frame_index = report.right_frame_index;
            pano_gpu_result result = pano_gpu_session_prepare_exposure_pair_scratch(
                session, &request, error_buffer, error_buffer_size);
            if (result == PANO_GPU_SUCCESS)
                result = pano_gpu_session_dispatch_exposure_pair_projection_samples(
                    session, &request, error_buffer, error_buffer_size);
            if (result == PANO_GPU_SUCCESS)
                result = pano_gpu_session_dispatch_exposure_pair_classification(
                    session, error_buffer, error_buffer_size);
            if (result == PANO_GPU_SUCCESS && filter_gradients)
                result = pano_gpu_session_dispatch_exposure_pair_gradient_limits(
                    session, request.sample_width, request.sample_height, error_buffer,
                    error_buffer_size);
            if (result == PANO_GPU_SUCCESS)
                result = filter_gradients
                    ? pano_gpu_session_dispatch_exposure_pair_filter_ratios(
                          session, error_buffer, error_buffer_size)
                    : dispatch_reference_exposure_pair_ratios(
                          session, error_buffer, error_buffer_size);
            if (result == PANO_GPU_SUCCESS)
                result = pano_gpu_session_dispatch_exposure_pair_trim(
                    session, error_buffer, error_buffer_size);
            pano_gpu_exposure_pair_reduction reduction {};
            reduction.size = sizeof(reduction);
            reduction.abi_version = PANO_GPU_ABI_VERSION;
            if (result == PANO_GPU_SUCCESS)
                result = pano_gpu_session_reduce_exposure_pair(
                    session, &reduction, error_buffer, error_buffer_size);
            if (result != PANO_GPU_SUCCESS)
                return result;
            report.rejection_reason = reduction.rejection_reason;
            report.valid_count = reduction.valid_count;
            report.inlier_count = reduction.inlier_count;
            report.geometric_count = session->resident_pair_geometric_count;
            if (reduction.rejection_reason == PANO_GPU_EXPOSURE_PAIR_ACCEPTED)
                equations.push_back(pano_gpu_exposure_equation {
                    report.left_frame_index, report.right_frame_index,
                    static_cast<double>(reduction.difference),
                    static_cast<double>(reduction.weight)});
            ++completed;
            if (progress != nullptr &&
                progress(progress_user_data, completed,
                         static_cast<uint32_t>(reports.size())) == 0)
            {
                write_error(error_buffer, error_buffer_size,
                            "D3D12 exposure-graph reduction cancelled");
                return PANO_GPU_CANCELLED;
            }
        }
        session->exposure_pair_reports.swap(reports);
        session->exposure_equations.swap(equations);
        session->exposure_solve_equations.clear();
        session->exposure_solve_graph_ready = false;
        session->exposure_log_gains.clear();
        session->exposure_solved = false;
        session->exposure_global_gains.clear();
        session->exposure_gains_uploaded = false;
        return PANO_GPU_SUCCESS;
    }
    catch (const std::bad_alloc &)
    {
        write_error(error_buffer, error_buffer_size, "cannot allocate D3D12 exposure-graph reduction storage");
        return PANO_GPU_OUT_OF_MEMORY;
    }
    catch (...)
    {
        write_error(error_buffer, error_buffer_size, "unexpected D3D12 exposure-graph reduction failure");
        return PANO_GPU_UNAVAILABLE;
    }
}

pano_gpu_result pano_gpu_session_reduce_exposure_graph(
    pano_gpu_session *const session, const pano_gpu_exposure_pair_request *const request_template,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    return reduce_exposure_graph(
        session, request_template, true, nullptr, nullptr, error_buffer,
        error_buffer_size);
}

pano_gpu_result pano_gpu_session_reduce_reference_exposure_graph(
    pano_gpu_session *const session, const pano_gpu_exposure_pair_request *const request_template,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    return reduce_exposure_graph(
        session, request_template, false, nullptr, nullptr, error_buffer,
        error_buffer_size);
}

pano_gpu_result pano_gpu_session_reduce_reference_exposure_graph_progress(
    pano_gpu_session *const session,
    const pano_gpu_exposure_pair_request *const request_template,
    pano_gpu_progress_callback progress, void *const progress_user_data,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    return reduce_exposure_graph(
        session, request_template, false, progress, progress_user_data,
        error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_session_query_exposure_graph(
    const pano_gpu_session *const session, pano_gpu_exposure_graph_diagnostics *const diagnostics,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (diagnostics == nullptr || diagnostics->size != sizeof(*diagnostics) ||
        diagnostics->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-graph diagnostics");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    diagnostics->pair_capacity = 0;
    diagnostics->pair_report_count = 0;
    diagnostics->equation_count = 0;
    diagnostics->solve_equation_count = 0;
    if (session == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 exposure-graph session is null");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    diagnostics->pair_capacity = session->exposure_pair_capacity;
    diagnostics->pair_report_count = static_cast<uint32_t>(session->exposure_pair_reports.size());
    diagnostics->equation_count = static_cast<uint32_t>(session->exposure_equations.size());
    diagnostics->solve_equation_count =
        static_cast<uint32_t>(session->exposure_solve_equations.size());
    return PANO_GPU_SUCCESS;
}

void pano_gpu_session_clear_exposure_graph(pano_gpu_session *const session) noexcept
{
    if (session == nullptr)
        return;
    std::vector<pano_gpu_exposure_equation> {}.swap(session->exposure_equations);
    std::vector<pano_gpu_exposure_equation> {}.swap(session->exposure_solve_equations);
    session->exposure_solve_graph_ready = false;
    std::vector<float> {}.swap(session->exposure_log_gains);
    session->exposure_solved = false;
    std::vector<float> {}.swap(session->exposure_global_gains);
    session->exposure_gains_uploaded = false;
    std::vector<pano_gpu_exposure_pair_report> {}.swap(session->exposure_pair_reports);
    session->exposure_pair_capacity = 0;
}

pano_gpu_result pano_gpu_session_prepare_exposure_pair_scratch(
    pano_gpu_session *const session, const pano_gpu_exposure_pair_request *const request,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION ||
        request->first_frame_index >= session->frame_count ||
        request->second_frame_index >= session->frame_count ||
        request->first_frame_index == request->second_frame_index || request->sample_width == 0 ||
        request->sample_height == 0 || request->reserved != 0)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 resident exposure-pair scratch request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    uint64_t sample_count = 0;
    if (!checked_multiply(request->sample_width, request->sample_height, &sample_count) ||
        sample_count > std::numeric_limits<uint32_t>::max())
    {
        write_error(error_buffer, error_buffer_size, "D3D12 resident exposure-pair sample count overflows");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    uint64_t sortable_capacity = 1;
    while (sortable_capacity < sample_count)
    {
        if (sortable_capacity > std::numeric_limits<uint32_t>::max() / 2ULL)
        {
            write_error(error_buffer, error_buffer_size, "D3D12 resident exposure-pair sort capacity overflows");
            return PANO_GPU_INVALID_ARGUMENT;
        }
        sortable_capacity *= 2;
    }
    const std::array<uint64_t, 15> resource_bytes {
        16 * sample_count, 4 * sample_count, 12 * sample_count, 12 * sample_count,
        8 * sample_count, 4 * sample_count, 8 * sample_count, 8 * sortable_capacity,
        2 * sizeof(float), 4 * sample_count, 4 * sortable_capacity, 2 * sizeof(float),
        8 * sizeof(uint32_t), 4 * sortable_capacity, 8 * sizeof(uint32_t)};
    uint64_t device_bytes = 0;
    for (const uint64_t bytes : resource_bytes)
    {
        if (bytes > std::numeric_limits<uint64_t>::max() - device_bytes)
        {
            write_error(error_buffer, error_buffer_size, "D3D12 resident exposure-pair byte count overflows");
            return PANO_GPU_INVALID_ARGUMENT;
        }
        device_bytes += bytes;
    }
#if !defined(_WIN32)
    (void)device_bytes;
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 resident exposure-pair device is unavailable");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 15> resources;
    for (size_t index = 0; index < resources.size(); ++index)
    {
        D3D12_RESOURCE_DESC description {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = resource_bytes[index];
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
#if defined(PANO_GPU_TEST_HOOKS)
        if (fail_next_allocation.exchange(false, std::memory_order_relaxed))
        {
            write_error(error_buffer, error_buffer_size, "cannot allocate D3D12 resident exposure-pair scratch");
            return PANO_GPU_OUT_OF_MEMORY;
        }
#endif
        if (FAILED(session->device_core->d3d_device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr, IID_PPV_ARGS(&resources[index]))))
        {
            write_error(error_buffer, error_buffer_size, "cannot allocate D3D12 resident exposure-pair scratch");
            return PANO_GPU_OUT_OF_MEMORY;
        }
    }
    session->resident_pair_scratch = std::move(resources);
    session->resident_pair_sample_count = sample_count;
    session->resident_pair_sortable_capacity = sortable_capacity;
    session->resident_pair_device_bytes = device_bytes;
    session->resident_pair_projected_sampled = false;
    session->resident_pair_classified = false;
    session->resident_pair_gradient_limits_ready = false;
    session->resident_pair_ratios_ready = false;
    session->resident_pair_trimmed = false;
    session->resident_pair_reduced = false;
    session->resident_pair_geometric_count = 0;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_session_query_exposure_pair_scratch(
    const pano_gpu_session *const session,
    pano_gpu_exposure_pair_scratch_diagnostics *const diagnostics, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || diagnostics == nullptr || diagnostics->size != sizeof(*diagnostics) ||
        diagnostics->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 resident exposure-pair diagnostics");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    diagnostics->sample_count = session->resident_pair_sample_count;
    diagnostics->sortable_capacity = session->resident_pair_sortable_capacity;
    diagnostics->device_bytes = session->resident_pair_device_bytes;
    diagnostics->readback_bytes = 0;
    diagnostics->resource_count = session->resident_pair_device_bytes == 0 ? 0 : 15;
    diagnostics->reserved = 0;
    return PANO_GPU_SUCCESS;
}

void pano_gpu_session_clear_exposure_pair_scratch(pano_gpu_session *const session) noexcept
{
    if (session == nullptr)
        return;
#if defined(_WIN32)
    session->resident_pair_scratch = {};
#endif
    session->resident_pair_sample_count = 0;
    session->resident_pair_sortable_capacity = 0;
    session->resident_pair_device_bytes = 0;
    session->resident_pair_projected_sampled = false;
    session->resident_pair_classified = false;
    session->resident_pair_gradient_limits_ready = false;
    session->resident_pair_ratios_ready = false;
    session->resident_pair_trimmed = false;
    session->resident_pair_reduced = false;
    session->resident_pair_geometric_count = 0;
}

pano_gpu_result pano_gpu_session_dispatch_exposure_pair_projection_samples(
    pano_gpu_session *const session, const pano_gpu_exposure_pair_request *const request,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    uint8_t validation_storage = 0;
    pano_gpu_exposure_pair_layout layout {};
    layout.size = sizeof(layout);
    layout.abi_version = PANO_GPU_ABI_VERSION;
    uint64_t sample_count = 0;
    uint64_t coordinate_bytes = 0;
    if (request == nullptr ||
        !checked_multiply(request->sample_width, request->sample_height, &sample_count) ||
        !checked_multiply(sample_count, 4 * sizeof(float), &coordinate_bytes))
    {
        write_error(error_buffer, error_buffer_size, "invalid resident D3D12 exposure-pair dispatch");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    const pano_gpu_result validation = pano_gpu_test_validate_exposure_pair_request(
        session, request, &validation_storage, coordinate_bytes, &validation_storage, sample_count,
        &layout, error_buffer, error_buffer_size);
    if (validation != PANO_GPU_SUCCESS)
        return validation;
    if (session->resident_pair_sample_count != sample_count ||
        session->resident_pair_device_bytes == 0 || session->resident_pair_projected_sampled)
    {
        write_error(error_buffer, error_buffer_size, "resident D3D12 exposure-pair scratch is not ready");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue ||
        !session->device_core->fence || !session->rotations || !session->exposure_proxies)
    {
        write_error(error_buffer, error_buffer_size, "resident D3D12 exposure-pair inputs are not ready");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const auto make_pipeline = [device](
                                   const D3D12_ROOT_SIGNATURE_DESC &description,
                                   const void *const shader, const size_t shader_bytes,
                                   Microsoft::WRL::ComPtr<ID3D12RootSignature> *const root,
                                   Microsoft::WRL::ComPtr<ID3D12PipelineState> *const pipeline) {
        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        if (FAILED(D3D12SerializeRootSignature(
                &description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
            FAILED(device->CreateRootSignature(
                0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                IID_PPV_ARGS(root->ReleaseAndGetAddressOf()))))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
        pipeline_description.pRootSignature = root->Get();
        pipeline_description.CS = {shader, shader_bytes};
        return SUCCEEDED(device->CreateComputePipelineState(
            &pipeline_description, IID_PPV_ARGS(pipeline->ReleaseAndGetAddressOf())));
    };
    D3D12_DESCRIPTOR_RANGE projection_ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 1}};
    D3D12_ROOT_PARAMETER projection_parameters[2] {};
    projection_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    projection_parameters[0].Constants = {0, 0, 9};
    projection_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    projection_parameters[1].DescriptorTable = {2, projection_ranges};
    const D3D12_ROOT_SIGNATURE_DESC projection_root_description {
        2, projection_parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    D3D12_DESCRIPTOR_RANGE sample_ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 2}};
    D3D12_ROOT_PARAMETER sample_parameters[2] {};
    sample_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    sample_parameters[0].Constants = {0, 0, 5};
    sample_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    sample_parameters[1].DescriptorTable = {2, sample_ranges};
    const D3D12_ROOT_SIGNATURE_DESC sample_root_description {
        2, sample_parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> projection_root, sample_root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> projection_pipeline, sample_pipeline;
    if (!make_pipeline(
            projection_root_description, pano_gpu_exposure_pair_projection_shader,
            sizeof(pano_gpu_exposure_pair_projection_shader), &projection_root,
            &projection_pipeline) ||
        !make_pipeline(
            sample_root_description, pano_gpu_exposure_pair_samples_shader,
            sizeof(pano_gpu_exposure_pair_samples_shader), &sample_root, &sample_pipeline))
    {
        write_error(error_buffer, error_buffer_size, "cannot create resident D3D12 exposure-pair pipelines");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_HEAP_DESC projection_heap_description {};
    projection_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    projection_heap_description.NumDescriptors = 3;
    projection_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D12_DESCRIPTOR_HEAP_DESC sample_heap_description = projection_heap_description;
    sample_heap_description.NumDescriptors = 4;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> projection_heap, sample_heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&projection_heap_description, IID_PPV_ARGS(&projection_heap))) ||
        FAILED(device->CreateDescriptorHeap(&sample_heap_description, IID_PPV_ARGS(&sample_heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), projection_pipeline.Get(),
            IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create resident D3D12 exposure-pair dispatch");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto descriptor = projection_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC rotation_srv {};
    rotation_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    rotation_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    rotation_srv.Format = DXGI_FORMAT_R32_FLOAT;
    rotation_srv.Buffer.NumElements = static_cast<UINT>(session->rotation_bytes / sizeof(float));
    device->CreateShaderResourceView(session->rotations.Get(), &rotation_srv, descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC coordinates_uav {};
    coordinates_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    coordinates_uav.Format = DXGI_FORMAT_UNKNOWN;
    coordinates_uav.Buffer.NumElements = static_cast<UINT>(sample_count);
    coordinates_uav.Buffer.StructureByteStride = 4 * sizeof(float);
    device->CreateUnorderedAccessView(
        session->resident_pair_scratch[0].Get(), nullptr, &coordinates_uav, descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC overlap_uav {};
    overlap_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    overlap_uav.Format = DXGI_FORMAT_UNKNOWN;
    overlap_uav.Buffer.NumElements = static_cast<UINT>(sample_count);
    overlap_uav.Buffer.StructureByteStride = sizeof(uint32_t);
    device->CreateUnorderedAccessView(
        session->resident_pair_scratch[1].Get(), nullptr, &overlap_uav, descriptor);
    pano_gpu_exposure_proxy_request proxy_request {};
    proxy_request.size = sizeof(proxy_request);
    proxy_request.abi_version = PANO_GPU_ABI_VERSION;
    proxy_request.frame_count = session->frame_count;
    proxy_request.source_width = session->source_width;
    proxy_request.source_height = session->source_height;
    pano_gpu_exposure_proxy_layout proxy_layout {};
    proxy_layout.size = sizeof(proxy_layout);
    proxy_layout.abi_version = PANO_GPU_ABI_VERSION;
    if (pano_gpu_test_plan_exposure_proxies(
            session, &proxy_request, &proxy_layout, error_buffer, error_buffer_size) != PANO_GPU_SUCCESS)
        return PANO_GPU_INVALID_ARGUMENT;
    uint32_t projection_constants[9] {
        request->first_frame_index, request->second_frame_index, request->sample_width,
        request->sample_height, proxy_layout.proxy_width, proxy_layout.proxy_height};
    std::memcpy(&projection_constants[6], &request->latitude_span_degrees, sizeof(float));
    std::memcpy(&projection_constants[7], &request->horizontal_fov_degrees, sizeof(float));
    std::memcpy(&projection_constants[8], &request->vertical_fov_degrees, sizeof(float));
    ID3D12DescriptorHeap *projection_heaps[] {projection_heap.Get()};
    list->SetDescriptorHeaps(1, projection_heaps);
    list->SetComputeRootSignature(projection_root.Get());
    list->SetComputeRoot32BitConstants(0, 9, projection_constants, 0);
    list->SetComputeRootDescriptorTable(1, projection_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(static_cast<UINT>((sample_count + 63) / 64), 1, 1);
    D3D12_RESOURCE_BARRIER coordinate_barrier {};
    coordinate_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    coordinate_barrier.Transition = {
        session->resident_pair_scratch[0].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &coordinate_barrier);
    descriptor = sample_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC proxy_srv {};
    proxy_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    proxy_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    proxy_srv.Format = DXGI_FORMAT_R32_FLOAT;
    proxy_srv.Buffer.NumElements = static_cast<UINT>(session->exposure_proxy_bytes / sizeof(float));
    device->CreateShaderResourceView(session->exposure_proxies.Get(), &proxy_srv, descriptor);
    descriptor.ptr += increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC coordinate_srv {};
    coordinate_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    coordinate_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    coordinate_srv.Format = DXGI_FORMAT_UNKNOWN;
    coordinate_srv.Buffer.NumElements = static_cast<UINT>(sample_count);
    coordinate_srv.Buffer.StructureByteStride = 4 * sizeof(float);
    device->CreateShaderResourceView(
        session->resident_pair_scratch[0].Get(), &coordinate_srv, descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC sample_uav {};
    sample_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    sample_uav.Format = DXGI_FORMAT_UNKNOWN;
    sample_uav.Buffer.NumElements = static_cast<UINT>(sample_count);
    sample_uav.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateUnorderedAccessView(
        session->resident_pair_scratch[2].Get(), nullptr, &sample_uav, descriptor);
    descriptor.ptr += increment;
    device->CreateUnorderedAccessView(
        session->resident_pair_scratch[3].Get(), nullptr, &sample_uav, descriptor);
    const uint32_t sample_constants[5] {
        request->first_frame_index, request->second_frame_index, static_cast<uint32_t>(sample_count),
        proxy_layout.proxy_width, proxy_layout.proxy_height};
    ID3D12DescriptorHeap *sample_heaps[] {sample_heap.Get()};
    list->SetPipelineState(sample_pipeline.Get());
    list->SetDescriptorHeaps(1, sample_heaps);
    list->SetComputeRootSignature(sample_root.Get());
    list->SetComputeRoot32BitConstants(0, 5, sample_constants, 0);
    list->SetComputeRootDescriptorTable(1, sample_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(static_cast<UINT>((sample_count + 63) / 64), 1, 1);
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close resident D3D12 exposure-pair dispatch");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(
            session->device_core.get(), fence_value, error_buffer, error_buffer_size,
            "resident D3D12 exposure-pair dispatch timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    session->resident_pair_projected_sampled = true;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_session_dispatch_exposure_pair_classification(
    pano_gpu_session *const session, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || !session->resident_pair_projected_sampled ||
        session->resident_pair_classified || session->resident_pair_sample_count == 0)
    {
        write_error(error_buffer, error_buffer_size, "resident D3D12 exposure-pair samples are not ready");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 3}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 2};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    const D3D12_ROOT_SIGNATURE_DESC root_description {
        2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create resident exposure classification root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root.Get();
    pipeline_description.CS = {
        pano_gpu_exposure_pair_classify_resident_shader,
        sizeof(pano_gpu_exposure_pair_classify_resident_shader)};
    if (FAILED(device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create resident exposure classification pipeline");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 6;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create resident exposure classification dispatch");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto descriptor = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sample_srv {};
    sample_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sample_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sample_srv.Format = DXGI_FORMAT_UNKNOWN;
    sample_srv.Buffer.NumElements = static_cast<UINT>(session->resident_pair_sample_count);
    sample_srv.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateShaderResourceView(session->resident_pair_scratch[2].Get(), &sample_srv, descriptor);
    descriptor.ptr += increment;
    device->CreateShaderResourceView(session->resident_pair_scratch[3].Get(), &sample_srv, descriptor);
    descriptor.ptr += increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC overlap_srv {};
    overlap_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    overlap_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    overlap_srv.Format = DXGI_FORMAT_R32_UINT;
    overlap_srv.Buffer.NumElements = static_cast<UINT>(session->resident_pair_sample_count);
    device->CreateShaderResourceView(session->resident_pair_scratch[1].Get(), &overlap_srv, descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC luminance_uav {};
    luminance_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    luminance_uav.Format = DXGI_FORMAT_UNKNOWN;
    luminance_uav.Buffer.NumElements = static_cast<UINT>(session->resident_pair_sample_count);
    luminance_uav.Buffer.StructureByteStride = 2 * sizeof(float);
    device->CreateUnorderedAccessView(
        session->resident_pair_scratch[4].Get(), nullptr, &luminance_uav, descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC accepted_uav {};
    accepted_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    accepted_uav.Format = DXGI_FORMAT_UNKNOWN;
    accepted_uav.Buffer.NumElements = static_cast<UINT>(session->resident_pair_sample_count);
    accepted_uav.Buffer.StructureByteStride = sizeof(uint32_t);
    device->CreateUnorderedAccessView(
        session->resident_pair_scratch[5].Get(), nullptr, &accepted_uav, descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC counts_uav {};
    counts_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    counts_uav.Format = DXGI_FORMAT_UNKNOWN;
    counts_uav.Buffer.NumElements = 8;
    counts_uav.Buffer.StructureByteStride = sizeof(uint32_t);
    device->CreateUnorderedAccessView(
        session->resident_pair_scratch[12].Get(), nullptr, &counts_uav, descriptor);
    std::array<D3D12_RESOURCE_BARRIER, 3> barriers {};
    for (size_t index = 0; index < barriers.size(); ++index)
    {
        barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[index].Transition = {
            session->resident_pair_scratch[index + 1].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    }
    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    const uint32_t constants[2] {
        static_cast<uint32_t>(session->resident_pair_sample_count), session->transfer_function};
    ID3D12DescriptorHeap *heaps[] {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    D3D12_GPU_DESCRIPTOR_HANDLE counts_gpu = heap->GetGPUDescriptorHandleForHeapStart();
    counts_gpu.ptr += 5ULL * increment;
    const UINT clear_values[4] {};
    list->ClearUnorderedAccessViewUint(
        counts_gpu, descriptor, session->resident_pair_scratch[12].Get(), clear_values, 0, nullptr);
    D3D12_RESOURCE_BARRIER counts_barrier {};
    counts_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    counts_barrier.UAV.pResource = session->resident_pair_scratch[12].Get();
    list->ResourceBarrier(1, &counts_barrier);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRoot32BitConstants(0, 2, constants, 0);
    list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(static_cast<UINT>((session->resident_pair_sample_count + 63) / 64), 1, 1);
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close resident exposure classification dispatch");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(
            session->device_core.get(), fence_value, error_buffer, error_buffer_size,
            "resident exposure classification timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    session->resident_pair_classified = true;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_validate_source_upload(
    const pano_gpu_session *const session, const pano_gpu_source_upload *const upload,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || upload == nullptr || upload->size != sizeof(*upload) ||
        upload->abi_version != PANO_GPU_ABI_VERSION || upload->data == nullptr ||
        upload->frame_index >= session->frame_count ||
        upload->source_sample_type != session->source_sample_type ||
        upload->source_row_stride_bytes != session->source_row_stride_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 source upload");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    uint64_t expected_bytes = 0;
    if (!checked_multiply(session->source_height, session->source_row_stride_bytes, &expected_bytes) ||
        upload->data_bytes != expected_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 source upload byte count");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_session_allocate_upload_slot(
    pano_gpu_session *const session, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || !session->device_core || session->source_bytes == 0)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 session upload-slot request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    uint64_t upload_slot_bytes = 0;
    if (!checked_multiply(session->source_height, session->source_row_stride_bytes, &upload_slot_bytes) ||
        !align_resource(upload_slot_bytes, &upload_slot_bytes))
    {
        write_error(error_buffer, error_buffer_size, "D3D12 upload-slot size overflows");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
#if defined(PANO_GPU_TEST_HOOKS)
    if (fail_next_upload_slot_allocation.exchange(false, std::memory_order_relaxed))
    {
        write_error(error_buffer, error_buffer_size, "injected D3D12 upload-slot allocation failure");
        return PANO_GPU_OUT_OF_MEMORY;
    }
#endif
    if (session->upload_slot)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 upload slot is already allocated");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = upload_slot_bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_slot;
    if (FAILED(session->device_core->d3d_device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&upload_slot))))
    {
        write_error(error_buffer, error_buffer_size, "cannot allocate D3D12 upload slot");
        return PANO_GPU_UNAVAILABLE;
    }
    void *mapped = nullptr;
    if (FAILED(upload_slot->Map(0, nullptr, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 upload slot");
        return PANO_GPU_UNAVAILABLE;
    }
    session->upload_slot = upload_slot;
    session->mapped_upload_slot = mapped;
    session->upload_slot_bytes = upload_slot_bytes;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_session_allocate_second_upload_slot(
    pano_gpu_session *const session, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || !session->device_core)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 second upload-slot request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
#if defined(PANO_GPU_TEST_HOOKS)
    if (fail_next_second_upload_slot_allocation.exchange(false, std::memory_order_relaxed))
    {
        write_error(error_buffer, error_buffer_size, "injected D3D12 second upload-slot allocation failure");
        return PANO_GPU_OUT_OF_MEMORY;
    }
#endif
    if (!session->upload_slot || session->second_upload_slot)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 second upload-slot request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = session->upload_slot_bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_slot;
    if (FAILED(session->device_core->d3d_device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&upload_slot))))
    {
        write_error(error_buffer, error_buffer_size, "cannot allocate D3D12 second upload slot");
        return PANO_GPU_UNAVAILABLE;
    }
    void *mapped = nullptr;
    if (FAILED(upload_slot->Map(0, nullptr, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 second upload slot");
        return PANO_GPU_UNAVAILABLE;
    }
    session->second_upload_slot = upload_slot;
    session->mapped_second_upload_slot = mapped;
    session->second_upload_slot_bytes = session->upload_slot_bytes;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result upload_frame_impl(
    pano_gpu_session *const session, const pano_gpu_source_upload *const upload,
    const pano_gpu_cancellation_token *const token, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    const pano_gpu_result validation_result =
        pano_gpu_validate_source_upload(session, upload, error_buffer, error_buffer_size);
    if (validation_result != PANO_GPU_SUCCESS)
        return validation_result;
    uint64_t uploaded_bytes = 0;
    if (session->upload_count == std::numeric_limits<uint32_t>::max() ||
        !checked_add(session->uploaded_bytes, upload->data_bytes, &uploaded_bytes))
    {
        write_error(error_buffer, error_buffer_size, "D3D12 source upload accounting overflows");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    (void)token;
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    const bool use_second_slot = session->second_upload_slot && (session->upload_count % 2U) != 0;
    const upload_slot_selection selected_slot = use_second_slot
        ? upload_slot_selection {
              session->second_upload_slot.Get(), session->mapped_second_upload_slot,
              &session->second_upload_slot_fence}
        : upload_slot_selection {
              session->upload_slot.Get(), session->mapped_upload_slot, &session->first_upload_slot_fence};
    if (!session->source || selected_slot.resource == nullptr || selected_slot.mapped == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 source upload storage is not allocated");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    if (token != nullptr && token->cancelled.load(std::memory_order_relaxed))
        return PANO_GPU_CANCELLED;
    const bool reusing_slot = *selected_slot.last_fence != 0;
    if (reusing_slot && wait_for_fence(
            session->device_core.get(), *selected_slot.last_fence, error_buffer, error_buffer_size,
            "D3D12 source upload fence timed out") != PANO_GPU_SUCCESS)
    {
        return PANO_GPU_UNAVAILABLE;
    }
    session->last_completed_upload_fence =
        std::max(session->last_completed_upload_fence, *selected_slot.last_fence);
#if defined(PANO_GPU_TEST_HOOKS)
    if (reusing_slot && token != nullptr &&
        cancel_after_next_upload_slot_wait.exchange(false, std::memory_order_relaxed))
    {
        const_cast<pano_gpu_cancellation_token *>(token)->cancelled.store(true, std::memory_order_relaxed);
    }
#endif
    if (token != nullptr && token->cancelled.load(std::memory_order_relaxed))
        return PANO_GPU_CANCELLED;
    std::memcpy(selected_slot.mapped, upload->data, static_cast<size_t>(upload->data_bytes));
    if (token != nullptr && token->cancelled.load(std::memory_order_relaxed))
        return PANO_GPU_CANCELLED;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(session->device_core->d3d_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(session->device_core->d3d_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 source upload commands");
        return PANO_GPU_UNAVAILABLE;
    }
    const uint64_t destination_offset = static_cast<uint64_t>(upload->frame_index) * session->source_frame_bytes;
    if (session->source_is_shader_readable)
    {
        D3D12_RESOURCE_BARRIER to_copy_destination {};
        to_copy_destination.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy_destination.Transition.pResource = session->source.Get();
        to_copy_destination.Transition.StateBefore =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE;
        to_copy_destination.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        to_copy_destination.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &to_copy_destination);
    }
    list->CopyBufferRegion(session->source.Get(), destination_offset, selected_slot.resource, 0, upload->data_bytes);
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 source upload command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] = {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    session->source_is_shader_readable = false;
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(PANO_GPU_TEST_HOOKS)
    const bool injected_signal_failure =
        fail_next_fence_signal.exchange(false, std::memory_order_relaxed);
#else
    const bool injected_signal_failure = false;
#endif
    if (injected_signal_failure ||
        FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)))
    {
        write_error(error_buffer, error_buffer_size, "cannot signal D3D12 source upload fence");
        return PANO_GPU_UNAVAILABLE;
    }
    *selected_slot.last_fence = fence_value;
    session->frame_upload_fences[upload->frame_index] = fence_value;
    session->upload_count += 1;
    session->uploaded_bytes = uploaded_bytes;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_session_upload_frame(
    pano_gpu_session *const session, const pano_gpu_source_upload *const upload,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    return upload_frame_impl(session, upload, nullptr, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_session_upload_frame_zero(
    pano_gpu_session *const session, const pano_gpu_source_upload *const upload,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (upload == nullptr || upload->frame_index != 0)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 first upload requires frame zero");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return pano_gpu_session_upload_frame(session, upload, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_session_upload_frame_cancellable(
    pano_gpu_session *const session, const pano_gpu_source_upload *const upload,
    const pano_gpu_cancellation_token *const token, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return upload_frame_impl(session, upload, token, error_buffer, error_buffer_size);
}

pano_gpu_result finish_uploads_impl(
    pano_gpu_session *const session, const pano_gpu_cancellation_token *const token,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || !session->device_core)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 session handle");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    (void)token;
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (token != nullptr && token->cancelled.load(std::memory_order_relaxed))
        return PANO_GPU_CANCELLED;
    if (session->first_upload_slot_fence != 0 &&
        wait_for_fence(
            session->device_core.get(), session->first_upload_slot_fence, error_buffer, error_buffer_size,
            "D3D12 source upload fence timed out") != PANO_GPU_SUCCESS)
    {
        return PANO_GPU_UNAVAILABLE;
    }
#if defined(PANO_GPU_TEST_HOOKS)
    if (token != nullptr &&
        cancel_after_next_upload_finish_wait.exchange(false, std::memory_order_relaxed))
    {
        const_cast<pano_gpu_cancellation_token *>(token)->cancelled.store(true, std::memory_order_relaxed);
    }
#endif
    if (token != nullptr && token->cancelled.load(std::memory_order_relaxed))
        return PANO_GPU_CANCELLED;
    session->last_completed_upload_fence =
        std::max(session->last_completed_upload_fence, session->first_upload_slot_fence);
    if (token != nullptr && token->cancelled.load(std::memory_order_relaxed))
        return PANO_GPU_CANCELLED;
    if (session->second_upload_slot_fence != 0 &&
        session->second_upload_slot_fence != session->first_upload_slot_fence &&
        wait_for_fence(
            session->device_core.get(), session->second_upload_slot_fence, error_buffer, error_buffer_size,
            "D3D12 source upload fence timed out") != PANO_GPU_SUCCESS)
    {
        return PANO_GPU_UNAVAILABLE;
    }
    if (token != nullptr && token->cancelled.load(std::memory_order_relaxed))
        return PANO_GPU_CANCELLED;
    session->last_completed_upload_fence =
        std::max(session->last_completed_upload_fence, session->second_upload_slot_fence);
    bool has_uploaded_frame = false;
    for (const uint64_t frame_fence : session->frame_upload_fences)
        has_uploaded_frame = has_uploaded_frame || frame_fence != 0;
    if (!has_uploaded_frame || session->source_is_shader_readable)
        return PANO_GPU_SUCCESS;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(session->device_core->d3d_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(session->device_core->d3d_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 source-ready transition commands");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_RESOURCE_BARRIER to_shader_read {};
    to_shader_read.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_shader_read.Transition.pResource = session->source.Get();
    to_shader_read.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_shader_read.Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE;
    to_shader_read.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &to_shader_read);
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 source-ready transition command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] = {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)))
    {
        write_error(error_buffer, error_buffer_size, "cannot signal D3D12 source-ready transition fence");
        return PANO_GPU_UNAVAILABLE;
    }
    const pano_gpu_result transition_result = wait_for_fence(
        session->device_core.get(), fence_value, error_buffer, error_buffer_size,
        "D3D12 source-ready transition fence timed out");
    if (transition_result != PANO_GPU_SUCCESS)
        return transition_result;
    session->source_is_shader_readable = true;
    session->last_completed_upload_fence = std::max(session->last_completed_upload_fence, fence_value);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_session_finish_uploads(
    pano_gpu_session *const session, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    return finish_uploads_impl(session, nullptr, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_session_finish_uploads_cancellable(
    pano_gpu_session *const session, const pano_gpu_cancellation_token *const token,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    return finish_uploads_impl(session, token, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_validate_output_create_options(
    const pano_gpu_session *const session, const pano_gpu_output_create_options *const options,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || options == nullptr || options->size != sizeof(*options) ||
        options->abi_version != PANO_GPU_ABI_VERSION || options->output_width == 0 ||
        options->output_height == 0 || options->output_sample_bytes == 0 ||
        options->output_workspace_bytes == 0 || options->descriptor_count != session->frame_count + 4)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 output-job options");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    if (options->output_band_rows != 0 &&
        (options->output_band_rows < 32 || options->output_band_rows % 32 != 0 ||
         options->output_band_rows > options->output_height))
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 output-band rows");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_output_create_empty(
    pano_gpu_session *const session, const pano_gpu_output_create_options *const options,
    pano_gpu_output **const output, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (output == nullptr)
        return PANO_GPU_INVALID_ARGUMENT;
    *output = nullptr;
    if (pano_gpu_validate_output_create_options(session, options, error_buffer, error_buffer_size) != PANO_GPU_SUCCESS)
        return PANO_GPU_INVALID_ARGUMENT;
    const uint32_t storage_rows = options->output_band_rows == 0 ? options->output_height : options->output_band_rows;
    uint64_t linear_bytes = 0;
    uint64_t coverage_bytes = 0;
    if (!checked_multiply(options->output_width, storage_rows, &linear_bytes) ||
        !checked_multiply(linear_bytes, 3 * sizeof(float), &linear_bytes) ||
        !align_resource(linear_bytes, &linear_bytes) ||
        !checked_multiply(options->output_width, storage_rows, &coverage_bytes) ||
        !align_resource(coverage_bytes, &coverage_bytes))
    {
        write_error(error_buffer, error_buffer_size, "D3D12 output linear allocation size overflows");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    try
    {
#if defined(PANO_GPU_TEST_HOOKS)
        if (fail_next_output_allocation.exchange(false, std::memory_order_relaxed))
            throw std::bad_alloc {};
#endif
        pano_gpu_output *const created = new pano_gpu_output;
        session->reference_count.fetch_add(1, std::memory_order_relaxed);
        created->session = session;
        created->output_width = options->output_width;
        created->output_height = options->output_height;
        created->output_band_rows = options->output_band_rows;
        created->band_row_count = options->output_band_rows;
        created->planned_linear_bytes = linear_bytes;
        created->planned_coverage_bytes = coverage_bytes;
        live_output_count.fetch_add(1, std::memory_order_relaxed);
        created->count_registered = true;
        *output = created;
        return PANO_GPU_SUCCESS;
    }
    catch (const std::bad_alloc &)
    {
        write_error(error_buffer, error_buffer_size, "cannot allocate D3D12 output handle");
        return PANO_GPU_OUT_OF_MEMORY;
    }
}

pano_gpu_result pano_gpu_output_allocate_linear(
    pano_gpu_output *const output, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (output == nullptr || output->session == nullptr || !output->session->device_core ||
        output->planned_linear_bytes == 0)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 output handle");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (output->linear)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 output linear storage is already allocated");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC buffer {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = output->planned_linear_bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const HRESULT result = output->session->device_core->d3d_device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&output->linear));
    if (FAILED(result))
    {
        write_hresult_error(error_buffer, error_buffer_size, "cannot allocate D3D12 output linear storage", result);
        return PANO_GPU_UNAVAILABLE;
    }
    output->linear_bytes = output->planned_linear_bytes;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_output_allocate_coverage(
    pano_gpu_output *const output, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (output == nullptr || output->session == nullptr || !output->session->device_core ||
        output->planned_coverage_bytes == 0)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 output handle");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (output->coverage)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 output coverage storage is already allocated");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC buffer {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = output->planned_coverage_bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const HRESULT result = output->session->device_core->d3d_device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&output->coverage));
    if (FAILED(result))
    {
        write_hresult_error(error_buffer, error_buffer_size, "cannot allocate D3D12 output coverage storage", result);
        return PANO_GPU_UNAVAILABLE;
    }
    output->coverage_bytes = output->planned_coverage_bytes;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_output_query_diagnostics(
    const pano_gpu_output *const output, pano_gpu_output_diagnostics *const diagnostics,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (diagnostics == nullptr || diagnostics->size != sizeof(*diagnostics) ||
        diagnostics->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 output diagnostics");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    diagnostics->is_banded = 0;
    diagnostics->output_band_rows = 0;
    diagnostics->band_row_start = 0;
    diagnostics->band_row_count = 0;
    diagnostics->planned_linear_bytes = 0;
    diagnostics->linear_bytes = 0;
    diagnostics->planned_coverage_bytes = 0;
    diagnostics->coverage_bytes = 0;
    if (output == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 output handle");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    diagnostics->is_banded = output->output_band_rows != 0;
    diagnostics->output_band_rows = output->output_band_rows;
    diagnostics->band_row_start = output->band_row_start;
    diagnostics->band_row_count = output->band_row_count;
    diagnostics->planned_linear_bytes = output->planned_linear_bytes;
    diagnostics->linear_bytes = output->linear_bytes;
    diagnostics->planned_coverage_bytes = output->planned_coverage_bytes;
    diagnostics->coverage_bytes = output->coverage_bytes;
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_output_prepare_auto_contrast_histogram(
    pano_gpu_output *const output, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    pano_gpu_histogram_request request {};
    request.size = sizeof(request);
    request.abi_version = PANO_GPU_ABI_VERSION;
    request.output_width = output == nullptr ? 0 : output->output_width;
    request.output_height = output == nullptr ? 0 : output->output_height;
    pano_gpu_histogram_layout layout {};
    layout.size = sizeof(layout);
    layout.abi_version = PANO_GPU_ABI_VERSION;
    if (output == nullptr || output->histogram_bytes != 0 ||
        pano_gpu_plan_auto_contrast_histogram(
            &request, &layout, error_buffer, error_buffer_size) != PANO_GPU_SUCCESS)
        return PANO_GPU_INVALID_ARGUMENT;
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = output->session->device_core->d3d_device.Get();
    D3D12_HEAP_PROPERTIES default_heap {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = layout.histogram_bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D12Resource> histogram;
    if (FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&histogram))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 1;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.Buffer.NumElements = layout.bin_count;
    uav.Buffer.StructureByteStride = layout.counter_bytes;
    const D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateUnorderedAccessView(histogram.Get(), nullptr, &uav, cpu);
    ID3D12DescriptorHeap *heaps[] {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    const UINT clear[4] {};
    list->ClearUnorderedAccessViewUint(
        heap->GetGPUDescriptorHandleForHeapStart(), cpu, histogram.Get(), clear, 0, nullptr);
    D3D12_RESOURCE_BARRIER ordering {};
    ordering.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ordering.UAV.pResource = histogram.Get();
    list->ResourceBarrier(1, &ordering);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    output->session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        output->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(output->session->device_core->queue->Signal(
            output->session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            output->session->device_core.get(), fence, error_buffer, error_buffer_size,
            "output histogram clear timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    output->histogram = std::move(histogram);
    output->histogram_bytes = layout.histogram_bytes;
    output->histogram_clear_count = 1;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_output_query_histogram_diagnostics(
    const pano_gpu_output *const output, pano_gpu_histogram_diagnostics *const diagnostics,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (diagnostics == nullptr || diagnostics->size != sizeof(*diagnostics) ||
        diagnostics->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid output histogram diagnostics");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    diagnostics->clear_count = 0;
    diagnostics->accumulated_band_count = 0;
    diagnostics->histogram_bytes = 0;
    diagnostics->accumulated_pixels = 0;
    if (output == nullptr)
        return PANO_GPU_INVALID_ARGUMENT;
    diagnostics->clear_count = output->histogram_clear_count;
    diagnostics->accumulated_band_count = output->histogram_accumulated_band_count;
    diagnostics->histogram_bytes = output->histogram_bytes;
    diagnostics->accumulated_pixels = output->histogram_accumulated_pixels;
    return PANO_GPU_SUCCESS;
}

static pano_gpu_result accumulate_auto_contrast_histogram_srgb_impl(
    pano_gpu_output *const output, const bool use_converted_input, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    const uint32_t rows = output == nullptr
        ? 0
        : (output->output_band_rows == 0 ? output->output_height : output->band_row_count);
    uint64_t pixels = 0;
    uint64_t next_population = 0;
    if (output == nullptr || output->histogram_bytes != 4096ULL * sizeof(uint32_t) ||
        (!use_converted_input && output->linear_bytes == 0) || output->coverage_bytes == 0 ||
        !checked_multiply(output->output_width, rows, &pixels) ||
        pixels > std::numeric_limits<uint32_t>::max() ||
        pixels > std::numeric_limits<uint64_t>::max() - output->histogram_accumulated_pixels ||
        (next_population = output->histogram_accumulated_pixels + pixels) >
            static_cast<uint64_t>(output->output_width) * output->output_height ||
        (output->output_band_rows == 0 && output->histogram_accumulated_band_count != 0) ||
        (output->output_band_rows != 0 &&
         output->band_row_start != output->histogram_accumulated_pixels / output->output_width))
    {
        write_error(error_buffer, error_buffer_size, "invalid output histogram accumulation state");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Resource *const input = use_converted_input
        ? output->converted_linear_srgb.Get()
        : output->linear.Get();
    const uint64_t expected_input_bytes = pixels * 3 * sizeof(float);
    if (input == nullptr || !output->coverage || !output->histogram ||
        (use_converted_input && output->converted_linear_srgb_bytes != expected_input_bytes))
        return PANO_GPU_INVALID_ARGUMENT;
    ID3D12Device *const device = output->session->device_core->d3d_device.Get();
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 1};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    const D3D12_ROOT_SIGNATURE_DESC root_description {
        2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root.Get();
    pipeline_description.CS = {pano_gpu_auto_contrast_histogram_srgb_shader,
                               sizeof(pano_gpu_auto_contrast_histogram_srgb_shader)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 3;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto descriptor = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC color_srv {};
    color_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    color_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    color_srv.Format = DXGI_FORMAT_UNKNOWN;
    color_srv.Buffer.NumElements = static_cast<UINT>(pixels);
    color_srv.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateShaderResourceView(input, &color_srv, descriptor);
    descriptor.ptr += increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC coverage_srv {};
    coverage_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    coverage_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    coverage_srv.Format = DXGI_FORMAT_R8_UINT;
    coverage_srv.Buffer.NumElements = static_cast<UINT>(pixels);
    device->CreateShaderResourceView(output->coverage.Get(), &coverage_srv, descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC histogram_uav {};
    histogram_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    histogram_uav.Format = DXGI_FORMAT_UNKNOWN;
    histogram_uav.Buffer.NumElements = 4096;
    histogram_uav.Buffer.StructureByteStride = sizeof(uint32_t);
    device->CreateUnorderedAccessView(output->histogram.Get(), nullptr, &histogram_uav, descriptor);
    std::array<D3D12_RESOURCE_BARRIER, 2> transitions {};
    ID3D12Resource *inputs[] {input, output->coverage.Get()};
    const D3D12_RESOURCE_STATES input_states[] {
        use_converted_input ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                            : D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_COPY_DEST};
    for (size_t index = 0; index < transitions.size(); ++index)
    {
        transitions[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        transitions[index].Transition = {inputs[index], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                         input_states[index],
                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    }
    list->ResourceBarrier(static_cast<UINT>(transitions.size()), transitions.data());
    ID3D12DescriptorHeap *heaps[] {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRoot32BitConstant(0, static_cast<uint32_t>(pixels), 0);
    list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((static_cast<UINT>(pixels) + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER ordering {};
    ordering.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ordering.UAV.pResource = output->histogram.Get();
    list->ResourceBarrier(1, &ordering);
    for (auto &transition : transitions)
        std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
    list->ResourceBarrier(static_cast<UINT>(transitions.size()), transitions.data());
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    output->session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        output->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(output->session->device_core->queue->Signal(
            output->session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            output->session->device_core.get(), fence, error_buffer, error_buffer_size,
            "output histogram accumulation timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    output->histogram_accumulated_pixels = next_population;
    ++output->histogram_accumulated_band_count;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_output_accumulate_auto_contrast_histogram_srgb(
    pano_gpu_output *const output, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return accumulate_auto_contrast_histogram_srgb_impl(
        output, false, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_output_accumulate_auto_contrast_histogram_converted_srgb(
    pano_gpu_output *const output, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return accumulate_auto_contrast_histogram_srgb_impl(
        output, true, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_output_select_auto_contrast_levels(
    pano_gpu_output *const output, pano_gpu_auto_contrast_levels *const levels,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    const uint64_t total_pixels = output == nullptr
        ? 0
        : static_cast<uint64_t>(output->output_width) * output->output_height;
    if (output == nullptr || levels == nullptr || levels->size != sizeof(*levels) ||
        levels->abi_version != PANO_GPU_ABI_VERSION || output->auto_contrast_levels_ready ||
        output->histogram_bytes != 4096ULL * sizeof(uint32_t) ||
        output->histogram_accumulated_pixels != total_pixels)
    {
        write_error(error_buffer, error_buffer_size, "invalid auto-contrast level selection state");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    levels->black = 0.0F;
    levels->white = 0.0F;
    levels->processed_pixels = 0;
    levels->reserved = 0;
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = output->session->device_core->d3d_device.Get();
    D3D12_HEAP_PROPERTIES default_heap {}, readback_heap {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC description {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = 2 * sizeof(float);
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D12Resource> selected, readback;
    if (FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&selected))))
        return PANO_GPU_UNAVAILABLE;
    description.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER parameter {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable = {2, ranges};
    const D3D12_ROOT_SIGNATURE_DESC root_description {
        1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root.Get();
    pipeline_description.CS = {pano_gpu_select_auto_contrast_levels_shader,
                               sizeof(pano_gpu_select_auto_contrast_levels_shader)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 2;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto descriptor = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.Buffer.NumElements = 4096;
    srv.Buffer.StructureByteStride = sizeof(uint32_t);
    device->CreateShaderResourceView(output->histogram.Get(), &srv, descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.Buffer.NumElements = 1;
    uav.Buffer.StructureByteStride = 2 * sizeof(float);
    device->CreateUnorderedAccessView(selected.Get(), nullptr, &uav, descriptor);
    D3D12_RESOURCE_BARRIER histogram_transition {};
    histogram_transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    histogram_transition.Transition = {output->histogram.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &histogram_transition);
    ID3D12DescriptorHeap *heaps[] {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER selected_transition {};
    selected_transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    selected_transition.Transition = {selected.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &selected_transition);
    list->CopyResource(readback.Get(), selected.Get());
    selected_transition.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    selected_transition.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &selected_transition);
    std::swap(
        histogram_transition.Transition.StateBefore, histogram_transition.Transition.StateAfter);
    list->ResourceBarrier(1, &histogram_transition);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    output->session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        output->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(output->session->device_core->queue->Signal(
            output->session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            output->session->device_core.get(), fence, error_buffer, error_buffer_size,
            "auto-contrast level selection timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    void *mapped = nullptr;
    const D3D12_RANGE range {0, 2 * sizeof(float)};
    if (FAILED(readback->Map(0, &range, &mapped)))
        return PANO_GPU_UNAVAILABLE;
    const auto *const selected_levels = static_cast<const float *>(mapped);
    levels->black = selected_levels[0];
    levels->white = selected_levels[1];
    readback->Unmap(0, nullptr);
    levels->processed_pixels = static_cast<uint32_t>(total_pixels);
    output->auto_contrast_levels = std::move(selected);
    output->auto_contrast_black = levels->black;
    output->auto_contrast_white = levels->white;
    output->auto_contrast_levels_ready = true;
    return PANO_GPU_SUCCESS;
#endif
}

static pano_gpu_result apply_auto_contrast_srgb_impl(
    pano_gpu_output *const output, const uint32_t apply_levels, const bool use_converted_input,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    const uint32_t rows = output == nullptr
        ? 0
        : (output->output_band_rows == 0 ? output->output_height : output->band_row_count);
    uint64_t pixels = 0;
    uint64_t bytes = 0;
    if (output == nullptr || apply_levels > 1 ||
        (apply_levels != 0 && !output->auto_contrast_levels_ready) ||
        (!use_converted_input && output->linear_bytes == 0) || rows == 0 ||
        !checked_multiply(output->output_width, rows, &pixels) ||
        !checked_multiply(pixels, 3 * sizeof(float), &bytes) ||
        pixels > std::numeric_limits<uint32_t>::max())
    {
        write_error(error_buffer, error_buffer_size, "invalid auto-contrast application state");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Resource *const input = use_converted_input
        ? output->converted_linear_srgb.Get()
        : output->linear.Get();
    if (input == nullptr || (use_converted_input && output->converted_linear_srgb_bytes != bytes))
        return PANO_GPU_INVALID_ARGUMENT;
    ID3D12Device *const device = output->session->device_core->d3d_device.Get();
    if (!output->normalized_srgb)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC description {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = output->planned_linear_bytes;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&output->normalized_srgb))))
            return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 4};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    const D3D12_ROOT_SIGNATURE_DESC root_description {
        2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root.Get();
    pipeline_description.CS = {pano_gpu_apply_auto_contrast_srgb_shader,
                               sizeof(pano_gpu_apply_auto_contrast_srgb_shader)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 2;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto descriptor = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC color_srv {};
    color_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    color_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    color_srv.Format = DXGI_FORMAT_UNKNOWN;
    color_srv.Buffer.NumElements = static_cast<UINT>(pixels);
    color_srv.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateShaderResourceView(input, &color_srv, descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC output_uav {};
    output_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    output_uav.Format = DXGI_FORMAT_UNKNOWN;
    output_uav.Buffer.NumElements = static_cast<UINT>(pixels);
    output_uav.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateUnorderedAccessView(
        output->normalized_srgb.Get(), nullptr, &output_uav, descriptor);
    D3D12_RESOURCE_BARRIER linear_transition {};
    linear_transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    linear_transition.Transition = {input, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                    use_converted_input
                                        ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                        : D3D12_RESOURCE_STATE_COPY_DEST,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &linear_transition);
    struct ApplyConstants
    {
        uint32_t pixel_count;
        uint32_t apply_levels;
        float black;
        float white;
    };
    const ApplyConstants constants {
        static_cast<uint32_t>(pixels), apply_levels,
        apply_levels == 0 ? 0.0F : output->auto_contrast_black,
        apply_levels == 0 ? 1.0F : output->auto_contrast_white};
    ID3D12DescriptorHeap *heaps[] {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRoot32BitConstants(0, 4, &constants, 0);
    list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((static_cast<UINT>(pixels) + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER ordering {};
    ordering.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ordering.UAV.pResource = output->normalized_srgb.Get();
    list->ResourceBarrier(1, &ordering);
    std::swap(linear_transition.Transition.StateBefore, linear_transition.Transition.StateAfter);
    list->ResourceBarrier(1, &linear_transition);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    output->session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        output->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(output->session->device_core->queue->Signal(
            output->session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            output->session->device_core.get(), fence, error_buffer, error_buffer_size,
            "auto-contrast application timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    output->normalized_srgb_bytes = bytes;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_output_apply_auto_contrast_srgb(
    pano_gpu_output *const output, const uint32_t apply_levels, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return apply_auto_contrast_srgb_impl(
        output, apply_levels, false, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_output_apply_auto_contrast_converted_srgb(
    pano_gpu_output *const output, const uint32_t apply_levels, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return apply_auto_contrast_srgb_impl(
        output, apply_levels, true, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_output_tone_map_rec2020(
    pano_gpu_output *const output, const float reference_white_nits,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    const uint32_t rows = output == nullptr
        ? 0
        : (output->output_band_rows == 0 ? output->output_height : output->band_row_count);
    uint64_t pixels = 0;
    uint64_t bytes = 0;
    if (output == nullptr || !std::isfinite(reference_white_nits) || reference_white_nits <= 0.0F ||
        output->linear_bytes == 0 || rows == 0 ||
        !checked_multiply(output->output_width, rows, &pixels) ||
        !checked_multiply(pixels, 3 * sizeof(float), &bytes) ||
        pixels > std::numeric_limits<uint32_t>::max())
    {
        write_error(error_buffer, error_buffer_size, "invalid Rec.2020 tone-map state");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!output->linear)
        return PANO_GPU_INVALID_ARGUMENT;
    ID3D12Device *const device = output->session->device_core->d3d_device.Get();
    if (!output->tone_mapped_rec2020)
    {
        D3D12_HEAP_PROPERTIES resource_heap {};
        resource_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC description {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = output->planned_linear_bytes;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(
                &resource_heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&output->tone_mapped_rec2020))))
            return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 2};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    const D3D12_ROOT_SIGNATURE_DESC root_description {
        2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root.Get();
    pipeline_description.CS = {
        pano_gpu_tone_map_rec2020_shader, sizeof(pano_gpu_tone_map_rec2020_shader)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 2;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&descriptor_heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto descriptor = descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.Buffer.NumElements = static_cast<UINT>(pixels);
    srv.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateShaderResourceView(output->linear.Get(), &srv, descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.Buffer.NumElements = static_cast<UINT>(pixels);
    uav.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateUnorderedAccessView(output->tone_mapped_rec2020.Get(), nullptr, &uav, descriptor);
    D3D12_RESOURCE_BARRIER linear_transition {};
    linear_transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    linear_transition.Transition = {output->linear.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                    D3D12_RESOURCE_STATE_COPY_DEST,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &linear_transition);
    uint32_t reference_white_bits = 0;
    std::memcpy(&reference_white_bits, &reference_white_nits, sizeof(reference_white_bits));
    const uint32_t constants[2] {static_cast<uint32_t>(pixels), reference_white_bits};
    ID3D12DescriptorHeap *heaps[] {descriptor_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRoot32BitConstants(0, 2, constants, 0);
    list->SetComputeRootDescriptorTable(1, descriptor_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((static_cast<UINT>(pixels) + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER ordering {};
    ordering.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ordering.UAV.pResource = output->tone_mapped_rec2020.Get();
    list->ResourceBarrier(1, &ordering);
    std::swap(linear_transition.Transition.StateBefore, linear_transition.Transition.StateAfter);
    list->ResourceBarrier(1, &linear_transition);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    output->session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        output->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(output->session->device_core->queue->Signal(
            output->session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            output->session->device_core.get(), fence, error_buffer, error_buffer_size,
            "Rec.2020 tone mapping timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    output->tone_mapped_rec2020_bytes = bytes;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_output_convert_tone_mapped_rec2020_to_linear_srgb(
    pano_gpu_output *const output, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    const uint32_t rows = output == nullptr
        ? 0
        : (output->output_band_rows == 0 ? output->output_height : output->band_row_count);
    uint64_t pixels = 0;
    uint64_t bytes = 0;
    if (output == nullptr || rows == 0 ||
        !checked_multiply(output->output_width, rows, &pixels) ||
        !checked_multiply(pixels, 3 * sizeof(float), &bytes) ||
        output->tone_mapped_rec2020_bytes != bytes ||
        pixels > std::numeric_limits<uint32_t>::max())
    {
        write_error(error_buffer, error_buffer_size, "invalid Rec.2020 matrix-conversion state");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!output->tone_mapped_rec2020)
        return PANO_GPU_INVALID_ARGUMENT;
    ID3D12Device *const device = output->session->device_core->d3d_device.Get();
    if (!output->converted_linear_srgb)
    {
        D3D12_HEAP_PROPERTIES resource_heap {};
        resource_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC description {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = output->planned_linear_bytes;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(
                &resource_heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&output->converted_linear_srgb))))
            return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 1};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    const D3D12_ROOT_SIGNATURE_DESC root_description {
        2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root.Get();
    pipeline_description.CS = {
        pano_gpu_rec2020_linear_srgb_shader, sizeof(pano_gpu_rec2020_linear_srgb_shader)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 2;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&descriptor_heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto descriptor = descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.Buffer.NumElements = static_cast<UINT>(pixels);
    srv.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateShaderResourceView(output->tone_mapped_rec2020.Get(), &srv, descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.Buffer.NumElements = static_cast<UINT>(pixels);
    uav.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateUnorderedAccessView(output->converted_linear_srgb.Get(), nullptr, &uav, descriptor);
    D3D12_RESOURCE_BARRIER input_transition {};
    input_transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    input_transition.Transition = {output->tone_mapped_rec2020.Get(),
                                   D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &input_transition);
    ID3D12DescriptorHeap *heaps[] {descriptor_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRoot32BitConstant(0, static_cast<uint32_t>(pixels), 0);
    list->SetComputeRootDescriptorTable(1, descriptor_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((static_cast<UINT>(pixels) + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER ordering {};
    ordering.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ordering.UAV.pResource = output->converted_linear_srgb.Get();
    list->ResourceBarrier(1, &ordering);
    std::swap(input_transition.Transition.StateBefore, input_transition.Transition.StateAfter);
    list->ResourceBarrier(1, &input_transition);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    output->session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        output->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(output->session->device_core->queue->Signal(
            output->session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            output->session->device_core.get(), fence, error_buffer, error_buffer_size,
            "Rec.2020 matrix conversion timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    output->converted_linear_srgb_bytes = bytes;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_output_copy_linear_float(
    pano_gpu_output *const output, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    const uint32_t rows = output == nullptr
        ? 0
        : (output->output_band_rows == 0 ? output->output_height : output->band_row_count);
    uint64_t bytes = 0;
    if (output == nullptr || output->linear_bytes == 0 || rows == 0 ||
        !checked_multiply(output->output_width, rows, &bytes) ||
        !checked_multiply(bytes, 3 * sizeof(float), &bytes))
    {
        write_error(error_buffer, error_buffer_size, "invalid float-output copy state");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!output->linear)
        return PANO_GPU_INVALID_ARGUMENT;
    ID3D12Device *const device = output->session->device_core->d3d_device.Get();
    if (!output->float_output)
    {
        D3D12_HEAP_PROPERTIES resource_heap {};
        resource_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC description {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = output->planned_linear_bytes;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(
                &resource_heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&output->float_output))))
            return PANO_GPU_UNAVAILABLE;
    }
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_RESOURCE_BARRIER transition {};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition = {output->linear.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &transition);
    list->CopyBufferRegion(output->float_output.Get(), 0, output->linear.Get(), 0, bytes);
    std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
    list->ResourceBarrier(1, &transition);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    output->session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        output->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(output->session->device_core->queue->Signal(
            output->session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            output->session->device_core.get(), fence, error_buffer, error_buffer_size,
            "float-output copy timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    output->float_output_bytes = bytes;
    return PANO_GPU_SUCCESS;
#endif
}

static pano_gpu_result download_output_impl(
    pano_gpu_output *const output, const pano_gpu_output_download_request *const request,
    const pano_gpu_cancellation_token *const token, const uint32_t output_kind,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    const uint32_t expected_rows = output == nullptr
        ? 0
        : (output->output_band_rows == 0 ? output->output_height : output->band_row_count);
    uint64_t expected_bytes = 0;
    if (output == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->data == nullptr ||
        request->output_width != output->output_width ||
        request->row_start != output->band_row_start || request->row_count != expected_rows ||
        !checked_multiply(request->output_width, request->row_count, &expected_bytes) ||
        !checked_multiply(
            expected_bytes, output_kind == 0 ? 3 : (output_kind == 1 ? 3 * sizeof(float) : 1),
            &expected_bytes) ||
        request->data_bytes != expected_bytes || output_kind > 2 ||
        (output_kind == 0 && output->quantized_srgb_bytes != expected_bytes) ||
        (output_kind == 1 && output->float_output_bytes != expected_bytes) ||
        (output_kind == 2 && output->coverage_bytes < expected_bytes))
    {
        write_error(error_buffer, error_buffer_size, "invalid output download layout");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    if (token != nullptr && token->cancelled.load(std::memory_order_relaxed))
        return PANO_GPU_CANCELLED;
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Resource *const source = output_kind == 0
        ? output->quantized_srgb.Get()
        : (output_kind == 1 ? output->float_output.Get() : output->coverage.Get());
    const D3D12_RESOURCE_STATES source_state = output_kind == 0
        ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
        : D3D12_RESOURCE_STATE_COPY_DEST;
    if (source == nullptr)
        return PANO_GPU_INVALID_ARGUMENT;
    ID3D12Device *const device = output->session->device_core->d3d_device.Get();
    if (!output->download_readback)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC description {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = output->planned_linear_bytes;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
#if defined(PANO_GPU_TEST_HOOKS)
        const bool injected_allocation_failure =
            fail_next_download_allocation.exchange(false, std::memory_order_relaxed);
#else
        const bool injected_allocation_failure = false;
#endif
        if (injected_allocation_failure || FAILED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&output->download_readback))))
        {
            write_error(error_buffer, error_buffer_size, "cannot allocate D3D12 output download readback");
            return PANO_GPU_UNAVAILABLE;
        }
        output->download_readback_bytes = output->planned_linear_bytes;
    }
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_RESOURCE_BARRIER transition {};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition = {source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             source_state,
                             D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &transition);
    list->CopyBufferRegion(output->download_readback.Get(), 0, source, 0, expected_bytes);
    std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
    list->ResourceBarrier(1, &transition);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    if (token != nullptr && token->cancelled.load(std::memory_order_relaxed))
        return PANO_GPU_CANCELLED;
#if defined(PANO_GPU_TEST_HOOKS)
    if (fail_next_download_submission.exchange(false, std::memory_order_relaxed))
    {
        write_error(error_buffer, error_buffer_size, "cannot submit D3D12 output download");
        return PANO_GPU_UNAVAILABLE;
    }
#endif
    ID3D12CommandList *lists[] {list.Get()};
    output->session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        output->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(output->session->device_core->queue->Signal(
            output->session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            output->session->device_core.get(), fence, error_buffer, error_buffer_size,
            "output download timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
#if defined(PANO_GPU_TEST_HOOKS)
    if (fail_next_download_fence_wait.exchange(false, std::memory_order_relaxed))
    {
        write_error(error_buffer, error_buffer_size, "D3D12 output download fence wait failed");
        return PANO_GPU_UNAVAILABLE;
    }
    if (token != nullptr &&
        cancel_after_next_output_download_wait.exchange(false, std::memory_order_relaxed))
        const_cast<pano_gpu_cancellation_token *>(token)->cancelled.store(
            true, std::memory_order_relaxed);
#endif
    if (token != nullptr && token->cancelled.load(std::memory_order_relaxed))
        return PANO_GPU_CANCELLED;
    void *mapped = nullptr;
    const D3D12_RANGE range {0, static_cast<SIZE_T>(expected_bytes)};
#if defined(PANO_GPU_TEST_HOOKS)
    const bool injected_map_failure =
        fail_next_download_map.exchange(false, std::memory_order_relaxed);
#else
    const bool injected_map_failure = false;
#endif
    if (injected_map_failure || FAILED(output->download_readback->Map(0, &range, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 output download readback");
        return PANO_GPU_UNAVAILABLE;
    }
    std::memcpy(request->data, mapped, static_cast<size_t>(expected_bytes));
    output->download_readback->Unmap(0, nullptr);
    ++output->download_count;
    output->downloaded_bytes += expected_bytes;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_output_download_srgb8(
    pano_gpu_output *const output, const pano_gpu_output_download_request *const request,
    const pano_gpu_cancellation_token *const token, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return download_output_impl(
        output, request, token, 0, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_output_download_float(
    pano_gpu_output *const output, const pano_gpu_output_download_request *const request,
    const pano_gpu_cancellation_token *const token, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return download_output_impl(
        output, request, token, 1, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_output_download_coverage(
    pano_gpu_output *const output, const pano_gpu_output_download_request *const request,
    const pano_gpu_cancellation_token *const token, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return download_output_impl(
        output, request, token, 2, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_output_query_transfer_diagnostics(
    const pano_gpu_output *const output, pano_gpu_output_transfer_diagnostics *const diagnostics,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (output == nullptr || diagnostics == nullptr || diagnostics->size != sizeof(*diagnostics) ||
        diagnostics->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid output transfer diagnostics");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    diagnostics->readback_bytes = output->download_readback_bytes;
    diagnostics->download_count = output->download_count;
    diagnostics->downloaded_bytes = output->downloaded_bytes;
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_output_quantize_normalized_srgb8(
    pano_gpu_output *const output, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    uint64_t channel_count = 0;
    if (output == nullptr || output->normalized_srgb_bytes == 0 ||
        output->normalized_srgb_bytes % sizeof(float) != 0 ||
        (channel_count = output->normalized_srgb_bytes / sizeof(float)) >
            std::numeric_limits<uint32_t>::max())
    {
        write_error(error_buffer, error_buffer_size, "invalid normalized-sRGB quantization state");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = output->session->device_core->d3d_device.Get();
    if (!output->normalized_srgb)
        return PANO_GPU_INVALID_ARGUMENT;
    if (!output->quantized_srgb)
    {
        uint64_t allocation_bytes = 0;
        const uint32_t storage_rows = output->output_band_rows == 0
            ? output->output_height
            : output->output_band_rows;
        if (!checked_multiply(output->output_width, storage_rows, &allocation_bytes) ||
            !checked_multiply(allocation_bytes, 3, &allocation_bytes) ||
            !align_resource(allocation_bytes, &allocation_bytes))
            return PANO_GPU_INVALID_ARGUMENT;
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC description {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = allocation_bytes;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&output->quantized_srgb))))
            return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 1};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    const D3D12_ROOT_SIGNATURE_DESC root_description {
        2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root.Get();
    pipeline_description.CS = {pano_gpu_quantize_srgb8_shader,
                               sizeof(pano_gpu_quantize_srgb8_shader)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 2;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto descriptor = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.Buffer.NumElements = static_cast<UINT>(channel_count);
    srv.Buffer.StructureByteStride = sizeof(float);
    device->CreateShaderResourceView(output->normalized_srgb.Get(), &srv, descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Format = DXGI_FORMAT_R8_UINT;
    uav.Buffer.NumElements = static_cast<UINT>(channel_count);
    device->CreateUnorderedAccessView(output->quantized_srgb.Get(), nullptr, &uav, descriptor);
    D3D12_RESOURCE_BARRIER input_transition {};
    input_transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    input_transition.Transition = {output->normalized_srgb.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &input_transition);
    ID3D12DescriptorHeap *heaps[] {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRoot32BitConstant(0, static_cast<uint32_t>(channel_count), 0);
    list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((static_cast<UINT>(channel_count) + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER ordering {};
    ordering.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ordering.UAV.pResource = output->quantized_srgb.Get();
    list->ResourceBarrier(1, &ordering);
    std::swap(input_transition.Transition.StateBefore, input_transition.Transition.StateAfter);
    list->ResourceBarrier(1, &input_transition);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    output->session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        output->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(output->session->device_core->queue->Signal(
            output->session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            output->session->device_core.get(), fence, error_buffer, error_buffer_size,
            "normalized-sRGB quantization timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    output->quantized_srgb_bytes = channel_count;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_set_output_band(
    pano_gpu_output *const output, const uint32_t row_start, const uint32_t row_count, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (output == nullptr || output->output_band_rows == 0 || row_count == 0 || row_count > output->output_band_rows ||
        row_start >= output->output_height || row_count > output->output_height - row_start)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 output-band range");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    output->band_row_start = row_start;
    output->band_row_count = row_count;
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_test_read_output_band(
    const pano_gpu_output *const output, void *const linear_rgb, const uint64_t linear_rgb_bytes,
    void *const coverage, const uint64_t coverage_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (output == nullptr || output->session == nullptr || linear_rgb == nullptr || coverage == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 output-band readback arguments");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    const uint32_t storage_rows = output->output_band_rows == 0 ? output->output_height : output->band_row_count;
    uint64_t expected_linear_bytes = 0;
    uint64_t expected_coverage_bytes = 0;
    if (!checked_multiply(output->output_width, storage_rows, &expected_coverage_bytes) ||
        !checked_multiply(expected_coverage_bytes, 3 * sizeof(float), &expected_linear_bytes) ||
        linear_rgb_bytes != expected_linear_bytes || coverage_bytes != expected_coverage_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 output-band readback buffer sizes");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!output->session->device_core || !output->linear || !output->coverage ||
        output->linear_bytes < expected_linear_bytes || output->coverage_bytes < expected_coverage_bytes)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 output-band storage is not allocated");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    ID3D12Device *const device = output->session->device_core->d3d_device.Get();
    const auto description = [](const uint64_t bytes) {
        D3D12_RESOURCE_DESC value {};
        value.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        value.Width = bytes;
        value.Height = 1;
        value.DepthOrArraySize = 1;
        value.MipLevels = 1;
        value.SampleDesc.Count = 1;
        value.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return value;
    };
    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    Microsoft::WRL::ComPtr<ID3D12Resource> linear_readback;
    Microsoft::WRL::ComPtr<ID3D12Resource> coverage_readback;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    const D3D12_RESOURCE_DESC linear_description = description(output->linear_bytes);
    const D3D12_RESOURCE_DESC coverage_description = description(output->coverage_bytes);
    if (FAILED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &linear_description, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&linear_readback))) ||
        FAILED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &coverage_description, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&coverage_readback))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 output-band readback resources");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_RESOURCE_BARRIER to_source[2] {};
    ID3D12Resource *const output_resources[] = {output->linear.Get(), output->coverage.Get()};
    for (UINT index = 0; index < 2; ++index)
    {
        to_source[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_source[index].Transition.pResource = output_resources[index];
        to_source[index].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        to_source[index].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        to_source[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    list->ResourceBarrier(2, to_source);
    list->CopyResource(linear_readback.Get(), output->linear.Get());
    list->CopyResource(coverage_readback.Get(), output->coverage.Get());
    for (UINT index = 0; index < 2; ++index)
        std::swap(to_source[index].Transition.StateBefore, to_source[index].Transition.StateAfter);
    list->ResourceBarrier(2, to_source);
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 output-band readback command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] = {list.Get()};
    output->session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence = output->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(output->session->device_core->queue->Signal(output->session->device_core->fence.Get(), fence)))
    {
        write_error(error_buffer, error_buffer_size, "cannot signal D3D12 output-band readback fence");
        return PANO_GPU_UNAVAILABLE;
    }
    if (wait_for_fence(output->session->device_core.get(), fence, error_buffer, error_buffer_size,
                       "D3D12 output-band readback fence timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    const auto copy = [](ID3D12Resource *const resource, const uint64_t bytes, void *const destination) {
        D3D12_RANGE range {0, static_cast<SIZE_T>(bytes)};
        void *mapped = nullptr;
        if (FAILED(resource->Map(0, &range, &mapped)))
            return false;
        std::memcpy(destination, mapped, static_cast<size_t>(bytes));
        resource->Unmap(0, nullptr);
        return true;
    };
    if (!copy(linear_readback.Get(), expected_linear_bytes, linear_rgb) ||
        !copy(coverage_readback.Get(), expected_coverage_bytes, coverage))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 output-band readback");
        return PANO_GPU_UNAVAILABLE;
    }
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_session_allocate_source(
    pano_gpu_session *const session, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || !session->device_core || session->frame_count == 0 || session->source_height == 0 ||
        session->source_row_stride_bytes == 0)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 session handle");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    uint64_t source_bytes = 0;
    if (!checked_multiply(session->frame_count, session->source_height, &source_bytes) ||
        !checked_multiply(source_bytes, session->source_row_stride_bytes, &source_bytes) ||
        !align_resource(source_bytes, &source_bytes))
    {
        write_error(error_buffer, error_buffer_size, "D3D12 source allocation size overflows");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (session->source)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 source storage is already allocated");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if defined(PANO_GPU_TEST_HOOKS)
    if (fail_next_source_allocation.exchange(false, std::memory_order_relaxed))
    {
        write_error(error_buffer, error_buffer_size, "cannot allocate D3D12 source storage");
        return PANO_GPU_OUT_OF_MEMORY;
    }
#endif
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC buffer {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = source_bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const HRESULT result = session->device_core->d3d_device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&session->source));
    if (FAILED(result))
    {
        write_hresult_error(error_buffer, error_buffer_size, "cannot allocate D3D12 source storage", result);
        return PANO_GPU_UNAVAILABLE;
    }
    session->source_bytes = source_bytes;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_session_allocate_rotations(
    pano_gpu_session *const session, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || !session->device_core)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 session handle");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    if (session->requested_rotation_bytes == 0)
        return PANO_GPU_SUCCESS;
    uint64_t rotation_bytes = 0;
    if (!align_resource(session->requested_rotation_bytes, &rotation_bytes))
    {
        write_error(error_buffer, error_buffer_size, "D3D12 rotation allocation size overflows");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (session->rotations)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 rotation storage is already allocated");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if defined(PANO_GPU_TEST_HOOKS)
    if (fail_next_rotation_allocation.exchange(false, std::memory_order_relaxed))
    {
        write_error(error_buffer, error_buffer_size, "cannot allocate D3D12 rotation storage");
        return PANO_GPU_OUT_OF_MEMORY;
    }
#endif
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC buffer {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = rotation_bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const HRESULT result = session->device_core->d3d_device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&session->rotations));
    if (FAILED(result))
    {
        write_hresult_error(error_buffer, error_buffer_size, "cannot allocate D3D12 rotation storage", result);
        return PANO_GPU_UNAVAILABLE;
    }
    session->rotation_bytes = rotation_bytes;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_session_upload_rotations(
    pano_gpu_session *const session, const void *const rotations, const uint64_t rotation_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || !session->device_core || rotations == nullptr ||
        rotation_bytes != session->requested_rotation_bytes || session->rotations_uploaded)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 rotation upload");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->rotations)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 rotation storage is not allocated");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    D3D12_HEAP_PROPERTIES upload_heap {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = session->rotation_bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(session->device_core->d3d_device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&upload))) ||
        FAILED(session->device_core->d3d_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(session->device_core->d3d_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 rotation upload resources");
        return PANO_GPU_UNAVAILABLE;
    }
    void *mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 rotation upload resource");
        return PANO_GPU_UNAVAILABLE;
    }
    std::memset(mapped, 0, static_cast<size_t>(session->rotation_bytes));
    std::memcpy(mapped, rotations, static_cast<size_t>(rotation_bytes));
    upload->Unmap(0, nullptr);
    list->CopyBufferRegion(session->rotations.Get(), 0, upload.Get(), 0, rotation_bytes);
    D3D12_RESOURCE_BARRIER to_shader_read {};
    to_shader_read.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_shader_read.Transition.pResource = session->rotations.Get();
    to_shader_read.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_shader_read.Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE;
    to_shader_read.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &to_shader_read);
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 rotation upload command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] = {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)))
    {
        write_error(error_buffer, error_buffer_size, "cannot signal D3D12 rotation upload fence");
        return PANO_GPU_UNAVAILABLE;
    }
    HANDLE event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_handle == nullptr ||
        FAILED(session->device_core->fence->SetEventOnCompletion(fence_value, event_handle)) ||
        WaitForSingleObject(event_handle, 10000) != WAIT_OBJECT_0)
    {
        if (event_handle != nullptr)
            CloseHandle(event_handle);
        write_error(error_buffer, error_buffer_size, "D3D12 rotation upload fence timed out");
        return PANO_GPU_UNAVAILABLE;
    }
    CloseHandle(event_handle);
    session->rotations_uploaded = true;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_session_allocate_encoding_metadata(
    pano_gpu_session *const session, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || !session->device_core)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 session handle");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    if (session->requested_encoding_metadata_bytes == 0)
        return PANO_GPU_SUCCESS;
    uint64_t encoding_metadata_bytes = 0;
    if (!align_resource(session->requested_encoding_metadata_bytes, &encoding_metadata_bytes))
    {
        write_error(error_buffer, error_buffer_size, "D3D12 encoding metadata allocation size overflows");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (session->encoding_metadata)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 encoding metadata storage is already allocated");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if defined(PANO_GPU_TEST_HOOKS)
    if (fail_next_encoding_metadata_allocation.exchange(false, std::memory_order_relaxed))
    {
        write_error(error_buffer, error_buffer_size, "cannot allocate D3D12 encoding metadata storage");
        return PANO_GPU_OUT_OF_MEMORY;
    }
#endif
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC buffer {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = encoding_metadata_bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const HRESULT result = session->device_core->d3d_device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&session->encoding_metadata));
    if (FAILED(result))
    {
        write_hresult_error(
            error_buffer, error_buffer_size, "cannot allocate D3D12 encoding metadata storage", result);
        return PANO_GPU_UNAVAILABLE;
    }
    session->encoding_metadata_bytes = encoding_metadata_bytes;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_session_upload_encoding_metadata(
    pano_gpu_session *const session, const void *const encoding_metadata,
    const uint64_t encoding_metadata_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || !session->device_core || encoding_metadata == nullptr ||
        encoding_metadata_bytes != session->requested_encoding_metadata_bytes ||
        session->encoding_metadata_uploaded)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 encoding metadata upload");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
#if defined(PANO_GPU_TEST_HOOKS)
    if (fail_next_encoding_metadata_upload.exchange(false, std::memory_order_relaxed))
    {
        write_error(error_buffer, error_buffer_size, "injected D3D12 encoding-metadata upload failure");
        return PANO_GPU_UNAVAILABLE;
    }
#endif
    if (!session->encoding_metadata)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 encoding metadata storage is not allocated");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    D3D12_HEAP_PROPERTIES upload_heap {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = session->encoding_metadata_bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(session->device_core->d3d_device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&upload))) ||
        FAILED(session->device_core->d3d_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(session->device_core->d3d_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 encoding metadata upload resources");
        return PANO_GPU_UNAVAILABLE;
    }
    void *mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 encoding metadata upload resource");
        return PANO_GPU_UNAVAILABLE;
    }
    std::memset(mapped, 0, static_cast<size_t>(session->encoding_metadata_bytes));
    std::memcpy(mapped, encoding_metadata, static_cast<size_t>(encoding_metadata_bytes));
    upload->Unmap(0, nullptr);
    list->CopyBufferRegion(
        session->encoding_metadata.Get(), 0, upload.Get(), 0, encoding_metadata_bytes);
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 encoding metadata upload command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] = {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)))
    {
        write_error(error_buffer, error_buffer_size, "cannot signal D3D12 encoding metadata upload fence");
        return PANO_GPU_UNAVAILABLE;
    }
    HANDLE event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_handle == nullptr ||
        FAILED(session->device_core->fence->SetEventOnCompletion(fence_value, event_handle)) ||
        WaitForSingleObject(event_handle, 10000) != WAIT_OBJECT_0)
    {
        if (event_handle != nullptr)
            CloseHandle(event_handle);
        write_error(error_buffer, error_buffer_size, "D3D12 encoding metadata upload fence timed out");
        return PANO_GPU_UNAVAILABLE;
    }
    CloseHandle(event_handle);
    session->encoding_metadata_uploaded = true;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_cancellation_token_create(
    pano_gpu_cancellation_token **const token, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (token == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "cancellation-token out-handle is null");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    *token = nullptr;
    try
    {
#if defined(PANO_GPU_TEST_HOOKS)
        if (fail_next_allocation.exchange(false, std::memory_order_relaxed))
            throw std::bad_alloc {};
#endif
        *token = new pano_gpu_cancellation_token;
        return PANO_GPU_SUCCESS;
    }
    catch (const std::bad_alloc &)
    {
        write_error(error_buffer, error_buffer_size, "cannot allocate cancellation token");
        return PANO_GPU_OUT_OF_MEMORY;
    }
    catch (...)
    {
        write_error(error_buffer, error_buffer_size, "unexpected cancellation-token creation failure");
        return PANO_GPU_UNAVAILABLE;
    }
}

void pano_gpu_cancellation_token_cancel(pano_gpu_cancellation_token *const token) noexcept
{
    if (token != nullptr)
        token->cancelled.store(true, std::memory_order_relaxed);
}

int32_t pano_gpu_cancellation_token_is_cancelled(const pano_gpu_cancellation_token *const token) noexcept
{
    return token != nullptr && token->cancelled.load(std::memory_order_relaxed) ? 1 : 0;
}

void pano_gpu_cancellation_token_destroy(pano_gpu_cancellation_token **const token) noexcept
{
    if (token == nullptr)
        return;
    pano_gpu_cancellation_token *const owned = *token;
    *token = nullptr;
    delete owned;
}

void pano_gpu_device_destroy(pano_gpu_device **const device) noexcept
{
    if (device == nullptr)
        return;
    pano_gpu_device *const owned = *device;
    *device = nullptr;
    delete owned;
}

void pano_gpu_session_destroy(pano_gpu_session **const session) noexcept
{
    if (session == nullptr)
        return;
    pano_gpu_session *const owned = *session;
    *session = nullptr;
    release_session(owned);
}

void pano_gpu_output_destroy(pano_gpu_output **const output) noexcept
{
    if (output == nullptr)
        return;
    pano_gpu_output *const owned = *output;
    *output = nullptr;
    delete owned;
}

#if defined(_WIN32)
static pano_gpu_result wait_surface_idle(
    pano_gpu_preview_surface *const surface, char *const error_buffer,
    const uint32_t error_buffer_size)
{
    const uint64_t fence =
        surface->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(surface->device_core->queue->Signal(
            surface->device_core->fence.Get(), fence)))
    {
        write_error(error_buffer, error_buffer_size, "cannot signal preview surface fence");
        return PANO_GPU_UNAVAILABLE;
    }
    return wait_for_fence(surface->device_core.get(), fence, error_buffer,
                          error_buffer_size, "preview surface fence timed out");
}

static bool create_surface_back_buffers(pano_gpu_preview_surface *const surface)
{
    ID3D12Device *const device = surface->device_core->d3d_device.Get();
    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto handle = surface->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT index = 0; index < surface->buffers.size(); ++index)
    {
        if (FAILED(surface->swap_chain->GetBuffer(
                index, IID_PPV_ARGS(&surface->buffers[index]))))
            return false;
        device->CreateRenderTargetView(surface->buffers[index].Get(), nullptr, handle);
        handle.ptr += increment;
    }
    return true;
}

static bool create_surface_present_texture(pano_gpu_preview_surface *const surface)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = surface->width;
    description.Height = surface->height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return SUCCEEDED(surface->device_core->d3d_device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&surface->present_texture)));
}

static void update_surface_device_lost(
    pano_gpu_preview_surface *const surface) noexcept
{
    surface->device_lost =
        FAILED(surface->device_core->d3d_device->GetDeviceRemovedReason());
}
#endif

pano_gpu_result pano_gpu_preview_surface_create(
    pano_gpu_device *const device,
    const pano_gpu_preview_surface_create_options *const options,
    pano_gpu_preview_surface **const surface, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (surface == nullptr)
        return PANO_GPU_INVALID_ARGUMENT;
    *surface = nullptr;
    if (device == nullptr || !device->core || options == nullptr ||
        options->size != sizeof(*options) ||
        options->abi_version != PANO_GPU_ABI_VERSION ||
        options->native_window == 0 || options->width == 0 || options->height == 0)
    {
        write_error(error_buffer, error_buffer_size, "invalid preview surface options");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    const HWND window = reinterpret_cast<HWND>(
        static_cast<uintptr_t>(options->native_window));
    if (!IsWindow(window))
    {
        write_error(error_buffer, error_buffer_size, "preview surface window is invalid");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    pano_gpu_preview_surface *created = nullptr;
    try
    {
        created = new pano_gpu_preview_surface;
        created->device_core = device->core;
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
            throw std::runtime_error("cannot create preview DXGI factory");
        DXGI_SWAP_CHAIN_DESC1 description {};
        description.Width = options->width;
        description.Height = options->height;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain;
        if (FAILED(factory->CreateSwapChainForHwnd(
                device->core->queue.Get(), window, &description, nullptr,
                nullptr, &swap_chain)) ||
            FAILED(swap_chain.As(&created->swap_chain)))
            throw std::runtime_error("cannot create preview swap chain");
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        D3D12_DESCRIPTOR_HEAP_DESC heap {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 2;
        if (FAILED(device->core->d3d_device->CreateDescriptorHeap(
                &heap, IID_PPV_ARGS(&created->rtv_heap))) ||
            !create_surface_back_buffers(created))
            throw std::runtime_error("cannot create preview back buffers");
        created->width = options->width;
        created->height = options->height;
        if (!create_surface_present_texture(created))
            throw std::runtime_error("cannot create preview presentation texture");
        live_preview_surface_count.fetch_add(1, std::memory_order_relaxed);
        created->count_registered = true;
        *surface = created;
        return PANO_GPU_SUCCESS;
    }
    catch (const std::bad_alloc &)
    {
        delete created;
        write_error(error_buffer, error_buffer_size, "cannot allocate preview surface");
        return PANO_GPU_OUT_OF_MEMORY;
    }
    catch (...)
    {
        delete created;
        write_error(error_buffer, error_buffer_size, "cannot create preview surface");
        return PANO_GPU_UNAVAILABLE;
    }
#endif
}

pano_gpu_result pano_gpu_preview_surface_resize(
    pano_gpu_preview_surface *const surface, const uint32_t width,
    const uint32_t height, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (surface == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "preview surface is null");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    if (width == 0 || height == 0)
    {
        surface->width = width;
        surface->height = height;
        surface->occluded = true;
        return PANO_GPU_SUCCESS;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (surface->width == width && surface->height == height)
        return PANO_GPU_SUCCESS;
    if (wait_surface_idle(surface, error_buffer, error_buffer_size) != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    for (auto &buffer : surface->buffers)
        buffer.Reset();
    surface->present_texture.Reset();
    if (FAILED(surface->swap_chain->ResizeBuffers(
            2, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0)) ||
        !create_surface_back_buffers(surface))
    {
        update_surface_device_lost(surface);
        write_error(error_buffer, error_buffer_size, "cannot resize preview surface");
        return PANO_GPU_UNAVAILABLE;
    }
    surface->width = width;
    surface->height = height;
    if (!create_surface_present_texture(surface))
    {
        write_error(error_buffer, error_buffer_size,
                    "cannot resize preview presentation texture");
        return PANO_GPU_UNAVAILABLE;
    }
    surface->occluded = false;
    ++surface->resize_count;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_preview_surface_clear_present(
    pano_gpu_preview_surface *const surface, const float rgba[4],
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (surface == nullptr || rgba == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "invalid preview clear request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    if (surface->width == 0 || surface->height == 0)
        return PANO_GPU_SUCCESS;
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = surface->device_core->d3d_device.Get();
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create preview clear commands");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT index = surface->swap_chain->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {surface->buffers[index].Get(),
                          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_PRESENT,
                          D3D12_RESOURCE_STATE_RENDER_TARGET};
    list->ResourceBarrier(1, &barrier);
    auto rtv = surface->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(index) *
               device->GetDescriptorHandleIncrementSize(
                   D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close preview clear commands");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] {list.Get()};
    surface->device_core->queue->ExecuteCommandLists(1, lists);
    if (wait_surface_idle(surface, error_buffer, error_buffer_size) != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    const HRESULT presented = surface->swap_chain->Present(0, 0);
    if (FAILED(presented))
    {
        update_surface_device_lost(surface);
        write_error(error_buffer, error_buffer_size, "cannot present preview surface");
        return PANO_GPU_UNAVAILABLE;
    }
    surface->occluded = presented == DXGI_STATUS_OCCLUDED;
    ++surface->present_count;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_preview_surface_present_base(
    pano_gpu_preview_surface *const surface, pano_gpu_preview *const preview,
    const pano_gpu_preview_surface_present_request *const request,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (surface == nullptr || preview == nullptr || request == nullptr ||
        request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->use_overview > 1 ||
        surface->device_core.get() != preview->session->device_core.get() ||
        (request->use_overview != 0 &&
         (request->crop_left != 0 || request->crop_top != 0 ||
          request->crop_width != 0 || request->crop_height != 0)) ||
        (request->use_overview == 0 &&
         (request->crop_width == 0 || request->crop_height == 0 ||
          request->crop_width > preview->preview_width ||
          request->crop_height > preview->preview_height ||
          request->crop_left > preview->preview_width - request->crop_width ||
          request->crop_top > preview->preview_height - request->crop_height)))
    {
        write_error(error_buffer, error_buffer_size,
                    "invalid preview surface presentation request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    if (surface->width == 0 || surface->height == 0)
        return PANO_GPU_SUCCESS;
    bool expected = false;
    if (!surface->presenting.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel))
    {
        write_error(error_buffer, error_buffer_size,
                    "concurrent preview surface presentation is rejected");
        return PANO_GPU_UNAVAILABLE;
    }
    struct PresentRelease
    {
        std::atomic<bool> &presenting;
        ~PresentRelease() { presenting.store(false, std::memory_order_release); }
    } release {surface->presenting};
#if defined(PANO_GPU_TEST_HOOKS)
    if (fail_next_preview_surface_device_removed.exchange(
            false, std::memory_order_relaxed))
    {
        surface->device_lost = true;
        write_error(error_buffer, error_buffer_size,
                    "preview surface device was removed");
        return PANO_GPU_UNAVAILABLE;
    }
#endif
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = surface->device_core->d3d_device.Get();
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 10};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    const D3D12_ROOT_SIGNATURE_DESC root_description {
        2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0,
            &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&root))))
    {
        write_error(error_buffer, error_buffer_size,
                    "cannot create preview presentation root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root.Get();
    pipeline_description.CS = {pano_gpu_preview_present_shader,
                               sizeof(pano_gpu_preview_present_shader)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 3;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateComputePipelineState(
            &pipeline_description, IID_PPV_ARGS(&pipeline))) ||
        FAILED(device->CreateDescriptorHeap(
            &heap_description, IID_PPV_ARGS(&heap))) ||
        FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(),
            IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size,
                    "cannot create preview presentation pipeline");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT increment = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto descriptor = heap->GetCPUDescriptorHandleForHeapStart();
    const auto make_srv = [&](ID3D12Resource *const resource,
                              const uint64_t bytes) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R8_UINT;
        srv.Buffer.NumElements = static_cast<UINT>(bytes);
        device->CreateShaderResourceView(resource, &srv, descriptor);
        descriptor.ptr += increment;
    };
    make_srv(preview->preview_rgb8.Get(), preview->preview_rgb8_bytes);
    make_srv(preview->overview_rgb8.Get(), preview->overview_rgb8_bytes);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device->CreateUnorderedAccessView(surface->present_texture.Get(), nullptr,
                                      &uav, descriptor);
    const uint32_t constants[10] {
        preview->preview_width, preview->overview_width,
        preview->overview_height, surface->width, surface->height,
        request->crop_left, request->crop_top, request->crop_width,
        request->crop_height, request->use_overview};
    ID3D12DescriptorHeap *heaps[] {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRoot32BitConstants(0, 10, constants, 0);
    list->SetComputeRootDescriptorTable(
        1, heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((surface->width + 7U) / 8U,
                   (surface->height + 7U) / 8U, 1);
    const UINT index = surface->swap_chain->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER barriers[2] {};
    for (auto &barrier : barriers)
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition = {surface->present_texture.Get(),
                              D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[1].Transition = {surface->buffers[index].Get(),
                              D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_PRESENT,
                              D3D12_RESOURCE_STATE_COPY_DEST};
    list->ResourceBarrier(2, barriers);
    list->CopyResource(surface->buffers[index].Get(),
                       surface->present_texture.Get());
    for (auto &barrier : barriers)
        std::swap(barrier.Transition.StateBefore,
                  barrier.Transition.StateAfter);
    list->ResourceBarrier(2, barriers);
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size,
                    "cannot close preview presentation commands");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] {list.Get()};
    surface->device_core->queue->ExecuteCommandLists(1, lists);
    if (wait_surface_idle(surface, error_buffer, error_buffer_size) !=
        PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    const HRESULT presented = surface->swap_chain->Present(0, 0);
    if (FAILED(presented))
    {
        update_surface_device_lost(surface);
        write_error(error_buffer, error_buffer_size,
                    "cannot present retained preview surface");
        return PANO_GPU_UNAVAILABLE;
    }
    surface->occluded = presented == DXGI_STATUS_OCCLUDED;
    ++surface->present_count;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_preview_surface_present_overlay(
    pano_gpu_preview_surface *const surface, pano_gpu_preview *const preview,
    const pano_gpu_preview_surface_overlay_request *const request,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (surface == nullptr || preview == nullptr || request == nullptr ||
        request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->use_overview > 1 ||
        request->target_mode > 1 || request->show_boundaries > 1 ||
        request->hovered_frames == nullptr ||
        request->hovered_frame_bytes != preview->frame_count ||
        request->target_pose < -1 ||
        request->target_pose >= static_cast<int32_t>(preview->frame_count) ||
        surface->device_core.get() != preview->session->device_core.get() ||
        (request->use_overview != 0 &&
         (request->crop_left != 0 || request->crop_top != 0 ||
          request->crop_width != 0 || request->crop_height != 0)) ||
        (request->use_overview == 0 &&
         (request->crop_width == 0 || request->crop_height == 0 ||
          request->crop_width > preview->preview_width ||
          request->crop_height > preview->preview_height ||
          request->crop_left > preview->preview_width - request->crop_width ||
          request->crop_top > preview->preview_height - request->crop_height)))
    {
        write_error(error_buffer, error_buffer_size,
                    "invalid preview surface overlay request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    if (surface->width == 0 || surface->height == 0)
        return PANO_GPU_SUCCESS;
    bool expected = false;
    if (!preview->rendering.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel))
    {
        write_error(error_buffer, error_buffer_size,
                    "concurrent preview rendering is rejected");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    struct PreviewRelease
    {
        std::atomic<bool> &value;
        ~PreviewRelease() { value.store(false, std::memory_order_release); }
    } preview_release {preview->rendering};
    expected = false;
    if (!surface->presenting.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel))
    {
        write_error(error_buffer, error_buffer_size,
                    "concurrent preview surface presentation is rejected");
        return PANO_GPU_UNAVAILABLE;
    }
    PreviewRelease surface_release {surface->presenting};
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = surface->device_core->d3d_device.Get();
    if (!preview->hovered_upload)
    {
        D3D12_HEAP_PROPERTIES upload_heap {}, default_heap {};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC description {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = preview->frame_count;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&preview->hovered_upload))) ||
            FAILED(device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&preview->hovered_frames))))
            return PANO_GPU_UNAVAILABLE;
    }
    void *mapped = nullptr;
    if (FAILED(preview->hovered_upload->Map(0, nullptr, &mapped)))
        return PANO_GPU_UNAVAILABLE;
    std::memcpy(mapped, request->hovered_frames, preview->frame_count);
    preview->hovered_upload->Unmap(0, nullptr);
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 4}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 17};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    const D3D12_ROOT_SIGNATURE_DESC root_description {
        2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 5;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0,
            &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&root))))
        return PANO_GPU_UNAVAILABLE;
    pipeline_description.pRootSignature = root.Get();
    pipeline_description.CS = {pano_gpu_preview_present_overlay_shader,
                               sizeof(pano_gpu_preview_present_overlay_shader)};
    if (FAILED(device->CreateComputePipelineState(
            &pipeline_description, IID_PPV_ARGS(&pipeline))) ||
        FAILED(device->CreateDescriptorHeap(
            &heap_description, IID_PPV_ARGS(&heap))) ||
        FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(),
            IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    if (preview->hovered_frames_ready)
    {
        D3D12_RESOURCE_BARRIER writable {};
        writable.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        writable.Transition = {preview->hovered_frames.Get(),
                               D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_STATE_COPY_DEST};
        list->ResourceBarrier(1, &writable);
    }
    list->CopyResource(preview->hovered_frames.Get(),
                       preview->hovered_upload.Get());
    D3D12_RESOURCE_BARRIER readable {};
    readable.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    readable.Transition = {preview->hovered_frames.Get(),
                           D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                           D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &readable);
    const UINT increment = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto descriptor = heap->GetCPUDescriptorHandleForHeapStart();
    const auto make_srv = [&](ID3D12Resource *const resource,
                              const uint64_t bytes) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R8_UINT;
        srv.Buffer.NumElements = static_cast<UINT>(bytes);
        device->CreateShaderResourceView(resource, &srv, descriptor);
        descriptor.ptr += increment;
    };
    make_srv(preview->preview_rgb8.Get(), preview->preview_rgb8_bytes);
    make_srv(preview->overview_rgb8.Get(), preview->overview_rgb8_bytes);
    make_srv(preview->compact_masks.Get(), preview->compact_mask_bytes);
    make_srv(preview->hovered_frames.Get(), preview->frame_count);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device->CreateUnorderedAccessView(surface->present_texture.Get(), nullptr,
                                      &uav, descriptor);
    const uint32_t constants[17] {
        preview->preview_width, preview->preview_height,
        preview->overview_width, preview->overview_height,
        preview->mask_width, preview->mask_height, surface->width,
        surface->height, request->crop_left, request->crop_top,
        request->crop_width, request->crop_height, preview->frame_count,
        static_cast<uint32_t>(request->target_pose), request->target_mode,
        request->show_boundaries, request->use_overview};
    ID3D12DescriptorHeap *heaps[] {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRoot32BitConstants(0, 17, constants, 0);
    list->SetComputeRootDescriptorTable(
        1, heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((surface->width + 7U) / 8U,
                   (surface->height + 7U) / 8U, 1);
    const UINT index = surface->swap_chain->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER barriers[2] {};
    for (auto &barrier : barriers)
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition = {surface->present_texture.Get(),
                              D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[1].Transition = {surface->buffers[index].Get(),
                              D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_PRESENT,
                              D3D12_RESOURCE_STATE_COPY_DEST};
    list->ResourceBarrier(2, barriers);
    list->CopyResource(surface->buffers[index].Get(),
                       surface->present_texture.Get());
    for (auto &barrier : barriers)
        std::swap(barrier.Transition.StateBefore,
                  barrier.Transition.StateAfter);
    list->ResourceBarrier(2, barriers);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    surface->device_core->queue->ExecuteCommandLists(1, lists);
    if (wait_surface_idle(surface, error_buffer, error_buffer_size) !=
        PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    preview->hovered_frames_ready = true;
    const HRESULT presented = surface->swap_chain->Present(0, 0);
    if (FAILED(presented))
    {
        update_surface_device_lost(surface);
        return PANO_GPU_UNAVAILABLE;
    }
    surface->occluded = presented == DXGI_STATUS_OCCLUDED;
    ++surface->present_count;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_preview_surface_query_diagnostics(
    const pano_gpu_preview_surface *const surface,
    pano_gpu_preview_surface_diagnostics *const diagnostics,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (surface == nullptr || diagnostics == nullptr ||
        diagnostics->size != sizeof(*diagnostics) ||
        diagnostics->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid preview surface diagnostics");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    diagnostics->width = surface->width;
    diagnostics->height = surface->height;
    diagnostics->present_count = surface->present_count;
    diagnostics->resize_count = surface->resize_count;
    diagnostics->occluded = surface->occluded ? 1U : 0U;
    diagnostics->live_surface_count =
        live_preview_surface_count.load(std::memory_order_relaxed);
    diagnostics->device_lost = surface->device_lost ? 1U : 0U;
    diagnostics->reserved = 0;
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_preview_create(
    pano_gpu_session *const session, const pano_gpu_preview_create_options *const options,
    pano_gpu_preview **const preview, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (preview == nullptr)
        return PANO_GPU_INVALID_ARGUMENT;
    *preview = nullptr;
    uint64_t preview_bytes = 0;
    uint64_t overview_bytes = 0;
    uint64_t mask_bytes = 0;
    if (session == nullptr || options == nullptr || options->size != sizeof(*options) ||
        options->abi_version != PANO_GPU_ABI_VERSION || options->frame_count != session->frame_count ||
        options->frame_count == 0 || options->preview_width == 0 || options->preview_height == 0 ||
        options->overview_width == 0 || options->overview_height == 0 || options->mask_width == 0 ||
        options->mask_height == 0 || options->preview_rgb8 == nullptr ||
        options->overview_rgb8 == nullptr || options->compact_masks == nullptr ||
        !checked_multiply(options->preview_width, options->preview_height, &preview_bytes) ||
        !checked_multiply(preview_bytes, 3, &preview_bytes) ||
        !checked_multiply(options->overview_width, options->overview_height, &overview_bytes) ||
        !checked_multiply(overview_bytes, 3, &overview_bytes) ||
        !checked_multiply(options->frame_count, options->mask_width, &mask_bytes) ||
        !checked_multiply(mask_bytes, options->mask_height, &mask_bytes) ||
        preview_bytes > std::numeric_limits<uint32_t>::max() ||
        overview_bytes > std::numeric_limits<uint32_t>::max() ||
        mask_bytes > std::numeric_limits<uint32_t>::max() ||
        options->preview_rgb8_bytes != preview_bytes ||
        options->overview_rgb8_bytes != overview_bytes || options->compact_mask_bytes != mask_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid retained preview layout");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    pano_gpu_preview *created = nullptr;
    try
    {
#if defined(PANO_GPU_TEST_HOOKS)
        if (fail_next_preview_allocation.exchange(false, std::memory_order_relaxed))
            throw std::bad_alloc {};
#endif
        created = new pano_gpu_preview;
        session->reference_count.fetch_add(1, std::memory_order_relaxed);
        created->session = session;
        ID3D12Device *const device = session->device_core->d3d_device.Get();
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        if (FAILED(device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                IID_PPV_ARGS(&list))))
            throw std::runtime_error("cannot create retained preview upload commands");
        D3D12_HEAP_PROPERTIES upload_heap {}, default_heap {};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        Microsoft::WRL::ComPtr<ID3D12Resource> preview_upload, overview_upload, masks_upload;
        const auto upload = [&](const void *const data, const uint64_t bytes,
                                Microsoft::WRL::ComPtr<ID3D12Resource> *const staging,
                                Microsoft::WRL::ComPtr<ID3D12Resource> *const retained) {
            D3D12_RESOURCE_DESC description {};
            description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            description.Width = bytes;
            description.Height = 1;
            description.DepthOrArraySize = 1;
            description.MipLevels = 1;
            description.SampleDesc.Count = 1;
            description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(device->CreateCommittedResource(
                    &upload_heap, D3D12_HEAP_FLAG_NONE, &description,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource),
                    reinterpret_cast<void **>(staging->ReleaseAndGetAddressOf()))) ||
                FAILED(device->CreateCommittedResource(
                    &default_heap, D3D12_HEAP_FLAG_NONE, &description,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
                    reinterpret_cast<void **>(retained->ReleaseAndGetAddressOf()))))
                return false;
            void *mapped = nullptr;
            if (FAILED((*staging)->Map(0, nullptr, &mapped)))
                return false;
            std::memcpy(mapped, data, static_cast<size_t>(bytes));
            (*staging)->Unmap(0, nullptr);
            list->CopyResource(retained->Get(), staging->Get());
            D3D12_RESOURCE_BARRIER transition {};
            transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            transition.Transition = {retained->Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                     D3D12_RESOURCE_STATE_COPY_DEST,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
            list->ResourceBarrier(1, &transition);
            return true;
        };
        if (!upload(options->preview_rgb8, preview_bytes, &preview_upload, &created->preview_rgb8) ||
            !upload(options->overview_rgb8, overview_bytes, &overview_upload, &created->overview_rgb8) ||
            !upload(options->compact_masks, mask_bytes, &masks_upload, &created->compact_masks) ||
            FAILED(list->Close()))
            throw std::runtime_error("cannot upload retained preview buffers");
        ID3D12CommandList *lists[] {list.Get()};
        session->device_core->queue->ExecuteCommandLists(1, lists);
        const uint64_t fence =
            session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
        if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence)) ||
            wait_for_fence(
                session->device_core.get(), fence, error_buffer, error_buffer_size,
                "retained preview upload timed out") != PANO_GPU_SUCCESS)
            throw std::runtime_error("cannot finish retained preview upload");
        created->frame_count = options->frame_count;
        created->preview_width = options->preview_width;
        created->preview_height = options->preview_height;
        created->overview_width = options->overview_width;
        created->overview_height = options->overview_height;
        created->mask_width = options->mask_width;
        created->mask_height = options->mask_height;
        created->preview_rgb8_bytes = preview_bytes;
        created->overview_rgb8_bytes = overview_bytes;
        created->compact_mask_bytes = mask_bytes;
        live_preview_count.fetch_add(1, std::memory_order_relaxed);
        created->count_registered = true;
        *preview = created;
        return PANO_GPU_SUCCESS;
    }
    catch (const std::bad_alloc &)
    {
        delete created;
        write_error(error_buffer, error_buffer_size, "cannot allocate retained preview");
        return PANO_GPU_OUT_OF_MEMORY;
    }
    catch (...)
    {
        delete created;
        write_error(error_buffer, error_buffer_size, "cannot create retained preview");
        return PANO_GPU_UNAVAILABLE;
    }
#endif
}

pano_gpu_result pano_gpu_preview_query_diagnostics(
    const pano_gpu_preview *const preview, pano_gpu_preview_diagnostics *const diagnostics,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (preview == nullptr || diagnostics == nullptr || diagnostics->size != sizeof(*diagnostics) ||
        diagnostics->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid retained preview diagnostics");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    diagnostics->frame_count = preview->frame_count;
    diagnostics->preview_width = preview->preview_width;
    diagnostics->preview_height = preview->preview_height;
    diagnostics->overview_width = preview->overview_width;
    diagnostics->overview_height = preview->overview_height;
    diagnostics->mask_width = preview->mask_width;
    diagnostics->mask_height = preview->mask_height;
    diagnostics->live_preview_count = live_preview_count.load(std::memory_order_relaxed);
    diagnostics->preview_rgb8_bytes = preview->preview_rgb8_bytes;
    diagnostics->overview_rgb8_bytes = preview->overview_rgb8_bytes;
    diagnostics->compact_mask_bytes = preview->compact_mask_bytes;
    diagnostics->retained_bytes =
        preview->preview_rgb8_bytes + preview->overview_rgb8_bytes + preview->compact_mask_bytes;
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_preview_render_base(
    pano_gpu_preview *const preview, const pano_gpu_preview_render_request *const request,
    const pano_gpu_cancellation_token *const token, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    uint64_t expected_bytes = 0;
    if (preview == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->use_overview > 1 ||
        request->output_rgb8 == nullptr ||
        !checked_multiply(preview->overview_width, preview->overview_height, &expected_bytes) ||
        !checked_multiply(expected_bytes, 3, &expected_bytes) ||
        request->output_rgb8_bytes != expected_bytes ||
        (request->use_overview != 0 &&
         (request->crop_left != 0 || request->crop_top != 0 || request->crop_width != 0 ||
          request->crop_height != 0)) ||
        (request->use_overview == 0 &&
         (request->crop_width != preview->overview_width ||
          request->crop_height != preview->overview_height ||
          request->crop_width > preview->preview_width ||
          request->crop_height > preview->preview_height ||
          request->crop_left > preview->preview_width - request->crop_width ||
          request->crop_top > preview->preview_height - request->crop_height)))
    {
        write_error(error_buffer, error_buffer_size, "invalid preview viewport request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    if (token != nullptr && token->cancelled.load(std::memory_order_relaxed))
        return PANO_GPU_CANCELLED;
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = preview->session->device_core->d3d_device.Get();
    if (!preview->viewport_rgb8)
    {
        D3D12_HEAP_PROPERTIES default_heap {}, readback_heap {};
        default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC description {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = expected_bytes;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&preview->viewport_rgb8))))
            return PANO_GPU_UNAVAILABLE;
        description.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(device->CreateCommittedResource(
                &readback_heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&preview->viewport_readback))))
            return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 7};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    const D3D12_ROOT_SIGNATURE_DESC root_description {
        2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root.Get();
    pipeline_description.CS = {pano_gpu_preview_base_shader, sizeof(pano_gpu_preview_base_shader)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 3;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto descriptor = heap->GetCPUDescriptorHandleForHeapStart();
    const auto make_srv = [&](ID3D12Resource *const resource, const uint64_t bytes) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R8_UINT;
        srv.Buffer.NumElements = static_cast<UINT>(bytes);
        device->CreateShaderResourceView(resource, &srv, descriptor);
        descriptor.ptr += increment;
    };
    make_srv(preview->preview_rgb8.Get(), preview->preview_rgb8_bytes);
    make_srv(preview->overview_rgb8.Get(), preview->overview_rgb8_bytes);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Format = DXGI_FORMAT_R8_UINT;
    uav.Buffer.NumElements = static_cast<UINT>(expected_bytes);
    device->CreateUnorderedAccessView(preview->viewport_rgb8.Get(), nullptr, &uav, descriptor);
    const uint32_t constants[7] {
        preview->preview_width, preview->overview_width, preview->overview_width,
        preview->overview_height, request->crop_left, request->crop_top, request->use_overview};
    ID3D12DescriptorHeap *heaps[] {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRoot32BitConstants(0, 7, constants, 0);
    list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    const uint32_t pixels = preview->overview_width * preview->overview_height;
    list->Dispatch((pixels + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER transition {};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition = {preview->viewport_rgb8.Get(),
                             D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &transition);
    list->CopyBufferRegion(
        preview->viewport_readback.Get(), 0, preview->viewport_rgb8.Get(), 0, expected_bytes);
    std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
    list->ResourceBarrier(1, &transition);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    if (token != nullptr && token->cancelled.load(std::memory_order_relaxed))
        return PANO_GPU_CANCELLED;
    ID3D12CommandList *lists[] {list.Get()};
    preview->session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        preview->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(preview->session->device_core->queue->Signal(
            preview->session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            preview->session->device_core.get(), fence, error_buffer, error_buffer_size,
            "preview viewport render timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    if (token != nullptr && token->cancelled.load(std::memory_order_relaxed))
        return PANO_GPU_CANCELLED;
    void *mapped = nullptr;
    const D3D12_RANGE range {0, static_cast<SIZE_T>(expected_bytes)};
    if (FAILED(preview->viewport_readback->Map(0, &range, &mapped)))
        return PANO_GPU_UNAVAILABLE;
    std::memcpy(request->output_rgb8, mapped, static_cast<size_t>(expected_bytes));
    preview->viewport_readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

static pano_gpu_result render_preview_overlay_impl(
    pano_gpu_preview *const preview, const pano_gpu_preview_overlay_request *const request,
    const uint64_t generation, const bool enforce_generation,
    const pano_gpu_cancellation_token *const token, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    uint64_t expected_bytes = 0;
    if (preview == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->use_overview > 1 ||
        request->target_mode > 1 || request->show_boundaries > 1 ||
        request->target_pose < -1 || request->target_pose >= static_cast<int32_t>(preview->frame_count) ||
        request->hovered_frames == nullptr || request->hovered_frame_bytes != preview->frame_count ||
        request->output_rgb8 == nullptr ||
        !checked_multiply(preview->overview_width, preview->overview_height, &expected_bytes) ||
        !checked_multiply(expected_bytes, 3, &expected_bytes) ||
        request->output_rgb8_bytes != expected_bytes ||
        (request->use_overview != 0 &&
         (request->crop_left != 0 || request->crop_top != 0 || request->crop_width != 0 ||
          request->crop_height != 0)) ||
        (request->use_overview == 0 &&
         (request->crop_width != preview->overview_width ||
          request->crop_height != preview->overview_height ||
          request->crop_width > preview->preview_width ||
          request->crop_height > preview->preview_height ||
          request->crop_left > preview->preview_width - request->crop_width ||
          request->crop_top > preview->preview_height - request->crop_height)))
    {
        write_error(error_buffer, error_buffer_size, "invalid preview overlay request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    if (token != nullptr && token->cancelled.load(std::memory_order_relaxed))
        return PANO_GPU_CANCELLED;
    if (enforce_generation &&
        generation != preview->latest_generation.load(std::memory_order_acquire))
        return PANO_GPU_CANCELLED;
    if (preview->rendering.exchange(true, std::memory_order_acq_rel))
    {
        write_error(error_buffer, error_buffer_size, "concurrent preview rendering is rejected");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    struct render_guard
    {
        std::atomic<bool> &rendering;
        ~render_guard() { rendering.store(false, std::memory_order_release); }
    } guard {preview->rendering};
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = preview->session->device_core->d3d_device.Get();
    if (!preview->viewport_rgb8)
    {
        D3D12_HEAP_PROPERTIES default_heap {}, readback_heap {};
        default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC description {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = expected_bytes;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&preview->viewport_rgb8))))
            return PANO_GPU_UNAVAILABLE;
        description.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(device->CreateCommittedResource(
                &readback_heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&preview->viewport_readback))))
            return PANO_GPU_UNAVAILABLE;
    }
    if (!preview->hovered_upload)
    {
        D3D12_HEAP_PROPERTIES upload_heap {}, default_heap {};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC description {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = preview->frame_count;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&preview->hovered_upload))) ||
            FAILED(device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&preview->hovered_frames))))
            return PANO_GPU_UNAVAILABLE;
    }
    void *mapped_hovered = nullptr;
    if (FAILED(preview->hovered_upload->Map(0, nullptr, &mapped_hovered)))
        return PANO_GPU_UNAVAILABLE;
    std::memcpy(mapped_hovered, request->hovered_frames, preview->frame_count);
    preview->hovered_upload->Unmap(0, nullptr);
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 4}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 13};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    const D3D12_ROOT_SIGNATURE_DESC root_description {
        2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root.Get();
    pipeline_description.CS = {
        pano_gpu_preview_overlay_shader, sizeof(pano_gpu_preview_overlay_shader)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 5;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    if (preview->hovered_frames_ready)
    {
        D3D12_RESOURCE_BARRIER writable {};
        writable.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        writable.Transition = {preview->hovered_frames.Get(),
                               D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_STATE_COPY_DEST};
        list->ResourceBarrier(1, &writable);
    }
    list->CopyResource(preview->hovered_frames.Get(), preview->hovered_upload.Get());
    D3D12_RESOURCE_BARRIER readable {};
    readable.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    readable.Transition = {preview->hovered_frames.Get(),
                           D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                           D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &readable);
    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto descriptor = heap->GetCPUDescriptorHandleForHeapStart();
    const auto make_srv = [&](ID3D12Resource *const resource, const uint64_t bytes) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R8_UINT;
        srv.Buffer.NumElements = static_cast<UINT>(bytes);
        device->CreateShaderResourceView(resource, &srv, descriptor);
        descriptor.ptr += increment;
    };
    make_srv(preview->preview_rgb8.Get(), preview->preview_rgb8_bytes);
    make_srv(preview->overview_rgb8.Get(), preview->overview_rgb8_bytes);
    make_srv(preview->compact_masks.Get(), preview->compact_mask_bytes);
    make_srv(preview->hovered_frames.Get(), preview->frame_count);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Format = DXGI_FORMAT_R8_UINT;
    uav.Buffer.NumElements = static_cast<UINT>(expected_bytes);
    device->CreateUnorderedAccessView(preview->viewport_rgb8.Get(), nullptr, &uav, descriptor);
    const uint32_t constants[13] {
        preview->preview_width, preview->preview_height, preview->overview_width,
        preview->overview_height, preview->mask_width, preview->mask_height, request->crop_left,
        request->crop_top, preview->frame_count, static_cast<uint32_t>(request->target_pose),
        request->target_mode, request->show_boundaries, request->use_overview};
    ID3D12DescriptorHeap *heaps[] {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRoot32BitConstants(0, 13, constants, 0);
    list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    const uint32_t pixels = preview->overview_width * preview->overview_height;
    list->Dispatch((pixels + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER output_transition {};
    output_transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    output_transition.Transition = {preview->viewport_rgb8.Get(),
                                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &output_transition);
    list->CopyBufferRegion(
        preview->viewport_readback.Get(), 0, preview->viewport_rgb8.Get(), 0, expected_bytes);
    std::swap(output_transition.Transition.StateBefore, output_transition.Transition.StateAfter);
    list->ResourceBarrier(1, &output_transition);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    if (token != nullptr && token->cancelled.load(std::memory_order_relaxed))
        return PANO_GPU_CANCELLED;
    if (enforce_generation &&
        generation != preview->latest_generation.load(std::memory_order_acquire))
        return PANO_GPU_CANCELLED;
    ID3D12CommandList *lists[] {list.Get()};
    preview->session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        preview->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(preview->session->device_core->queue->Signal(
            preview->session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            preview->session->device_core.get(), fence, error_buffer, error_buffer_size,
            "preview overlay render timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    preview->hovered_frames_ready = true;
#if defined(PANO_GPU_TEST_HOOKS)
    if (enforce_generation &&
        stale_after_next_preview_wait.exchange(false, std::memory_order_relaxed))
        preview->latest_generation.fetch_add(1, std::memory_order_release);
#endif
    if (token != nullptr && token->cancelled.load(std::memory_order_relaxed))
        return PANO_GPU_CANCELLED;
    if (enforce_generation &&
        generation != preview->latest_generation.load(std::memory_order_acquire))
        return PANO_GPU_CANCELLED;
    void *mapped = nullptr;
    const D3D12_RANGE range {0, static_cast<SIZE_T>(expected_bytes)};
    if (FAILED(preview->viewport_readback->Map(0, &range, &mapped)))
        return PANO_GPU_UNAVAILABLE;
    std::memcpy(request->output_rgb8, mapped, static_cast<size_t>(expected_bytes));
    preview->viewport_readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_preview_render_overlay(
    pano_gpu_preview *const preview, const pano_gpu_preview_overlay_request *const request,
    const pano_gpu_cancellation_token *const token, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return render_preview_overlay_impl(
        preview, request, 0, false, token, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_preview_set_generation(
    pano_gpu_preview *const preview, const uint64_t generation, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (preview == nullptr || generation == 0)
    {
        write_error(error_buffer, error_buffer_size, "invalid preview generation");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    uint64_t current = preview->latest_generation.load(std::memory_order_relaxed);
    while (generation > current &&
           !preview->latest_generation.compare_exchange_weak(
               current, generation, std::memory_order_release, std::memory_order_relaxed))
    {
    }
    if (generation < current)
    {
        write_error(error_buffer, error_buffer_size, "preview generation moved backwards");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_preview_render_overlay_generation(
    pano_gpu_preview *const preview, const pano_gpu_preview_overlay_request *const request,
    const uint64_t generation, const pano_gpu_cancellation_token *const token,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    return render_preview_overlay_impl(
        preview, request, generation, true, token, error_buffer, error_buffer_size);
}

void pano_gpu_preview_destroy(pano_gpu_preview **const preview) noexcept
{
    if (preview == nullptr)
        return;
    pano_gpu_preview *const owned = *preview;
    *preview = nullptr;
    delete owned;
}

void pano_gpu_preview_surface_destroy(
    pano_gpu_preview_surface **const surface) noexcept
{
    if (surface == nullptr)
        return;
    pano_gpu_preview_surface *const owned = *surface;
    *surface = nullptr;
    delete owned;
}

#if defined(PANO_GPU_TEST_HOOKS)
void pano_gpu_test_fail_next_preview_surface_device_removed(void) noexcept
{
    fail_next_preview_surface_device_removed.store(true,
                                                    std::memory_order_relaxed);
}

 pano_gpu_result pano_gpu_test_read_preview_surface(
    pano_gpu_preview_surface *const surface, uint8_t *const rgba8,
    const uint64_t rgba8_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    uint64_t expected = 0;
    if (surface == nullptr || rgba8 == nullptr ||
        !checked_multiply(surface->width, surface->height, &expected) ||
        !checked_multiply(expected, 4, &expected) || rgba8_bytes != expected)
    {
        write_error(error_buffer, error_buffer_size,
                    "invalid preview surface readback request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = surface->device_core->d3d_device.Get();
    const auto texture = surface->present_texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 total_bytes = 0;
    device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, nullptr,
                                  nullptr, &total_bytes);
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total_bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback))) ||
        FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {surface->present_texture.Get(),
                          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION destination {};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source {};
    source.pResource = surface->present_texture.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    surface->device_core->queue->ExecuteCommandLists(1, lists);
    if (wait_surface_idle(surface, error_buffer, error_buffer_size) !=
        PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    void *mapped = nullptr;
    const D3D12_RANGE range {0, static_cast<SIZE_T>(total_bytes)};
    if (FAILED(readback->Map(0, &range, &mapped)))
        return PANO_GPU_UNAVAILABLE;
    for (uint32_t row = 0; row < surface->height; ++row)
        std::memcpy(rgba8 + static_cast<size_t>(row) * surface->width * 4,
                    static_cast<const uint8_t *>(mapped) +
                        static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                    static_cast<size_t>(surface->width) * 4);
    readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

void pano_gpu_test_fail_next_allocation(void) noexcept
{
    fail_next_allocation.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_device_creation(void) noexcept
{
    fail_next_device_creation.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_pipeline_creation(void) noexcept
{
    fail_next_pipeline_creation.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_descriptor_creation(void) noexcept
{
    fail_next_descriptor_creation.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_resource_creation(void) noexcept
{
    fail_next_resource_creation.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_composite_before_dispatch(void) noexcept
{
    fail_next_composite_before_dispatch.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_composite_after_dispatch(void) noexcept
{
    fail_next_composite_after_dispatch.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_device_removed_before_dispatch(void) noexcept
{
    fail_next_device_removed_before_dispatch.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_device_removed_after_dispatch(void) noexcept
{
    fail_next_device_removed_after_dispatch.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_download_allocation(void) noexcept
{
    fail_next_download_allocation.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_download_submission(void) noexcept
{
    fail_next_download_submission.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_download_fence_wait(void) noexcept
{
    fail_next_download_fence_wait.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_download_map(void) noexcept
{
    fail_next_download_map.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_fence_wait(void) noexcept
{
    fail_next_fence_wait.store(true, std::memory_order_relaxed);
}

pano_gpu_result pano_gpu_test_convert_linear_srgb(
    const pano_gpu_session *const session, const float *const linear_rgb,
    const uint32_t pixel_count, float *const normalized_srgb, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    uint64_t value_count = 0;
    uint64_t bytes = 0;
    if (session == nullptr || linear_rgb == nullptr || normalized_srgb == nullptr || pixel_count == 0 ||
        !checked_multiply(pixel_count, 3, &value_count) ||
        !checked_multiply(value_count, sizeof(float), &bytes))
    {
        write_error(error_buffer, error_buffer_size, "invalid linear-sRGB conversion request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    for (uint64_t index = 0; index < value_count; ++index)
        if (!std::isfinite(linear_rgb[index]))
        {
            write_error(error_buffer, error_buffer_size, "linear-sRGB conversion requires finite input");
            return PANO_GPU_INVALID_ARGUMENT;
        }
#if !defined(_WIN32)
    (void)bytes;
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const auto buffer = [](const uint64_t width, const D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC description {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = width;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags = flags;
        return description;
    };
    D3D12_HEAP_PROPERTIES upload_heap {}, default_heap {}, readback_heap {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    const D3D12_RESOURCE_DESC input_description = buffer(bytes, D3D12_RESOURCE_FLAG_NONE);
    const D3D12_RESOURCE_DESC output_description =
        buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    Microsoft::WRL::ComPtr<ID3D12Resource> input, output, readback;
    if (FAILED(device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &input_description,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&input))) ||
        FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &output_description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output))) ||
        FAILED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &input_description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
        return PANO_GPU_UNAVAILABLE;
    void *mapped = nullptr;
    if (FAILED(input->Map(0, nullptr, &mapped)))
        return PANO_GPU_UNAVAILABLE;
    std::memcpy(mapped, linear_rgb, static_cast<size_t>(bytes));
    input->Unmap(0, nullptr);
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 1};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    const D3D12_ROOT_SIGNATURE_DESC root_description {
        2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root.Get();
    pipeline_description.CS = {
        pano_gpu_convert_linear_srgb_shader, sizeof(pano_gpu_convert_linear_srgb_shader)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 2;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto descriptor = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.Buffer.NumElements = pixel_count;
    srv.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateShaderResourceView(input.Get(), &srv, descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.Buffer.NumElements = pixel_count;
    uav.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, descriptor);
    ID3D12DescriptorHeap *heaps[] {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRoot32BitConstant(0, pixel_count, 0);
    list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((pixel_count + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER transition {};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition = {output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &transition);
    list->CopyResource(readback.Get(), output.Get());
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            session->device_core.get(), fence, error_buffer, error_buffer_size,
            "linear-sRGB conversion timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    const D3D12_RANGE range {0, static_cast<SIZE_T>(bytes)};
    if (FAILED(readback->Map(0, &range, &mapped)))
        return PANO_GPU_UNAVAILABLE;
    std::memcpy(normalized_srgb, mapped, static_cast<size_t>(bytes));
    readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_build_linear_srgb_histogram(
    const pano_gpu_session *const session, const float *const linear_rgb,
    const uint8_t *const coverage, const uint32_t pixel_count, uint32_t *const histogram,
    const uint64_t histogram_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    constexpr uint64_t expected_histogram_bytes = 4096ULL * sizeof(uint32_t);
    uint64_t color_bytes = 0;
    if (session == nullptr || linear_rgb == nullptr || coverage == nullptr || histogram == nullptr ||
        pixel_count == 0 || histogram_bytes != expected_histogram_bytes ||
        !checked_multiply(pixel_count, 3 * sizeof(float), &color_bytes))
    {
        write_error(error_buffer, error_buffer_size, "invalid linear-sRGB histogram request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    (void)color_bytes;
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const auto description = [](const uint64_t bytes, const D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC value {};
        value.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        value.Width = bytes;
        value.Height = 1;
        value.DepthOrArraySize = 1;
        value.MipLevels = 1;
        value.SampleDesc.Count = 1;
        value.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        value.Flags = flags;
        return value;
    };
    D3D12_HEAP_PROPERTIES upload_heap {}, default_heap {}, readback_heap {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    const auto make_resource = [&](const D3D12_HEAP_PROPERTIES &heap, const uint64_t bytes,
                                   const D3D12_RESOURCE_FLAGS flags,
                                   const D3D12_RESOURCE_STATES state,
                                   Microsoft::WRL::ComPtr<ID3D12Resource> *const resource) {
        const D3D12_RESOURCE_DESC resource_description = description(bytes, flags);
        return SUCCEEDED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &resource_description, state, nullptr,
            IID_PPV_ARGS(resource->ReleaseAndGetAddressOf())));
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> color_input, coverage_input, histogram_resource, readback;
    if (!make_resource(
            upload_heap, color_bytes, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_GENERIC_READ, &color_input) ||
        !make_resource(
            upload_heap, pixel_count, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_GENERIC_READ, &coverage_input) ||
        !make_resource(
            default_heap, expected_histogram_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &histogram_resource) ||
        !make_resource(
            readback_heap, expected_histogram_bytes, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST, &readback))
        return PANO_GPU_UNAVAILABLE;
    const auto upload = [](ID3D12Resource *const resource, const void *const source,
                           const size_t bytes) {
        void *mapped = nullptr;
        if (FAILED(resource->Map(0, nullptr, &mapped)))
            return false;
        std::memcpy(mapped, source, bytes);
        resource->Unmap(0, nullptr);
        return true;
    };
    if (!upload(color_input.Get(), linear_rgb, static_cast<size_t>(color_bytes)) ||
        !upload(coverage_input.Get(), coverage, pixel_count))
        return PANO_GPU_UNAVAILABLE;
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 1};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    const D3D12_ROOT_SIGNATURE_DESC root_description {
        2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root.Get();
    pipeline_description.CS = {pano_gpu_auto_contrast_histogram_srgb_shader,
                               sizeof(pano_gpu_auto_contrast_histogram_srgb_shader)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 3;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto descriptor = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC color_srv {};
    color_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    color_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    color_srv.Format = DXGI_FORMAT_UNKNOWN;
    color_srv.Buffer.NumElements = pixel_count;
    color_srv.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateShaderResourceView(color_input.Get(), &color_srv, descriptor);
    descriptor.ptr += increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC coverage_srv {};
    coverage_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    coverage_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    coverage_srv.Format = DXGI_FORMAT_R8_UINT;
    coverage_srv.Buffer.NumElements = pixel_count;
    device->CreateShaderResourceView(coverage_input.Get(), &coverage_srv, descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC histogram_uav {};
    histogram_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    histogram_uav.Format = DXGI_FORMAT_UNKNOWN;
    histogram_uav.Buffer.NumElements = 4096;
    histogram_uav.Buffer.StructureByteStride = sizeof(uint32_t);
    device->CreateUnorderedAccessView(
        histogram_resource.Get(), nullptr, &histogram_uav, descriptor);
    ID3D12DescriptorHeap *heaps[] {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    D3D12_GPU_DESCRIPTOR_HANDLE histogram_gpu = heap->GetGPUDescriptorHandleForHeapStart();
    histogram_gpu.ptr += 2ULL * increment;
    const UINT clear[4] {};
    list->ClearUnorderedAccessViewUint(
        histogram_gpu, descriptor, histogram_resource.Get(), clear, 0, nullptr);
    D3D12_RESOURCE_BARRIER ordering {};
    ordering.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ordering.UAV.pResource = histogram_resource.Get();
    list->ResourceBarrier(1, &ordering);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRoot32BitConstant(0, pixel_count, 0);
    list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((pixel_count + 63) / 64, 1, 1);
    ordering.UAV.pResource = histogram_resource.Get();
    list->ResourceBarrier(1, &ordering);
    D3D12_RESOURCE_BARRIER transition {};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition = {histogram_resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &transition);
    list->CopyResource(readback.Get(), histogram_resource.Get());
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            session->device_core.get(), fence, error_buffer, error_buffer_size,
            "linear-sRGB histogram timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    const D3D12_RANGE range {0, static_cast<SIZE_T>(expected_histogram_bytes)};
    void *mapped = nullptr;
    if (FAILED(readback->Map(0, &range, &mapped)))
        return PANO_GPU_UNAVAILABLE;
    std::memcpy(histogram, mapped, static_cast<size_t>(expected_histogram_bytes));
    readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_upload_output_histogram_band(
    pano_gpu_output *const output, const float *const linear_rgb,
    const uint64_t linear_rgb_bytes, const uint8_t *const coverage,
    const uint64_t coverage_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    const uint32_t rows = output == nullptr
        ? 0
        : (output->output_band_rows == 0 ? output->output_height : output->band_row_count);
    uint64_t pixels = 0;
    uint64_t expected_linear_bytes = 0;
    if (output == nullptr || linear_rgb == nullptr || coverage == nullptr || rows == 0 ||
        !checked_multiply(output->output_width, rows, &pixels) ||
        !checked_multiply(pixels, 3 * sizeof(float), &expected_linear_bytes) ||
        linear_rgb_bytes != expected_linear_bytes || coverage_bytes != pixels)
        return PANO_GPU_INVALID_ARGUMENT;
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!output->linear || !output->coverage)
        return PANO_GPU_INVALID_ARGUMENT;
    ID3D12Device *const device = output->session->device_core->d3d_device.Get();
    const auto create_upload = [&](const uint64_t bytes, const void *const source,
                                   Microsoft::WRL::ComPtr<ID3D12Resource> *const resource) {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC description {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = bytes;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(resource->ReleaseAndGetAddressOf()))))
            return false;
        void *mapped = nullptr;
        if (FAILED((*resource)->Map(0, nullptr, &mapped)))
            return false;
        std::memcpy(mapped, source, static_cast<size_t>(bytes));
        (*resource)->Unmap(0, nullptr);
        return true;
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> linear_upload, coverage_upload;
    if (!create_upload(expected_linear_bytes, linear_rgb, &linear_upload) ||
        !create_upload(coverage_bytes, coverage, &coverage_upload))
        return PANO_GPU_UNAVAILABLE;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    list->CopyBufferRegion(output->linear.Get(), 0, linear_upload.Get(), 0, expected_linear_bytes);
    list->CopyBufferRegion(output->coverage.Get(), 0, coverage_upload.Get(), 0, coverage_bytes);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    output->session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        output->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(output->session->device_core->queue->Signal(
            output->session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            output->session->device_core.get(), fence, error_buffer, error_buffer_size,
            "output histogram band upload timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_read_output_histogram(
    pano_gpu_output *const output, uint32_t *const histogram,
    const uint64_t histogram_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (output == nullptr || histogram == nullptr || histogram_bytes != output->histogram_bytes ||
        histogram_bytes != 4096ULL * sizeof(uint32_t))
        return PANO_GPU_INVALID_ARGUMENT;
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!output->histogram)
        return PANO_GPU_INVALID_ARGUMENT;
    ID3D12Device *const device = output->session->device_core->d3d_device.Get();
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC description {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = histogram_bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_RESOURCE_BARRIER transition {};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition = {output->histogram.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &transition);
    list->CopyResource(readback.Get(), output->histogram.Get());
    std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
    list->ResourceBarrier(1, &transition);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    output->session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        output->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(output->session->device_core->queue->Signal(
            output->session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            output->session->device_core.get(), fence, error_buffer, error_buffer_size,
            "output histogram readback timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    void *mapped = nullptr;
    const D3D12_RANGE range {0, static_cast<SIZE_T>(histogram_bytes)};
    if (FAILED(readback->Map(0, &range, &mapped)))
        return PANO_GPU_UNAVAILABLE;
    std::memcpy(histogram, mapped, static_cast<size_t>(histogram_bytes));
    readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_read_output_normalized_srgb(
    pano_gpu_output *const output, float *const normalized_srgb,
    const uint64_t normalized_srgb_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (output == nullptr || normalized_srgb == nullptr || output->normalized_srgb_bytes == 0 ||
        normalized_srgb_bytes != output->normalized_srgb_bytes)
        return PANO_GPU_INVALID_ARGUMENT;
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = output->session->device_core->d3d_device.Get();
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC description {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = normalized_srgb_bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_RESOURCE_BARRIER transition {};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition = {output->normalized_srgb.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &transition);
    list->CopyBufferRegion(
        readback.Get(), 0, output->normalized_srgb.Get(), 0, normalized_srgb_bytes);
    std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
    list->ResourceBarrier(1, &transition);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    output->session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        output->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(output->session->device_core->queue->Signal(
            output->session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            output->session->device_core.get(), fence, error_buffer, error_buffer_size,
            "normalized-sRGB readback timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    void *mapped = nullptr;
    const D3D12_RANGE range {0, static_cast<SIZE_T>(normalized_srgb_bytes)};
    if (FAILED(readback->Map(0, &range, &mapped)))
        return PANO_GPU_UNAVAILABLE;
    std::memcpy(normalized_srgb, mapped, static_cast<size_t>(normalized_srgb_bytes));
    readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_read_output_srgb8(
    pano_gpu_output *const output, uint8_t *const srgb, const uint64_t srgb_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (output == nullptr || srgb == nullptr || output->quantized_srgb_bytes == 0 ||
        srgb_bytes != output->quantized_srgb_bytes)
        return PANO_GPU_INVALID_ARGUMENT;
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = output->session->device_core->d3d_device.Get();
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC description {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = srgb_bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_RESOURCE_BARRIER transition {};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition = {output->quantized_srgb.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &transition);
    list->CopyBufferRegion(readback.Get(), 0, output->quantized_srgb.Get(), 0, srgb_bytes);
    std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
    list->ResourceBarrier(1, &transition);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    output->session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        output->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(output->session->device_core->queue->Signal(
            output->session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            output->session->device_core.get(), fence, error_buffer, error_buffer_size,
            "sRGB8 readback timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    void *mapped = nullptr;
    const D3D12_RANGE range {0, static_cast<SIZE_T>(srgb_bytes)};
    if (FAILED(readback->Map(0, &range, &mapped)))
        return PANO_GPU_UNAVAILABLE;
    std::memcpy(srgb, mapped, static_cast<size_t>(srgb_bytes));
    readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_read_output_tone_mapped_rec2020(
    pano_gpu_output *const output, float *const tone_mapped_rec2020,
    const uint64_t tone_mapped_rec2020_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (output == nullptr || tone_mapped_rec2020 == nullptr ||
        output->tone_mapped_rec2020_bytes == 0 ||
        tone_mapped_rec2020_bytes != output->tone_mapped_rec2020_bytes)
        return PANO_GPU_INVALID_ARGUMENT;
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = output->session->device_core->d3d_device.Get();
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC description {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = tone_mapped_rec2020_bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_RESOURCE_BARRIER transition {};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition = {output->tone_mapped_rec2020.Get(),
                             D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &transition);
    list->CopyBufferRegion(
        readback.Get(), 0, output->tone_mapped_rec2020.Get(), 0,
        tone_mapped_rec2020_bytes);
    std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
    list->ResourceBarrier(1, &transition);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    output->session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        output->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(output->session->device_core->queue->Signal(
            output->session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            output->session->device_core.get(), fence, error_buffer, error_buffer_size,
            "tone-mapped Rec.2020 readback timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    void *mapped = nullptr;
    const D3D12_RANGE range {0, static_cast<SIZE_T>(tone_mapped_rec2020_bytes)};
    if (FAILED(readback->Map(0, &range, &mapped)))
        return PANO_GPU_UNAVAILABLE;
    std::memcpy(tone_mapped_rec2020, mapped, static_cast<size_t>(tone_mapped_rec2020_bytes));
    readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_read_output_linear_srgb(
    pano_gpu_output *const output, float *const linear_srgb, const uint64_t linear_srgb_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (output == nullptr || linear_srgb == nullptr || output->converted_linear_srgb_bytes == 0 ||
        linear_srgb_bytes != output->converted_linear_srgb_bytes)
        return PANO_GPU_INVALID_ARGUMENT;
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = output->session->device_core->d3d_device.Get();
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC description {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = linear_srgb_bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_RESOURCE_BARRIER transition {};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition = {output->converted_linear_srgb.Get(),
                             D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &transition);
    list->CopyBufferRegion(
        readback.Get(), 0, output->converted_linear_srgb.Get(), 0, linear_srgb_bytes);
    std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
    list->ResourceBarrier(1, &transition);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    output->session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        output->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(output->session->device_core->queue->Signal(
            output->session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            output->session->device_core.get(), fence, error_buffer, error_buffer_size,
            "linear-sRGB readback timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    void *mapped = nullptr;
    const D3D12_RANGE range {0, static_cast<SIZE_T>(linear_srgb_bytes)};
    if (FAILED(readback->Map(0, &range, &mapped)))
        return PANO_GPU_UNAVAILABLE;
    std::memcpy(linear_srgb, mapped, static_cast<size_t>(linear_srgb_bytes));
    readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_read_output_float(
    pano_gpu_output *const output, float *const linear_rgb, const uint64_t linear_rgb_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (output == nullptr || linear_rgb == nullptr || output->float_output_bytes == 0 ||
        linear_rgb_bytes != output->float_output_bytes)
        return PANO_GPU_INVALID_ARGUMENT;
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = output->session->device_core->d3d_device.Get();
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC description {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = linear_rgb_bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_RESOURCE_BARRIER transition {};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition = {output->float_output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &transition);
    list->CopyBufferRegion(readback.Get(), 0, output->float_output.Get(), 0, linear_rgb_bytes);
    std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
    list->ResourceBarrier(1, &transition);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    output->session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        output->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(output->session->device_core->queue->Signal(
            output->session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            output->session->device_core.get(), fence, error_buffer, error_buffer_size,
            "float-output readback timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    void *mapped = nullptr;
    const D3D12_RANGE range {0, static_cast<SIZE_T>(linear_rgb_bytes)};
    if (FAILED(readback->Map(0, &range, &mapped)))
        return PANO_GPU_UNAVAILABLE;
    std::memcpy(linear_rgb, mapped, static_cast<size_t>(linear_rgb_bytes));
    readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_read_preview_retained(
    pano_gpu_preview *const preview, uint8_t *const preview_rgb8,
    const uint64_t preview_rgb8_bytes, uint8_t *const overview_rgb8,
    const uint64_t overview_rgb8_bytes, uint8_t *const compact_masks,
    const uint64_t compact_mask_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    uint64_t total_bytes = 0;
    if (preview == nullptr || preview_rgb8 == nullptr || overview_rgb8 == nullptr ||
        compact_masks == nullptr || preview_rgb8_bytes != preview->preview_rgb8_bytes ||
        overview_rgb8_bytes != preview->overview_rgb8_bytes ||
        compact_mask_bytes != preview->compact_mask_bytes ||
        !checked_add(preview_rgb8_bytes, overview_rgb8_bytes, &total_bytes) ||
        !checked_add(total_bytes, compact_mask_bytes, &total_bytes))
        return PANO_GPU_INVALID_ARGUMENT;
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = preview->session->device_core->d3d_device.Get();
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC description {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = total_bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    ID3D12Resource *resources[] {
        preview->preview_rgb8.Get(), preview->overview_rgb8.Get(), preview->compact_masks.Get()};
    D3D12_RESOURCE_BARRIER transitions[3] {};
    for (size_t index = 0; index < 3; ++index)
    {
        transitions[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        transitions[index].Transition = {
            resources[index], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE};
    }
    list->ResourceBarrier(3, transitions);
    list->CopyBufferRegion(readback.Get(), 0, resources[0], 0, preview_rgb8_bytes);
    list->CopyBufferRegion(
        readback.Get(), preview_rgb8_bytes, resources[1], 0, overview_rgb8_bytes);
    list->CopyBufferRegion(
        readback.Get(), preview_rgb8_bytes + overview_rgb8_bytes, resources[2], 0,
        compact_mask_bytes);
    for (D3D12_RESOURCE_BARRIER &transition : transitions)
        std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
    list->ResourceBarrier(3, transitions);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    preview->session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence =
        preview->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(preview->session->device_core->queue->Signal(
            preview->session->device_core->fence.Get(), fence)) ||
        wait_for_fence(
            preview->session->device_core.get(), fence, error_buffer, error_buffer_size,
            "retained preview readback timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    void *mapped = nullptr;
    const D3D12_RANGE range {0, static_cast<SIZE_T>(total_bytes)};
    if (FAILED(readback->Map(0, &range, &mapped)))
        return PANO_GPU_UNAVAILABLE;
    const uint8_t *const bytes = static_cast<const uint8_t *>(mapped);
    std::memcpy(preview_rgb8, bytes, static_cast<size_t>(preview_rgb8_bytes));
    std::memcpy(
        overview_rgb8, bytes + preview_rgb8_bytes, static_cast<size_t>(overview_rgb8_bytes));
    std::memcpy(
        compact_masks, bytes + preview_rgb8_bytes + overview_rgb8_bytes,
        static_cast<size_t>(compact_mask_bytes));
    readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_replace_exposure_graph(
    pano_gpu_session *const session, const pano_gpu_exposure_pair_report *const reports,
    const uint32_t report_count, const pano_gpu_exposure_equation *const equations,
    const uint32_t equation_count, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || report_count != session->exposure_pair_capacity ||
        (report_count != 0 && reports == nullptr) || (equation_count != 0 && equations == nullptr))
    {
        write_error(error_buffer, error_buffer_size, "invalid test exposure graph");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    for (uint32_t index = 0; index < report_count; ++index)
        if (reports[index].left_frame_index >= session->frame_count ||
            reports[index].right_frame_index >= session->frame_count ||
            reports[index].left_frame_index >= reports[index].right_frame_index)
        {
            write_error(error_buffer, error_buffer_size, "invalid test exposure report");
            return PANO_GPU_INVALID_ARGUMENT;
        }
    for (uint32_t index = 0; index < equation_count; ++index)
        if (equations[index].left_frame_index >= session->frame_count ||
            equations[index].right_frame_index >= session->frame_count ||
            equations[index].left_frame_index == equations[index].right_frame_index ||
            !std::isfinite(equations[index].difference) || !std::isfinite(equations[index].weight) ||
            equations[index].weight <= 0.0)
        {
            write_error(error_buffer, error_buffer_size, "invalid test exposure equation");
            return PANO_GPU_INVALID_ARGUMENT;
        }
    try
    {
        std::vector<pano_gpu_exposure_pair_report> replacement_reports;
        std::vector<pano_gpu_exposure_equation> replacement_equations;
        if (report_count != 0)
            replacement_reports.assign(reports, reports + report_count);
        if (equation_count != 0)
            replacement_equations.assign(equations, equations + equation_count);
        session->exposure_pair_reports.swap(replacement_reports);
        session->exposure_equations.swap(replacement_equations);
        session->exposure_solve_equations.clear();
        session->exposure_solve_graph_ready = false;
        session->exposure_log_gains.clear();
        session->exposure_solved = false;
        session->exposure_global_gains.clear();
        session->exposure_gains_uploaded = false;
        return PANO_GPU_SUCCESS;
    }
    catch (const std::bad_alloc &)
    {
        write_error(error_buffer, error_buffer_size, "cannot allocate test exposure graph");
        return PANO_GPU_OUT_OF_MEMORY;
    }
}

void pano_gpu_test_fail_next_session_allocation(void) noexcept
{
    fail_next_session_allocation.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_output_allocation(void) noexcept
{
    fail_next_output_allocation.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_preview_allocation(void) noexcept
{
    fail_next_preview_allocation.store(true, std::memory_order_relaxed);
}

uint32_t pano_gpu_test_live_preview_count(void) noexcept
{
    return live_preview_count.load(std::memory_order_relaxed);
}

int32_t pano_gpu_test_claim_preview_rendering(pano_gpu_preview *const preview) noexcept
{
    return preview != nullptr && !preview->rendering.exchange(true, std::memory_order_acq_rel);
}

void pano_gpu_test_release_preview_rendering(pano_gpu_preview *const preview) noexcept
{
    if (preview != nullptr)
        preview->rendering.store(false, std::memory_order_release);
}

pano_gpu_result pano_gpu_test_validate_projection_request(
    const pano_gpu_session *const session, const pano_gpu_projection_request *const request,
    pano_gpu_projection_result_layout *const layout, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (layout == nullptr || layout->size != sizeof(*layout) || layout->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 projection result layout");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    std::memset(layout, 0, sizeof(*layout));
    layout->size = sizeof(*layout);
    layout->abi_version = PANO_GPU_ABI_VERSION;
    if (session == nullptr || session->source_width == 0 || session->source_height == 0 || request == nullptr ||
        request->size != sizeof(*request) || request->abi_version != PANO_GPU_ABI_VERSION ||
        request->output_width == 0 || request->output_height == 0 || request->row_count == 0 ||
        request->row_start > request->output_height || request->row_count > request->output_height - request->row_start ||
        !(request->latitude_span_degrees > 0.0F && request->latitude_span_degrees <= 180.0F) ||
        !(request->horizontal_fov_degrees > 0.0F && request->horizontal_fov_degrees < 180.0F) ||
        !(request->vertical_fov_degrees > 0.0F && request->vertical_fov_degrees < 180.0F))
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 projection request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    for (const float value : request->world_to_camera)
    {
        if (!std::isfinite(value))
        {
            write_error(error_buffer, error_buffer_size, "invalid D3D12 projection rotation");
            return PANO_GPU_INVALID_ARGUMENT;
        }
    }
    uint64_t pixel_count = 0;
    uint64_t ray_bytes = 0;
    uint64_t projected_coordinate_bytes = 0;
    if (!checked_multiply(request->output_width, request->row_count, &pixel_count) ||
        pixel_count > std::numeric_limits<uint32_t>::max() ||
        !checked_multiply(pixel_count, 3 * sizeof(float), &ray_bytes) ||
        !checked_multiply(pixel_count, 2 * sizeof(float), &projected_coordinate_bytes))
    {
        write_error(error_buffer, error_buffer_size, "D3D12 projection result layout overflows");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    layout->pixel_count = pixel_count;
    layout->world_ray_bytes = ray_bytes;
    layout->camera_ray_bytes = ray_bytes;
    layout->projected_coordinate_bytes = projected_coordinate_bytes;
    layout->validity_bytes = pixel_count;
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_test_dispatch_rays(
    const pano_gpu_session *const session, const pano_gpu_projection_request *const request,
    void *const world_rays, const uint64_t world_ray_bytes, void *const projected_coordinates,
    const uint64_t projected_coordinate_bytes, void *const validity, const uint64_t validity_bytes,
    char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    pano_gpu_projection_result_layout layout {};
    layout.size = sizeof(layout);
    layout.abi_version = PANO_GPU_ABI_VERSION;
    const pano_gpu_result validation_result =
        pano_gpu_test_validate_projection_request(session, request, &layout, error_buffer, error_buffer_size);
    if (validation_result != PANO_GPU_SUCCESS)
        return validation_result;
    if ((world_rays == nullptr) != (world_ray_bytes == 0) ||
        (world_rays != nullptr && world_ray_bytes != layout.world_ray_bytes))
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 ray readback buffer");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    if ((projected_coordinates == nullptr) != (projected_coordinate_bytes == 0) ||
        (projected_coordinates != nullptr && projected_coordinate_bytes != layout.projected_coordinate_bytes))
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 projected-coordinate readback buffer");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    if ((validity == nullptr) != (validity_bytes == 0) ||
        (validity != nullptr && validity_bytes != layout.validity_bytes))
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 validity readback buffer");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    D3D12_DESCRIPTOR_RANGE range {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 24};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &range};
    D3D12_ROOT_SIGNATURE_DESC root_description {2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)))
    {
        write_error(error_buffer, error_buffer_size, "cannot serialize D3D12 ray root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root_signature.Get();
    pipeline_description.CS = {pano_gpu_ray_shader, sizeof(pano_gpu_ray_shader)};
    if (FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root_signature))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 ray root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    pipeline_description.pRootSignature = root_signature.Get();
    const HRESULT pipeline_result = device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline));
    if (FAILED(pipeline_result))
    {
        write_device_error(
            error_buffer, error_buffer_size, "cannot create D3D12 ray pipeline", pipeline_result, device);
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES default_heap {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC output_buffer {};
    output_buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    output_buffer.Width = layout.world_ray_bytes;
    output_buffer.Height = 1;
    output_buffer.DepthOrArraySize = 1;
    output_buffer.MipLevels = 1;
    output_buffer.SampleDesc.Count = 1;
    output_buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    output_buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D12Resource> output;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Microsoft::WRL::ComPtr<ID3D12Resource> projected_coordinate_output;
    Microsoft::WRL::ComPtr<ID3D12Resource> projected_coordinate_readback;
    Microsoft::WRL::ComPtr<ID3D12Resource> validity_output;
    Microsoft::WRL::ComPtr<ID3D12Resource> validity_readback;
    if (FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &output_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&output))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 ray output");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_RESOURCE_DESC projected_coordinate_buffer = output_buffer;
    projected_coordinate_buffer.Width = layout.projected_coordinate_bytes;
    if (FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &projected_coordinate_buffer,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&projected_coordinate_output))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 projected-coordinate output");
        return PANO_GPU_UNAVAILABLE;
    }
    const uint64_t validity_output_bytes = ((layout.pixel_count + 31) / 32) * sizeof(uint32_t);
    D3D12_RESOURCE_DESC validity_buffer = output_buffer;
    validity_buffer.Width = validity_output_bytes;
    if (FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &validity_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&validity_output))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 validity output");
        return PANO_GPU_UNAVAILABLE;
    }
    if (world_rays != nullptr)
    {
        D3D12_HEAP_PROPERTIES readback_heap {};
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC readback_description = output_buffer;
        readback_description.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(device->CreateCommittedResource(
                &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_description, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&readback))))
        {
            write_error(error_buffer, error_buffer_size, "cannot create D3D12 ray readback");
            return PANO_GPU_UNAVAILABLE;
        }
    }
    if (projected_coordinates != nullptr)
    {
        D3D12_HEAP_PROPERTIES readback_heap {};
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC readback_description = projected_coordinate_buffer;
        readback_description.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(device->CreateCommittedResource(
                &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_description, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&projected_coordinate_readback))))
        {
            write_error(error_buffer, error_buffer_size, "cannot create D3D12 projected-coordinate readback");
            return PANO_GPU_UNAVAILABLE;
        }
    }
    if (validity != nullptr)
    {
        D3D12_HEAP_PROPERTIES readback_heap {};
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC readback_description = validity_buffer;
        readback_description.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(device->CreateCommittedResource(
                &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_description, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&validity_readback))))
        {
            write_error(error_buffer, error_buffer_size, "cannot create D3D12 validity readback");
            return PANO_GPU_UNAVAILABLE;
        }
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 3;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&descriptor_heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 ray dispatch resources");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.Buffer.NumElements = static_cast<UINT>(layout.pixel_count);
    uav.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, descriptor_heap->GetCPUDescriptorHandleForHeapStart());
    uav.Buffer.StructureByteStride = 2 * sizeof(float);
    const UINT descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE projected_coordinate_handle = descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    projected_coordinate_handle.ptr += descriptor_size;
    device->CreateUnorderedAccessView(projected_coordinate_output.Get(), nullptr, &uav, projected_coordinate_handle);
    D3D12_UNORDERED_ACCESS_VIEW_DESC raw_uav {};
    raw_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    raw_uav.Format = DXGI_FORMAT_R32_TYPELESS;
    raw_uav.Buffer.NumElements = static_cast<UINT>(validity_output_bytes / sizeof(uint32_t));
    raw_uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    D3D12_CPU_DESCRIPTOR_HANDLE validity_handle = projected_coordinate_handle;
    validity_handle.ptr += descriptor_size;
    device->CreateUnorderedAccessView(validity_output.Get(), nullptr, &raw_uav, validity_handle);
    uint32_t constants[24] {request->output_width, request->output_height, request->row_start, request->row_count, 0};
    std::memcpy(&constants[4], &request->latitude_span_degrees, sizeof(constants[4]));
    std::memcpy(&constants[8], request->world_to_camera, 3 * sizeof(float));
    std::memcpy(&constants[12], request->world_to_camera + 3, 3 * sizeof(float));
    std::memcpy(&constants[16], request->world_to_camera + 6, 3 * sizeof(float));
    const float source_camera[4] {
        static_cast<float>(session->source_width),
        static_cast<float>(session->source_height),
        session->source_width / (2.0F * std::tan(request->horizontal_fov_degrees * 0.00872664625997165F)),
        session->source_height / (2.0F * std::tan(request->vertical_fov_degrees * 0.00872664625997165F)),
    };
    std::memcpy(&constants[20], source_camera, sizeof(source_camera));
    ID3D12DescriptorHeap *heaps[] = {descriptor_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    const UINT clear_values[4] {};
    D3D12_GPU_DESCRIPTOR_HANDLE validity_gpu_handle = descriptor_heap->GetGPUDescriptorHandleForHeapStart();
    validity_gpu_handle.ptr += 2 * descriptor_size;
    list->ClearUnorderedAccessViewUint(validity_gpu_handle, validity_handle, validity_output.Get(), clear_values, 0, nullptr);
    list->SetComputeRootSignature(root_signature.Get());
    list->SetComputeRoot32BitConstants(0, 24, constants, 0);
    list->SetComputeRootDescriptorTable(1, descriptor_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((request->output_width + 7) / 8, (request->row_count + 7) / 8, 1);
    if (readback)
    {
        D3D12_RESOURCE_BARRIER to_copy {};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition.pResource = output.Get();
        to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &to_copy);
        list->CopyResource(readback.Get(), output.Get());
    }
    if (projected_coordinate_readback)
    {
        D3D12_RESOURCE_BARRIER to_copy {};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition.pResource = projected_coordinate_output.Get();
        to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &to_copy);
        list->CopyResource(projected_coordinate_readback.Get(), projected_coordinate_output.Get());
    }
    if (validity_readback)
    {
        D3D12_RESOURCE_BARRIER to_copy {};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition.pResource = validity_output.Get();
        to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &to_copy);
        list->CopyResource(validity_readback.Get(), validity_output.Get());
    }
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 ray command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] = {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value = session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)))
    {
        write_error(error_buffer, error_buffer_size, "cannot signal D3D12 ray fence");
        return PANO_GPU_UNAVAILABLE;
    }
    const pano_gpu_result wait_result =
        wait_for_fence(session->device_core.get(), fence_value, error_buffer, error_buffer_size, "D3D12 ray fence timed out");
    if (wait_result != PANO_GPU_SUCCESS)
        return wait_result;
    if (readback)
    {
        const D3D12_RANGE read_range {0, static_cast<SIZE_T>(world_ray_bytes)};
        void *mapped = nullptr;
        if (FAILED(readback->Map(0, &read_range, &mapped)))
        {
            write_error(error_buffer, error_buffer_size, "cannot map D3D12 ray readback");
            return PANO_GPU_UNAVAILABLE;
        }
        std::memcpy(world_rays, mapped, static_cast<size_t>(world_ray_bytes));
        readback->Unmap(0, nullptr);
    }
    if (projected_coordinate_readback)
    {
        const D3D12_RANGE read_range {0, static_cast<SIZE_T>(projected_coordinate_bytes)};
        void *mapped = nullptr;
        if (FAILED(projected_coordinate_readback->Map(0, &read_range, &mapped)))
        {
            write_error(error_buffer, error_buffer_size, "cannot map D3D12 projected-coordinate readback");
            return PANO_GPU_UNAVAILABLE;
        }
        std::memcpy(projected_coordinates, mapped, static_cast<size_t>(projected_coordinate_bytes));
        projected_coordinate_readback->Unmap(0, nullptr);
    }
    if (validity_readback)
    {
        const D3D12_RANGE read_range {0, static_cast<SIZE_T>(validity_output_bytes)};
        void *mapped = nullptr;
        if (FAILED(validity_readback->Map(0, &read_range, &mapped)))
        {
            write_error(error_buffer, error_buffer_size, "cannot map D3D12 validity readback");
            return PANO_GPU_UNAVAILABLE;
        }
        const uint32_t *const words = static_cast<const uint32_t *>(mapped);
        uint8_t *const values = static_cast<uint8_t *>(validity);
        for (uint64_t index = 0; index < layout.pixel_count; ++index)
            values[index] = static_cast<uint8_t>((words[index / 32] >> (index & 31)) & 1U);
        validity_readback->Unmap(0, nullptr);
    }
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_validate_one_frame_composite_request(
    const pano_gpu_session *const session, const pano_gpu_one_frame_composite_request *const request,
    pano_gpu_one_frame_composite_result_layout *const layout, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (layout == nullptr || layout->size != sizeof(*layout) || layout->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 one-frame composite result layout");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    std::memset(layout, 0, sizeof(*layout));
    layout->size = sizeof(*layout);
    layout->abi_version = PANO_GPU_ABI_VERSION;
    if (session == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->frame_index >= session->frame_count ||
        request->source_sample_type != session->source_sample_type || request->output_width == 0 ||
        request->output_height == 0 || request->row_count == 0 || request->row_start >= request->output_height ||
        request->row_count > request->output_height - request->row_start || request->rectilinear_output > 1 ||
        !std::isfinite(request->latitude_span_degrees) ||
        !std::isfinite(request->horizontal_fov_degrees) || !std::isfinite(request->vertical_fov_degrees) ||
        (request->rectilinear_output != 0 && (!std::isfinite(request->output_vertical_fov_degrees) ||
                                             request->output_vertical_fov_degrees <= 0.0F ||
                                             request->output_vertical_fov_degrees >= 180.0F)) ||
        request->latitude_span_degrees <= 0.0F || request->horizontal_fov_degrees <= 0.0F ||
        request->vertical_fov_degrees <= 0.0F || session->source_bytes > std::numeric_limits<uint32_t>::max() ||
        session->source_frame_bytes > std::numeric_limits<uint32_t>::max() ||
        static_cast<uint64_t>(request->frame_index) * session->source_frame_bytes >
            std::numeric_limits<uint32_t>::max())
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 one-frame composite request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    for (const float value : request->world_to_camera)
    {
        if (!std::isfinite(value))
        {
            write_error(error_buffer, error_buffer_size, "invalid D3D12 one-frame composite rotation");
            return PANO_GPU_INVALID_ARGUMENT;
        }
    }
    uint64_t pixel_count = 0;
    uint64_t linear_rgb_bytes = 0;
    if (!checked_multiply(request->output_width, request->row_count, &pixel_count) ||
        pixel_count > std::numeric_limits<uint32_t>::max() ||
        !checked_multiply(pixel_count, 3 * sizeof(float), &linear_rgb_bytes))
    {
        write_error(error_buffer, error_buffer_size, "D3D12 one-frame composite layout overflows");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->fence || !session->source ||
        request->frame_index >= session->frame_upload_fences.size())
    {
        write_error(error_buffer, error_buffer_size, "D3D12 one-frame composite source is not ready");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    const uint64_t frame_fence = session->frame_upload_fences[request->frame_index];
    if (frame_fence == 0 || session->device_core->fence->GetCompletedValue() < frame_fence)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 one-frame composite source upload is unfinished");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    layout->pixel_count = pixel_count;
    layout->linear_rgb_bytes = linear_rgb_bytes;
    layout->coverage_bytes = pixel_count;
    layout->candidate_edge_distance_bytes = pixel_count * sizeof(float);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_validate_ordered_hard_composite_request(
    const pano_gpu_session *const session, const pano_gpu_ordered_hard_composite_request *const request,
    pano_gpu_ordered_hard_composite_result_layout *const layout, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (layout == nullptr || layout->size != sizeof(*layout) || layout->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 ordered hard-composite result layout");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    std::memset(layout, 0, sizeof(*layout));
    layout->size = sizeof(*layout);
    layout->abi_version = PANO_GPU_ABI_VERSION;
    if (session == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->frame_request_count < 2 ||
        request->frame_request_count > session->frame_count || request->reserved != 0 || request->frame_requests == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 ordered hard-composite request");
        return PANO_GPU_INVALID_ARGUMENT;
    }

    pano_gpu_one_frame_composite_result_layout first_layout {};
    first_layout.size = sizeof(first_layout);
    first_layout.abi_version = PANO_GPU_ABI_VERSION;
    const pano_gpu_result first_result = pano_gpu_test_validate_one_frame_composite_request(
        session, &request->frame_requests[0], &first_layout, error_buffer, error_buffer_size);
    if (first_result != PANO_GPU_SUCCESS)
        return first_result;
    uint32_t previous_frame_index = request->frame_requests[0].frame_index;
    for (uint32_t index = 1; index < request->frame_request_count; ++index)
    {
        const pano_gpu_one_frame_composite_request &frame_request = request->frame_requests[index];
        pano_gpu_one_frame_composite_result_layout frame_layout {};
        frame_layout.size = sizeof(frame_layout);
        frame_layout.abi_version = PANO_GPU_ABI_VERSION;
        const pano_gpu_result frame_result = pano_gpu_test_validate_one_frame_composite_request(
            session, &frame_request, &frame_layout, error_buffer, error_buffer_size);
        if (frame_result != PANO_GPU_SUCCESS)
            return frame_result;
        const pano_gpu_one_frame_composite_request &first_request = request->frame_requests[0];
        if (frame_request.frame_index <= previous_frame_index ||
            frame_request.output_width != first_request.output_width ||
            frame_request.output_height != first_request.output_height || frame_request.row_start != first_request.row_start ||
            frame_request.row_count != first_request.row_count ||
            frame_request.source_sample_type != first_request.source_sample_type ||
            frame_layout.pixel_count != first_layout.pixel_count)
        {
            write_error(error_buffer, error_buffer_size, "incompatible D3D12 ordered hard-composite frame request");
            return PANO_GPU_INVALID_ARGUMENT;
        }
        previous_frame_index = frame_request.frame_index;
    }
    layout->pixel_count = first_layout.pixel_count;
    layout->selected_rgb_bytes = first_layout.linear_rgb_bytes;
    layout->selected_weight_bytes = first_layout.candidate_edge_distance_bytes;
    layout->coverage_bytes = first_layout.coverage_bytes;
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_test_validate_output_hard_composite_request(
    const pano_gpu_output *const output, const pano_gpu_ordered_hard_composite_request *const request,
    pano_gpu_ordered_hard_composite_result_layout *const layout, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (output == nullptr || output->session == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 output hard-composite handle");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    const pano_gpu_result result = pano_gpu_test_validate_ordered_hard_composite_request(
        output->session, request, layout, error_buffer, error_buffer_size);
    if (result != PANO_GPU_SUCCESS)
        return result;
    const pano_gpu_one_frame_composite_request &first = request->frame_requests[0];
    const uint32_t storage_rows = output->output_band_rows == 0 ? output->output_height : output->band_row_count;
    const uint32_t storage_row_start = output->output_band_rows == 0 ? 0 : output->band_row_start;
    if (output->output_width != first.output_width || output->output_height != first.output_height)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 output dimensions do not match ordered hard-composite request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    if (storage_row_start != first.row_start || storage_rows != first.row_count)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 output row range does not match ordered hard-composite request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!output->linear || !output->coverage || output->linear_bytes < layout->selected_rgb_bytes ||
        output->coverage_bytes < layout->coverage_bytes)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 output hard-composite storage is not allocated");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return PANO_GPU_SUCCESS;
#endif
}

static pano_gpu_result validate_typed_hard_composite_request(
    const pano_gpu_session *const session, const pano_gpu_ordered_hard_composite_request *const request,
    pano_gpu_ordered_hard_composite_result_layout *const layout, char *const error_buffer,
    const uint32_t error_buffer_size, const uint32_t expected_frame_count,
    const uint32_t expected_sample_type) noexcept
{
    const pano_gpu_result result = pano_gpu_test_validate_ordered_hard_composite_request(
        session, request, layout, error_buffer, error_buffer_size);
    if (result != PANO_GPU_SUCCESS)
        return result;
    if (request->frame_request_count != expected_frame_count)
    {
        std::memset(layout, 0, sizeof(*layout));
        layout->size = sizeof(*layout);
        layout->abi_version = PANO_GPU_ABI_VERSION;
        write_error(error_buffer, error_buffer_size, "invalid D3D12 two-frame uint8 hard-composite request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    for (uint32_t index = 0; index < expected_frame_count; ++index)
    {
        if (request->frame_requests[index].source_sample_type != expected_sample_type)
        {
            std::memset(layout, 0, sizeof(*layout));
            layout->size = sizeof(*layout);
            layout->abi_version = PANO_GPU_ABI_VERSION;
            write_error(error_buffer, error_buffer_size, "invalid D3D12 uint8 hard-composite request");
            return PANO_GPU_INVALID_ARGUMENT;
        }
    }
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_test_validate_two_frame_uint8_hard_composite_request(
    const pano_gpu_session *const session, const pano_gpu_ordered_hard_composite_request *const request,
    pano_gpu_ordered_hard_composite_result_layout *const layout, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return validate_typed_hard_composite_request(
        session, request, layout, error_buffer, error_buffer_size, 2, PANO_GPU_SAMPLE_UINT8);
}

pano_gpu_result pano_gpu_test_validate_three_frame_uint8_hard_composite_request(
    const pano_gpu_session *const session, const pano_gpu_ordered_hard_composite_request *const request,
    pano_gpu_ordered_hard_composite_result_layout *const layout, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return validate_typed_hard_composite_request(
        session, request, layout, error_buffer, error_buffer_size, 3, PANO_GPU_SAMPLE_UINT8);
}

pano_gpu_result pano_gpu_test_validate_two_frame_uint16_hard_composite_request(
    const pano_gpu_session *const session, const pano_gpu_ordered_hard_composite_request *const request,
    pano_gpu_ordered_hard_composite_result_layout *const layout, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return validate_typed_hard_composite_request(
        session, request, layout, error_buffer, error_buffer_size, 2, PANO_GPU_SAMPLE_UINT16);
}

pano_gpu_result pano_gpu_test_validate_three_frame_uint16_hard_composite_request(
    const pano_gpu_session *const session, const pano_gpu_ordered_hard_composite_request *const request,
    pano_gpu_ordered_hard_composite_result_layout *const layout, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return validate_typed_hard_composite_request(
        session, request, layout, error_buffer, error_buffer_size, 3, PANO_GPU_SAMPLE_UINT16);
}

pano_gpu_result pano_gpu_test_validate_two_frame_float32_hard_composite_request(
    const pano_gpu_session *const session, const pano_gpu_ordered_hard_composite_request *const request,
    pano_gpu_ordered_hard_composite_result_layout *const layout, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return validate_typed_hard_composite_request(
        session, request, layout, error_buffer, error_buffer_size, 2, PANO_GPU_SAMPLE_FLOAT32);
}

pano_gpu_result pano_gpu_test_validate_three_frame_float32_hard_composite_request(
    const pano_gpu_session *const session, const pano_gpu_ordered_hard_composite_request *const request,
    pano_gpu_ordered_hard_composite_result_layout *const layout, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return validate_typed_hard_composite_request(
        session, request, layout, error_buffer, error_buffer_size, 3, PANO_GPU_SAMPLE_FLOAT32);
}

static pano_gpu_result dispatch_typed_hard_composite(
    pano_gpu_output *const output, const pano_gpu_session *const session,
    const pano_gpu_ordered_hard_composite_request *const request,
    void *const selected_rgb, const uint64_t selected_rgb_bytes, void *const selected_weight,
    const uint64_t selected_weight_bytes, void *const coverage, const uint64_t coverage_bytes,
    char *const error_buffer, const uint32_t error_buffer_size, const uint32_t expected_frame_count,
    const uint32_t expected_sample_type, const float *const global_gains = nullptr,
    const float *const local_fields = nullptr, const uint64_t local_field_bytes = 0,
    const bool feather_mode = false) noexcept
{
    pano_gpu_ordered_hard_composite_result_layout layout {};
    layout.size = sizeof(layout);
    layout.abi_version = PANO_GPU_ABI_VERSION;
    const pano_gpu_result validation = validate_typed_hard_composite_request(
        session, request, &layout, error_buffer, error_buffer_size, expected_frame_count, expected_sample_type);
    if (validation != PANO_GPU_SUCCESS)
        return validation;
    if (feather_mode && output == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 feather composition requires an output job");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    if (global_gains != nullptr)
        for (uint32_t index = 0; index < expected_frame_count; ++index)
            if (!std::isfinite(global_gains[index]) || global_gains[index] <= 0.0F)
            {
                write_error(error_buffer, error_buffer_size, "invalid D3D12 hard-composite global gain");
                return PANO_GPU_INVALID_ARGUMENT;
            }
    if (local_fields != nullptr)
    {
        const uint32_t field_width = (request->frame_requests[0].output_width - 1) / 4 + 1;
        const uint32_t field_height = (request->frame_requests[0].output_height - 1) / 4 + 1;
        uint64_t field_values = 0;
        uint64_t expected_bytes = 0;
        if (!checked_multiply(field_width, field_height, &field_values) ||
            !checked_multiply(field_values, expected_frame_count * sizeof(float), &expected_bytes) ||
            local_field_bytes != expected_bytes)
        {
            write_error(error_buffer, error_buffer_size, "invalid D3D12 hard-composite local-field layout");
            return PANO_GPU_INVALID_ARGUMENT;
        }
        for (uint64_t index = 0; index < field_values * expected_frame_count; ++index)
            if (!std::isfinite(local_fields[index]))
            {
                write_error(error_buffer, error_buffer_size, "invalid D3D12 hard-composite local field");
                return PANO_GPU_INVALID_ARGUMENT;
            }
    }
    if (output != nullptr)
    {
        const pano_gpu_one_frame_composite_request &first = request->frame_requests[0];
        if (output->output_band_rows != 0)
        {
            const uint32_t remaining_rows = first.row_start < output->output_height
                ? output->output_height - first.row_start
                : 0;
            const uint32_t expected_rows = std::min(output->output_band_rows, remaining_rows);
            if (first.row_start % output->output_band_rows != 0 ||
                first.row_count != expected_rows || expected_rows == 0)
            {
                write_error(error_buffer, error_buffer_size, "invalid D3D12 output composition band");
                return PANO_GPU_INVALID_ARGUMENT;
            }
            output->band_row_start = first.row_start;
            output->band_row_count = first.row_count;
        }
        const pano_gpu_result output_validation = pano_gpu_test_validate_output_hard_composite_request(
            output, request, &layout, error_buffer, error_buffer_size);
        if (output_validation != PANO_GPU_SUCCESS)
            return output_validation;
    }
    if (output == nullptr && (selected_rgb == nullptr || selected_rgb_bytes != layout.selected_rgb_bytes || selected_weight == nullptr ||
        selected_weight_bytes != layout.selected_weight_bytes || coverage == nullptr || coverage_bytes != layout.coverage_bytes)
        )
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 two-frame uint8 hard-composite result buffers");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const uint32_t source_element_bytes = expected_sample_type == PANO_GPU_SAMPLE_FLOAT32
        ? sizeof(float)
        : (expected_sample_type == PANO_GPU_SAMPLE_UINT16 ? sizeof(uint16_t) : sizeof(uint8_t));
    const uint32_t pixels = static_cast<uint32_t>(layout.pixel_count);
    const uint64_t valid_bytes = ((layout.pixel_count + 31) / 32) * sizeof(uint32_t);
    const auto desc = [](const uint64_t bytes, const D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC value {};
        value.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; value.Width = bytes; value.Height = 1;
        value.DepthOrArraySize = 1; value.MipLevels = 1; value.SampleDesc.Count = 1;
        value.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; value.Flags = flags; return value;
    };
    D3D12_HEAP_PROPERTIES default_heap {}; default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const auto make_default = [&](const uint64_t bytes, Microsoft::WRL::ComPtr<ID3D12Resource> *out) {
        const D3D12_RESOURCE_DESC resource_desc = desc(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        return SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &resource_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(out->ReleaseAndGetAddressOf())));
    };
    using resource_array = std::unique_ptr<Microsoft::WRL::ComPtr<ID3D12Resource>[]>;
    resource_array candidate_rgb {new (std::nothrow)
                                      Microsoft::WRL::ComPtr<ID3D12Resource>[expected_frame_count]};
    resource_array adjusted_rgb {new (std::nothrow)
                                     Microsoft::WRL::ComPtr<ID3D12Resource>[expected_frame_count]};
    resource_array validity {new (std::nothrow)
                                 Microsoft::WRL::ComPtr<ID3D12Resource>[expected_frame_count]};
    resource_array edge {new (std::nothrow)
                             Microsoft::WRL::ComPtr<ID3D12Resource>[expected_frame_count]};
    resource_array feather_weight {new (std::nothrow)
                                       Microsoft::WRL::ComPtr<ID3D12Resource>[expected_frame_count]};
    if (!candidate_rgb || !adjusted_rgb || !validity || !edge || !feather_weight)
    {
        write_error(error_buffer, error_buffer_size, "cannot allocate hard-composite resource owners");
        return PANO_GPU_OUT_OF_MEMORY;
    }
    Microsoft::WRL::ComPtr<ID3D12Resource> accumulator_rgb[2], accumulator_weight[2], coverage_resource;
    for (uint32_t index = 0; index < expected_frame_count; ++index)
        if (!make_default(layout.selected_rgb_bytes, &candidate_rgb[index]) ||
            !make_default(valid_bytes, &validity[index]) ||
            !make_default(layout.selected_weight_bytes, &edge[index]) ||
            (feather_mode && !make_default(layout.selected_weight_bytes, &feather_weight[index])) ||
            (local_fields != nullptr &&
             !make_default(layout.selected_rgb_bytes, &adjusted_rgb[index])))
        {
            write_error(error_buffer, error_buffer_size, "cannot create D3D12 hard-composite frame buffers");
            return PANO_GPU_UNAVAILABLE;
        }
    if (!make_default(layout.selected_rgb_bytes, &accumulator_rgb[0]) ||
        !make_default(layout.selected_rgb_bytes, &accumulator_rgb[1]) ||
        !make_default(layout.selected_weight_bytes, &accumulator_weight[0]) ||
        !make_default(layout.selected_weight_bytes, &accumulator_weight[1]) ||
        !make_default(layout.coverage_bytes, &coverage_resource))
    { write_error(error_buffer, error_buffer_size, "cannot create D3D12 hard-composite accumulators"); return PANO_GPU_UNAVAILABLE; }
    D3D12_HEAP_PROPERTIES upload_heap {}; upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const auto make_zero_upload = [&](const uint64_t bytes, Microsoft::WRL::ComPtr<ID3D12Resource> *out) {
        const D3D12_RESOURCE_DESC resource_desc = desc(bytes, D3D12_RESOURCE_FLAG_NONE);
        if (FAILED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &resource_desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(out->ReleaseAndGetAddressOf())))) return false;
        void *mapped = nullptr; if (FAILED((*out)->Map(0, nullptr, &mapped))) return false;
        std::memset(mapped, 0, static_cast<size_t>(bytes)); (*out)->Unmap(0, nullptr); return true;
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> zero_rgb, zero_weight;
    if (!make_zero_upload(layout.selected_rgb_bytes, &zero_rgb) || !make_zero_upload(layout.selected_weight_bytes, &zero_weight))
    { write_error(error_buffer, error_buffer_size, "cannot create D3D12 two-frame uint8 initial accumulator"); return PANO_GPU_UNAVAILABLE; }
    const uint32_t local_field_width = (request->frame_requests[0].output_width - 1) / 4 + 1;
    const uint32_t local_field_height = (request->frame_requests[0].output_height - 1) / 4 + 1;
    const uint64_t local_field_per_frame_bytes = static_cast<uint64_t>(local_field_width) * local_field_height * sizeof(float);
    resource_array local_field_resources {new (std::nothrow)
                                             Microsoft::WRL::ComPtr<ID3D12Resource>[expected_frame_count]};
    if (!local_field_resources)
    {
        write_error(error_buffer, error_buffer_size, "cannot allocate local-field resource owners");
        return PANO_GPU_OUT_OF_MEMORY;
    }
    if (local_fields != nullptr)
    {
        for (uint32_t index = 0; index < expected_frame_count; ++index)
        {
            const D3D12_RESOURCE_DESC field_desc = desc(local_field_per_frame_bytes, D3D12_RESOURCE_FLAG_NONE);
            if (FAILED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &field_desc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(local_field_resources[index].ReleaseAndGetAddressOf()))))
            { write_error(error_buffer, error_buffer_size, "cannot create D3D12 hard-composite local field"); return PANO_GPU_UNAVAILABLE; }
            void *mapped = nullptr;
            if (FAILED(local_field_resources[index]->Map(0, nullptr, &mapped)))
            { write_error(error_buffer, error_buffer_size, "cannot map D3D12 hard-composite local field"); return PANO_GPU_UNAVAILABLE; }
            std::memcpy(mapped, local_fields + index * local_field_width * local_field_height,
                        static_cast<size_t>(local_field_per_frame_bytes));
            local_field_resources[index]->Unmap(0, nullptr);
        }
    }
    D3D12_DESCRIPTOR_RANGE candidate_ranges[2] {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 1}};
    D3D12_ROOT_PARAMETER candidate_params[2] {}; candidate_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    candidate_params[0].Constants = {0, 0, 36}; candidate_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; candidate_params[1].DescriptorTable = {2, candidate_ranges};
    D3D12_ROOT_SIGNATURE_DESC candidate_root_desc {2, candidate_params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    D3D12_DESCRIPTOR_RANGE local_ranges[2] {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2}};
    D3D12_ROOT_PARAMETER local_params[2] {}; local_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    local_params[0].Constants = {0, 0, 8}; local_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; local_params[1].DescriptorTable = {2, local_ranges};
    D3D12_ROOT_SIGNATURE_DESC local_root_desc {2, local_params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    D3D12_DESCRIPTOR_RANGE select_ranges[2] {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 5}};
    D3D12_ROOT_PARAMETER select_params[2] {}; select_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    select_params[0].Constants = {0, 0, 1}; select_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; select_params[1].DescriptorTable = {2, select_ranges};
    D3D12_ROOT_SIGNATURE_DESC select_root_desc {2, select_params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    D3D12_DESCRIPTOR_RANGE feather_weight_ranges[2] {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2}};
    D3D12_ROOT_PARAMETER feather_weight_params[2] {}; feather_weight_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    feather_weight_params[0].Constants = {0, 0, 3}; feather_weight_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; feather_weight_params[1].DescriptorTable = {2, feather_weight_ranges};
    D3D12_ROOT_SIGNATURE_DESC feather_weight_root_desc {2, feather_weight_params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    D3D12_DESCRIPTOR_RANGE feather_accumulate_ranges[2] {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 4}};
    D3D12_ROOT_PARAMETER feather_accumulate_params[2] {}; feather_accumulate_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    feather_accumulate_params[0].Constants = {0, 0, 1}; feather_accumulate_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; feather_accumulate_params[1].DescriptorTable = {2, feather_accumulate_ranges};
    D3D12_ROOT_SIGNATURE_DESC feather_accumulate_root_desc {2, feather_accumulate_params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    D3D12_DESCRIPTOR_RANGE feather_normalize_ranges[2] {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 2}};
    D3D12_ROOT_PARAMETER feather_normalize_params[2] {}; feather_normalize_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    feather_normalize_params[0].Constants = {0, 0, 1}; feather_normalize_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; feather_normalize_params[1].DescriptorTable = {2, feather_normalize_ranges};
    D3D12_ROOT_SIGNATURE_DESC feather_normalize_root_desc {2, feather_normalize_params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> candidate_blob, local_blob, select_blob, errors;
    if (FAILED(D3D12SerializeRootSignature(&candidate_root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &candidate_blob, &errors)) ||
        FAILED(D3D12SerializeRootSignature(&local_root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &local_blob, &errors)) ||
        FAILED(D3D12SerializeRootSignature(&select_root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &select_blob, &errors)))
    { write_error(error_buffer, error_buffer_size, "cannot serialize D3D12 two-frame uint8 root signatures"); return PANO_GPU_UNAVAILABLE; }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> candidate_root, local_root, select_root;
    if (FAILED(device->CreateRootSignature(0, candidate_blob->GetBufferPointer(), candidate_blob->GetBufferSize(), IID_PPV_ARGS(&candidate_root))) ||
        FAILED(device->CreateRootSignature(0, local_blob->GetBufferPointer(), local_blob->GetBufferSize(), IID_PPV_ARGS(&local_root))) ||
        FAILED(device->CreateRootSignature(0, select_blob->GetBufferPointer(), select_blob->GetBufferSize(), IID_PPV_ARGS(&select_root))))
    { write_error(error_buffer, error_buffer_size, "cannot create D3D12 two-frame uint8 root signatures"); return PANO_GPU_UNAVAILABLE; }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> feather_weight_root, feather_accumulate_root, feather_normalize_root;
    if (feather_mode)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> weight_blob, accumulate_blob, normalize_blob;
        if (FAILED(D3D12SerializeRootSignature(&feather_weight_root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &weight_blob, &errors)) ||
            FAILED(D3D12SerializeRootSignature(&feather_accumulate_root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &accumulate_blob, &errors)) ||
            FAILED(D3D12SerializeRootSignature(&feather_normalize_root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &normalize_blob, &errors)) ||
            FAILED(device->CreateRootSignature(0, weight_blob->GetBufferPointer(), weight_blob->GetBufferSize(), IID_PPV_ARGS(&feather_weight_root))) ||
            FAILED(device->CreateRootSignature(0, accumulate_blob->GetBufferPointer(), accumulate_blob->GetBufferSize(), IID_PPV_ARGS(&feather_accumulate_root))) ||
            FAILED(device->CreateRootSignature(0, normalize_blob->GetBufferPointer(), normalize_blob->GetBufferSize(), IID_PPV_ARGS(&feather_normalize_root))))
        { write_error(error_buffer, error_buffer_size, "cannot create D3D12 feather-composite root signatures"); return PANO_GPU_UNAVAILABLE; }
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC candidate_pso_desc {}; candidate_pso_desc.pRootSignature = candidate_root.Get(); candidate_pso_desc.CS = expected_sample_type == PANO_GPU_SAMPLE_FLOAT32 ? D3D12_SHADER_BYTECODE {pano_gpu_float32_candidate_shader, sizeof(pano_gpu_float32_candidate_shader)} : (expected_sample_type == PANO_GPU_SAMPLE_UINT16 ? D3D12_SHADER_BYTECODE {pano_gpu_uint16_candidate_shader, sizeof(pano_gpu_uint16_candidate_shader)} : D3D12_SHADER_BYTECODE {pano_gpu_uint8_candidate_shader, sizeof(pano_gpu_uint8_candidate_shader)});
    D3D12_COMPUTE_PIPELINE_STATE_DESC local_pso_desc {}; local_pso_desc.pRootSignature = local_root.Get(); local_pso_desc.CS = {pano_gpu_local_exposure_shader, sizeof(pano_gpu_local_exposure_shader)};
    D3D12_COMPUTE_PIPELINE_STATE_DESC select_pso_desc {}; select_pso_desc.pRootSignature = select_root.Get(); select_pso_desc.CS = {pano_gpu_hard_selection_shader, sizeof(pano_gpu_hard_selection_shader)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> candidate_pso, local_pso, select_pso;
    if (FAILED(device->CreateComputePipelineState(&candidate_pso_desc, IID_PPV_ARGS(&candidate_pso))) || FAILED(device->CreateComputePipelineState(&local_pso_desc, IID_PPV_ARGS(&local_pso))) || FAILED(device->CreateComputePipelineState(&select_pso_desc, IID_PPV_ARGS(&select_pso))))
    { write_error(error_buffer, error_buffer_size, "cannot create D3D12 two-frame uint8 pipelines"); return PANO_GPU_UNAVAILABLE; }
    Microsoft::WRL::ComPtr<ID3D12PipelineState> feather_weight_pso, feather_accumulate_pso, feather_normalize_pso;
    if (feather_mode)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC weight_desc {}; weight_desc.pRootSignature = feather_weight_root.Get(); weight_desc.CS = {pano_gpu_feather_weight_shader, sizeof(pano_gpu_feather_weight_shader)};
        D3D12_COMPUTE_PIPELINE_STATE_DESC accumulate_desc {}; accumulate_desc.pRootSignature = feather_accumulate_root.Get(); accumulate_desc.CS = {pano_gpu_feather_accumulate_shader, sizeof(pano_gpu_feather_accumulate_shader)};
        D3D12_COMPUTE_PIPELINE_STATE_DESC normalize_desc {}; normalize_desc.pRootSignature = feather_normalize_root.Get(); normalize_desc.CS = {pano_gpu_feather_normalize_output_shader, sizeof(pano_gpu_feather_normalize_output_shader)};
        if (FAILED(device->CreateComputePipelineState(&weight_desc, IID_PPV_ARGS(&feather_weight_pso))) ||
            FAILED(device->CreateComputePipelineState(&accumulate_desc, IID_PPV_ARGS(&feather_accumulate_pso))) ||
            FAILED(device->CreateComputePipelineState(&normalize_desc, IID_PPV_ARGS(&feather_normalize_pso))))
        { write_error(error_buffer, error_buffer_size, "cannot create D3D12 feather-composite pipelines"); return PANO_GPU_UNAVAILABLE; }
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc {}; heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap_desc.NumDescriptors = feather_mode ? expected_frame_count * (local_fields == nullptr ? 13 : 16) + 4 : expected_frame_count * (local_fields == nullptr ? 12 : 15); heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap; Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator; Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap))) || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), candidate_pso.Get(), IID_PPV_ARGS(&list))))
    { write_error(error_buffer, error_buffer_size, "cannot create D3D12 two-frame uint8 command resources"); return PANO_GPU_UNAVAILABLE; }
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto cpu = [&](const UINT slot) { D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += static_cast<SIZE_T>(slot) * increment; return h; };
    const auto gpu = [&](const UINT slot) { D3D12_GPU_DESCRIPTOR_HANDLE h = heap->GetGPUDescriptorHandleForHeapStart(); h.ptr += static_cast<UINT64>(slot) * increment; return h; };
    D3D12_SHADER_RESOURCE_VIEW_DESC source_srv {}; source_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; source_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; source_srv.Format = expected_sample_type == PANO_GPU_SAMPLE_FLOAT32 ? DXGI_FORMAT_R32_FLOAT : (expected_sample_type == PANO_GPU_SAMPLE_UINT16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R8_UINT); source_srv.Buffer.NumElements = static_cast<UINT>(session->source_bytes / source_element_bytes);
    D3D12_UNORDERED_ACCESS_VIEW_DESC rgb_uav {}; rgb_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; rgb_uav.Format = DXGI_FORMAT_UNKNOWN; rgb_uav.Buffer.NumElements = pixels; rgb_uav.Buffer.StructureByteStride = 3 * sizeof(float);
    D3D12_UNORDERED_ACCESS_VIEW_DESC weight_uav = rgb_uav; weight_uav.Buffer.StructureByteStride = sizeof(float);
    D3D12_UNORDERED_ACCESS_VIEW_DESC valid_uav {}; valid_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; valid_uav.Format = DXGI_FORMAT_R32_TYPELESS; valid_uav.Buffer.NumElements = static_cast<UINT>(valid_bytes / 4); valid_uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    D3D12_UNORDERED_ACCESS_VIEW_DESC coverage_uav {}; coverage_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; coverage_uav.Format = DXGI_FORMAT_R8_UINT; coverage_uav.Buffer.NumElements = pixels;
    const auto structured_srv = [&](ID3D12Resource *resource, UINT stride, UINT slot) { D3D12_SHADER_RESOURCE_VIEW_DESC v {}; v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; v.Format = DXGI_FORMAT_UNKNOWN; v.Buffer.NumElements = pixels; v.Buffer.StructureByteStride = stride; device->CreateShaderResourceView(resource, &v, cpu(slot)); };
    const UINT local_base = expected_frame_count * 4;
    const UINT select_base = expected_frame_count * (local_fields == nullptr ? 4 : 7);
    const UINT feather_accumulate_base = select_base + expected_frame_count * 3;
    const UINT feather_normalize_base = feather_accumulate_base + expected_frame_count * 6;
    D3D12_SHADER_RESOURCE_VIEW_DESC valid_srv {}; valid_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; valid_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; valid_srv.Format = DXGI_FORMAT_R32_TYPELESS; valid_srv.Buffer.NumElements = static_cast<UINT>(valid_bytes / 4); valid_srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    for (UINT pass = 0; pass < expected_frame_count; ++pass) {
        const UINT candidate_slot = pass * 4; device->CreateShaderResourceView(session->source.Get(), &source_srv, cpu(candidate_slot));
        device->CreateUnorderedAccessView(candidate_rgb[pass].Get(), nullptr, &rgb_uav, cpu(candidate_slot + 1)); device->CreateUnorderedAccessView(validity[pass].Get(), nullptr, &valid_uav, cpu(candidate_slot + 2)); device->CreateUnorderedAccessView(edge[pass].Get(), nullptr, &weight_uav, cpu(candidate_slot + 3));
        if (local_fields != nullptr) {
            const UINT local_slot = local_base + pass * 3;
            structured_srv(candidate_rgb[pass].Get(), 3 * sizeof(float), local_slot);
            D3D12_SHADER_RESOURCE_VIEW_DESC field_srv {}; field_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; field_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; field_srv.Format = DXGI_FORMAT_UNKNOWN; field_srv.Buffer.NumElements = local_field_width * local_field_height; field_srv.Buffer.StructureByteStride = sizeof(float); device->CreateShaderResourceView(local_field_resources[pass].Get(), &field_srv, cpu(local_slot + 1));
            device->CreateUnorderedAccessView(adjusted_rgb[pass].Get(), nullptr, &rgb_uav, cpu(local_slot + 2));
        }
        if (!feather_mode) {
            const UINT select_slot = select_base + pass * 8; structured_srv((local_fields == nullptr ? candidate_rgb[pass] : adjusted_rgb[pass]).Get(), 3 * sizeof(float), select_slot); device->CreateShaderResourceView(validity[pass].Get(), &valid_srv, cpu(select_slot + 1)); structured_srv(edge[pass].Get(), sizeof(float), select_slot + 2);
            structured_srv(pass == 0 ? zero_rgb.Get() : accumulator_rgb[(pass + 1) % 2].Get(), 3 * sizeof(float), select_slot + 3); structured_srv(pass == 0 ? zero_weight.Get() : accumulator_weight[(pass + 1) % 2].Get(), sizeof(float), select_slot + 4);
            device->CreateUnorderedAccessView(accumulator_rgb[pass % 2].Get(), nullptr, &rgb_uav, cpu(select_slot + 5)); device->CreateUnorderedAccessView(accumulator_weight[pass % 2].Get(), nullptr, &weight_uav, cpu(select_slot + 6)); device->CreateUnorderedAccessView(coverage_resource.Get(), nullptr, &coverage_uav, cpu(select_slot + 7));
        } else {
            const UINT weight_slot = select_base + pass * 3; device->CreateShaderResourceView(validity[pass].Get(), &valid_srv, cpu(weight_slot)); structured_srv(edge[pass].Get(), sizeof(float), weight_slot + 1); device->CreateUnorderedAccessView(feather_weight[pass].Get(), nullptr, &weight_uav, cpu(weight_slot + 2));
            const UINT accumulate_slot = feather_accumulate_base + pass * 6; structured_srv((local_fields == nullptr ? candidate_rgb[pass] : adjusted_rgb[pass]).Get(), 3 * sizeof(float), accumulate_slot); structured_srv(feather_weight[pass].Get(), sizeof(float), accumulate_slot + 1); structured_srv(pass == 0 ? zero_rgb.Get() : accumulator_rgb[(pass + 1) % 2].Get(), 3 * sizeof(float), accumulate_slot + 2); structured_srv(pass == 0 ? zero_weight.Get() : accumulator_weight[(pass + 1) % 2].Get(), sizeof(float), accumulate_slot + 3); device->CreateUnorderedAccessView(accumulator_rgb[pass % 2].Get(), nullptr, &rgb_uav, cpu(accumulate_slot + 4)); device->CreateUnorderedAccessView(accumulator_weight[pass % 2].Get(), nullptr, &weight_uav, cpu(accumulate_slot + 5));
        }
    }
    if (feather_mode) {
        const UINT final_accumulator = (expected_frame_count - 1) % 2; structured_srv(accumulator_rgb[final_accumulator].Get(), 3 * sizeof(float), feather_normalize_base); structured_srv(accumulator_weight[final_accumulator].Get(), sizeof(float), feather_normalize_base + 1); device->CreateUnorderedAccessView(output->linear.Get(), nullptr, &rgb_uav, cpu(feather_normalize_base + 2)); device->CreateUnorderedAccessView(output->coverage.Get(), nullptr, &coverage_uav, cpu(feather_normalize_base + 3));
    }
    ID3D12DescriptorHeap *heaps[] = {heap.Get()}; list->SetDescriptorHeaps(1, heaps); const UINT clear[4] {};
    for (UINT pass = 0; pass < expected_frame_count; ++pass) {
        if (pass >= 2) {
            D3D12_RESOURCE_BARRIER writable_accumulators[2] {};
            writable_accumulators[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            writable_accumulators[0].Transition = {accumulator_rgb[pass % 2].Get(),
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
            writable_accumulators[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            writable_accumulators[1].Transition = {accumulator_weight[pass % 2].Get(),
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
            list->ResourceBarrier(2, writable_accumulators);
        }
        const pano_gpu_one_frame_composite_request &frame = request->frame_requests[pass]; list->ClearUnorderedAccessViewUint(gpu(pass * 4 + 2), cpu(pass * 4 + 2), validity[pass].Get(), clear, 0, nullptr); D3D12_RESOURCE_BARRIER cleared_validity {}; cleared_validity.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; cleared_validity.UAV.pResource = validity[pass].Get(); list->ResourceBarrier(1, &cleared_validity);
        uint32_t constants[36] {frame.output_width, frame.output_height, frame.row_start, frame.row_count, session->source_width, session->source_height, session->source_row_stride_bytes / source_element_bytes, static_cast<uint32_t>(static_cast<uint64_t>(frame.frame_index) * session->source_frame_bytes / source_element_bytes)};
        std::memcpy(&constants[8], &frame.latitude_span_degrees, sizeof(float)); std::memcpy(&constants[12], frame.world_to_camera, 3 * sizeof(float)); std::memcpy(&constants[16], frame.world_to_camera + 3, 3 * sizeof(float)); std::memcpy(&constants[20], frame.world_to_camera + 6, 3 * sizeof(float));
        const float camera[4] {static_cast<float>(session->source_width), static_cast<float>(session->source_height), session->source_width / (2.0F * std::tan(frame.horizontal_fov_degrees * 0.00872664625997165F)), session->source_height / (2.0F * std::tan(frame.vertical_fov_degrees * 0.00872664625997165F))}; std::memcpy(&constants[24], camera, sizeof(camera));
        const float gain = global_gains == nullptr ? 1.0F : global_gains[pass]; std::memcpy(&constants[28], &gain, sizeof(gain));
        const float rectilinear_output = frame.rectilinear_output != 0 ? 1.0F : 0.0F;
        std::memcpy(&constants[32], &rectilinear_output, sizeof(rectilinear_output)); std::memcpy(&constants[33], &frame.output_vertical_fov_degrees, sizeof(frame.output_vertical_fov_degrees));
        constants[34] = session->transfer_function;
        list->SetPipelineState(candidate_pso.Get()); list->SetComputeRootSignature(candidate_root.Get()); list->SetComputeRoot32BitConstants(0, 36, constants, 0); list->SetComputeRootDescriptorTable(1, gpu(pass * 4)); list->Dispatch((frame.output_width + 7) / 8, (frame.row_count + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER bars[3] {}; ID3D12Resource *candidate_resources[] = {candidate_rgb[pass].Get(), validity[pass].Get(), edge[pass].Get()}; for (UINT i = 0; i < 3; ++i) { bars[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; bars[i].Transition.pResource = candidate_resources[i]; bars[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; bars[i].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; bars[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; } list->ResourceBarrier(3, bars);
        if (local_fields != nullptr) {
            const uint32_t local_constants[8] {frame.output_width, frame.output_height, frame.row_start, frame.row_count, local_field_width, local_field_height};
            list->SetPipelineState(local_pso.Get()); list->SetComputeRootSignature(local_root.Get()); list->SetComputeRoot32BitConstants(0, 8, local_constants, 0); list->SetComputeRootDescriptorTable(1, gpu(local_base + pass * 3)); list->Dispatch((pixels + 63) / 64, 1, 1);
            D3D12_RESOURCE_BARRIER adjusted_barrier {}; adjusted_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; adjusted_barrier.Transition.pResource = adjusted_rgb[pass].Get(); adjusted_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; adjusted_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; adjusted_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; list->ResourceBarrier(1, &adjusted_barrier);
        }
        if (!feather_mode) {
            list->SetPipelineState(select_pso.Get()); list->SetComputeRootSignature(select_root.Get()); list->SetComputeRoot32BitConstants(0, 1, &pixels, 0); list->SetComputeRootDescriptorTable(1, gpu(select_base + pass * 8)); list->Dispatch((pixels + 63) / 64, 1, 1);
        } else {
            const uint32_t weight_constants[3] {pixels, session->source_width, session->source_height}; list->SetPipelineState(feather_weight_pso.Get()); list->SetComputeRootSignature(feather_weight_root.Get()); list->SetComputeRoot32BitConstants(0, 3, weight_constants, 0); list->SetComputeRootDescriptorTable(1, gpu(select_base + pass * 3)); list->Dispatch((pixels + 63) / 64, 1, 1);
            D3D12_RESOURCE_BARRIER weight_ready {}; weight_ready.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; weight_ready.Transition = {feather_weight[pass].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}; list->ResourceBarrier(1, &weight_ready);
            list->SetPipelineState(feather_accumulate_pso.Get()); list->SetComputeRootSignature(feather_accumulate_root.Get()); list->SetComputeRoot32BitConstants(0, 1, &pixels, 0); list->SetComputeRootDescriptorTable(1, gpu(feather_accumulate_base + pass * 6)); list->Dispatch((pixels + 63) / 64, 1, 1);
        }
        if (pass + 1 < expected_frame_count) { D3D12_RESOURCE_BARRIER prior[5] {}; for (UINT i = 0; i < 2; ++i) { prior[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; prior[i].UAV.pResource = i == 0 ? accumulator_rgb[pass % 2].Get() : accumulator_weight[pass % 2].Get(); prior[i + 2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; prior[i + 2].Transition.pResource = prior[i].UAV.pResource; prior[i + 2].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; prior[i + 2].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; prior[i + 2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; } prior[4].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; prior[4].UAV.pResource = coverage_resource.Get(); list->ResourceBarrier(feather_mode ? 4 : 5, prior); if (FAILED(list->Close())) { write_error(error_buffer, error_buffer_size, "cannot close D3D12 composite command list"); return PANO_GPU_UNAVAILABLE; } ID3D12CommandList *pass_lists[] = {list.Get()}; session->device_core->queue->ExecuteCommandLists(1, pass_lists); const uint64_t pass_fence = session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1; if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), pass_fence)) || wait_for_fence(session->device_core.get(), pass_fence, error_buffer, error_buffer_size, "D3D12 composite fence timed out") != PANO_GPU_SUCCESS || FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), candidate_pso.Get()))) { write_error(error_buffer, error_buffer_size, "cannot restart D3D12 composite command list"); return PANO_GPU_UNAVAILABLE; } list->SetDescriptorHeaps(1, heaps); }
    }
    const UINT final_accumulator = (expected_frame_count - 1) % 2;
    if (feather_mode) {
        ID3D12Resource *final_accumulators[] = {accumulator_rgb[final_accumulator].Get(), accumulator_weight[final_accumulator].Get()}; D3D12_RESOURCE_BARRIER accumulator_ready[4] {}; for (UINT i = 0; i < 2; ++i) { accumulator_ready[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; accumulator_ready[i].UAV.pResource = final_accumulators[i]; accumulator_ready[i + 2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; accumulator_ready[i + 2].Transition = {final_accumulators[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}; } list->ResourceBarrier(4, accumulator_ready);
        ID3D12Resource *output_resources[] = {output->linear.Get(), output->coverage.Get()}; D3D12_RESOURCE_BARRIER output_writable[2] {}; for (UINT i = 0; i < 2; ++i) { output_writable[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; output_writable[i].Transition = {output_resources[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}; } list->ResourceBarrier(2, output_writable);
        list->SetPipelineState(feather_normalize_pso.Get()); list->SetComputeRootSignature(feather_normalize_root.Get()); list->SetComputeRoot32BitConstants(0, 1, &pixels, 0); list->SetComputeRootDescriptorTable(1, gpu(feather_normalize_base)); list->Dispatch((pixels + 63) / 64, 1, 1);
        D3D12_RESOURCE_BARRIER output_ordering[2] {}; for (UINT i = 0; i < 2; ++i) { output_ordering[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; output_ordering[i].UAV.pResource = output_resources[i]; } list->ResourceBarrier(2, output_ordering); for (UINT i = 0; i < 2; ++i) std::swap(output_writable[i].Transition.StateBefore, output_writable[i].Transition.StateAfter); list->ResourceBarrier(2, output_writable);
        if (FAILED(list->Close())) { write_error(error_buffer, error_buffer_size, "cannot close D3D12 output feather-composite command list"); return PANO_GPU_UNAVAILABLE; } ID3D12CommandList *feather_lists[] = {list.Get()}; session->device_core->queue->ExecuteCommandLists(1, feather_lists); const uint64_t feather_fence = session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1; if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), feather_fence))) { write_error(error_buffer, error_buffer_size, "cannot signal D3D12 output feather-composite fence"); return PANO_GPU_UNAVAILABLE; } return wait_for_fence(session->device_core.get(), feather_fence, error_buffer, error_buffer_size, "D3D12 output feather-composite fence timed out");
    }
    ID3D12Resource *final_resources[] = {accumulator_rgb[final_accumulator].Get(), accumulator_weight[final_accumulator].Get(), coverage_resource.Get()}; D3D12_RESOURCE_BARRIER final_ordering[3] {}; for (UINT i = 0; i < 3; ++i) { final_ordering[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; final_ordering[i].UAV.pResource = final_resources[i]; } list->ResourceBarrier(3, final_ordering); D3D12_RESOURCE_BARRIER final_bars[3] {}; for (UINT i = 0; i < 3; ++i) { final_bars[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; final_bars[i].Transition.pResource = final_resources[i]; final_bars[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; final_bars[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; final_bars[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; } list->ResourceBarrier(3, final_bars);
    if (output != nullptr)
    {
        list->CopyBufferRegion(output->linear.Get(), 0, accumulator_rgb[final_accumulator].Get(), 0,
                               layout.selected_rgb_bytes);
        list->CopyBufferRegion(output->coverage.Get(), 0, coverage_resource.Get(), 0, layout.coverage_bytes);
        if (FAILED(list->Close()))
        {
            write_error(error_buffer, error_buffer_size, "cannot close D3D12 output hard-composite command list");
            return PANO_GPU_UNAVAILABLE;
        }
        ID3D12CommandList *output_lists[] = {list.Get()};
        session->device_core->queue->ExecuteCommandLists(1, output_lists);
        const uint64_t output_fence = session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
        if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), output_fence)))
        {
            write_error(error_buffer, error_buffer_size, "cannot signal D3D12 output hard-composite fence");
            return PANO_GPU_UNAVAILABLE;
        }
        return wait_for_fence(session->device_core.get(), output_fence, error_buffer, error_buffer_size,
                              "D3D12 output hard-composite fence timed out");
    }
    D3D12_HEAP_PROPERTIES readback_heap {}; readback_heap.Type = D3D12_HEAP_TYPE_READBACK; const auto make_readback = [&](uint64_t bytes, Microsoft::WRL::ComPtr<ID3D12Resource> *out) { const D3D12_RESOURCE_DESC resource_desc = desc(bytes, D3D12_RESOURCE_FLAG_NONE); return SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &resource_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(out->ReleaseAndGetAddressOf()))); };
    Microsoft::WRL::ComPtr<ID3D12Resource> rgb_readback, weight_readback, coverage_readback; if (!make_readback(layout.selected_rgb_bytes, &rgb_readback) || !make_readback(layout.selected_weight_bytes, &weight_readback) || !make_readback(layout.coverage_bytes, &coverage_readback)) { write_error(error_buffer, error_buffer_size, "cannot create D3D12 two-frame uint8 readback buffers"); return PANO_GPU_UNAVAILABLE; }
    list->CopyResource(rgb_readback.Get(), accumulator_rgb[final_accumulator].Get()); list->CopyResource(weight_readback.Get(), accumulator_weight[final_accumulator].Get()); list->CopyResource(coverage_readback.Get(), coverage_resource.Get());
    if (FAILED(list->Close())) { write_error(error_buffer, error_buffer_size, "cannot close D3D12 two-frame uint8 command list"); return PANO_GPU_UNAVAILABLE; }
    ID3D12CommandList *lists[] = {list.Get()}; session->device_core->queue->ExecuteCommandLists(1, lists); const uint64_t fence = session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence))) { write_error(error_buffer, error_buffer_size, "cannot signal D3D12 two-frame uint8 fence"); return PANO_GPU_UNAVAILABLE; }
    if (wait_for_fence(session->device_core.get(), fence, error_buffer, error_buffer_size, "D3D12 two-frame uint8 fence timed out") != PANO_GPU_SUCCESS) return PANO_GPU_UNAVAILABLE;
    const auto copy = [&](ID3D12Resource *resource, uint64_t bytes, void *out) { D3D12_RANGE range {0, static_cast<SIZE_T>(bytes)}; void *mapped = nullptr; if (FAILED(resource->Map(0, &range, &mapped))) return false; std::memcpy(out, mapped, static_cast<size_t>(bytes)); resource->Unmap(0, nullptr); return true; };
    if (!copy(rgb_readback.Get(), layout.selected_rgb_bytes, selected_rgb) || !copy(weight_readback.Get(), layout.selected_weight_bytes, selected_weight) || !copy(coverage_readback.Get(), layout.coverage_bytes, coverage)) { write_error(error_buffer, error_buffer_size, "cannot map D3D12 two-frame uint8 readback"); return PANO_GPU_UNAVAILABLE; }
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_dispatch_two_frame_uint8_hard_composite(
    const pano_gpu_session *const session, const pano_gpu_ordered_hard_composite_request *const request,
    void *const selected_rgb, const uint64_t selected_rgb_bytes, void *const selected_weight,
    const uint64_t selected_weight_bytes, void *const coverage, const uint64_t coverage_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    return dispatch_typed_hard_composite(
        nullptr, session, request, selected_rgb, selected_rgb_bytes, selected_weight, selected_weight_bytes, coverage,
        coverage_bytes, error_buffer, error_buffer_size, 2, PANO_GPU_SAMPLE_UINT8);
}

pano_gpu_result pano_gpu_test_dispatch_two_frame_uint8_hard_composite_with_gains(
    const pano_gpu_session *const session, const pano_gpu_ordered_hard_composite_request *const request,
    const float *const global_gains, const uint64_t global_gain_bytes, void *const selected_rgb,
    const uint64_t selected_rgb_bytes, void *const selected_weight, const uint64_t selected_weight_bytes,
    void *const coverage, const uint64_t coverage_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (global_gains == nullptr || global_gain_bytes != 2 * sizeof(float))
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 hard-composite global-gain layout");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return dispatch_typed_hard_composite(
        nullptr, session, request, selected_rgb, selected_rgb_bytes, selected_weight, selected_weight_bytes, coverage,
        coverage_bytes, error_buffer, error_buffer_size, 2, PANO_GPU_SAMPLE_UINT8, global_gains);
}

pano_gpu_result pano_gpu_test_dispatch_two_frame_uint8_hard_composite_with_exposure(
    const pano_gpu_session *const session, const pano_gpu_ordered_hard_composite_request *const request,
    const float *const global_gains, const uint64_t global_gain_bytes, const float *const local_fields,
    const uint64_t local_field_bytes, void *const selected_rgb, const uint64_t selected_rgb_bytes,
    void *const selected_weight, const uint64_t selected_weight_bytes, void *const coverage,
    const uint64_t coverage_bytes, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (global_gains == nullptr || global_gain_bytes != 2 * sizeof(float) || local_fields == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 hard-composite exposure layout");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return dispatch_typed_hard_composite(
        nullptr, session, request, selected_rgb, selected_rgb_bytes, selected_weight, selected_weight_bytes, coverage,
        coverage_bytes, error_buffer, error_buffer_size, 2, PANO_GPU_SAMPLE_UINT8, global_gains, local_fields,
        local_field_bytes);
}

pano_gpu_result pano_gpu_test_dispatch_three_frame_uint8_hard_composite(
    const pano_gpu_session *const session, const pano_gpu_ordered_hard_composite_request *const request,
    void *const selected_rgb, const uint64_t selected_rgb_bytes, void *const selected_weight,
    const uint64_t selected_weight_bytes, void *const coverage, const uint64_t coverage_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    return dispatch_typed_hard_composite(
        nullptr, session, request, selected_rgb, selected_rgb_bytes, selected_weight, selected_weight_bytes, coverage,
        coverage_bytes, error_buffer, error_buffer_size, 3, PANO_GPU_SAMPLE_UINT8);
}

pano_gpu_result pano_gpu_test_dispatch_three_frame_uint8_hard_composite_with_session_gains(
    const pano_gpu_session *const session, const pano_gpu_ordered_hard_composite_request *const request,
    void *const selected_rgb, const uint64_t selected_rgb_bytes, void *const selected_weight,
    const uint64_t selected_weight_bytes, void *const coverage, const uint64_t coverage_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || !session->exposure_gains_uploaded ||
        session->exposure_global_gains.size() != 3)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 exposure gains are not bound");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return dispatch_typed_hard_composite(
        nullptr, session, request, selected_rgb, selected_rgb_bytes, selected_weight,
        selected_weight_bytes, coverage, coverage_bytes, error_buffer, error_buffer_size, 3,
        PANO_GPU_SAMPLE_UINT8, session->exposure_global_gains.data());
}

pano_gpu_result pano_gpu_test_dispatch_output_uint8_hard_composite(
    pano_gpu_output *const output, const pano_gpu_ordered_hard_composite_request *const request,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (output == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 output hard-composite handle");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return dispatch_typed_hard_composite(
        output, output->session, request, nullptr, 0, nullptr, 0, nullptr, 0, error_buffer, error_buffer_size, 3,
        PANO_GPU_SAMPLE_UINT8);
}

static pano_gpu_result mark_output_incomplete(
    pano_gpu_output *const output, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (output == nullptr || output->session == nullptr || output->linear_bytes == 0 || output->coverage_bytes == 0)
    {
        write_error(error_buffer, error_buffer_size, "invalid incomplete-output job");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    const uint32_t rows = output->output_band_rows == 0 ? output->output_height : output->band_row_count;
    uint64_t pixel_count64 = 0;
    if (!checked_multiply(output->output_width, rows, &pixel_count64) || pixel_count64 > UINT32_MAX)
    {
        write_error(error_buffer, error_buffer_size, "incomplete-output pixel count overflows");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    const uint32_t pixel_count = static_cast<uint32_t>(pixel_count64);
    ID3D12Device *const device = output->session->device_core->d3d_device.Get();
    D3D12_DESCRIPTOR_RANGE ranges[2] {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER parameters[2] {}; parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[0].Constants = {0, 0, 1}; parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[1].DescriptorTable = {2, ranges};
    const D3D12_ROOT_SIGNATURE_DESC root_description {2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors; Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    if (FAILED(D3D12SerializeRootSignature(&root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) || FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)))) { write_error(error_buffer, error_buffer_size, "cannot create incomplete-output root signature"); return PANO_GPU_UNAVAILABLE; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {}; pipeline_description.pRootSignature = root.Get(); pipeline_description.CS = {pano_gpu_mark_incomplete_output_shader, sizeof(pano_gpu_mark_incomplete_output_shader)}; Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline)))) { write_error(error_buffer, error_buffer_size, "cannot create incomplete-output pipeline"); return PANO_GPU_UNAVAILABLE; }
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {}; heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap_description.NumDescriptors = 2; heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap; Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator; Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&heap))) || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) || FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list)))) { write_error(error_buffer, error_buffer_size, "cannot create incomplete-output dispatch resources"); return PANO_GPU_UNAVAILABLE; }
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); D3D12_CPU_DESCRIPTOR_HANDLE descriptor = heap->GetCPUDescriptorHandleForHeapStart(); D3D12_SHADER_RESOURCE_VIEW_DESC coverage_srv {}; coverage_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; coverage_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; coverage_srv.Format = DXGI_FORMAT_R8_UINT; coverage_srv.Buffer.NumElements = pixel_count; device->CreateShaderResourceView(output->coverage.Get(), &coverage_srv, descriptor); descriptor.ptr += increment; D3D12_UNORDERED_ACCESS_VIEW_DESC linear_uav {}; linear_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; linear_uav.Format = DXGI_FORMAT_UNKNOWN; linear_uav.Buffer.NumElements = pixel_count; linear_uav.Buffer.StructureByteStride = 3 * sizeof(float); device->CreateUnorderedAccessView(output->linear.Get(), nullptr, &linear_uav, descriptor);
    ID3D12Resource *resources[] = {output->coverage.Get(), output->linear.Get()}; const D3D12_RESOURCE_STATES readable_states[] = {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}; D3D12_RESOURCE_BARRIER transitions[2] {}; for (UINT index = 0; index < 2; ++index) { transitions[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; transitions[index].Transition = {resources[index], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, readable_states[index]}; } list->ResourceBarrier(2, transitions); ID3D12DescriptorHeap *heaps[] = {heap.Get()}; list->SetDescriptorHeaps(1, heaps); list->SetComputeRootSignature(root.Get()); list->SetComputeRoot32BitConstants(0, 1, &pixel_count, 0); list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart()); list->Dispatch((pixel_count + 63) / 64, 1, 1); D3D12_RESOURCE_BARRIER ordering {}; ordering.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; ordering.UAV.pResource = output->linear.Get(); list->ResourceBarrier(1, &ordering); for (UINT index = 0; index < 2; ++index) std::swap(transitions[index].Transition.StateBefore, transitions[index].Transition.StateAfter); list->ResourceBarrier(2, transitions);
    if (FAILED(list->Close())) { write_error(error_buffer, error_buffer_size, "cannot close incomplete-output command list"); return PANO_GPU_UNAVAILABLE; } ID3D12CommandList *lists[] = {list.Get()}; output->session->device_core->queue->ExecuteCommandLists(1, lists); const uint64_t fence = output->session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1; if (FAILED(output->session->device_core->queue->Signal(output->session->device_core->fence.Get(), fence))) { write_error(error_buffer, error_buffer_size, "cannot signal incomplete-output fence"); return PANO_GPU_UNAVAILABLE; } return wait_for_fence(output->session->device_core.get(), fence, error_buffer, error_buffer_size, "incomplete-output fence timed out");
#endif
}

static pano_gpu_result compose_output_with_inputs(
    pano_gpu_output *const output, const pano_gpu_ordered_hard_composite_request *const request,
    const pano_gpu_composite_inputs *const inputs, const bool feather_mode, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (output == nullptr || output->session == nullptr || inputs == nullptr || inputs->size != sizeof(*inputs) ||
        inputs->abi_version != PANO_GPU_ABI_VERSION || inputs->use_session_exposure_gains > 1 ||
        inputs->mark_incomplete > 1 || inputs->reserved != 0 ||
        (inputs->use_session_exposure_gains != 0 && inputs->global_gains != nullptr) ||
        (inputs->global_gains == nullptr && inputs->global_gain_bytes != 0) ||
        (inputs->local_fields == nullptr && inputs->local_field_bytes != 0))
    {
        write_error(error_buffer, error_buffer_size, "invalid production composite inputs");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    const pano_gpu_session *const session = output->session;
    const float *global_gains = inputs->global_gains;
    if (inputs->use_session_exposure_gains != 0)
    {
        if (!session->exposure_gains_uploaded || session->exposure_global_gains.size() != session->frame_count)
        {
            write_error(error_buffer, error_buffer_size, "retained exposure gains are not ready");
            return PANO_GPU_INVALID_ARGUMENT;
        }
        global_gains = session->exposure_global_gains.data();
    }
    uint64_t expected_gain_bytes = 0;
    if (!checked_multiply(session->frame_count, sizeof(float), &expected_gain_bytes) ||
        (global_gains != nullptr && inputs->use_session_exposure_gains == 0 &&
         inputs->global_gain_bytes != expected_gain_bytes))
    {
        write_error(error_buffer, error_buffer_size, "invalid production global-gain layout");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if defined(PANO_GPU_TEST_HOOKS)
#if defined(_WIN32)
    if (fail_next_device_removed_before_dispatch.exchange(false, std::memory_order_relaxed))
    {
        write_error(
            error_buffer, error_buffer_size,
            "D3D12 device removed before first numerical dispatch "
            "(HRESULT 0x887a0005; device reason 0x887a0007)");
        return PANO_GPU_UNAVAILABLE;
    }
#endif
    if (fail_next_composite_before_dispatch.exchange(false, std::memory_order_relaxed))
    {
        write_error(error_buffer, error_buffer_size, "injected failure before first D3D12 numerical dispatch");
        return PANO_GPU_UNAVAILABLE;
    }
#endif
    const pano_gpu_result compose_result = dispatch_typed_hard_composite(
        output, session, request, nullptr, 0, nullptr, 0, nullptr, 0, error_buffer, error_buffer_size,
        session->frame_count, session->source_sample_type, global_gains, inputs->local_fields,
        inputs->local_field_bytes, feather_mode);
    if (compose_result != PANO_GPU_SUCCESS)
        return compose_result;
#if defined(PANO_GPU_TEST_HOOKS)
#if defined(_WIN32)
    if (fail_next_device_removed_after_dispatch.exchange(false, std::memory_order_relaxed))
    {
        write_error(
            error_buffer, error_buffer_size,
            "D3D12 device removed after first numerical dispatch "
            "(HRESULT 0x887a0005; device reason 0x887a0007)");
        return PANO_GPU_UNAVAILABLE;
    }
#endif
    if (fail_next_composite_after_dispatch.exchange(false, std::memory_order_relaxed))
    {
        write_error(error_buffer, error_buffer_size, "injected failure after first D3D12 numerical dispatch");
        return PANO_GPU_UNAVAILABLE;
    }
#endif
    if (inputs->mark_incomplete == 0)
        return PANO_GPU_SUCCESS;
    return mark_output_incomplete(output, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_output_compose_hard(
    pano_gpu_output *const output, const pano_gpu_ordered_hard_composite_request *const request,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (output == nullptr || output->session == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "invalid production hard-composite output");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return dispatch_typed_hard_composite(
        output, output->session, request, nullptr, 0, nullptr, 0, nullptr, 0, error_buffer,
        error_buffer_size, output->session->frame_count, output->session->source_sample_type);
}

pano_gpu_result pano_gpu_output_compose_feather(
    pano_gpu_output *const output, const pano_gpu_ordered_hard_composite_request *const request,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (output == nullptr || output->session == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "invalid production feather-composite output");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return dispatch_typed_hard_composite(
        output, output->session, request, nullptr, 0, nullptr, 0, nullptr, 0, error_buffer,
        error_buffer_size, output->session->frame_count, output->session->source_sample_type,
        nullptr, nullptr, 0, true);
}

pano_gpu_result pano_gpu_output_compose_hard_with_inputs(
    pano_gpu_output *const output, const pano_gpu_ordered_hard_composite_request *const request,
    const pano_gpu_composite_inputs *const inputs, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return compose_output_with_inputs(output, request, inputs, false, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_output_compose_feather_with_inputs(
    pano_gpu_output *const output, const pano_gpu_ordered_hard_composite_request *const request,
    const pano_gpu_composite_inputs *const inputs, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return compose_output_with_inputs(output, request, inputs, true, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_test_dispatch_two_frame_uint16_hard_composite(
    const pano_gpu_session *const session, const pano_gpu_ordered_hard_composite_request *const request,
    void *const selected_rgb, const uint64_t selected_rgb_bytes, void *const selected_weight,
    const uint64_t selected_weight_bytes, void *const coverage, const uint64_t coverage_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    return dispatch_typed_hard_composite(
        nullptr, session, request, selected_rgb, selected_rgb_bytes, selected_weight, selected_weight_bytes, coverage,
        coverage_bytes, error_buffer, error_buffer_size, 2, PANO_GPU_SAMPLE_UINT16);
}

pano_gpu_result pano_gpu_test_dispatch_three_frame_uint16_hard_composite(
    const pano_gpu_session *const session, const pano_gpu_ordered_hard_composite_request *const request,
    void *const selected_rgb, const uint64_t selected_rgb_bytes, void *const selected_weight,
    const uint64_t selected_weight_bytes, void *const coverage, const uint64_t coverage_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    return dispatch_typed_hard_composite(
        nullptr, session, request, selected_rgb, selected_rgb_bytes, selected_weight, selected_weight_bytes, coverage,
        coverage_bytes, error_buffer, error_buffer_size, 3, PANO_GPU_SAMPLE_UINT16);
}

pano_gpu_result pano_gpu_test_dispatch_two_frame_float32_hard_composite(
    const pano_gpu_session *const session, const pano_gpu_ordered_hard_composite_request *const request,
    void *const selected_rgb, const uint64_t selected_rgb_bytes, void *const selected_weight,
    const uint64_t selected_weight_bytes, void *const coverage, const uint64_t coverage_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    return dispatch_typed_hard_composite(
        nullptr, session, request, selected_rgb, selected_rgb_bytes, selected_weight, selected_weight_bytes, coverage,
        coverage_bytes, error_buffer, error_buffer_size, 2, PANO_GPU_SAMPLE_FLOAT32);
}

pano_gpu_result pano_gpu_test_dispatch_three_frame_float32_hard_composite(
    const pano_gpu_session *const session, const pano_gpu_ordered_hard_composite_request *const request,
    void *const selected_rgb, const uint64_t selected_rgb_bytes, void *const selected_weight,
    const uint64_t selected_weight_bytes, void *const coverage, const uint64_t coverage_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    return dispatch_typed_hard_composite(
        nullptr, session, request, selected_rgb, selected_rgb_bytes, selected_weight, selected_weight_bytes, coverage,
        coverage_bytes, error_buffer, error_buffer_size, 3, PANO_GPU_SAMPLE_FLOAT32);
}

pano_gpu_result pano_gpu_test_dispatch_one_frame_projection(
    const pano_gpu_session *const session, const pano_gpu_one_frame_composite_request *const request,
    void *const projected_coordinates, const uint64_t projected_coordinate_bytes, void *const validity,
    const uint64_t validity_bytes, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    pano_gpu_one_frame_composite_result_layout layout {};
    layout.size = sizeof(layout);
    layout.abi_version = PANO_GPU_ABI_VERSION;
    const pano_gpu_result validation_result = pano_gpu_test_validate_one_frame_composite_request(
        session, request, &layout, error_buffer, error_buffer_size);
    if (validation_result != PANO_GPU_SUCCESS)
        return validation_result;
    uint64_t expected_coordinate_bytes = 0;
    if (!checked_multiply(layout.pixel_count, 2 * sizeof(float), &expected_coordinate_bytes) ||
        projected_coordinates == nullptr || projected_coordinate_bytes != expected_coordinate_bytes || validity == nullptr ||
        validity_bytes != layout.coverage_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 one-frame projection readback buffer");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    pano_gpu_projection_request projection {};
    projection.size = sizeof(projection);
    projection.abi_version = PANO_GPU_ABI_VERSION;
    projection.output_width = request->output_width;
    projection.output_height = request->output_height;
    projection.row_start = request->row_start;
    projection.row_count = request->row_count;
    projection.latitude_span_degrees = request->latitude_span_degrees;
    projection.horizontal_fov_degrees = request->horizontal_fov_degrees;
    projection.vertical_fov_degrees = request->vertical_fov_degrees;
    std::memcpy(projection.world_to_camera, request->world_to_camera, sizeof(projection.world_to_camera));
    return pano_gpu_test_dispatch_rays(
        session, &projection, nullptr, 0, projected_coordinates, projected_coordinate_bytes, validity, validity_bytes,
        error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_test_validate_hard_selection_request(
    const pano_gpu_session *const session, const pano_gpu_hard_selection_request *const request,
    const void *const candidate_rgb, const uint64_t candidate_rgb_bytes, const void *const candidate_validity,
    const uint64_t candidate_validity_bytes, const void *const candidate_edge_distance,
    const uint64_t candidate_edge_distance_bytes, const void *const prior_rgb, const uint64_t prior_rgb_bytes,
    const void *const prior_weight, const uint64_t prior_weight_bytes, pano_gpu_hard_selection_result_layout *const layout,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (layout == nullptr || layout->size != sizeof(*layout) || layout->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 hard-selection result layout");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    std::memset(layout, 0, sizeof(*layout));
    layout->size = sizeof(*layout);
    layout->abi_version = PANO_GPU_ABI_VERSION;
    if (session == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->pixel_count == 0 || request->reserved != 0 ||
        candidate_rgb == nullptr || candidate_validity == nullptr || candidate_edge_distance == nullptr ||
        prior_rgb == nullptr || prior_weight == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 hard-selection request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    uint64_t rgb_bytes = 0;
    uint64_t weight_bytes = 0;
    if (!checked_multiply(request->pixel_count, 3 * sizeof(float), &rgb_bytes) ||
        !checked_multiply(request->pixel_count, sizeof(float), &weight_bytes) || candidate_rgb_bytes != rgb_bytes ||
        candidate_validity_bytes != request->pixel_count || candidate_edge_distance_bytes != weight_bytes ||
        prior_rgb_bytes != rgb_bytes || prior_weight_bytes != weight_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 hard-selection buffer layout");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    const uint8_t *const validity = static_cast<const uint8_t *>(candidate_validity);
    const float *const edge_distances = static_cast<const float *>(candidate_edge_distance);
    const float *const weights = static_cast<const float *>(prior_weight);
    for (uint32_t index = 0; index < request->pixel_count; ++index)
    {
        if (validity[index] > 1 || !std::isfinite(edge_distances[index]) || edge_distances[index] < 0.0F ||
            !std::isfinite(weights[index]) || weights[index] < 0.0F)
        {
            write_error(error_buffer, error_buffer_size, "invalid D3D12 hard-selection weights or validity");
            return PANO_GPU_INVALID_ARGUMENT;
        }
    }
    layout->candidate_rgb_bytes = rgb_bytes;
    layout->candidate_validity_bytes = request->pixel_count;
    layout->candidate_edge_distance_bytes = weight_bytes;
    layout->prior_rgb_bytes = rgb_bytes;
    layout->prior_weight_bytes = weight_bytes;
    layout->selected_rgb_bytes = rgb_bytes;
    layout->selected_weight_bytes = weight_bytes;
    layout->coverage_bytes = request->pixel_count;
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_test_validate_feather_accumulation_request(
    const pano_gpu_session *const session, const pano_gpu_feather_accumulation_request *const request,
    const void *const candidate_rgb, const uint64_t candidate_rgb_bytes, const void *const candidate_validity,
    const uint64_t candidate_validity_bytes, const void *const candidate_edge_distance,
    const uint64_t candidate_edge_distance_bytes, const void *const accumulator_rgb,
    const uint64_t accumulator_rgb_bytes, const void *const accumulator_weight,
    const uint64_t accumulator_weight_bytes, pano_gpu_feather_accumulation_result_layout *const layout,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (layout == nullptr || layout->size != sizeof(*layout) || layout->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 feather-accumulation result layout");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    std::memset(layout, 0, sizeof(*layout));
    layout->size = sizeof(*layout);
    layout->abi_version = PANO_GPU_ABI_VERSION;
    if (session == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->pixel_count == 0 || request->source_width == 0 ||
        request->source_height == 0 || request->reserved != 0 || candidate_rgb == nullptr ||
        candidate_validity == nullptr || candidate_edge_distance == nullptr || accumulator_rgb == nullptr ||
        accumulator_weight == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 feather-accumulation request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    uint64_t rgb_bytes = 0;
    uint64_t weight_bytes = 0;
    if (!checked_multiply(request->pixel_count, 3 * sizeof(float), &rgb_bytes) ||
        !checked_multiply(request->pixel_count, sizeof(float), &weight_bytes) || candidate_rgb_bytes != rgb_bytes ||
        candidate_validity_bytes != request->pixel_count || candidate_edge_distance_bytes != weight_bytes ||
        accumulator_rgb_bytes != rgb_bytes || accumulator_weight_bytes != weight_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 feather-accumulation buffer layout");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    const uint8_t *const validity = static_cast<const uint8_t *>(candidate_validity);
    const float *const edge_distances = static_cast<const float *>(candidate_edge_distance);
    const float *const weights = static_cast<const float *>(accumulator_weight);
    for (uint32_t index = 0; index < request->pixel_count; ++index)
    {
        if (validity[index] > 1 || !std::isfinite(edge_distances[index]) || edge_distances[index] < 0.0F ||
            !std::isfinite(weights[index]) || weights[index] < 0.0F)
        {
            write_error(error_buffer, error_buffer_size, "invalid D3D12 feather-accumulation weights or validity");
            return PANO_GPU_INVALID_ARGUMENT;
        }
    }
    layout->candidate_rgb_bytes = rgb_bytes;
    layout->candidate_validity_bytes = request->pixel_count;
    layout->candidate_edge_distance_bytes = weight_bytes;
    layout->accumulator_rgb_bytes = rgb_bytes;
    layout->accumulator_weight_bytes = weight_bytes;
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_test_validate_exposure_request(
    const pano_gpu_session *const session, const pano_gpu_exposure_request *const request,
    const void *const global_gains, const uint64_t global_gain_bytes, const void *const local_field,
    const uint64_t local_field_bytes, pano_gpu_exposure_result_layout *const layout, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (layout == nullptr || layout->size != sizeof(*layout) || layout->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure result layout");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    std::memset(layout, 0, sizeof(*layout));
    layout->size = sizeof(*layout);
    layout->abi_version = PANO_GPU_ABI_VERSION;
    if (session == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->frame_count != session->frame_count ||
        request->output_width == 0 || request->output_height == 0 || request->reserved != 0 ||
        global_gains == nullptr || local_field == nullptr ||
        request->local_field_width != ((request->output_width - 1) / 4 + 1) ||
        request->local_field_height != ((request->output_height - 1) / 4 + 1))
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    uint64_t field_pixels = 0;
    uint64_t expected_field_bytes = 0;
    uint64_t expected_gain_bytes = 0;
    if (!checked_multiply(request->local_field_width, request->local_field_height, &field_pixels) ||
        !checked_multiply(field_pixels, sizeof(float), &expected_field_bytes) ||
        !checked_multiply(request->frame_count, sizeof(float), &expected_gain_bytes) ||
        global_gain_bytes != expected_gain_bytes || local_field_bytes != expected_field_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure buffer layout");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    const float *const gains = static_cast<const float *>(global_gains);
    const float *const field = static_cast<const float *>(local_field);
    for (uint32_t index = 0; index < request->frame_count; ++index)
        if (!std::isfinite(gains[index]) || gains[index] <= 0.0F)
        {
            write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure gain");
            return PANO_GPU_INVALID_ARGUMENT;
        }
    for (uint64_t index = 0; index < field_pixels; ++index)
        if (!std::isfinite(field[index]))
        {
            write_error(error_buffer, error_buffer_size, "invalid D3D12 local exposure field");
            return PANO_GPU_INVALID_ARGUMENT;
        }
    layout->global_gain_bytes = expected_gain_bytes;
    layout->local_field_bytes = expected_field_bytes;
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_test_plan_exposure_proxies(
    const pano_gpu_session *const session, const pano_gpu_exposure_proxy_request *const request,
    pano_gpu_exposure_proxy_layout *const layout, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (layout == nullptr || layout->size != sizeof(*layout) || layout->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-proxy layout");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    std::memset(layout, 0, sizeof(*layout));
    layout->size = sizeof(*layout);
    layout->abi_version = PANO_GPU_ABI_VERSION;
    if (session == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->frame_count != session->frame_count ||
        request->source_width != session->source_width || request->source_height != session->source_height ||
        request->source_width == 0 || request->source_height == 0 || request->reserved != 0)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-proxy request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    const uint32_t proxy_width = std::min(256U, request->source_width);
    const uint64_t scaled_height = static_cast<uint64_t>(request->source_height) * proxy_width;
    const uint64_t height_quotient = scaled_height / request->source_width;
    const uint64_t height_remainder = scaled_height % request->source_width;
    const uint64_t rounded_height = height_quotient +
        (2 * height_remainder > request->source_width ||
         (2 * height_remainder == request->source_width && height_quotient % 2 != 0));
    const uint32_t proxy_height = std::max(1U, static_cast<uint32_t>(rounded_height));
    uint64_t frame_pixels = 0;
    uint64_t frame_bytes = 0;
    uint64_t total_bytes = 0;
    if (!checked_multiply(proxy_width, proxy_height, &frame_pixels) ||
        !checked_multiply(frame_pixels, 3 * sizeof(float), &frame_bytes) ||
        !checked_multiply(frame_bytes, request->frame_count, &total_bytes))
    {
        write_error(error_buffer, error_buffer_size, "D3D12 exposure-proxy layout overflows");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    layout->proxy_width = proxy_width;
    layout->proxy_height = proxy_height;
    layout->proxy_frame_offset_bytes = frame_bytes;
    layout->proxy_frame_bytes = frame_bytes;
    layout->proxy_total_bytes = total_bytes;
    return PANO_GPU_SUCCESS;
}

static pano_gpu_result dispatch_typed_exposure_proxies(
    const pano_gpu_session *const session, const pano_gpu_exposure_proxy_request *const request,
    void *const proxies, const uint64_t proxy_bytes, char *const error_buffer,
    const uint32_t error_buffer_size, const uint32_t expected_sample_type,
    pano_gpu_session *const retained_session = nullptr) noexcept
{
    pano_gpu_exposure_proxy_layout layout {};
    layout.size = sizeof(layout);
    layout.abi_version = PANO_GPU_ABI_VERSION;
    const pano_gpu_result plan_result = pano_gpu_test_plan_exposure_proxies(
        session, request, &layout, error_buffer, error_buffer_size);
    if (plan_result != PANO_GPU_SUCCESS)
        return plan_result;
    if (session->source_sample_type != expected_sample_type ||
        (retained_session == nullptr && (proxies == nullptr || proxy_bytes != layout.proxy_total_bytes)) ||
        (retained_session != nullptr && (retained_session != session || retained_session->exposure_proxies_retained)))
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 uint8 exposure-proxy dispatch");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->fence || !session->source ||
        !session->source_is_shader_readable || session->frame_upload_fences.size() != session->frame_count)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 uint8 exposure-proxy source is not ready");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    for (const uint64_t frame_fence : session->frame_upload_fences)
    {
        if (frame_fence == 0 || session->device_core->fence->GetCompletedValue() < frame_fence)
        {
            write_error(error_buffer, error_buffer_size, "D3D12 uint8 exposure-proxy source upload is unfinished");
            return PANO_GPU_INVALID_ARGUMENT;
        }
    }
    if (session->source_bytes > std::numeric_limits<uint32_t>::max() ||
        session->source_frame_bytes > std::numeric_limits<uint32_t>::max())
    {
        write_error(error_buffer, error_buffer_size, "D3D12 uint8 exposure-proxy source is too large");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1},
    };
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 8};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC root_description {2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root_signature))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 uint8 exposure-proxy root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root_signature.Get();
    const bool is_uint16 = expected_sample_type == PANO_GPU_SAMPLE_UINT16;
    const bool is_float32 = expected_sample_type == PANO_GPU_SAMPLE_FLOAT32;
    const uint32_t source_element_bytes = is_float32 ? sizeof(float) : (is_uint16 ? sizeof(uint16_t) : sizeof(uint8_t));
    pipeline_description.CS = is_float32
        ? D3D12_SHADER_BYTECODE {pano_gpu_exposure_proxy_float32_shader, sizeof(pano_gpu_exposure_proxy_float32_shader)}
        : (is_uint16 ? D3D12_SHADER_BYTECODE {pano_gpu_exposure_proxy_uint16_shader, sizeof(pano_gpu_exposure_proxy_uint16_shader)}
                    : D3D12_SHADER_BYTECODE {pano_gpu_exposure_proxy_uint8_shader, sizeof(pano_gpu_exposure_proxy_uint8_shader)});
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 uint8 exposure-proxy pipeline");
        return PANO_GPU_UNAVAILABLE;
    }
    const auto buffer = [](const uint64_t bytes, const D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC value {};
        value.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        value.Width = bytes;
        value.Height = 1;
        value.DepthOrArraySize = 1;
        value.MipLevels = 1;
        value.SampleDesc.Count = 1;
        value.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        value.Flags = flags;
        return value;
    };
    D3D12_HEAP_PROPERTIES default_heap {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> output;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    const D3D12_RESOURCE_DESC output_desc = buffer(layout.proxy_total_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &output_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 uint8 exposure-proxy resources");
        return PANO_GPU_UNAVAILABLE;
    }
    if (retained_session == nullptr)
    {
        D3D12_HEAP_PROPERTIES readback_heap {};
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        const D3D12_RESOURCE_DESC readback_desc = buffer(layout.proxy_total_bytes, D3D12_RESOURCE_FLAG_NONE);
        if (FAILED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
        {
            write_error(error_buffer, error_buffer_size, "cannot create D3D12 uint8 exposure-proxy readback");
            return PANO_GPU_UNAVAILABLE;
        }
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 2;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&descriptor_heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 uint8 exposure-proxy dispatch");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor = descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC source_srv {};
    source_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    source_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    source_srv.Format = is_float32 ? DXGI_FORMAT_R32_FLOAT : (is_uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R8_UINT);
    source_srv.Buffer.NumElements = static_cast<UINT>(session->source_bytes / source_element_bytes);
    device->CreateShaderResourceView(session->source.Get(), &source_srv, descriptor);
    descriptor.ptr += descriptor_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC output_uav {};
    output_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    output_uav.Format = DXGI_FORMAT_UNKNOWN;
    output_uav.Buffer.NumElements = static_cast<UINT>(layout.proxy_total_bytes / (3 * sizeof(float)));
    output_uav.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateUnorderedAccessView(output.Get(), nullptr, &output_uav, descriptor);
    const uint32_t constants[8] {session->frame_count, session->source_width, session->source_height,
        layout.proxy_width, layout.proxy_height, session->source_row_stride_bytes / source_element_bytes,
        static_cast<uint32_t>(session->source_frame_bytes / source_element_bytes), session->transfer_function};
    ID3D12DescriptorHeap *heaps[] = {descriptor_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_signature.Get());
    list->SetComputeRoot32BitConstants(0, 8, constants, 0);
    list->SetComputeRootDescriptorTable(1, descriptor_heap->GetGPUDescriptorHandleForHeapStart());
    const uint64_t proxy_pixels = layout.proxy_total_bytes / (3 * sizeof(float));
    list->Dispatch(static_cast<UINT>((proxy_pixels + 63) / 64), 1, 1);
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          retained_session == nullptr ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &barrier);
    if (retained_session == nullptr)
        list->CopyResource(readback.Get(), output.Get());
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 uint8 exposure-proxy command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] = {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value = session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(session->device_core.get(), fence_value, error_buffer, error_buffer_size,
                       "D3D12 uint8 exposure-proxy fence timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    if (retained_session != nullptr)
    {
        retained_session->exposure_proxies = std::move(output);
        retained_session->exposure_proxy_bytes = layout.proxy_total_bytes;
        retained_session->exposure_proxies_retained = true;
        return PANO_GPU_SUCCESS;
    }
    const D3D12_RANGE range {0, static_cast<SIZE_T>(proxy_bytes)};
    void *mapped = nullptr;
    if (FAILED(readback->Map(0, &range, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 uint8 exposure-proxy readback");
        return PANO_GPU_UNAVAILABLE;
    }
    std::memcpy(proxies, mapped, static_cast<size_t>(proxy_bytes));
    readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_session_build_exposure_proxies(
    pano_gpu_session *const session, const pano_gpu_exposure_proxy_request *const request,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-proxy session");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return dispatch_typed_exposure_proxies(
        session, request, nullptr, 0, error_buffer, error_buffer_size, session->source_sample_type, session);
}

uint64_t pano_gpu_test_session_exposure_proxy_bytes(const pano_gpu_session *const session) noexcept
{
    return session == nullptr ? 0 : session->exposure_proxy_bytes;
}

pano_gpu_result pano_gpu_test_validate_exposure_pair_request(
    const pano_gpu_session *const session, const pano_gpu_exposure_pair_request *const request,
    const void *const paired_coordinates, const uint64_t paired_coordinate_bytes, const void *const overlap,
    const uint64_t overlap_bytes, pano_gpu_exposure_pair_layout *const layout, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (layout == nullptr || layout->size != sizeof(*layout) || layout->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-pair layout");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    std::memset(layout, 0, sizeof(*layout));
    layout->size = sizeof(*layout);
    layout->abi_version = PANO_GPU_ABI_VERSION;
    if (session == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->first_frame_index >= session->frame_count ||
        request->second_frame_index >= session->frame_count || request->first_frame_index == request->second_frame_index ||
        request->sample_width == 0 || request->sample_height == 0 || request->reserved != 0 ||
        !std::isfinite(request->latitude_span_degrees) || !std::isfinite(request->horizontal_fov_degrees) ||
        !std::isfinite(request->vertical_fov_degrees) || request->latitude_span_degrees <= 0.0F ||
        request->latitude_span_degrees > 180.0F || request->horizontal_fov_degrees <= 0.0F ||
        request->horizontal_fov_degrees >= 180.0F || request->vertical_fov_degrees <= 0.0F ||
        request->vertical_fov_degrees >= 180.0F || paired_coordinates == nullptr || overlap == nullptr ||
        !session->exposure_proxies_retained || session->exposure_proxy_bytes == 0 ||
        !session->rotations_uploaded ||
        session->frame_upload_fences.size() != session->frame_count ||
        session->frame_upload_fences[request->first_frame_index] == 0 ||
        session->frame_upload_fences[request->second_frame_index] == 0)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-pair request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    uint64_t samples = 0;
    uint64_t coordinate_bytes = 0;
    if (!checked_multiply(request->sample_width, request->sample_height, &samples) ||
        !checked_multiply(samples, 4 * sizeof(float), &coordinate_bytes) ||
        paired_coordinate_bytes != coordinate_bytes || overlap_bytes != samples)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-pair buffers");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if defined(_WIN32)
    if (!session->device_core || !session->device_core->fence ||
        session->device_core->fence->GetCompletedValue() < session->frame_upload_fences[request->first_frame_index] ||
        session->device_core->fence->GetCompletedValue() < session->frame_upload_fences[request->second_frame_index])
    {
        write_error(error_buffer, error_buffer_size, "D3D12 exposure-pair source upload is unfinished");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#endif
    layout->sample_count = samples;
    layout->paired_coordinate_bytes = coordinate_bytes;
    layout->overlap_bytes = samples;
    return PANO_GPU_SUCCESS;
}

pano_gpu_result pano_gpu_test_dispatch_exposure_pair_projection(
    const pano_gpu_session *const session, const pano_gpu_exposure_pair_request *const request,
    void *const paired_coordinates, const uint64_t paired_coordinate_bytes, void *const overlap,
    const uint64_t overlap_bytes, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    pano_gpu_exposure_pair_layout layout {};
    layout.size = sizeof(layout);
    layout.abi_version = PANO_GPU_ABI_VERSION;
    const pano_gpu_result validation = pano_gpu_test_validate_exposure_pair_request(
        session, request, paired_coordinates, paired_coordinate_bytes, overlap, overlap_bytes, &layout,
        error_buffer, error_buffer_size);
    if (validation != PANO_GPU_SUCCESS)
        return validation;
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue ||
        !session->device_core->fence || !session->rotations || !session->exposure_proxies ||
        layout.sample_count > std::numeric_limits<UINT>::max())
    {
        write_error(error_buffer, error_buffer_size, "D3D12 exposure-pair resources are not ready");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const auto buffer = [](const uint64_t bytes, const D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC value {};
        value.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        value.Width = bytes;
        value.Height = 1;
        value.DepthOrArraySize = 1;
        value.MipLevels = 1;
        value.SampleDesc.Count = 1;
        value.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        value.Flags = flags;
        return value;
    };
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 1},
    };
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 9};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC root_description {2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root_signature))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root_signature.Get();
    pipeline_description.CS = {
        pano_gpu_exposure_pair_projection_shader, sizeof(pano_gpu_exposure_pair_projection_shader)};
    if (FAILED(device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair pipeline");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES default_heap {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    Microsoft::WRL::ComPtr<ID3D12Resource> coordinate_output;
    Microsoft::WRL::ComPtr<ID3D12Resource> overlap_output;
    Microsoft::WRL::ComPtr<ID3D12Resource> coordinate_readback;
    Microsoft::WRL::ComPtr<ID3D12Resource> overlap_readback;
    const uint64_t overlap_word_bytes = layout.sample_count * sizeof(uint32_t);
    const D3D12_RESOURCE_DESC coordinate_output_desc =
        buffer(layout.paired_coordinate_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC overlap_output_desc =
        buffer(overlap_word_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC coordinate_readback_desc =
        buffer(layout.paired_coordinate_bytes, D3D12_RESOURCE_FLAG_NONE);
    const D3D12_RESOURCE_DESC overlap_readback_desc = buffer(overlap_word_bytes, D3D12_RESOURCE_FLAG_NONE);
    if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE,
            &coordinate_output_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&coordinate_output))) ||
        FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE,
            &overlap_output_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&overlap_output))) ||
        FAILED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
            &coordinate_readback_desc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&coordinate_readback))) ||
        FAILED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
            &overlap_readback_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&overlap_readback))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair resources");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 3;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&descriptor_heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(),
            IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair dispatch");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor = descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC rotation_srv {};
    rotation_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    rotation_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    rotation_srv.Format = DXGI_FORMAT_R32_FLOAT;
    rotation_srv.Buffer.NumElements = static_cast<UINT>(session->rotation_bytes / sizeof(float));
    device->CreateShaderResourceView(session->rotations.Get(), &rotation_srv, descriptor);
    descriptor.ptr += descriptor_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC coordinate_uav {};
    coordinate_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    coordinate_uav.Format = DXGI_FORMAT_UNKNOWN;
    coordinate_uav.Buffer.NumElements = static_cast<UINT>(layout.sample_count);
    coordinate_uav.Buffer.StructureByteStride = 4 * sizeof(float);
    device->CreateUnorderedAccessView(coordinate_output.Get(), nullptr, &coordinate_uav, descriptor);
    descriptor.ptr += descriptor_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC overlap_uav {};
    overlap_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    overlap_uav.Format = DXGI_FORMAT_UNKNOWN;
    overlap_uav.Buffer.NumElements = static_cast<UINT>(layout.sample_count);
    overlap_uav.Buffer.StructureByteStride = sizeof(uint32_t);
    device->CreateUnorderedAccessView(overlap_output.Get(), nullptr, &overlap_uav, descriptor);
    uint32_t constants[9] {request->first_frame_index, request->second_frame_index, request->sample_width,
        request->sample_height};
    pano_gpu_exposure_proxy_request proxy_request {};
    proxy_request.size = sizeof(proxy_request);
    proxy_request.abi_version = PANO_GPU_ABI_VERSION;
    proxy_request.frame_count = session->frame_count;
    proxy_request.source_width = session->source_width;
    proxy_request.source_height = session->source_height;
    pano_gpu_exposure_proxy_layout proxy_layout {};
    proxy_layout.size = sizeof(proxy_layout);
    proxy_layout.abi_version = PANO_GPU_ABI_VERSION;
    if (pano_gpu_test_plan_exposure_proxies(session, &proxy_request, &proxy_layout, error_buffer, error_buffer_size) !=
        PANO_GPU_SUCCESS)
        return PANO_GPU_INVALID_ARGUMENT;
    constants[4] = proxy_layout.proxy_width;
    constants[5] = proxy_layout.proxy_height;
    std::memcpy(&constants[6], &request->latitude_span_degrees, sizeof(float));
    std::memcpy(&constants[7], &request->horizontal_fov_degrees, sizeof(float));
    std::memcpy(&constants[8], &request->vertical_fov_degrees, sizeof(float));
    ID3D12DescriptorHeap *heaps[] = {descriptor_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_signature.Get());
    list->SetComputeRoot32BitConstants(0, 9, constants, 0);
    list->SetComputeRootDescriptorTable(1, descriptor_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(static_cast<UINT>((layout.sample_count + 63) / 64), 1, 1);
    D3D12_RESOURCE_BARRIER barriers[2] {};
    for (uint32_t index = 0; index < 2; ++index)
    {
        barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[index].Transition.pResource = index == 0 ? coordinate_output.Get() : overlap_output.Get();
        barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[index].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    list->ResourceBarrier(2, barriers);
    list->CopyResource(coordinate_readback.Get(), coordinate_output.Get());
    list->CopyResource(overlap_readback.Get(), overlap_output.Get());
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 exposure-pair command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] = {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value = session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(session->device_core.get(), fence_value, error_buffer, error_buffer_size,
            "D3D12 exposure-pair fence timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    const D3D12_RANGE coordinate_range {0, static_cast<SIZE_T>(layout.paired_coordinate_bytes)};
    const D3D12_RANGE overlap_range {0, static_cast<SIZE_T>(overlap_word_bytes)};
    void *mapped_coordinates = nullptr;
    void *mapped_overlap = nullptr;
    const HRESULT coordinate_map_result = coordinate_readback->Map(0, &coordinate_range, &mapped_coordinates);
    const HRESULT overlap_map_result = SUCCEEDED(coordinate_map_result)
        ? overlap_readback->Map(0, &overlap_range, &mapped_overlap)
        : coordinate_map_result;
    if (SUCCEEDED(coordinate_map_result) && SUCCEEDED(overlap_map_result))
    {
        std::memcpy(paired_coordinates, mapped_coordinates, static_cast<size_t>(layout.paired_coordinate_bytes));
        const auto *const overlap_words = static_cast<const uint32_t *>(mapped_overlap);
        auto *const overlap_bytes_out = static_cast<uint8_t *>(overlap);
        for (uint64_t index = 0; index < layout.sample_count; ++index)
            overlap_bytes_out[index] = static_cast<uint8_t>(overlap_words[index] != 0);
        overlap_readback->Unmap(0, nullptr);
        coordinate_readback->Unmap(0, nullptr);
        return PANO_GPU_SUCCESS;
    }
    if (mapped_overlap != nullptr)
        overlap_readback->Unmap(0, nullptr);
    if (mapped_coordinates != nullptr)
        coordinate_readback->Unmap(0, nullptr);
    write_error(error_buffer, error_buffer_size, "cannot map D3D12 exposure-pair readback");
    return PANO_GPU_UNAVAILABLE;
#endif
}

pano_gpu_result pano_gpu_test_dispatch_exposure_pair_samples(
    const pano_gpu_session *const session, const pano_gpu_exposure_pair_request *const request,
    const void *const paired_coordinates, const uint64_t paired_coordinate_bytes,
    const void *const geometric_overlap, const uint64_t geometric_overlap_bytes, void *const sampled_pairs,
    const uint64_t sampled_pair_bytes, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    pano_gpu_exposure_pair_layout layout {};
    layout.size = sizeof(layout);
    layout.abi_version = PANO_GPU_ABI_VERSION;
    const pano_gpu_result validation = pano_gpu_test_validate_exposure_pair_request(
        session, request, paired_coordinates, paired_coordinate_bytes, geometric_overlap, geometric_overlap_bytes,
        &layout, error_buffer, error_buffer_size);
    uint64_t expected_sampled_pair_bytes = 0;
    if (validation != PANO_GPU_SUCCESS || sampled_pairs == nullptr ||
        !checked_multiply(layout.sample_count, 6 * sizeof(float), &expected_sampled_pair_bytes) ||
        sampled_pair_bytes != expected_sampled_pair_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-pair sample buffers");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue ||
        !session->device_core->fence || !session->exposure_proxies ||
        layout.sample_count > std::numeric_limits<UINT>::max())
    {
        write_error(error_buffer, error_buffer_size, "D3D12 exposure-pair proxy resources are not ready");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    pano_gpu_exposure_proxy_request proxy_request {};
    proxy_request.size = sizeof(proxy_request);
    proxy_request.abi_version = PANO_GPU_ABI_VERSION;
    proxy_request.frame_count = session->frame_count;
    proxy_request.source_width = session->source_width;
    proxy_request.source_height = session->source_height;
    pano_gpu_exposure_proxy_layout proxy_layout {};
    proxy_layout.size = sizeof(proxy_layout);
    proxy_layout.abi_version = PANO_GPU_ABI_VERSION;
    if (pano_gpu_test_plan_exposure_proxies(session, &proxy_request, &proxy_layout, error_buffer, error_buffer_size) !=
        PANO_GPU_SUCCESS)
        return PANO_GPU_INVALID_ARGUMENT;
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const auto buffer = [](const uint64_t bytes, const D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC value {};
        value.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        value.Width = bytes;
        value.Height = 1;
        value.DepthOrArraySize = 1;
        value.MipLevels = 1;
        value.SampleDesc.Count = 1;
        value.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        value.Flags = flags;
        return value;
    };
    D3D12_DESCRIPTOR_RANGE ranges[2] {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 2}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 5};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC root_description {2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(D3D12SerializeRootSignature(&root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&root_signature))))
    { write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair sample root signature"); return PANO_GPU_UNAVAILABLE; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root_signature.Get();
    pipeline_description.CS = {pano_gpu_exposure_pair_samples_shader, sizeof(pano_gpu_exposure_pair_samples_shader)};
    if (FAILED(device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline))))
    { write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair sample pipeline"); return PANO_GPU_UNAVAILABLE; }
    D3D12_HEAP_PROPERTIES upload_heap {}; upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES default_heap {}; default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES readback_heap {}; readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    const uint64_t sample_rgb_bytes = layout.sample_count * 3 * sizeof(float);
    const D3D12_RESOURCE_DESC coordinate_desc = buffer(layout.paired_coordinate_bytes, D3D12_RESOURCE_FLAG_NONE);
    const D3D12_RESOURCE_DESC sample_output_desc = buffer(sample_rgb_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC sample_readback_desc = buffer(sample_rgb_bytes, D3D12_RESOURCE_FLAG_NONE);
    Microsoft::WRL::ComPtr<ID3D12Resource> coordinates, first_output, second_output, first_readback, second_readback;
    if (FAILED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &coordinate_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&coordinates))) ||
        FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &sample_output_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&first_output))) ||
        FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &sample_output_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&second_output))) ||
        FAILED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &sample_readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&first_readback))) ||
        FAILED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &sample_readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&second_readback))))
    { write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair sample resources"); return PANO_GPU_UNAVAILABLE; }
    void *mapped_coordinates = nullptr;
    if (FAILED(coordinates->Map(0, nullptr, &mapped_coordinates)))
    { write_error(error_buffer, error_buffer_size, "cannot map D3D12 exposure-pair coordinates"); return PANO_GPU_UNAVAILABLE; }
    std::memcpy(mapped_coordinates, paired_coordinates, static_cast<size_t>(layout.paired_coordinate_bytes));
    coordinates->Unmap(0, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap_description.NumDescriptors = 4;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&descriptor_heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
    { write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair sample dispatch"); return PANO_GPU_UNAVAILABLE; }
    const UINT descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor = descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC proxy_srv {};
    proxy_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; proxy_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    proxy_srv.Format = DXGI_FORMAT_R32_FLOAT; proxy_srv.Buffer.NumElements = static_cast<UINT>(session->exposure_proxy_bytes / sizeof(float));
    device->CreateShaderResourceView(session->exposure_proxies.Get(), &proxy_srv, descriptor); descriptor.ptr += descriptor_size;
    D3D12_SHADER_RESOURCE_VIEW_DESC coordinate_srv {};
    coordinate_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; coordinate_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    coordinate_srv.Format = DXGI_FORMAT_UNKNOWN; coordinate_srv.Buffer.NumElements = static_cast<UINT>(layout.sample_count); coordinate_srv.Buffer.StructureByteStride = 4 * sizeof(float);
    device->CreateShaderResourceView(coordinates.Get(), &coordinate_srv, descriptor); descriptor.ptr += descriptor_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC sample_uav {};
    sample_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; sample_uav.Format = DXGI_FORMAT_UNKNOWN; sample_uav.Buffer.NumElements = static_cast<UINT>(layout.sample_count); sample_uav.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateUnorderedAccessView(first_output.Get(), nullptr, &sample_uav, descriptor); descriptor.ptr += descriptor_size;
    device->CreateUnorderedAccessView(second_output.Get(), nullptr, &sample_uav, descriptor);
    const uint32_t constants[5] {request->first_frame_index, request->second_frame_index, static_cast<uint32_t>(layout.sample_count), proxy_layout.proxy_width, proxy_layout.proxy_height};
    ID3D12DescriptorHeap *heaps[] = {descriptor_heap.Get()}; list->SetDescriptorHeaps(1, heaps); list->SetComputeRootSignature(root_signature.Get()); list->SetComputeRoot32BitConstants(0, 5, constants, 0); list->SetComputeRootDescriptorTable(1, descriptor_heap->GetGPUDescriptorHandleForHeapStart()); list->Dispatch(static_cast<UINT>((layout.sample_count + 63) / 64), 1, 1);
    D3D12_RESOURCE_BARRIER barriers[2] {};
    for (uint32_t index = 0; index < 2; ++index) { barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barriers[index].Transition.pResource = index == 0 ? first_output.Get() : second_output.Get(); barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; barriers[index].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; }
    list->ResourceBarrier(2, barriers); list->CopyResource(first_readback.Get(), first_output.Get()); list->CopyResource(second_readback.Get(), second_output.Get());
    if (FAILED(list->Close())) { write_error(error_buffer, error_buffer_size, "cannot close D3D12 exposure-pair sample command list"); return PANO_GPU_UNAVAILABLE; }
    ID3D12CommandList *lists[] = {list.Get()}; session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value = session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) || wait_for_fence(session->device_core.get(), fence_value, error_buffer, error_buffer_size, "D3D12 exposure-pair sample fence timed out") != PANO_GPU_SUCCESS) return PANO_GPU_UNAVAILABLE;
    const D3D12_RANGE range {0, static_cast<SIZE_T>(sample_rgb_bytes)}; void *first_mapped = nullptr; void *second_mapped = nullptr;
    if (FAILED(first_readback->Map(0, &range, &first_mapped)) || FAILED(second_readback->Map(0, &range, &second_mapped))) { if (second_mapped != nullptr) second_readback->Unmap(0, nullptr); if (first_mapped != nullptr) first_readback->Unmap(0, nullptr); write_error(error_buffer, error_buffer_size, "cannot map D3D12 exposure-pair samples"); return PANO_GPU_UNAVAILABLE; }
    const auto *const first = static_cast<const float *>(first_mapped); const auto *const second = static_cast<const float *>(second_mapped); auto *const output = static_cast<float *>(sampled_pairs);
    for (uint64_t index = 0; index < layout.sample_count; ++index) { std::memcpy(output + index * 6, first + index * 3, 3 * sizeof(float)); std::memcpy(output + index * 6 + 3, second + index * 3, 3 * sizeof(float)); }
    second_readback->Unmap(0, nullptr); first_readback->Unmap(0, nullptr); return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_read_resident_exposure_pair_projection_samples(
    const pano_gpu_session *const session, void *const paired_coordinates,
    const uint64_t paired_coordinate_bytes, void *const overlap, const uint64_t overlap_bytes,
    void *const sampled_pairs, const uint64_t sampled_pair_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    uint64_t expected_coordinates = 0;
    uint64_t expected_overlap_words = 0;
    uint64_t expected_sampled_pairs = 0;
    if (session == nullptr || !session->resident_pair_projected_sampled || paired_coordinates == nullptr ||
        overlap == nullptr || sampled_pairs == nullptr ||
        !checked_multiply(session->resident_pair_sample_count, 4 * sizeof(float), &expected_coordinates) ||
        !checked_multiply(session->resident_pair_sample_count, sizeof(uint32_t), &expected_overlap_words) ||
        !checked_multiply(session->resident_pair_sample_count, 6 * sizeof(float), &expected_sampled_pairs) ||
        paired_coordinate_bytes != expected_coordinates ||
        overlap_bytes != session->resident_pair_sample_count || sampled_pair_bytes != expected_sampled_pairs)
    {
        write_error(error_buffer, error_buffer_size, "invalid resident D3D12 exposure-pair readback");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const std::array<uint64_t, 4> bytes {
        expected_coordinates, expected_overlap_words,
        3 * session->resident_pair_sample_count * sizeof(float),
        3 * session->resident_pair_sample_count * sizeof(float)};
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 4> readbacks;
    for (size_t index = 0; index < readbacks.size(); ++index)
    {
        D3D12_RESOURCE_DESC description {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = bytes[index];
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&readbacks[index]))))
        {
            write_error(error_buffer, error_buffer_size, "cannot create resident D3D12 exposure-pair readback");
            return PANO_GPU_UNAVAILABLE;
        }
    }
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create resident D3D12 exposure-pair readback commands");
        return PANO_GPU_UNAVAILABLE;
    }
    std::array<D3D12_RESOURCE_BARRIER, 4> barriers {};
    for (size_t index = 0; index < barriers.size(); ++index)
    {
        barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[index].Transition.pResource = session->resident_pair_scratch[index].Get();
        barriers[index].Transition.StateBefore = index == 0
            ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[index].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    for (size_t index = 0; index < readbacks.size(); ++index)
        list->CopyResource(readbacks[index].Get(), session->resident_pair_scratch[index].Get());
    for (auto &barrier : barriers)
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close resident D3D12 exposure-pair readback");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(
            session->device_core.get(), fence_value, error_buffer, error_buffer_size,
            "resident D3D12 exposure-pair readback timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    std::array<void *, 4> mapped {};
    for (size_t index = 0; index < readbacks.size(); ++index)
    {
        const D3D12_RANGE range {0, static_cast<SIZE_T>(bytes[index])};
        if (FAILED(readbacks[index]->Map(0, &range, &mapped[index])))
        {
            for (size_t previous = 0; previous < index; ++previous)
                readbacks[previous]->Unmap(0, nullptr);
            write_error(error_buffer, error_buffer_size, "cannot map resident D3D12 exposure-pair readback");
            return PANO_GPU_UNAVAILABLE;
        }
    }
    std::memcpy(paired_coordinates, mapped[0], static_cast<size_t>(expected_coordinates));
    const auto *const overlap_words = static_cast<const uint32_t *>(mapped[1]);
    auto *const overlap_output = static_cast<uint8_t *>(overlap);
    for (uint64_t index = 0; index < session->resident_pair_sample_count; ++index)
        overlap_output[index] = static_cast<uint8_t>(overlap_words[index] != 0);
    const auto *const first_samples = static_cast<const float *>(mapped[2]);
    const auto *const second_samples = static_cast<const float *>(mapped[3]);
    auto *const sample_output = static_cast<float *>(sampled_pairs);
    for (uint64_t index = 0; index < session->resident_pair_sample_count; ++index)
    {
        std::memcpy(sample_output + 6 * index, first_samples + 3 * index, 3 * sizeof(float));
        std::memcpy(sample_output + 6 * index + 3, second_samples + 3 * index, 3 * sizeof(float));
    }
    for (auto &readback : readbacks)
        readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_read_resident_exposure_pair_classification(
    const pano_gpu_session *const session, void *const pair_luminance,
    const uint64_t pair_luminance_bytes, void *const accepted, const uint64_t accepted_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    const uint64_t expected_luminance = session == nullptr
        ? 0
        : session->resident_pair_sample_count * 2 * sizeof(float);
    const uint64_t accepted_word_bytes = session == nullptr
        ? 0
        : session->resident_pair_sample_count * sizeof(uint32_t);
    if (session == nullptr || !session->resident_pair_classified || pair_luminance == nullptr ||
        accepted == nullptr || pair_luminance_bytes != expected_luminance ||
        accepted_bytes != session->resident_pair_sample_count)
    {
        write_error(error_buffer, error_buffer_size, "invalid resident exposure classification readback");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    (void)accepted_word_bytes;
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const std::array<uint64_t, 2> sizes {expected_luminance, accepted_word_bytes};
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> readbacks;
    for (size_t index = 0; index < 2; ++index)
    {
        D3D12_RESOURCE_DESC description {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = sizes[index];
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&readbacks[index]))))
            return PANO_GPU_UNAVAILABLE;
    }
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    std::array<D3D12_RESOURCE_BARRIER, 2> barriers {};
    for (size_t index = 0; index < 2; ++index)
    {
        barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[index].Transition = {
            session->resident_pair_scratch[index + 4].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    }
    list->ResourceBarrier(2, barriers.data());
    for (size_t index = 0; index < 2; ++index)
        list->CopyResource(readbacks[index].Get(), session->resident_pair_scratch[index + 4].Get());
    for (auto &barrier : barriers)
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(2, barriers.data());
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(
            session->device_core.get(), fence_value, error_buffer, error_buffer_size,
            "resident exposure classification readback timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    void *mapped_luminance = nullptr;
    void *mapped_accepted = nullptr;
    const D3D12_RANGE luminance_range {0, static_cast<SIZE_T>(expected_luminance)};
    const D3D12_RANGE accepted_range {0, static_cast<SIZE_T>(accepted_word_bytes)};
    if (FAILED(readbacks[0]->Map(0, &luminance_range, &mapped_luminance)) ||
        FAILED(readbacks[1]->Map(0, &accepted_range, &mapped_accepted)))
    {
        if (mapped_luminance != nullptr)
            readbacks[0]->Unmap(0, nullptr);
        return PANO_GPU_UNAVAILABLE;
    }
    std::memcpy(pair_luminance, mapped_luminance, static_cast<size_t>(expected_luminance));
    const auto *const accepted_words = static_cast<const uint32_t *>(mapped_accepted);
    auto *const accepted_output = static_cast<uint8_t *>(accepted);
    for (uint64_t index = 0; index < session->resident_pair_sample_count; ++index)
        accepted_output[index] = static_cast<uint8_t>(accepted_words[index] != 0);
    readbacks[1]->Unmap(0, nullptr);
    readbacks[0]->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_classify_exposure_pair_samples(
    const pano_gpu_session *const session, const pano_gpu_exposure_pair_request *const request,
    const void *const sampled_pairs, const uint64_t sampled_pair_bytes, const void *const geometric_overlap,
    const uint64_t geometric_overlap_bytes, void *const pair_luminance, const uint64_t pair_luminance_bytes,
    void *const accepted, const uint64_t accepted_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    pano_gpu_exposure_pair_layout layout {};
    layout.size = sizeof(layout);
    layout.abi_version = PANO_GPU_ABI_VERSION;
    const pano_gpu_result validation = pano_gpu_test_validate_exposure_pair_request(
        session, request, sampled_pairs, 0, geometric_overlap, geometric_overlap_bytes, &layout, error_buffer,
        error_buffer_size);
    uint64_t expected_samples = 0;
    uint64_t expected_luminance_bytes = 0;
    if (validation == PANO_GPU_INVALID_ARGUMENT && sampled_pairs != nullptr)
    {
        // The pair validator owns request/lifetime validation; its coordinate-size requirement is irrelevant here.
        layout.size = sizeof(layout);
        layout.abi_version = PANO_GPU_ABI_VERSION;
        const pano_gpu_result layout_validation = pano_gpu_test_validate_exposure_pair_request(
            session, request, sampled_pairs, request == nullptr ? 0 :
                static_cast<uint64_t>(request->sample_width) * request->sample_height * 4 * sizeof(float),
            geometric_overlap, geometric_overlap_bytes, &layout, error_buffer, error_buffer_size);
        if (layout_validation != PANO_GPU_SUCCESS)
            return layout_validation;
    }
    else if (validation != PANO_GPU_SUCCESS)
        return validation;
    if (sampled_pairs == nullptr || pair_luminance == nullptr || accepted == nullptr ||
        !checked_multiply(layout.sample_count, 6 * sizeof(float), &expected_samples) ||
        sampled_pair_bytes != expected_samples || !checked_multiply(layout.sample_count, 2 * sizeof(float), &expected_luminance_bytes) ||
        pair_luminance_bytes != expected_luminance_bytes || accepted_bytes != layout.sample_count)
    { write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-pair classification buffers"); return PANO_GPU_INVALID_ARGUMENT; }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows"); return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue || !session->device_core->fence || layout.sample_count > std::numeric_limits<UINT>::max())
    { write_error(error_buffer, error_buffer_size, "D3D12 exposure-pair classification resources are not ready"); return PANO_GPU_INVALID_ARGUMENT; }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const auto buffer = [](const uint64_t bytes, const D3D12_RESOURCE_FLAGS flags) { D3D12_RESOURCE_DESC value {}; value.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; value.Width=bytes; value.Height=1; value.DepthOrArraySize=1; value.MipLevels=1; value.SampleDesc.Count=1; value.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; value.Flags=flags; return value; };
    D3D12_DESCRIPTOR_RANGE ranges[2] {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,2}};
    D3D12_ROOT_PARAMETER parameters[2] {}; parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[0].Constants={0,0,2}; parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[1].DescriptorTable={2,ranges}; D3D12_ROOT_SIGNATURE_DESC root_description {2,parameters,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE}; Microsoft::WRL::ComPtr<ID3DBlob> serialized,errors; Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature; Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if(FAILED(D3D12SerializeRootSignature(&root_description,D3D_ROOT_SIGNATURE_VERSION_1_0,&serialized,&errors))||FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root_signature)))) { write_error(error_buffer,error_buffer_size,"cannot create D3D12 exposure-pair classification root signature"); return PANO_GPU_UNAVAILABLE; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {}; pipeline_description.pRootSignature=root_signature.Get(); pipeline_description.CS={pano_gpu_exposure_pair_classify_shader,sizeof(pano_gpu_exposure_pair_classify_shader)}; if(FAILED(device->CreateComputePipelineState(&pipeline_description,IID_PPV_ARGS(&pipeline)))) { write_error(error_buffer,error_buffer_size,"cannot create D3D12 exposure-pair classification pipeline"); return PANO_GPU_UNAVAILABLE; }
    D3D12_HEAP_PROPERTIES upload_heap {}; upload_heap.Type=D3D12_HEAP_TYPE_UPLOAD; D3D12_HEAP_PROPERTIES default_heap {}; default_heap.Type=D3D12_HEAP_TYPE_DEFAULT; D3D12_HEAP_PROPERTIES readback_heap {}; readback_heap.Type=D3D12_HEAP_TYPE_READBACK; const uint64_t overlap_word_bytes=layout.sample_count*sizeof(uint32_t); const D3D12_RESOURCE_DESC samples_desc=buffer(sampled_pair_bytes,D3D12_RESOURCE_FLAG_NONE), overlap_desc=buffer(overlap_word_bytes,D3D12_RESOURCE_FLAG_NONE), luminance_desc=buffer(pair_luminance_bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), accepted_desc=buffer(overlap_word_bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), readback_luminance_desc=buffer(pair_luminance_bytes,D3D12_RESOURCE_FLAG_NONE), readback_accepted_desc=buffer(overlap_word_bytes,D3D12_RESOURCE_FLAG_NONE); Microsoft::WRL::ComPtr<ID3D12Resource> samples_resource,overlap_resource,luminance_resource,accepted_resource,luminance_readback,accepted_readback;
    if(FAILED(device->CreateCommittedResource(&upload_heap,D3D12_HEAP_FLAG_NONE,&samples_desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&samples_resource)))||FAILED(device->CreateCommittedResource(&upload_heap,D3D12_HEAP_FLAG_NONE,&overlap_desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&overlap_resource)))||FAILED(device->CreateCommittedResource(&default_heap,D3D12_HEAP_FLAG_NONE,&luminance_desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&luminance_resource)))||FAILED(device->CreateCommittedResource(&default_heap,D3D12_HEAP_FLAG_NONE,&accepted_desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&accepted_resource)))||FAILED(device->CreateCommittedResource(&readback_heap,D3D12_HEAP_FLAG_NONE,&readback_luminance_desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&luminance_readback)))||FAILED(device->CreateCommittedResource(&readback_heap,D3D12_HEAP_FLAG_NONE,&readback_accepted_desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&accepted_readback)))) { write_error(error_buffer,error_buffer_size,"cannot create D3D12 exposure-pair classification resources"); return PANO_GPU_UNAVAILABLE; }
    void *mapped=nullptr; if(FAILED(samples_resource->Map(0,nullptr,&mapped))) { write_error(error_buffer,error_buffer_size,"cannot map D3D12 exposure-pair samples"); return PANO_GPU_UNAVAILABLE; } std::memcpy(mapped,sampled_pairs,static_cast<size_t>(sampled_pair_bytes)); samples_resource->Unmap(0,nullptr); if(FAILED(overlap_resource->Map(0,nullptr,&mapped))) { write_error(error_buffer,error_buffer_size,"cannot map D3D12 exposure-pair overlap"); return PANO_GPU_UNAVAILABLE; } const auto *const geometric=static_cast<const uint8_t *>(geometric_overlap); auto *const overlap_words=static_cast<uint32_t *>(mapped); for(uint64_t index=0;index<layout.sample_count;++index) overlap_words[index]=geometric[index] != 0 ? 1U : 0U; overlap_resource->Unmap(0,nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {}; heap_description.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap_description.NumDescriptors=4; heap_description.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap; Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator; Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list; if(FAILED(device->CreateDescriptorHeap(&heap_description,IID_PPV_ARGS(&descriptor_heap)))||FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))||FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),pipeline.Get(),IID_PPV_ARGS(&list)))) { write_error(error_buffer,error_buffer_size,"cannot create D3D12 exposure-pair classification dispatch"); return PANO_GPU_UNAVAILABLE; }
    const UINT increment=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); auto descriptor=descriptor_heap->GetCPUDescriptorHandleForHeapStart(); D3D12_SHADER_RESOURCE_VIEW_DESC float_srv {}; float_srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER; float_srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; float_srv.Format=DXGI_FORMAT_R32_FLOAT; float_srv.Buffer.NumElements=static_cast<UINT>(sampled_pair_bytes/sizeof(float)); device->CreateShaderResourceView(samples_resource.Get(),&float_srv,descriptor); descriptor.ptr+=increment; D3D12_SHADER_RESOURCE_VIEW_DESC overlap_srv {}; overlap_srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER; overlap_srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; overlap_srv.Format=DXGI_FORMAT_R32_UINT; overlap_srv.Buffer.NumElements=static_cast<UINT>(layout.sample_count); device->CreateShaderResourceView(overlap_resource.Get(),&overlap_srv,descriptor); descriptor.ptr+=increment; D3D12_UNORDERED_ACCESS_VIEW_DESC luminance_uav {}; luminance_uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER; luminance_uav.Format=DXGI_FORMAT_UNKNOWN; luminance_uav.Buffer.NumElements=static_cast<UINT>(layout.sample_count); luminance_uav.Buffer.StructureByteStride=2*sizeof(float); device->CreateUnorderedAccessView(luminance_resource.Get(),nullptr,&luminance_uav,descriptor); descriptor.ptr+=increment; D3D12_UNORDERED_ACCESS_VIEW_DESC accepted_uav {}; accepted_uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER; accepted_uav.Format=DXGI_FORMAT_UNKNOWN; accepted_uav.Buffer.NumElements=static_cast<UINT>(layout.sample_count); accepted_uav.Buffer.StructureByteStride=sizeof(uint32_t); device->CreateUnorderedAccessView(accepted_resource.Get(),nullptr,&accepted_uav,descriptor);
    const uint32_t constants[] {static_cast<uint32_t>(layout.sample_count), session->transfer_function}; ID3D12DescriptorHeap *heaps[]{descriptor_heap.Get()}; list->SetDescriptorHeaps(1,heaps); list->SetComputeRootSignature(root_signature.Get()); list->SetComputeRoot32BitConstants(0,2,constants,0); list->SetComputeRootDescriptorTable(1,descriptor_heap->GetGPUDescriptorHandleForHeapStart()); list->Dispatch(static_cast<UINT>((layout.sample_count+63)/64),1,1); D3D12_RESOURCE_BARRIER barriers[2] {}; for(uint32_t index=0;index<2;++index){barriers[index].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barriers[index].Transition.pResource=index==0?luminance_resource.Get():accepted_resource.Get();barriers[index].Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;barriers[index].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;barriers[index].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;} list->ResourceBarrier(2,barriers); list->CopyResource(luminance_readback.Get(),luminance_resource.Get()); list->CopyResource(accepted_readback.Get(),accepted_resource.Get()); if(FAILED(list->Close())) { write_error(error_buffer,error_buffer_size,"cannot close D3D12 exposure-pair classification command list"); return PANO_GPU_UNAVAILABLE; } ID3D12CommandList *lists[]{list.Get()}; session->device_core->queue->ExecuteCommandLists(1,lists); const uint64_t fence_value=session->device_core->next_fence_value.fetch_add(1,std::memory_order_relaxed)+1; if(FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(),fence_value))||wait_for_fence(session->device_core.get(),fence_value,error_buffer,error_buffer_size,"D3D12 exposure-pair classification fence timed out")!=PANO_GPU_SUCCESS)return PANO_GPU_UNAVAILABLE;
    const D3D12_RANGE luminance_range{0,static_cast<SIZE_T>(pair_luminance_bytes)},accepted_range{0,static_cast<SIZE_T>(overlap_word_bytes)}; void *mapped_luminance=nullptr,*mapped_accepted=nullptr; if(FAILED(luminance_readback->Map(0,&luminance_range,&mapped_luminance))||FAILED(accepted_readback->Map(0,&accepted_range,&mapped_accepted))){if(mapped_accepted!=nullptr)accepted_readback->Unmap(0,nullptr);if(mapped_luminance!=nullptr)luminance_readback->Unmap(0,nullptr);write_error(error_buffer,error_buffer_size,"cannot map D3D12 exposure-pair classification readback");return PANO_GPU_UNAVAILABLE;} std::memcpy(pair_luminance,mapped_luminance,static_cast<size_t>(pair_luminance_bytes)); const auto *const accepted_words=static_cast<const uint32_t *>(mapped_accepted);auto *const accepted_bytes_out=static_cast<uint8_t *>(accepted);for(uint64_t index=0;index<layout.sample_count;++index)accepted_bytes_out[index]=static_cast<uint8_t>(accepted_words[index]!=0);accepted_readback->Unmap(0,nullptr);luminance_readback->Unmap(0,nullptr);return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_dispatch_exposure_pair_gradients(
    const pano_gpu_session *const session, const pano_gpu_exposure_pair_request *const request,
    const void *const pair_luminance, const uint64_t pair_luminance_bytes, void *const gradients,
    const uint64_t gradient_bytes, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    pano_gpu_exposure_pair_layout layout {};
    layout.size = sizeof(layout);
    layout.abi_version = PANO_GPU_ABI_VERSION;
    uint64_t coordinate_bytes = 0;
    uint64_t sample_count = 0;
    uint8_t validation_overlap = 0;
    if (request == nullptr || !checked_multiply(static_cast<uint64_t>(request->sample_width), request->sample_height, &coordinate_bytes) ||
        !checked_multiply(coordinate_bytes, 4 * sizeof(float), &coordinate_bytes) ||
        !checked_multiply(static_cast<uint64_t>(request->sample_width), request->sample_height, &sample_count) ||
        pano_gpu_test_validate_exposure_pair_request(session, request, pair_luminance, coordinate_bytes, &validation_overlap, sample_count, &layout, error_buffer, error_buffer_size) != PANO_GPU_SUCCESS)
    { write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-pair gradient request"); return PANO_GPU_INVALID_ARGUMENT; }
    uint64_t expected_bytes = 0;
    if (pair_luminance == nullptr || gradients == nullptr || !checked_multiply(layout.sample_count, 2 * sizeof(float), &expected_bytes) ||
        pair_luminance_bytes != expected_bytes || gradient_bytes != expected_bytes)
    { write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-pair gradient buffers"); return PANO_GPU_INVALID_ARGUMENT; }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows"); return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue || !session->device_core->fence || layout.sample_count > std::numeric_limits<UINT>::max())
    { write_error(error_buffer, error_buffer_size, "D3D12 exposure-pair gradient resources are not ready"); return PANO_GPU_INVALID_ARGUMENT; }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const auto buffer = [](const uint64_t bytes, const D3D12_RESOURCE_FLAGS flags) { D3D12_RESOURCE_DESC value {}; value.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; value.Width=bytes; value.Height=1; value.DepthOrArraySize=1; value.MipLevels=1; value.SampleDesc.Count=1; value.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; value.Flags=flags; return value; };
    D3D12_DESCRIPTOR_RANGE ranges[2] {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1}}; D3D12_ROOT_PARAMETER parameters[2] {}; parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[0].Constants={0,0,2}; parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[1].DescriptorTable={2,ranges}; D3D12_ROOT_SIGNATURE_DESC root_description {2,parameters,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE}; Microsoft::WRL::ComPtr<ID3DBlob> serialized,errors; Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature; Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if(FAILED(D3D12SerializeRootSignature(&root_description,D3D_ROOT_SIGNATURE_VERSION_1_0,&serialized,&errors))||FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root_signature)))) { write_error(error_buffer,error_buffer_size,"cannot create D3D12 exposure-pair gradient root signature"); return PANO_GPU_UNAVAILABLE; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {}; pipeline_description.pRootSignature=root_signature.Get(); pipeline_description.CS={pano_gpu_exposure_pair_gradient_shader,sizeof(pano_gpu_exposure_pair_gradient_shader)}; if(FAILED(device->CreateComputePipelineState(&pipeline_description,IID_PPV_ARGS(&pipeline)))) { write_error(error_buffer,error_buffer_size,"cannot create D3D12 exposure-pair gradient pipeline"); return PANO_GPU_UNAVAILABLE; }
    D3D12_HEAP_PROPERTIES upload_heap {}; upload_heap.Type=D3D12_HEAP_TYPE_UPLOAD; D3D12_HEAP_PROPERTIES default_heap {}; default_heap.Type=D3D12_HEAP_TYPE_DEFAULT; D3D12_HEAP_PROPERTIES readback_heap {}; readback_heap.Type=D3D12_HEAP_TYPE_READBACK; const D3D12_RESOURCE_DESC input_desc=buffer(pair_luminance_bytes,D3D12_RESOURCE_FLAG_NONE), output_desc=buffer(gradient_bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), readback_desc=buffer(gradient_bytes,D3D12_RESOURCE_FLAG_NONE); Microsoft::WRL::ComPtr<ID3D12Resource> input,output,readback;
    if(FAILED(device->CreateCommittedResource(&upload_heap,D3D12_HEAP_FLAG_NONE,&input_desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&input)))||FAILED(device->CreateCommittedResource(&default_heap,D3D12_HEAP_FLAG_NONE,&output_desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&output)))||FAILED(device->CreateCommittedResource(&readback_heap,D3D12_HEAP_FLAG_NONE,&readback_desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)))) { write_error(error_buffer,error_buffer_size,"cannot create D3D12 exposure-pair gradient resources"); return PANO_GPU_UNAVAILABLE; }
    void *mapped=nullptr; if(FAILED(input->Map(0,nullptr,&mapped))) { write_error(error_buffer,error_buffer_size,"cannot map D3D12 exposure-pair gradient input"); return PANO_GPU_UNAVAILABLE; } std::memcpy(mapped,pair_luminance,static_cast<size_t>(pair_luminance_bytes)); input->Unmap(0,nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {}; heap_description.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap_description.NumDescriptors=2; heap_description.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap; Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator; Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list; if(FAILED(device->CreateDescriptorHeap(&heap_description,IID_PPV_ARGS(&descriptor_heap)))||FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))||FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),pipeline.Get(),IID_PPV_ARGS(&list)))) { write_error(error_buffer,error_buffer_size,"cannot create D3D12 exposure-pair gradient dispatch"); return PANO_GPU_UNAVAILABLE; }
    const UINT increment=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); auto descriptor=descriptor_heap->GetCPUDescriptorHandleForHeapStart(); D3D12_SHADER_RESOURCE_VIEW_DESC input_srv {}; input_srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER; input_srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; input_srv.Format=DXGI_FORMAT_UNKNOWN; input_srv.Buffer.NumElements=static_cast<UINT>(layout.sample_count); input_srv.Buffer.StructureByteStride=2*sizeof(float); device->CreateShaderResourceView(input.Get(),&input_srv,descriptor); descriptor.ptr+=increment; D3D12_UNORDERED_ACCESS_VIEW_DESC output_uav {}; output_uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER; output_uav.Format=DXGI_FORMAT_UNKNOWN; output_uav.Buffer.NumElements=static_cast<UINT>(layout.sample_count); output_uav.Buffer.StructureByteStride=2*sizeof(float); device->CreateUnorderedAccessView(output.Get(),nullptr,&output_uav,descriptor); const uint32_t constants[]{request->sample_width,request->sample_height}; ID3D12DescriptorHeap *heaps[]{descriptor_heap.Get()}; list->SetDescriptorHeaps(1,heaps); list->SetComputeRootSignature(root_signature.Get()); list->SetComputeRoot32BitConstants(0,2,constants,0); list->SetComputeRootDescriptorTable(1,descriptor_heap->GetGPUDescriptorHandleForHeapStart()); list->Dispatch(static_cast<UINT>((layout.sample_count+63)/64),1,1); D3D12_RESOURCE_BARRIER barrier {}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition.pResource=output.Get(); barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS; barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE; barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; list->ResourceBarrier(1,&barrier); list->CopyResource(readback.Get(),output.Get()); if(FAILED(list->Close())) { write_error(error_buffer,error_buffer_size,"cannot close D3D12 exposure-pair gradient command list"); return PANO_GPU_UNAVAILABLE; } ID3D12CommandList *lists[]{list.Get()}; session->device_core->queue->ExecuteCommandLists(1,lists); const uint64_t fence_value=session->device_core->next_fence_value.fetch_add(1,std::memory_order_relaxed)+1; if(FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(),fence_value))||wait_for_fence(session->device_core.get(),fence_value,error_buffer,error_buffer_size,"D3D12 exposure-pair gradient fence timed out")!=PANO_GPU_SUCCESS)return PANO_GPU_UNAVAILABLE; const D3D12_RANGE range{0,static_cast<SIZE_T>(gradient_bytes)}; if(FAILED(readback->Map(0,&range,&mapped))) { write_error(error_buffer,error_buffer_size,"cannot map D3D12 exposure-pair gradient readback"); return PANO_GPU_UNAVAILABLE; } std::memcpy(gradients,mapped,static_cast<size_t>(gradient_bytes)); readback->Unmap(0,nullptr); return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_compute_exposure_pair_gradient_limits(
    const pano_gpu_session *const session, const pano_gpu_exposure_pair_request *const request,
    const void *const gradients, const uint64_t gradient_bytes, float *const gradient_limits,
    const uint64_t gradient_limit_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    uint64_t sample_count = 0;
    uint64_t expected_gradient_bytes = 0;
    if (session == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->first_frame_index >= session->frame_count ||
        request->second_frame_index >= session->frame_count ||
        request->first_frame_index == request->second_frame_index || request->sample_width == 0 ||
        request->sample_height == 0 || gradients == nullptr || gradient_limits == nullptr ||
        !checked_multiply(
            static_cast<uint64_t>(request->sample_width), request->sample_height, &sample_count) ||
        !checked_multiply(sample_count, 2 * sizeof(float), &expected_gradient_bytes) ||
        gradient_bytes != expected_gradient_bytes || gradient_limit_bytes != 2 * sizeof(float))
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-gradient limit arguments");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    uint64_t capacity = 1;
    while (capacity < sample_count)
    {
        if (capacity > std::numeric_limits<uint64_t>::max() / 2)
        {
            write_error(error_buffer, error_buffer_size, "D3D12 exposure-gradient sort capacity overflows");
            return PANO_GPU_INVALID_ARGUMENT;
        }
        capacity *= 2;
    }
    uint64_t sortable_count = 0;
    uint64_t sortable_bytes = 0;
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue ||
        !session->device_core->fence || !checked_multiply(capacity, 2, &sortable_count) ||
        !checked_multiply(sortable_count, sizeof(float), &sortable_bytes) ||
        sortable_count > std::numeric_limits<UINT>::max() || capacity > std::numeric_limits<UINT>::max())
    {
        write_error(error_buffer, error_buffer_size, "D3D12 exposure-gradient resources are not ready");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const auto buffer = [](const uint64_t bytes, const D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC value {};
        value.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        value.Width = bytes;
        value.Height = 1;
        value.DepthOrArraySize = 1;
        value.MipLevels = 1;
        value.SampleDesc.Count = 1;
        value.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        value.Flags = flags;
        return value;
    };
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 3};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC root_description {
        2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&root_signature))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-gradient root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    const auto create_pipeline = [&](const BYTE *const shader, const size_t shader_bytes,
                                     Microsoft::WRL::ComPtr<ID3D12PipelineState> *const pipeline) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC description {};
        description.pRootSignature = root_signature.Get();
        description.CS = {shader, shader_bytes};
        return device->CreateComputePipelineState(&description, IID_PPV_ARGS(pipeline->GetAddressOf()));
    };
    Microsoft::WRL::ComPtr<ID3D12PipelineState> prepare_pipeline, sort_pipeline, bounds_pipeline;
    if (FAILED(create_pipeline(
            pano_gpu_exposure_gradient_sort_prepare_shader,
            sizeof(pano_gpu_exposure_gradient_sort_prepare_shader), &prepare_pipeline)) ||
        FAILED(create_pipeline(
            pano_gpu_exposure_gradient_sort_shader,
            sizeof(pano_gpu_exposure_gradient_sort_shader), &sort_pipeline)) ||
        FAILED(create_pipeline(
            pano_gpu_exposure_gradient_bounds_shader,
            sizeof(pano_gpu_exposure_gradient_bounds_shader), &bounds_pipeline)))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-gradient pipelines");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES upload_heap {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES default_heap {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    const D3D12_RESOURCE_DESC gradient_description = buffer(gradient_bytes, D3D12_RESOURCE_FLAG_NONE);
    const D3D12_RESOURCE_DESC sortable_description =
        buffer(sortable_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC limit_description =
        buffer(2 * sizeof(float), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC readback_description = buffer(2 * sizeof(float), D3D12_RESOURCE_FLAG_NONE);
    Microsoft::WRL::ComPtr<ID3D12Resource> gradient_resource, sortable_resource, limit_resource, readback;
    if (FAILED(device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &gradient_description,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&gradient_resource))) ||
        FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &sortable_description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&sortable_resource))) ||
        FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &limit_description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&limit_resource))) ||
        FAILED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-gradient resources");
        return PANO_GPU_UNAVAILABLE;
    }
    void *mapped = nullptr;
    if (FAILED(gradient_resource->Map(0, nullptr, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 exposure gradients");
        return PANO_GPU_UNAVAILABLE;
    }
    std::memcpy(mapped, gradients, static_cast<size_t>(gradient_bytes));
    gradient_resource->Unmap(0, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 2;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> work_descriptors, bounds_descriptors;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&work_descriptors))) ||
        FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&bounds_descriptors))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), prepare_pipeline.Get(),
            IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-gradient dispatch");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto create_srv = [&](ID3D12Resource *const resource, const UINT elements,
                                const UINT stride, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view {};
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.Buffer.NumElements = elements;
        view.Buffer.StructureByteStride = stride;
        device->CreateShaderResourceView(resource, &view, descriptor);
    };
    const auto create_uav = [&](ID3D12Resource *const resource, const UINT elements,
                                const UINT stride, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view {};
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.Buffer.NumElements = elements;
        view.Buffer.StructureByteStride = stride;
        device->CreateUnorderedAccessView(resource, nullptr, &view, descriptor);
    };
    auto descriptor = work_descriptors->GetCPUDescriptorHandleForHeapStart();
    create_srv(gradient_resource.Get(), static_cast<UINT>(sample_count), 2 * sizeof(float), descriptor);
    descriptor.ptr += increment;
    create_uav(sortable_resource.Get(), static_cast<UINT>(sortable_count), sizeof(float), descriptor);
    descriptor = bounds_descriptors->GetCPUDescriptorHandleForHeapStart();
    create_srv(sortable_resource.Get(), static_cast<UINT>(sortable_count), sizeof(float), descriptor);
    descriptor.ptr += increment;
    create_uav(limit_resource.Get(), 1, 2 * sizeof(float), descriptor);
    ID3D12DescriptorHeap *work_heaps[] {work_descriptors.Get()};
    list->SetDescriptorHeaps(1, work_heaps);
    list->SetComputeRootSignature(root_signature.Get());
    const uint32_t prepare_constants[] {
        static_cast<uint32_t>(sample_count), static_cast<uint32_t>(capacity), 0};
    list->SetComputeRoot32BitConstants(0, 3, prepare_constants, 0);
    list->SetComputeRootDescriptorTable(1, work_descriptors->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((static_cast<UINT>(sortable_count) + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER uav_barrier {};
    uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_barrier.UAV.pResource = sortable_resource.Get();
    list->ResourceBarrier(1, &uav_barrier);
    list->SetPipelineState(sort_pipeline.Get());
    for (uint32_t sequence = 2; sequence <= static_cast<uint32_t>(capacity); sequence *= 2)
    {
        for (uint32_t stride = sequence / 2; stride > 0; stride /= 2)
        {
            const uint32_t sort_constants[] {
                static_cast<uint32_t>(capacity), sequence, stride};
            list->SetComputeRoot32BitConstants(0, 3, sort_constants, 0);
            list->Dispatch((static_cast<UINT>(sortable_count) + 63) / 64, 1, 1);
            list->ResourceBarrier(1, &uav_barrier);
        }
        if (sequence == static_cast<uint32_t>(capacity))
            break;
    }
    D3D12_RESOURCE_BARRIER sortable_transition {};
    sortable_transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    sortable_transition.Transition.pResource = sortable_resource.Get();
    sortable_transition.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    sortable_transition.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    sortable_transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &sortable_transition);
    ID3D12DescriptorHeap *bounds_heaps[] {bounds_descriptors.Get()};
    list->SetDescriptorHeaps(1, bounds_heaps);
    list->SetPipelineState(bounds_pipeline.Get());
    const uint32_t bounds_constants[] {
        static_cast<uint32_t>(sample_count), static_cast<uint32_t>(capacity), 0};
    list->SetComputeRoot32BitConstants(0, 3, bounds_constants, 0);
    list->SetComputeRootDescriptorTable(1, bounds_descriptors->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER limit_transition {};
    limit_transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    limit_transition.Transition.pResource = limit_resource.Get();
    limit_transition.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    limit_transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    limit_transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &limit_transition);
    list->CopyResource(readback.Get(), limit_resource.Get());
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 exposure-gradient command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(
            session->device_core.get(), fence_value, error_buffer, error_buffer_size,
            "D3D12 exposure-gradient fence timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    const D3D12_RANGE range {0, 2 * sizeof(float)};
    if (FAILED(readback->Map(0, &range, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 exposure-gradient limits");
        return PANO_GPU_UNAVAILABLE;
    }
    std::memcpy(gradient_limits, mapped, 2 * sizeof(float));
    readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_session_dispatch_exposure_pair_gradient_limits(
    pano_gpu_session *const session, const uint32_t sample_width, const uint32_t sample_height,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    uint64_t sample_count = 0;
    if (session == nullptr || !session->resident_pair_classified ||
        session->resident_pair_gradient_limits_ready || sample_width == 0 || sample_height == 0 ||
        !checked_multiply(sample_width, sample_height, &sample_count) ||
        sample_count != session->resident_pair_sample_count)
    {
        write_error(error_buffer, error_buffer_size, "invalid resident D3D12 exposure-gradient request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const uint32_t capacity = static_cast<uint32_t>(session->resident_pair_sortable_capacity);
    const uint32_t sortable_count = 2 * capacity;
    const auto create_root = [device](
                                 const uint32_t constants, D3D12_DESCRIPTOR_RANGE *const ranges,
                                 Microsoft::WRL::ComPtr<ID3D12RootSignature> *const root) {
        D3D12_ROOT_PARAMETER parameters[2] {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = {0, 0, constants};
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = {2, ranges};
        const D3D12_ROOT_SIGNATURE_DESC description {
            2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
        return SUCCEEDED(D3D12SerializeRootSignature(
                   &description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) &&
            SUCCEEDED(device->CreateRootSignature(
                0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                IID_PPV_ARGS(root->ReleaseAndGetAddressOf())));
    };
    D3D12_DESCRIPTOR_RANGE gradient_ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_DESCRIPTOR_RANGE sort_ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> gradient_root, sort_root;
    if (!create_root(2, gradient_ranges, &gradient_root) || !create_root(3, sort_ranges, &sort_root))
    {
        write_error(error_buffer, error_buffer_size, "cannot create resident exposure-gradient roots");
        return PANO_GPU_UNAVAILABLE;
    }
    const auto create_pipeline = [device](
                                      ID3D12RootSignature *const root, const BYTE *const shader,
                                      const size_t shader_bytes,
                                      Microsoft::WRL::ComPtr<ID3D12PipelineState> *const pipeline) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC description {};
        description.pRootSignature = root;
        description.CS = {shader, shader_bytes};
        return SUCCEEDED(device->CreateComputePipelineState(
            &description, IID_PPV_ARGS(pipeline->ReleaseAndGetAddressOf())));
    };
    Microsoft::WRL::ComPtr<ID3D12PipelineState> gradient_pipeline, prepare_pipeline, sort_pipeline,
        bounds_pipeline;
    if (!create_pipeline(
            gradient_root.Get(), pano_gpu_exposure_pair_gradient_shader,
            sizeof(pano_gpu_exposure_pair_gradient_shader), &gradient_pipeline) ||
        !create_pipeline(
            sort_root.Get(), pano_gpu_exposure_gradient_sort_prepare_shader,
            sizeof(pano_gpu_exposure_gradient_sort_prepare_shader), &prepare_pipeline) ||
        !create_pipeline(
            sort_root.Get(), pano_gpu_exposure_gradient_sort_shader,
            sizeof(pano_gpu_exposure_gradient_sort_shader), &sort_pipeline) ||
        !create_pipeline(
            sort_root.Get(), pano_gpu_exposure_gradient_bounds_shader,
            sizeof(pano_gpu_exposure_gradient_bounds_shader), &bounds_pipeline))
    {
        write_error(error_buffer, error_buffer_size, "cannot create resident exposure-gradient pipelines");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 2;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> gradient_heap, work_heap, bounds_heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&gradient_heap))) ||
        FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&work_heap))) ||
        FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&bounds_heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), gradient_pipeline.Get(),
            IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create resident exposure-gradient dispatch");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto create_srv = [device](
                                ID3D12Resource *const resource, const UINT elements, const UINT stride,
                                const D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view {};
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.Buffer.NumElements = elements;
        view.Buffer.StructureByteStride = stride;
        device->CreateShaderResourceView(resource, &view, descriptor);
    };
    const auto create_uav = [device](
                                ID3D12Resource *const resource, const UINT elements, const UINT stride,
                                const D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view {};
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.Buffer.NumElements = elements;
        view.Buffer.StructureByteStride = stride;
        device->CreateUnorderedAccessView(resource, nullptr, &view, descriptor);
    };
    auto descriptor = gradient_heap->GetCPUDescriptorHandleForHeapStart();
    create_srv(session->resident_pair_scratch[4].Get(), static_cast<UINT>(sample_count), 2 * sizeof(float), descriptor);
    descriptor.ptr += increment;
    create_uav(session->resident_pair_scratch[6].Get(), static_cast<UINT>(sample_count), 2 * sizeof(float), descriptor);
    descriptor = work_heap->GetCPUDescriptorHandleForHeapStart();
    create_srv(session->resident_pair_scratch[6].Get(), static_cast<UINT>(sample_count), 2 * sizeof(float), descriptor);
    descriptor.ptr += increment;
    create_uav(session->resident_pair_scratch[7].Get(), sortable_count, sizeof(float), descriptor);
    descriptor = bounds_heap->GetCPUDescriptorHandleForHeapStart();
    create_srv(session->resident_pair_scratch[7].Get(), sortable_count, sizeof(float), descriptor);
    descriptor.ptr += increment;
    create_uav(session->resident_pair_scratch[8].Get(), 1, 2 * sizeof(float), descriptor);
    D3D12_RESOURCE_BARRIER luminance_transition {};
    luminance_transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    luminance_transition.Transition = {
        session->resident_pair_scratch[4].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &luminance_transition);
    ID3D12DescriptorHeap *gradient_heaps[] {gradient_heap.Get()};
    list->SetDescriptorHeaps(1, gradient_heaps);
    list->SetComputeRootSignature(gradient_root.Get());
    const uint32_t gradient_constants[2] {sample_width, sample_height};
    list->SetComputeRoot32BitConstants(0, 2, gradient_constants, 0);
    list->SetComputeRootDescriptorTable(1, gradient_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(static_cast<UINT>((sample_count + 63) / 64), 1, 1);
    D3D12_RESOURCE_BARRIER gradient_transition {};
    gradient_transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    gradient_transition.Transition = {
        session->resident_pair_scratch[6].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &gradient_transition);
    ID3D12DescriptorHeap *work_heaps[] {work_heap.Get()};
    list->SetPipelineState(prepare_pipeline.Get());
    list->SetDescriptorHeaps(1, work_heaps);
    list->SetComputeRootSignature(sort_root.Get());
    const uint32_t prepare_constants[3] {static_cast<uint32_t>(sample_count), capacity, 0};
    list->SetComputeRoot32BitConstants(0, 3, prepare_constants, 0);
    list->SetComputeRootDescriptorTable(1, work_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((sortable_count + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER uav_barrier {};
    uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_barrier.UAV.pResource = session->resident_pair_scratch[7].Get();
    list->ResourceBarrier(1, &uav_barrier);
    list->SetPipelineState(sort_pipeline.Get());
    for (uint32_t sequence = 2; sequence <= capacity; sequence *= 2)
    {
        for (uint32_t stride = sequence / 2; stride > 0; stride /= 2)
        {
            const uint32_t constants[3] {capacity, sequence, stride};
            list->SetComputeRoot32BitConstants(0, 3, constants, 0);
            list->Dispatch((sortable_count + 63) / 64, 1, 1);
            list->ResourceBarrier(1, &uav_barrier);
        }
        if (sequence == capacity)
            break;
    }
    D3D12_RESOURCE_BARRIER sort_transition {};
    sort_transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    sort_transition.Transition = {
        session->resident_pair_scratch[7].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &sort_transition);
    ID3D12DescriptorHeap *bounds_heaps[] {bounds_heap.Get()};
    list->SetPipelineState(bounds_pipeline.Get());
    list->SetDescriptorHeaps(1, bounds_heaps);
    const uint32_t bounds_constants[3] {static_cast<uint32_t>(sample_count), capacity, 0};
    list->SetComputeRoot32BitConstants(0, 3, bounds_constants, 0);
    list->SetComputeRootDescriptorTable(1, bounds_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(1, 1, 1);
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close resident exposure-gradient dispatch");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(
            session->device_core.get(), fence_value, error_buffer, error_buffer_size,
            "resident exposure-gradient dispatch timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    session->resident_pair_gradient_limits_ready = true;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_read_resident_exposure_pair_gradient_limits(
    const pano_gpu_session *const session, void *const gradients, const uint64_t gradient_bytes,
    float *const gradient_limits, const uint64_t gradient_limit_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    const uint64_t expected_gradients = session == nullptr
        ? 0
        : session->resident_pair_sample_count * 2 * sizeof(float);
    if (session == nullptr || !session->resident_pair_gradient_limits_ready || gradients == nullptr ||
        gradient_limits == nullptr || gradient_bytes != expected_gradients ||
        gradient_limit_bytes != 2 * sizeof(float))
    {
        write_error(error_buffer, error_buffer_size, "invalid resident exposure-gradient readback");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const std::array<uint64_t, 2> sizes {expected_gradients, 2 * sizeof(float)};
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> readbacks;
    for (size_t index = 0; index < 2; ++index)
    {
        D3D12_RESOURCE_DESC description {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = sizes[index];
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&readbacks[index]))))
        {
            write_error(error_buffer, error_buffer_size, "cannot create resident exposure-gradient readback");
            return PANO_GPU_UNAVAILABLE;
        }
    }
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    std::array<D3D12_RESOURCE_BARRIER, 2> barriers {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition = {
        session->resident_pair_scratch[6].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition = {
        session->resident_pair_scratch[8].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(2, barriers.data());
    list->CopyResource(readbacks[0].Get(), session->resident_pair_scratch[6].Get());
    list->CopyResource(readbacks[1].Get(), session->resident_pair_scratch[8].Get());
    for (auto &barrier : barriers)
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(2, barriers.data());
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(
            session->device_core.get(), fence_value, error_buffer, error_buffer_size,
            "resident exposure-gradient readback timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    std::array<void *, 2> mapped {};
    for (size_t index = 0; index < 2; ++index)
    {
        const D3D12_RANGE range {0, static_cast<SIZE_T>(sizes[index])};
        if (FAILED(readbacks[index]->Map(0, &range, &mapped[index])))
        {
            if (index != 0)
                readbacks[0]->Unmap(0, nullptr);
            return PANO_GPU_UNAVAILABLE;
        }
    }
    std::memcpy(gradients, mapped[0], static_cast<size_t>(expected_gradients));
    std::memcpy(gradient_limits, mapped[1], 2 * sizeof(float));
    readbacks[1]->Unmap(0, nullptr);
    readbacks[0]->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_session_dispatch_exposure_pair_filter_ratios(
    pano_gpu_session *const session, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || !session->resident_pair_gradient_limits_ready ||
        session->resident_pair_ratios_ready)
    {
        write_error(error_buffer, error_buffer_size, "resident exposure-pair filter inputs are not ready");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const auto create_root = [device](
                                 const UINT srv_count, Microsoft::WRL::ComPtr<ID3D12RootSignature> *const root) {
        D3D12_DESCRIPTOR_RANGE ranges[2] {
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, srv_count, 0, 0, 0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, srv_count}};
        D3D12_ROOT_PARAMETER parameters[2] {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = {0, 0, 1};
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = {2, ranges};
        const D3D12_ROOT_SIGNATURE_DESC description {
            2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
        return SUCCEEDED(D3D12SerializeRootSignature(
                   &description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) &&
            SUCCEEDED(device->CreateRootSignature(
                0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                IID_PPV_ARGS(root->ReleaseAndGetAddressOf())));
    };
    Microsoft::WRL::ComPtr<ID3D12RootSignature> filter_root, ratio_root;
    if (!create_root(3, &filter_root) || !create_root(2, &ratio_root))
        return PANO_GPU_UNAVAILABLE;
    const auto create_pipeline = [device](
                                      ID3D12RootSignature *const root, const BYTE *const shader,
                                      const size_t shader_bytes,
                                      Microsoft::WRL::ComPtr<ID3D12PipelineState> *const pipeline) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC description {};
        description.pRootSignature = root;
        description.CS = {shader, shader_bytes};
        return SUCCEEDED(device->CreateComputePipelineState(
            &description, IID_PPV_ARGS(pipeline->ReleaseAndGetAddressOf())));
    };
    Microsoft::WRL::ComPtr<ID3D12PipelineState> filter_pipeline, ratio_pipeline;
    if (!create_pipeline(
            filter_root.Get(), pano_gpu_exposure_pair_filter_resident_shader,
            sizeof(pano_gpu_exposure_pair_filter_resident_shader), &filter_pipeline) ||
        !create_pipeline(
            ratio_root.Get(), pano_gpu_exposure_pair_ratio_shader,
            sizeof(pano_gpu_exposure_pair_ratio_shader), &ratio_pipeline))
        return PANO_GPU_UNAVAILABLE;
    D3D12_DESCRIPTOR_HEAP_DESC filter_heap_description {};
    filter_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    filter_heap_description.NumDescriptors = 4;
    filter_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D12_DESCRIPTOR_HEAP_DESC ratio_heap_description = filter_heap_description;
    ratio_heap_description.NumDescriptors = 3;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> filter_heap, ratio_heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&filter_heap_description, IID_PPV_ARGS(&filter_heap))) ||
        FAILED(device->CreateDescriptorHeap(&ratio_heap_description, IID_PPV_ARGS(&ratio_heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), filter_pipeline.Get(),
            IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    const UINT count = static_cast<UINT>(session->resident_pair_sample_count);
    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto structured_srv = [device](
                                    ID3D12Resource *const resource, const UINT elements,
                                    const UINT stride, const D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view {};
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.Buffer.NumElements = elements;
        view.Buffer.StructureByteStride = stride;
        device->CreateShaderResourceView(resource, &view, descriptor);
    };
    const auto uint_srv = [device](
                              ID3D12Resource *const resource, const UINT elements,
                              const D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view {};
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = DXGI_FORMAT_R32_UINT;
        view.Buffer.NumElements = elements;
        device->CreateShaderResourceView(resource, &view, descriptor);
    };
    auto descriptor = filter_heap->GetCPUDescriptorHandleForHeapStart();
    structured_srv(session->resident_pair_scratch[6].Get(), count, 2 * sizeof(float), descriptor);
    descriptor.ptr += increment;
    uint_srv(session->resident_pair_scratch[5].Get(), count, descriptor);
    descriptor.ptr += increment;
    structured_srv(session->resident_pair_scratch[8].Get(), 1, 2 * sizeof(float), descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC accepted_uav {};
    accepted_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    accepted_uav.Format = DXGI_FORMAT_R32_UINT;
    accepted_uav.Buffer.NumElements = count;
    device->CreateUnorderedAccessView(
        session->resident_pair_scratch[1].Get(), nullptr, &accepted_uav, descriptor);
    descriptor = ratio_heap->GetCPUDescriptorHandleForHeapStart();
    structured_srv(session->resident_pair_scratch[4].Get(), count, 2 * sizeof(float), descriptor);
    descriptor.ptr += increment;
    uint_srv(session->resident_pair_scratch[1].Get(), count, descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ratio_uav {};
    ratio_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    ratio_uav.Format = DXGI_FORMAT_R32_FLOAT;
    ratio_uav.Buffer.NumElements = count;
    device->CreateUnorderedAccessView(
        session->resident_pair_scratch[9].Get(), nullptr, &ratio_uav, descriptor);
    std::array<D3D12_RESOURCE_BARRIER, 3> input_barriers {};
    input_barriers[0].Transition.pResource = session->resident_pair_scratch[5].Get();
    input_barriers[1].Transition.pResource = session->resident_pair_scratch[8].Get();
    input_barriers[2].Transition.pResource = session->resident_pair_scratch[1].Get();
    for (size_t index = 0; index < input_barriers.size(); ++index)
    {
        input_barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        input_barriers[index].Transition.StateBefore = index == 2
            ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        input_barriers[index].Transition.StateAfter = index == 2
            ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
            : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        input_barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    list->ResourceBarrier(3, input_barriers.data());
    const uint32_t constants[1] {count};
    ID3D12DescriptorHeap *filter_heaps[] {filter_heap.Get()};
    list->SetDescriptorHeaps(1, filter_heaps);
    list->SetComputeRootSignature(filter_root.Get());
    list->SetComputeRoot32BitConstants(0, 1, constants, 0);
    list->SetComputeRootDescriptorTable(1, filter_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((count + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER accepted_transition {};
    accepted_transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    accepted_transition.Transition = {
        session->resident_pair_scratch[1].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &accepted_transition);
    ID3D12DescriptorHeap *ratio_heaps[] {ratio_heap.Get()};
    list->SetPipelineState(ratio_pipeline.Get());
    list->SetDescriptorHeaps(1, ratio_heaps);
    list->SetComputeRootSignature(ratio_root.Get());
    list->SetComputeRoot32BitConstants(0, 1, constants, 0);
    list->SetComputeRootDescriptorTable(1, ratio_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((count + 63) / 64, 1, 1);
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(
            session->device_core.get(), fence_value, error_buffer, error_buffer_size,
            "resident exposure-pair filter/ratio dispatch timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    session->resident_pair_ratios_ready = true;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_read_resident_exposure_pair_filter_ratios(
    const pano_gpu_session *const session, void *const accepted, const uint64_t accepted_bytes,
    void *const log_ratios, const uint64_t log_ratio_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    const uint64_t count = session == nullptr ? 0 : session->resident_pair_sample_count;
    if (session == nullptr || !session->resident_pair_ratios_ready || accepted == nullptr ||
        log_ratios == nullptr || accepted_bytes != count || log_ratio_bytes != count * sizeof(float))
    {
        write_error(error_buffer, error_buffer_size, "invalid resident exposure-pair ratio readback");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const uint64_t word_bytes = count * sizeof(uint32_t);
    const std::array<uint64_t, 2> sizes {word_bytes, log_ratio_bytes};
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> readbacks;
    for (size_t index = 0; index < 2; ++index)
    {
        D3D12_RESOURCE_DESC description {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = sizes[index];
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&readbacks[index]))))
            return PANO_GPU_UNAVAILABLE;
    }
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    std::array<D3D12_RESOURCE_BARRIER, 2> barriers {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition = {
        session->resident_pair_scratch[1].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition = {
        session->resident_pair_scratch[9].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(2, barriers.data());
    list->CopyResource(readbacks[0].Get(), session->resident_pair_scratch[1].Get());
    list->CopyResource(readbacks[1].Get(), session->resident_pair_scratch[9].Get());
    for (auto &barrier : barriers)
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(2, barriers.data());
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(
            session->device_core.get(), fence_value, error_buffer, error_buffer_size,
            "resident exposure-pair ratio readback timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    std::array<void *, 2> mapped {};
    for (size_t index = 0; index < 2; ++index)
    {
        const D3D12_RANGE range {0, static_cast<SIZE_T>(sizes[index])};
        if (FAILED(readbacks[index]->Map(0, &range, &mapped[index])))
        {
            if (index != 0)
                readbacks[0]->Unmap(0, nullptr);
            return PANO_GPU_UNAVAILABLE;
        }
    }
    const auto *const accepted_words = static_cast<const uint32_t *>(mapped[0]);
    auto *const accepted_output = static_cast<uint8_t *>(accepted);
    for (uint64_t index = 0; index < count; ++index)
        accepted_output[index] = static_cast<uint8_t>(accepted_words[index] != 0);
    std::memcpy(log_ratios, mapped[1], static_cast<size_t>(log_ratio_bytes));
    readbacks[1]->Unmap(0, nullptr);
    readbacks[0]->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_session_dispatch_exposure_pair_trim(
    pano_gpu_session *const session, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || !session->resident_pair_ratios_ready || session->resident_pair_trimmed)
    {
        write_error(error_buffer, error_buffer_size, "resident exposure-pair trim inputs are not ready");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const UINT count = static_cast<UINT>(session->resident_pair_sample_count);
    const UINT capacity = static_cast<UINT>(session->resident_pair_sortable_capacity);
    const auto create_root = [device](
                                 const UINT constants, const UINT srv_count, const UINT uav_count,
                                 Microsoft::WRL::ComPtr<ID3D12RootSignature> *const root) {
        D3D12_DESCRIPTOR_RANGE ranges[2] {
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, srv_count, 0, 0, 0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, uav_count, 0, 0, srv_count}};
        D3D12_ROOT_PARAMETER parameters[2] {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = {0, 0, constants};
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = {srv_count == 0 ? 1U : 2U, srv_count == 0 ? ranges + 1 : ranges};
        const D3D12_ROOT_SIGNATURE_DESC description {
            2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
        return SUCCEEDED(D3D12SerializeRootSignature(
                   &description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) &&
            SUCCEEDED(device->CreateRootSignature(
                0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                IID_PPV_ARGS(root->ReleaseAndGetAddressOf())));
    };
    Microsoft::WRL::ComPtr<ID3D12RootSignature> prepare_root, sort_root, bounds_root, trim_root;
    if (!create_root(2, 2, 1, &prepare_root) || !create_root(3, 0, 1, &sort_root) ||
        !create_root(1, 2, 1, &bounds_root) || !create_root(1, 3, 1, &trim_root))
    {
        write_error(error_buffer, error_buffer_size, "cannot create resident exposure-pair trim roots");
        return PANO_GPU_UNAVAILABLE;
    }
    const auto create_pipeline = [device](
                                      ID3D12RootSignature *const root, const BYTE *const shader,
                                      const size_t shader_bytes,
                                      Microsoft::WRL::ComPtr<ID3D12PipelineState> *const pipeline) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC description {};
        description.pRootSignature = root;
        description.CS = {shader, shader_bytes};
        return SUCCEEDED(device->CreateComputePipelineState(
            &description, IID_PPV_ARGS(pipeline->ReleaseAndGetAddressOf())));
    };
    Microsoft::WRL::ComPtr<ID3D12PipelineState> prepare_pipeline, sort_pipeline, bounds_pipeline,
        trim_pipeline;
    if (!create_pipeline(
            prepare_root.Get(), pano_gpu_exposure_pair_sort_prepare_shader,
            sizeof(pano_gpu_exposure_pair_sort_prepare_shader), &prepare_pipeline) ||
        !create_pipeline(
            sort_root.Get(), pano_gpu_exposure_pair_sort_shader,
            sizeof(pano_gpu_exposure_pair_sort_shader), &sort_pipeline) ||
        !create_pipeline(
            bounds_root.Get(), pano_gpu_exposure_pair_bounds_shader,
            sizeof(pano_gpu_exposure_pair_bounds_shader), &bounds_pipeline) ||
        !create_pipeline(
            trim_root.Get(), pano_gpu_exposure_pair_trim_shader,
            sizeof(pano_gpu_exposure_pair_trim_shader), &trim_pipeline))
    {
        write_error(error_buffer, error_buffer_size, "cannot create resident exposure-pair trim pipelines");
        return PANO_GPU_UNAVAILABLE;
    }
    const std::array<UINT, 4> descriptor_counts {3, 1, 3, 4};
    std::array<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>, 4> heaps;
    for (size_t index = 0; index < heaps.size(); ++index)
    {
        D3D12_DESCRIPTOR_HEAP_DESC description {};
        description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        description.NumDescriptors = descriptor_counts[index];
        description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&heaps[index]))))
        {
            write_error(error_buffer, error_buffer_size, "cannot create resident exposure-pair trim descriptors");
            return PANO_GPU_UNAVAILABLE;
        }
    }
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), prepare_pipeline.Get(),
            IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create resident exposure-pair trim commands");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto float_srv = [device](ID3D12Resource *resource, UINT elements, UINT stride,
                                    D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view {};
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.Buffer.NumElements = elements;
        view.Buffer.StructureByteStride = stride;
        device->CreateShaderResourceView(resource, &view, descriptor);
    };
    const auto uint_srv = [device](ID3D12Resource *resource, UINT elements,
                                   D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view {};
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = DXGI_FORMAT_R32_UINT;
        view.Buffer.NumElements = elements;
        device->CreateShaderResourceView(resource, &view, descriptor);
    };
    const auto float_uav = [device](ID3D12Resource *resource, UINT elements, UINT stride,
                                    D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view {};
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.Buffer.NumElements = elements;
        view.Buffer.StructureByteStride = stride;
        device->CreateUnorderedAccessView(resource, nullptr, &view, descriptor);
    };
    auto descriptor = heaps[0]->GetCPUDescriptorHandleForHeapStart();
    float_srv(session->resident_pair_scratch[9].Get(), count, sizeof(float), descriptor);
    descriptor.ptr += increment;
    uint_srv(session->resident_pair_scratch[1].Get(), count, descriptor);
    descriptor.ptr += increment;
    float_uav(session->resident_pair_scratch[10].Get(), capacity, sizeof(float), descriptor);
    D3D12_UNORDERED_ACCESS_VIEW_DESC sort_uav {};
    sort_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    sort_uav.Format = DXGI_FORMAT_R32_FLOAT;
    sort_uav.Buffer.NumElements = capacity;
    device->CreateUnorderedAccessView(
        session->resident_pair_scratch[10].Get(), nullptr, &sort_uav,
        heaps[1]->GetCPUDescriptorHandleForHeapStart());
    descriptor = heaps[2]->GetCPUDescriptorHandleForHeapStart();
    float_srv(session->resident_pair_scratch[10].Get(), capacity, sizeof(float), descriptor);
    descriptor.ptr += increment;
    uint_srv(session->resident_pair_scratch[1].Get(), count, descriptor);
    descriptor.ptr += increment;
    float_uav(session->resident_pair_scratch[11].Get(), 1, 2 * sizeof(float), descriptor);
    descriptor = heaps[3]->GetCPUDescriptorHandleForHeapStart();
    float_srv(session->resident_pair_scratch[9].Get(), count, sizeof(float), descriptor);
    descriptor.ptr += increment;
    uint_srv(session->resident_pair_scratch[1].Get(), count, descriptor);
    descriptor.ptr += increment;
    float_srv(session->resident_pair_scratch[11].Get(), 1, 2 * sizeof(float), descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC trim_uav {};
    trim_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    trim_uav.Format = DXGI_FORMAT_R32_UINT;
    trim_uav.Buffer.NumElements = count;
    device->CreateUnorderedAccessView(
        session->resident_pair_scratch[5].Get(), nullptr, &trim_uav, descriptor);
    D3D12_RESOURCE_BARRIER ratio_transition {};
    ratio_transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    ratio_transition.Transition = {
        session->resident_pair_scratch[9].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &ratio_transition);
    ID3D12DescriptorHeap *bound_heaps[] {heaps[0].Get()};
    list->SetDescriptorHeaps(1, bound_heaps);
    list->SetComputeRootSignature(prepare_root.Get());
    const uint32_t prepare_constants[2] {count, capacity};
    list->SetComputeRoot32BitConstants(0, 2, prepare_constants, 0);
    list->SetComputeRootDescriptorTable(1, heaps[0]->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((capacity + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER uav_barrier {};
    uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_barrier.UAV.pResource = session->resident_pair_scratch[10].Get();
    list->ResourceBarrier(1, &uav_barrier);
    bound_heaps[0] = heaps[1].Get();
    list->SetPipelineState(sort_pipeline.Get());
    list->SetDescriptorHeaps(1, bound_heaps);
    list->SetComputeRootSignature(sort_root.Get());
    list->SetComputeRootDescriptorTable(1, heaps[1]->GetGPUDescriptorHandleForHeapStart());
    for (uint32_t sequence = 2; sequence <= capacity; sequence *= 2)
    {
        for (uint32_t stride = sequence / 2; stride > 0; stride /= 2)
        {
            const uint32_t constants[3] {capacity, sequence, stride};
            list->SetComputeRoot32BitConstants(0, 3, constants, 0);
            list->Dispatch((capacity + 63) / 64, 1, 1);
            list->ResourceBarrier(1, &uav_barrier);
        }
        if (sequence == capacity)
            break;
    }
    D3D12_RESOURCE_BARRIER sort_transition {};
    sort_transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    sort_transition.Transition = {
        session->resident_pair_scratch[10].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &sort_transition);
    bound_heaps[0] = heaps[2].Get();
    list->SetPipelineState(bounds_pipeline.Get());
    list->SetDescriptorHeaps(1, bound_heaps);
    list->SetComputeRootSignature(bounds_root.Get());
    list->SetComputeRoot32BitConstant(0, count, 0);
    list->SetComputeRootDescriptorTable(1, heaps[2]->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(1, 1, 1);
    std::array<D3D12_RESOURCE_BARRIER, 2> trim_transitions {};
    trim_transitions[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    trim_transitions[0].Transition = {
        session->resident_pair_scratch[11].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    trim_transitions[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    trim_transitions[1].Transition = {
        session->resident_pair_scratch[5].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    list->ResourceBarrier(2, trim_transitions.data());
    bound_heaps[0] = heaps[3].Get();
    list->SetPipelineState(trim_pipeline.Get());
    list->SetDescriptorHeaps(1, bound_heaps);
    list->SetComputeRootSignature(trim_root.Get());
    list->SetComputeRoot32BitConstant(0, count, 0);
    list->SetComputeRootDescriptorTable(1, heaps[3]->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((count + 63) / 64, 1, 1);
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close resident exposure-pair trim commands");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(
            session->device_core.get(), fence_value, error_buffer, error_buffer_size,
            "resident exposure-pair trim dispatch timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    session->resident_pair_trimmed = true;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_read_resident_exposure_pair_trim(
    const pano_gpu_session *const session, void *const sorted_ratios,
    const uint64_t sorted_ratio_bytes, float *const trim_bounds, const uint64_t trim_bound_bytes,
    void *const trimmed, const uint64_t trimmed_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    const uint64_t count = session == nullptr ? 0 : session->resident_pair_sample_count;
    const uint64_t capacity = session == nullptr ? 0 : session->resident_pair_sortable_capacity;
    if (session == nullptr || !session->resident_pair_trimmed || sorted_ratios == nullptr ||
        trim_bounds == nullptr || trimmed == nullptr || sorted_ratio_bytes != capacity * sizeof(float) ||
        trim_bound_bytes != 2 * sizeof(float) || trimmed_bytes != count)
    {
        write_error(error_buffer, error_buffer_size, "invalid resident exposure-pair trim readback");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    const std::array<uint64_t, 3> sizes {
        sorted_ratio_bytes, trim_bound_bytes, count * sizeof(uint32_t)};
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 3> readbacks;
    for (size_t index = 0; index < readbacks.size(); ++index)
    {
        D3D12_RESOURCE_DESC description {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = sizes[index];
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&readbacks[index]))))
            return PANO_GPU_UNAVAILABLE;
    }
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    const std::array<size_t, 3> resources {10, 11, 5};
    std::array<D3D12_RESOURCE_BARRIER, 3> barriers {};
    for (size_t index = 0; index < barriers.size(); ++index)
    {
        barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[index].Transition = {
            session->resident_pair_scratch[resources[index]].Get(),
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            index == 2 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                       : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_SOURCE};
    }
    list->ResourceBarrier(3, barriers.data());
    for (size_t index = 0; index < readbacks.size(); ++index)
        list->CopyResource(readbacks[index].Get(), session->resident_pair_scratch[resources[index]].Get());
    for (auto &barrier : barriers)
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(3, barriers.data());
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(
            session->device_core.get(), fence_value, error_buffer, error_buffer_size,
            "resident exposure-pair trim readback timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    std::array<void *, 3> mapped {};
    for (size_t index = 0; index < mapped.size(); ++index)
    {
        const D3D12_RANGE range {0, static_cast<SIZE_T>(sizes[index])};
        if (FAILED(readbacks[index]->Map(0, &range, &mapped[index])))
        {
            for (size_t previous = 0; previous < index; ++previous)
                readbacks[previous]->Unmap(0, nullptr);
            return PANO_GPU_UNAVAILABLE;
        }
    }
    std::memcpy(sorted_ratios, mapped[0], static_cast<size_t>(sorted_ratio_bytes));
    std::memcpy(trim_bounds, mapped[1], static_cast<size_t>(trim_bound_bytes));
    const auto *const words = static_cast<const uint32_t *>(mapped[2]);
    auto *const trimmed_output = static_cast<uint8_t *>(trimmed);
    for (uint64_t index = 0; index < count; ++index)
        trimmed_output[index] = static_cast<uint8_t>(words[index] != 0);
    for (auto &readback : readbacks)
        readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_session_reduce_exposure_pair(
    pano_gpu_session *const session, pano_gpu_exposure_pair_reduction *const reduction,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (reduction == nullptr || reduction->size != sizeof(*reduction) ||
        reduction->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid resident exposure-pair reduction result");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    reduction->rejection_reason = PANO_GPU_EXPOSURE_PAIR_NONFINITE;
    reduction->valid_count = 0;
    reduction->inlier_count = 0;
    reduction->difference = std::numeric_limits<float>::quiet_NaN();
    reduction->mad = std::numeric_limits<float>::quiet_NaN();
    reduction->weight = 0.0F;
    reduction->downloaded_bytes = 0;
    if (session == nullptr || !session->resident_pair_trimmed || session->resident_pair_reduced)
    {
        write_error(error_buffer, error_buffer_size, "resident exposure-pair reduction inputs are not ready");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const UINT count = static_cast<UINT>(session->resident_pair_sample_count);
    const UINT capacity = static_cast<UINT>(session->resident_pair_sortable_capacity);
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 3};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    const D3D12_ROOT_SIGNATURE_DESC root_description {
        2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root))))
        return PANO_GPU_UNAVAILABLE;
    const auto create_pipeline = [device, &root](
                                      const BYTE *const shader, const size_t shader_bytes,
                                      Microsoft::WRL::ComPtr<ID3D12PipelineState> *const pipeline) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC description {};
        description.pRootSignature = root.Get();
        description.CS = {shader, shader_bytes};
        return SUCCEEDED(device->CreateComputePipelineState(
            &description, IID_PPV_ARGS(pipeline->ReleaseAndGetAddressOf())));
    };
    Microsoft::WRL::ComPtr<ID3D12PipelineState> summary_pipeline, deviation_pipeline, sort_pipeline,
        result_pipeline;
    if (!create_pipeline(
            pano_gpu_exposure_pair_reduce_summary_shader,
            sizeof(pano_gpu_exposure_pair_reduce_summary_shader), &summary_pipeline) ||
        !create_pipeline(
            pano_gpu_exposure_pair_reduce_deviations_shader,
            sizeof(pano_gpu_exposure_pair_reduce_deviations_shader), &deviation_pipeline) ||
        !create_pipeline(
            pano_gpu_exposure_pair_sort_shader, sizeof(pano_gpu_exposure_pair_sort_shader),
            &sort_pipeline) ||
        !create_pipeline(
            pano_gpu_exposure_pair_reduce_result_shader,
            sizeof(pano_gpu_exposure_pair_reduce_result_shader), &result_pipeline))
        return PANO_GPU_UNAVAILABLE;
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 4;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    std::array<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>, 4> heaps;
    for (auto &heap : heaps)
        if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&heap))))
            return PANO_GPU_UNAVAILABLE;
    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto srv = [device](ID3D12Resource *resource, UINT elements, UINT stride,
                              D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view {};
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.Buffer.NumElements = elements;
        view.Buffer.StructureByteStride = stride;
        device->CreateShaderResourceView(resource, &view, descriptor);
    };
    const auto uav = [device](ID3D12Resource *resource, UINT elements, UINT stride,
                              D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view {};
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.Buffer.NumElements = elements;
        view.Buffer.StructureByteStride = stride;
        device->CreateUnorderedAccessView(resource, nullptr, &view, descriptor);
    };
    const auto fill_heap = [&](const size_t heap_index, ID3D12Resource *first, UINT first_count,
                               UINT first_stride, ID3D12Resource *second, UINT second_count,
                               UINT second_stride, ID3D12Resource *third, UINT third_count,
                               UINT third_stride, ID3D12Resource *output, UINT output_count,
                               UINT output_stride) {
        auto descriptor = heaps[heap_index]->GetCPUDescriptorHandleForHeapStart();
        srv(first, first_count, first_stride, descriptor);
        descriptor.ptr += increment;
        srv(second, second_count, second_stride, descriptor);
        descriptor.ptr += increment;
        srv(third, third_count, third_stride, descriptor);
        descriptor.ptr += increment;
        uav(output, output_count, output_stride, descriptor);
    };
    fill_heap(0, session->resident_pair_scratch[10].Get(), capacity, sizeof(float),
              session->resident_pair_scratch[5].Get(), count, sizeof(uint32_t),
              session->resident_pair_scratch[11].Get(), 1, 2 * sizeof(float),
              session->resident_pair_scratch[12].Get(), 2, 4 * sizeof(uint32_t));
    fill_heap(1, session->resident_pair_scratch[9].Get(), count, sizeof(float),
              session->resident_pair_scratch[5].Get(), count, sizeof(uint32_t),
              session->resident_pair_scratch[12].Get(), 2, 4 * sizeof(uint32_t),
              session->resident_pair_scratch[13].Get(), capacity, sizeof(float));
    fill_heap(2, session->resident_pair_scratch[9].Get(), count, sizeof(float),
              session->resident_pair_scratch[5].Get(), count, sizeof(uint32_t),
              session->resident_pair_scratch[12].Get(), 2, 4 * sizeof(uint32_t),
              session->resident_pair_scratch[13].Get(), capacity, sizeof(float));
    fill_heap(3, session->resident_pair_scratch[13].Get(), capacity, sizeof(float),
              session->resident_pair_scratch[12].Get(), 2, 4 * sizeof(uint32_t),
              session->resident_pair_scratch[12].Get(), 2, 4 * sizeof(uint32_t),
              session->resident_pair_scratch[14].Get(), 2, 4 * sizeof(uint32_t));
    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readback_description {};
    readback_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_description.Width = 8 * sizeof(uint32_t);
    readback_description.Height = 1;
    readback_description.DepthOrArraySize = 1;
    readback_description.MipLevels = 1;
    readback_description.SampleDesc.Count = 1;
    readback_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), summary_pipeline.Get(),
            IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_RESOURCE_BARRIER retained_transition {};
    retained_transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    retained_transition.Transition = {
        session->resident_pair_scratch[5].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &retained_transition);
    const uint32_t base_constants[3] {count, capacity, 0};
    const auto bind = [&](const size_t heap_index, ID3D12PipelineState *pipeline) {
        ID3D12DescriptorHeap *bound[] {heaps[heap_index].Get()};
        list->SetDescriptorHeaps(1, bound);
        list->SetPipelineState(pipeline);
        list->SetComputeRootSignature(root.Get());
        list->SetComputeRoot32BitConstants(0, 3, base_constants, 0);
        list->SetComputeRootDescriptorTable(1, heaps[heap_index]->GetGPUDescriptorHandleForHeapStart());
    };
    bind(0, summary_pipeline.Get());
    list->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER transition {};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition = {
        session->resident_pair_scratch[12].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &transition);
    bind(1, deviation_pipeline.Get());
    list->Dispatch((capacity + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER uav_barrier {};
    uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_barrier.UAV.pResource = session->resident_pair_scratch[13].Get();
    list->ResourceBarrier(1, &uav_barrier);
    bind(2, sort_pipeline.Get());
    for (uint32_t sequence = 2; sequence <= capacity; sequence *= 2)
    {
        for (uint32_t stride = sequence / 2; stride > 0; stride /= 2)
        {
            const uint32_t constants[3] {capacity, sequence, stride};
            list->SetComputeRoot32BitConstants(0, 3, constants, 0);
            list->Dispatch((capacity + 63) / 64, 1, 1);
            list->ResourceBarrier(1, &uav_barrier);
        }
        if (sequence == capacity)
            break;
    }
    transition.Transition = {
        session->resident_pair_scratch[13].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &transition);
    bind(3, result_pipeline.Get());
    list->Dispatch(1, 1, 1);
    transition.Transition = {
        session->resident_pair_scratch[14].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &transition);
    list->CopyResource(readback.Get(), session->resident_pair_scratch[14].Get());
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(
            session->device_core.get(), fence_value, error_buffer, error_buffer_size,
            "resident exposure-pair reduction timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    void *mapped = nullptr;
    const D3D12_RANGE range {0, 8 * sizeof(uint32_t)};
    if (FAILED(readback->Map(0, &range, &mapped)))
        return PANO_GPU_UNAVAILABLE;
    const auto *const words = static_cast<const uint32_t *>(mapped);
    reduction->rejection_reason = words[0];
    reduction->valid_count = words[1];
    reduction->inlier_count = words[2];
    std::memcpy(&reduction->difference, words + 3, sizeof(float));
    std::memcpy(&reduction->mad, words + 4, sizeof(float));
    std::memcpy(&reduction->weight, words + 5, sizeof(float));
    session->resident_pair_geometric_count = words[6];
    reduction->downloaded_bytes = 8 * sizeof(uint32_t);
    readback->Unmap(0, nullptr);
    session->resident_pair_reduced = true;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_filter_exposure_pair_acceptance(
    const pano_gpu_session *const session, const pano_gpu_exposure_pair_request *const request,
    const void *const gradients, const uint64_t gradient_bytes, const float *const gradient_limits,
    const uint64_t gradient_limit_bytes, void *const accepted, const uint64_t accepted_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    pano_gpu_exposure_pair_layout layout {};
    layout.size = sizeof(layout);
    layout.abi_version = PANO_GPU_ABI_VERSION;
    uint64_t coordinate_bytes = 0, sample_count = 0, expected_gradient_bytes = 0;
    uint8_t validation_overlap = 0;
    if (request == nullptr || !checked_multiply(static_cast<uint64_t>(request->sample_width), request->sample_height, &sample_count) ||
        !checked_multiply(sample_count, 4 * sizeof(float), &coordinate_bytes) ||
        pano_gpu_test_validate_exposure_pair_request(session, request, gradients, coordinate_bytes, &validation_overlap, sample_count, &layout, error_buffer, error_buffer_size) != PANO_GPU_SUCCESS ||
        gradients == nullptr || gradient_limits == nullptr || accepted == nullptr || !std::isfinite(gradient_limits[0]) || !std::isfinite(gradient_limits[1]) || gradient_limits[0] < 0.0F || gradient_limits[1] < 0.0F ||
        !checked_multiply(layout.sample_count, 2 * sizeof(float), &expected_gradient_bytes) || gradient_bytes != expected_gradient_bytes || gradient_limit_bytes != 2 * sizeof(float) || accepted_bytes != layout.sample_count)
    { write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-pair filter arguments"); return PANO_GPU_INVALID_ARGUMENT; }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows"); return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue || !session->device_core->fence || layout.sample_count > std::numeric_limits<UINT>::max())
    { write_error(error_buffer, error_buffer_size, "D3D12 exposure-pair filter resources are not ready"); return PANO_GPU_INVALID_ARGUMENT; }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const auto buffer = [](const uint64_t bytes, const D3D12_RESOURCE_FLAGS flags) { D3D12_RESOURCE_DESC value {}; value.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; value.Width=bytes; value.Height=1; value.DepthOrArraySize=1; value.MipLevels=1; value.SampleDesc.Count=1; value.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; value.Flags=flags; return value; };
    D3D12_DESCRIPTOR_RANGE ranges[2] {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,2}}; D3D12_ROOT_PARAMETER parameters[2] {}; parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[0].Constants={0,0,3}; parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[1].DescriptorTable={2,ranges}; D3D12_ROOT_SIGNATURE_DESC root_description {2,parameters,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE}; Microsoft::WRL::ComPtr<ID3DBlob> serialized,errors; Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature; Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if(FAILED(D3D12SerializeRootSignature(&root_description,D3D_ROOT_SIGNATURE_VERSION_1_0,&serialized,&errors))||FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root_signature)))) { write_error(error_buffer,error_buffer_size,"cannot create D3D12 exposure-pair filter root signature"); return PANO_GPU_UNAVAILABLE; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {}; pipeline_description.pRootSignature=root_signature.Get(); pipeline_description.CS={pano_gpu_exposure_pair_filter_shader,sizeof(pano_gpu_exposure_pair_filter_shader)}; if(FAILED(device->CreateComputePipelineState(&pipeline_description,IID_PPV_ARGS(&pipeline)))) { write_error(error_buffer,error_buffer_size,"cannot create D3D12 exposure-pair filter pipeline"); return PANO_GPU_UNAVAILABLE; }
    D3D12_HEAP_PROPERTIES upload_heap {}; upload_heap.Type=D3D12_HEAP_TYPE_UPLOAD; D3D12_HEAP_PROPERTIES default_heap {}; default_heap.Type=D3D12_HEAP_TYPE_DEFAULT; D3D12_HEAP_PROPERTIES readback_heap {}; readback_heap.Type=D3D12_HEAP_TYPE_READBACK; const uint64_t word_bytes=layout.sample_count*sizeof(uint32_t); const D3D12_RESOURCE_DESC gradients_desc=buffer(gradient_bytes,D3D12_RESOURCE_FLAG_NONE), categories_desc=buffer(word_bytes,D3D12_RESOURCE_FLAG_NONE), accepted_desc=buffer(word_bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), readback_desc=buffer(word_bytes,D3D12_RESOURCE_FLAG_NONE); Microsoft::WRL::ComPtr<ID3D12Resource> gradients_resource,categories_resource,accepted_resource,readback;
    if(FAILED(device->CreateCommittedResource(&upload_heap,D3D12_HEAP_FLAG_NONE,&gradients_desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&gradients_resource)))||FAILED(device->CreateCommittedResource(&upload_heap,D3D12_HEAP_FLAG_NONE,&categories_desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&categories_resource)))||FAILED(device->CreateCommittedResource(&default_heap,D3D12_HEAP_FLAG_NONE,&accepted_desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&accepted_resource)))||FAILED(device->CreateCommittedResource(&readback_heap,D3D12_HEAP_FLAG_NONE,&readback_desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)))) { write_error(error_buffer,error_buffer_size,"cannot create D3D12 exposure-pair filter resources"); return PANO_GPU_UNAVAILABLE; }
    void *mapped=nullptr; if(FAILED(gradients_resource->Map(0,nullptr,&mapped))) { write_error(error_buffer,error_buffer_size,"cannot map D3D12 exposure-pair gradients"); return PANO_GPU_UNAVAILABLE; } std::memcpy(mapped,gradients,static_cast<size_t>(gradient_bytes)); gradients_resource->Unmap(0,nullptr); if(FAILED(categories_resource->Map(0,nullptr,&mapped))) { write_error(error_buffer,error_buffer_size,"cannot map D3D12 exposure-pair categories"); return PANO_GPU_UNAVAILABLE; } const auto *const category_bytes=static_cast<const uint8_t *>(accepted); auto *const category_words=static_cast<uint32_t *>(mapped); for(uint64_t index=0;index<layout.sample_count;++index)category_words[index]=category_bytes[index] != 0 ? 1U : 0U; categories_resource->Unmap(0,nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC heap {}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors=3; heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors; Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator; Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list; if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&descriptors)))||FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))||FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),pipeline.Get(),IID_PPV_ARGS(&list)))) { write_error(error_buffer,error_buffer_size,"cannot create D3D12 exposure-pair filter dispatch"); return PANO_GPU_UNAVAILABLE; }
    const UINT increment=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); auto descriptor=descriptors->GetCPUDescriptorHandleForHeapStart(); D3D12_SHADER_RESOURCE_VIEW_DESC gradient_srv {}; gradient_srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER; gradient_srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; gradient_srv.Format=DXGI_FORMAT_UNKNOWN; gradient_srv.Buffer.NumElements=static_cast<UINT>(layout.sample_count); gradient_srv.Buffer.StructureByteStride=2*sizeof(float); device->CreateShaderResourceView(gradients_resource.Get(),&gradient_srv,descriptor); descriptor.ptr+=increment; D3D12_SHADER_RESOURCE_VIEW_DESC category_srv {}; category_srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER; category_srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; category_srv.Format=DXGI_FORMAT_R32_UINT; category_srv.Buffer.NumElements=static_cast<UINT>(layout.sample_count); device->CreateShaderResourceView(categories_resource.Get(),&category_srv,descriptor); descriptor.ptr+=increment; D3D12_UNORDERED_ACCESS_VIEW_DESC accepted_uav {}; accepted_uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER; accepted_uav.Format=DXGI_FORMAT_R32_UINT; accepted_uav.Buffer.NumElements=static_cast<UINT>(layout.sample_count); device->CreateUnorderedAccessView(accepted_resource.Get(),nullptr,&accepted_uav,descriptor);
    const uint32_t constants[]{static_cast<uint32_t>(layout.sample_count)}; ID3D12DescriptorHeap *heaps[]{descriptors.Get()}; list->SetDescriptorHeaps(1,heaps); list->SetComputeRootSignature(root_signature.Get()); list->SetComputeRoot32BitConstants(0,1,constants,0); list->SetComputeRoot32BitConstant(0,*reinterpret_cast<const uint32_t *>(&gradient_limits[0]),1); list->SetComputeRoot32BitConstant(0,*reinterpret_cast<const uint32_t *>(&gradient_limits[1]),2); list->SetComputeRootDescriptorTable(1,descriptors->GetGPUDescriptorHandleForHeapStart()); list->Dispatch(static_cast<UINT>((layout.sample_count+63)/64),1,1); D3D12_RESOURCE_BARRIER barrier {}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition.pResource=accepted_resource.Get(); barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS; barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE; barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; list->ResourceBarrier(1,&barrier); list->CopyResource(readback.Get(),accepted_resource.Get()); if(FAILED(list->Close())) { write_error(error_buffer,error_buffer_size,"cannot close D3D12 exposure-pair filter command list"); return PANO_GPU_UNAVAILABLE; } ID3D12CommandList *lists[]{list.Get()}; session->device_core->queue->ExecuteCommandLists(1,lists); const uint64_t fence_value=session->device_core->next_fence_value.fetch_add(1,std::memory_order_relaxed)+1; if(FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(),fence_value))||wait_for_fence(session->device_core.get(),fence_value,error_buffer,error_buffer_size,"D3D12 exposure-pair filter fence timed out")!=PANO_GPU_SUCCESS)return PANO_GPU_UNAVAILABLE; const D3D12_RANGE range{0,static_cast<SIZE_T>(word_bytes)}; if(FAILED(readback->Map(0,&range,&mapped))) { write_error(error_buffer,error_buffer_size,"cannot map D3D12 exposure-pair filter readback"); return PANO_GPU_UNAVAILABLE; } const auto *const result_words=static_cast<const uint32_t *>(mapped); auto *const result_bytes=static_cast<uint8_t *>(accepted); for(uint64_t index=0;index<layout.sample_count;++index)result_bytes[index]=static_cast<uint8_t>(result_words[index]!=0); readback->Unmap(0,nullptr); return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_build_exposure_pair_ratios(
    pano_gpu_session *const session, const pano_gpu_exposure_pair_request *const request,
    const void *const pair_luminance, const uint64_t pair_luminance_bytes, const void *const accepted,
    const uint64_t accepted_bytes, void *const log_ratios, const uint64_t log_ratio_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    pano_gpu_exposure_pair_layout layout {};
    layout.size = sizeof(layout);
    layout.abi_version = PANO_GPU_ABI_VERSION;
    uint64_t sample_count = 0, coordinate_bytes = 0, luminance_bytes = 0, ratio_bytes = 0;
    uint8_t validation_overlap = 0;
    if (request == nullptr || !checked_multiply(static_cast<uint64_t>(request->sample_width), request->sample_height, &sample_count) ||
        !checked_multiply(sample_count, 4 * sizeof(float), &coordinate_bytes) || !checked_multiply(sample_count, 2 * sizeof(float), &luminance_bytes) || !checked_multiply(sample_count, sizeof(float), &ratio_bytes) ||
        pano_gpu_test_validate_exposure_pair_request(session, request, pair_luminance, coordinate_bytes, &validation_overlap, sample_count, &layout, error_buffer, error_buffer_size) != PANO_GPU_SUCCESS ||
        pair_luminance == nullptr || accepted == nullptr || log_ratios == nullptr || pair_luminance_bytes != luminance_bytes || accepted_bytes != sample_count || log_ratio_bytes != ratio_bytes)
    { write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-pair ratio arguments"); return PANO_GPU_INVALID_ARGUMENT; }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows"); return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue || !session->device_core->fence || layout.sample_count > std::numeric_limits<UINT>::max())
    { write_error(error_buffer, error_buffer_size, "D3D12 exposure-pair ratio resources are not ready"); return PANO_GPU_INVALID_ARGUMENT; }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const auto buffer = [](const uint64_t bytes, const D3D12_RESOURCE_FLAGS flags) { D3D12_RESOURCE_DESC value {}; value.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; value.Width=bytes; value.Height=1; value.DepthOrArraySize=1; value.MipLevels=1; value.SampleDesc.Count=1; value.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; value.Flags=flags; return value; };
    D3D12_DESCRIPTOR_RANGE ranges[2] {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,2}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants={0,0,1};
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable={2,ranges};
    D3D12_ROOT_SIGNATURE_DESC root_description {2,parameters,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(D3D12SerializeRootSignature(&root_description,D3D_ROOT_SIGNATURE_VERSION_1_0,&serialized,&errors)) || FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root_signature))))
    { write_error(error_buffer,error_buffer_size,"cannot create D3D12 exposure-pair ratio root signature"); return PANO_GPU_UNAVAILABLE; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature=root_signature.Get();
    pipeline_description.CS={pano_gpu_exposure_pair_ratio_shader,sizeof(pano_gpu_exposure_pair_ratio_shader)};
    if (FAILED(device->CreateComputePipelineState(&pipeline_description,IID_PPV_ARGS(&pipeline))))
    { write_error(error_buffer,error_buffer_size,"cannot create D3D12 exposure-pair ratio pipeline"); return PANO_GPU_UNAVAILABLE; }
    D3D12_HEAP_PROPERTIES upload_heap {}; upload_heap.Type=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES default_heap {}; default_heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES readback_heap {}; readback_heap.Type=D3D12_HEAP_TYPE_READBACK;
    const uint64_t accepted_word_bytes=layout.sample_count*sizeof(uint32_t);
    const D3D12_RESOURCE_DESC luminance_desc=buffer(pair_luminance_bytes,D3D12_RESOURCE_FLAG_NONE), accepted_upload_desc=buffer(accepted_word_bytes,D3D12_RESOURCE_FLAG_NONE), accepted_desc=buffer(accepted_word_bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), ratio_desc=buffer(ratio_bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), readback_desc=buffer(ratio_bytes,D3D12_RESOURCE_FLAG_NONE);
    Microsoft::WRL::ComPtr<ID3D12Resource> luminance_resource, accepted_upload, accepted_resource, ratio_resource, readback;
    if (FAILED(device->CreateCommittedResource(&upload_heap,D3D12_HEAP_FLAG_NONE,&luminance_desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&luminance_resource))) || FAILED(device->CreateCommittedResource(&upload_heap,D3D12_HEAP_FLAG_NONE,&accepted_upload_desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&accepted_upload))) || FAILED(device->CreateCommittedResource(&default_heap,D3D12_HEAP_FLAG_NONE,&accepted_desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&accepted_resource))) || FAILED(device->CreateCommittedResource(&default_heap,D3D12_HEAP_FLAG_NONE,&ratio_desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&ratio_resource))) || FAILED(device->CreateCommittedResource(&readback_heap,D3D12_HEAP_FLAG_NONE,&readback_desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))))
    { write_error(error_buffer,error_buffer_size,"cannot create D3D12 exposure-pair ratio resources"); return PANO_GPU_UNAVAILABLE; }
    void *mapped=nullptr;
    if (FAILED(luminance_resource->Map(0,nullptr,&mapped))) { write_error(error_buffer,error_buffer_size,"cannot map D3D12 exposure-pair luminance"); return PANO_GPU_UNAVAILABLE; }
    std::memcpy(mapped,pair_luminance,static_cast<size_t>(pair_luminance_bytes)); luminance_resource->Unmap(0,nullptr);
    if (FAILED(accepted_upload->Map(0,nullptr,&mapped))) { write_error(error_buffer,error_buffer_size,"cannot map D3D12 exposure-pair accepted mask"); return PANO_GPU_UNAVAILABLE; }
    const auto *const accepted_bytes_input=static_cast<const uint8_t *>(accepted); auto *const accepted_words=static_cast<uint32_t *>(mapped);
    for (uint64_t index=0;index<layout.sample_count;++index) accepted_words[index]=accepted_bytes_input[index] != 0 ? 1U : 0U;
    accepted_upload->Unmap(0,nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC heap {}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors=3; heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors; Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator; Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&descriptors))) || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))) || FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),pipeline.Get(),IID_PPV_ARGS(&list))))
    { write_error(error_buffer,error_buffer_size,"cannot create D3D12 exposure-pair ratio dispatch"); return PANO_GPU_UNAVAILABLE; }
    const UINT increment=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); auto descriptor=descriptors->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC luminance_srv {}; luminance_srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER; luminance_srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; luminance_srv.Format=DXGI_FORMAT_UNKNOWN; luminance_srv.Buffer.NumElements=static_cast<UINT>(layout.sample_count); luminance_srv.Buffer.StructureByteStride=2*sizeof(float); device->CreateShaderResourceView(luminance_resource.Get(),&luminance_srv,descriptor); descriptor.ptr+=increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC accepted_srv {}; accepted_srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER; accepted_srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; accepted_srv.Format=DXGI_FORMAT_R32_UINT; accepted_srv.Buffer.NumElements=static_cast<UINT>(layout.sample_count); device->CreateShaderResourceView(accepted_upload.Get(),&accepted_srv,descriptor); descriptor.ptr+=increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ratio_uav {}; ratio_uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER; ratio_uav.Format=DXGI_FORMAT_R32_FLOAT; ratio_uav.Buffer.NumElements=static_cast<UINT>(layout.sample_count); device->CreateUnorderedAccessView(ratio_resource.Get(),nullptr,&ratio_uav,descriptor);
    const uint32_t constants[]{static_cast<uint32_t>(layout.sample_count)}; ID3D12DescriptorHeap *heaps[]{descriptors.Get()}; list->SetDescriptorHeaps(1,heaps); list->SetComputeRootSignature(root_signature.Get()); list->SetComputeRoot32BitConstants(0,1,constants,0); list->SetComputeRootDescriptorTable(1,descriptors->GetGPUDescriptorHandleForHeapStart()); list->Dispatch(static_cast<UINT>((layout.sample_count+63)/64),1,1);
    D3D12_RESOURCE_BARRIER barriers[2] {};
    barriers[0].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barriers[0].Transition.pResource=accepted_resource.Get(); barriers[0].Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS; barriers[0].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST; barriers[0].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barriers[1].Transition.pResource=ratio_resource.Get(); barriers[1].Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS; barriers[1].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE; barriers[1].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(2,barriers); list->CopyResource(accepted_resource.Get(),accepted_upload.Get()); list->CopyResource(readback.Get(),ratio_resource.Get());
    if (FAILED(list->Close())) { write_error(error_buffer,error_buffer_size,"cannot close D3D12 exposure-pair ratio command list"); return PANO_GPU_UNAVAILABLE; }
    ID3D12CommandList *lists[]{list.Get()}; session->device_core->queue->ExecuteCommandLists(1,lists); const uint64_t fence_value=session->device_core->next_fence_value.fetch_add(1,std::memory_order_relaxed)+1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(),fence_value)) || wait_for_fence(session->device_core.get(),fence_value,error_buffer,error_buffer_size,"D3D12 exposure-pair ratio fence timed out")!=PANO_GPU_SUCCESS) return PANO_GPU_UNAVAILABLE;
    const D3D12_RANGE range{0,static_cast<SIZE_T>(ratio_bytes)};
    if (FAILED(readback->Map(0,&range,&mapped))) { write_error(error_buffer,error_buffer_size,"cannot map D3D12 exposure-pair ratio readback"); return PANO_GPU_UNAVAILABLE; }
    std::memcpy(log_ratios,mapped,static_cast<size_t>(ratio_bytes)); readback->Unmap(0,nullptr);
    session->exposure_pair_accepted=std::move(accepted_resource); session->exposure_pair_log_ratios=std::move(ratio_resource); session->exposure_pair_sortable_ratios.Reset(); session->exposure_pair_trim_bounds.Reset(); session->exposure_pair_scratch_sample_count=layout.sample_count; session->exposure_pair_sortable_capacity=0; session->exposure_pair_scratch_retained=true; session->exposure_pair_sort_prepared=false; session->exposure_pair_sorted=false; session->exposure_pair_bounds_ready=false; session->exposure_pair_trimmed=false;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_prepare_exposure_pair_sort(
    pano_gpu_session *const session, const pano_gpu_exposure_pair_request *const request,
    void *const sortable_ratios, const uint64_t sortable_ratio_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    uint64_t sample_count = 0;
    if (session == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION ||
        !checked_multiply(static_cast<uint64_t>(request->sample_width), request->sample_height, &sample_count) ||
        sample_count == 0 || sample_count != session->exposure_pair_scratch_sample_count ||
        !session->exposure_pair_scratch_retained || session->exposure_pair_sort_prepared ||
        sortable_ratios == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-pair sort preparation arguments");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    uint64_t capacity = 1;
    while (capacity < sample_count)
    {
        if (capacity > std::numeric_limits<uint64_t>::max() / 2)
        {
            write_error(error_buffer, error_buffer_size, "D3D12 exposure-pair sort capacity overflow");
            return PANO_GPU_INVALID_ARGUMENT;
        }
        capacity *= 2;
    }
    uint64_t expected_bytes = 0;
    if (!checked_multiply(capacity, sizeof(float), &expected_bytes) || sortable_ratio_bytes != expected_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-pair sortable byte count");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue ||
        !session->device_core->fence || !session->exposure_pair_accepted ||
        !session->exposure_pair_log_ratios || capacity > std::numeric_limits<UINT>::max())
    {
        write_error(error_buffer, error_buffer_size, "D3D12 exposure-pair sort preparation resources are not ready");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const auto buffer = [](const uint64_t bytes, const D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC value {};
        value.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        value.Width = bytes;
        value.Height = 1;
        value.DepthOrArraySize = 1;
        value.MipLevels = 1;
        value.SampleDesc.Count = 1;
        value.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        value.Flags = flags;
        return value;
    };
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 2};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC root_description {
        2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&root_signature))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair sort preparation root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root_signature.Get();
    pipeline_description.CS = {
        pano_gpu_exposure_pair_sort_prepare_shader,
        sizeof(pano_gpu_exposure_pair_sort_prepare_shader)};
    if (FAILED(device->CreateComputePipelineState(
            &pipeline_description, IID_PPV_ARGS(&pipeline))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair sort preparation pipeline");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES default_heap {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    const D3D12_RESOURCE_DESC sortable_description =
        buffer(expected_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC readback_description =
        buffer(expected_bytes, D3D12_RESOURCE_FLAG_NONE);
    Microsoft::WRL::ComPtr<ID3D12Resource> sortable_resource, readback;
    if (FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &sortable_description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&sortable_resource))) ||
        FAILED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair sortable resources");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 3;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&descriptors))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair sort preparation dispatch");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto descriptor = descriptors->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC ratio_srv {};
    ratio_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    ratio_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ratio_srv.Format = DXGI_FORMAT_UNKNOWN;
    ratio_srv.Buffer.NumElements = static_cast<UINT>(sample_count);
    ratio_srv.Buffer.StructureByteStride = sizeof(float);
    device->CreateShaderResourceView(session->exposure_pair_log_ratios.Get(), &ratio_srv, descriptor);
    descriptor.ptr += increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC accepted_srv {};
    accepted_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    accepted_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    accepted_srv.Format = DXGI_FORMAT_R32_UINT;
    accepted_srv.Buffer.NumElements = static_cast<UINT>(sample_count);
    device->CreateShaderResourceView(session->exposure_pair_accepted.Get(), &accepted_srv, descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC sortable_uav {};
    sortable_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    sortable_uav.Format = DXGI_FORMAT_R32_FLOAT;
    sortable_uav.Buffer.NumElements = static_cast<UINT>(capacity);
    device->CreateUnorderedAccessView(sortable_resource.Get(), nullptr, &sortable_uav, descriptor);
    D3D12_RESOURCE_BARRIER input_barriers[2] {};
    input_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    input_barriers[0].Transition.pResource = session->exposure_pair_log_ratios.Get();
    input_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    input_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    input_barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    input_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    input_barriers[1].Transition.pResource = session->exposure_pair_accepted.Get();
    input_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    input_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    input_barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(2, input_barriers);
    const uint32_t constants[] {
        static_cast<uint32_t>(sample_count), static_cast<uint32_t>(capacity)};
    ID3D12DescriptorHeap *heaps[] {descriptors.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_signature.Get());
    list->SetComputeRoot32BitConstants(0, 2, constants, 0);
    list->SetComputeRootDescriptorTable(1, descriptors->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((static_cast<UINT>(capacity) + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER output_barrier {};
    output_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    output_barrier.Transition.pResource = sortable_resource.Get();
    output_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    output_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    output_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &output_barrier);
    list->CopyResource(readback.Get(), sortable_resource.Get());
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 exposure-pair sort preparation command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(
            session->device_core.get(), fence_value, error_buffer, error_buffer_size,
            "D3D12 exposure-pair sort preparation fence timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    void *mapped = nullptr;
    const D3D12_RANGE range {0, static_cast<SIZE_T>(expected_bytes)};
    if (FAILED(readback->Map(0, &range, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 exposure-pair sortable readback");
        return PANO_GPU_UNAVAILABLE;
    }
    std::memcpy(sortable_ratios, mapped, static_cast<size_t>(expected_bytes));
    readback->Unmap(0, nullptr);
    session->exposure_pair_sortable_ratios = std::move(sortable_resource);
    session->exposure_pair_sortable_capacity = capacity;
    session->exposure_pair_sort_prepared = true;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_sort_exposure_pair(
    pano_gpu_session *const session, void *const sorted_ratios, const uint64_t sorted_ratio_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    uint64_t expected_bytes = 0;
    if (session == nullptr || !session->exposure_pair_scratch_retained ||
        !session->exposure_pair_sort_prepared || session->exposure_pair_sorted ||
        session->exposure_pair_sortable_capacity == 0 || sorted_ratios == nullptr ||
        !checked_multiply(session->exposure_pair_sortable_capacity, sizeof(float), &expected_bytes) ||
        sorted_ratio_bytes != expected_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-pair sort arguments");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue ||
        !session->device_core->fence || !session->exposure_pair_sortable_ratios ||
        session->exposure_pair_sortable_capacity > std::numeric_limits<UINT>::max())
    {
        write_error(error_buffer, error_buffer_size, "D3D12 exposure-pair sort resources are not ready");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    D3D12_DESCRIPTOR_RANGE range {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 3};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &range};
    D3D12_ROOT_SIGNATURE_DESC root_description {
        2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&root_signature))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair sort root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root_signature.Get();
    pipeline_description.CS = {
        pano_gpu_exposure_pair_sort_shader, sizeof(pano_gpu_exposure_pair_sort_shader)};
    if (FAILED(device->CreateComputePipelineState(
            &pipeline_description, IID_PPV_ARGS(&pipeline))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair sort pipeline");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 1;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&descriptors))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair sort dispatch");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC sortable_uav {};
    sortable_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    sortable_uav.Format = DXGI_FORMAT_R32_FLOAT;
    sortable_uav.Buffer.NumElements = static_cast<UINT>(session->exposure_pair_sortable_capacity);
    device->CreateUnorderedAccessView(
        session->exposure_pair_sortable_ratios.Get(), nullptr, &sortable_uav,
        descriptors->GetCPUDescriptorHandleForHeapStart());
    D3D12_RESOURCE_BARRIER transition {};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition.pResource = session->exposure_pair_sortable_ratios.Get();
    transition.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    transition.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &transition);
    ID3D12DescriptorHeap *heaps[] {descriptors.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_signature.Get());
    list->SetComputeRootDescriptorTable(1, descriptors->GetGPUDescriptorHandleForHeapStart());
    const uint32_t capacity = static_cast<uint32_t>(session->exposure_pair_sortable_capacity);
    for (uint32_t sequence_size = 2; capacity > 1; sequence_size *= 2)
    {
        for (uint32_t comparison_stride = sequence_size / 2; comparison_stride > 0;
             comparison_stride /= 2)
        {
            const uint32_t constants[] {capacity, sequence_size, comparison_stride};
            list->SetComputeRoot32BitConstants(0, 3, constants, 0);
            list->Dispatch((capacity + 63) / 64, 1, 1);
            D3D12_RESOURCE_BARRIER uav_barrier {};
            uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav_barrier.UAV.pResource = session->exposure_pair_sortable_ratios.Get();
            list->ResourceBarrier(1, &uav_barrier);
        }
        if (sequence_size == capacity)
            break;
    }
    transition.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &transition);
    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readback_description {};
    readback_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_description.Width = expected_bytes;
    readback_description.Height = 1;
    readback_description.DepthOrArraySize = 1;
    readback_description.MipLevels = 1;
    readback_description.SampleDesc.Count = 1;
    readback_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair sort readback");
        return PANO_GPU_UNAVAILABLE;
    }
    list->CopyResource(readback.Get(), session->exposure_pair_sortable_ratios.Get());
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 exposure-pair sort command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(
            session->device_core.get(), fence_value, error_buffer, error_buffer_size,
            "D3D12 exposure-pair sort fence timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    void *mapped = nullptr;
    const D3D12_RANGE read_range {0, static_cast<SIZE_T>(expected_bytes)};
    if (FAILED(readback->Map(0, &read_range, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 exposure-pair sort readback");
        return PANO_GPU_UNAVAILABLE;
    }
    std::memcpy(sorted_ratios, mapped, static_cast<size_t>(expected_bytes));
    readback->Unmap(0, nullptr);
    session->exposure_pair_sorted = true;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_extract_exposure_pair_trim_bounds(
    pano_gpu_session *const session, float *const trim_bounds, const uint64_t trim_bound_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || trim_bounds == nullptr || trim_bound_bytes != 2 * sizeof(float) ||
        !session->exposure_pair_scratch_retained || !session->exposure_pair_sorted ||
        session->exposure_pair_bounds_ready || session->exposure_pair_scratch_sample_count == 0 ||
        session->exposure_pair_sortable_capacity == 0)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-pair trim-bound arguments");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue ||
        !session->device_core->fence || !session->exposure_pair_accepted ||
        !session->exposure_pair_sortable_ratios ||
        session->exposure_pair_scratch_sample_count > std::numeric_limits<UINT>::max() ||
        session->exposure_pair_sortable_capacity > std::numeric_limits<UINT>::max())
    {
        write_error(error_buffer, error_buffer_size, "D3D12 exposure-pair trim-bound resources are not ready");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 1};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC root_description {
        2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&root_signature))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair trim-bound root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root_signature.Get();
    pipeline_description.CS = {
        pano_gpu_exposure_pair_bounds_shader, sizeof(pano_gpu_exposure_pair_bounds_shader)};
    if (FAILED(device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair trim-bound pipeline");
        return PANO_GPU_UNAVAILABLE;
    }
    const auto buffer = [](const uint64_t bytes, const D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC value {};
        value.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        value.Width = bytes;
        value.Height = 1;
        value.DepthOrArraySize = 1;
        value.MipLevels = 1;
        value.SampleDesc.Count = 1;
        value.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        value.Flags = flags;
        return value;
    };
    D3D12_HEAP_PROPERTIES default_heap {}; default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES readback_heap {}; readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    const D3D12_RESOURCE_DESC bounds_description =
        buffer(2 * sizeof(float), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC readback_description =
        buffer(2 * sizeof(float), D3D12_RESOURCE_FLAG_NONE);
    Microsoft::WRL::ComPtr<ID3D12Resource> bounds_resource, readback;
    if (FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &bounds_description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&bounds_resource))) ||
        FAILED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair trim-bound resources");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 3;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&descriptors))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair trim-bound dispatch");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto descriptor = descriptors->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sorted_srv {};
    sorted_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sorted_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sorted_srv.Format = DXGI_FORMAT_UNKNOWN;
    sorted_srv.Buffer.NumElements = static_cast<UINT>(session->exposure_pair_sortable_capacity);
    sorted_srv.Buffer.StructureByteStride = sizeof(float);
    device->CreateShaderResourceView(session->exposure_pair_sortable_ratios.Get(), &sorted_srv, descriptor);
    descriptor.ptr += increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC accepted_srv {};
    accepted_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    accepted_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    accepted_srv.Format = DXGI_FORMAT_R32_UINT;
    accepted_srv.Buffer.NumElements = static_cast<UINT>(session->exposure_pair_scratch_sample_count);
    device->CreateShaderResourceView(session->exposure_pair_accepted.Get(), &accepted_srv, descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC bounds_uav {};
    bounds_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    bounds_uav.Format = DXGI_FORMAT_UNKNOWN;
    bounds_uav.Buffer.NumElements = 1;
    bounds_uav.Buffer.StructureByteStride = 2 * sizeof(float);
    device->CreateUnorderedAccessView(bounds_resource.Get(), nullptr, &bounds_uav, descriptor);
    D3D12_RESOURCE_BARRIER input_barrier {};
    input_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    input_barrier.Transition.pResource = session->exposure_pair_sortable_ratios.Get();
    input_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    input_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    input_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &input_barrier);
    const uint32_t sample_count = static_cast<uint32_t>(session->exposure_pair_scratch_sample_count);
    ID3D12DescriptorHeap *heaps[] {descriptors.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_signature.Get());
    list->SetComputeRoot32BitConstants(0, 1, &sample_count, 0);
    list->SetComputeRootDescriptorTable(1, descriptors->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER output_barrier {};
    output_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    output_barrier.Transition.pResource = bounds_resource.Get();
    output_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    output_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    output_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &output_barrier);
    list->CopyResource(readback.Get(), bounds_resource.Get());
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 exposure-pair trim-bound command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(
            session->device_core.get(), fence_value, error_buffer, error_buffer_size,
            "D3D12 exposure-pair trim-bound fence timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    void *mapped = nullptr;
    const D3D12_RANGE read_range {0, static_cast<SIZE_T>(2 * sizeof(float))};
    if (FAILED(readback->Map(0, &read_range, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 exposure-pair trim-bound readback");
        return PANO_GPU_UNAVAILABLE;
    }
    std::memcpy(trim_bounds, mapped, 2 * sizeof(float));
    readback->Unmap(0, nullptr);
    session->exposure_pair_trim_bounds = std::move(bounds_resource);
    session->exposure_pair_bounds_ready = true;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_trim_exposure_pair(
    pano_gpu_session *const session, void *const trimmed, const uint64_t trimmed_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || trimmed == nullptr || !session->exposure_pair_scratch_retained ||
        !session->exposure_pair_bounds_ready || session->exposure_pair_trimmed ||
        session->exposure_pair_scratch_sample_count == 0 ||
        trimmed_bytes != session->exposure_pair_scratch_sample_count)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-pair trim arguments");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue ||
        !session->device_core->fence || !session->exposure_pair_log_ratios ||
        !session->exposure_pair_accepted || !session->exposure_pair_trim_bounds ||
        session->exposure_pair_scratch_sample_count > std::numeric_limits<UINT>::max())
    {
        write_error(error_buffer, error_buffer_size, "D3D12 exposure-pair trim resources are not ready");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const uint64_t word_bytes = session->exposure_pair_scratch_sample_count * sizeof(uint32_t);
    const auto buffer = [](const uint64_t bytes, const D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC value {};
        value.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        value.Width = bytes;
        value.Height = 1;
        value.DepthOrArraySize = 1;
        value.MipLevels = 1;
        value.SampleDesc.Count = 1;
        value.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        value.Flags = flags;
        return value;
    };
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 1};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC root_description {
        2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&root_signature))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair trim root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root_signature.Get();
    pipeline_description.CS = {
        pano_gpu_exposure_pair_trim_shader, sizeof(pano_gpu_exposure_pair_trim_shader)};
    if (FAILED(device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair trim pipeline");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES default_heap {}; default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES readback_heap {}; readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    const D3D12_RESOURCE_DESC trimmed_description =
        buffer(word_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC readback_description =
        buffer(word_bytes, D3D12_RESOURCE_FLAG_NONE);
    Microsoft::WRL::ComPtr<ID3D12Resource> trimmed_resource, readback;
    if (FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &trimmed_description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&trimmed_resource))) ||
        FAILED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair trim resources");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 4;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&descriptors))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair trim dispatch");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto descriptor = descriptors->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC ratio_srv {};
    ratio_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    ratio_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ratio_srv.Format = DXGI_FORMAT_UNKNOWN;
    ratio_srv.Buffer.NumElements = static_cast<UINT>(session->exposure_pair_scratch_sample_count);
    ratio_srv.Buffer.StructureByteStride = sizeof(float);
    device->CreateShaderResourceView(session->exposure_pair_log_ratios.Get(), &ratio_srv, descriptor);
    descriptor.ptr += increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC accepted_srv {};
    accepted_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    accepted_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    accepted_srv.Format = DXGI_FORMAT_R32_UINT;
    accepted_srv.Buffer.NumElements = static_cast<UINT>(session->exposure_pair_scratch_sample_count);
    device->CreateShaderResourceView(session->exposure_pair_accepted.Get(), &accepted_srv, descriptor);
    descriptor.ptr += increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC bounds_srv {};
    bounds_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    bounds_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    bounds_srv.Format = DXGI_FORMAT_UNKNOWN;
    bounds_srv.Buffer.NumElements = 1;
    bounds_srv.Buffer.StructureByteStride = 2 * sizeof(float);
    device->CreateShaderResourceView(session->exposure_pair_trim_bounds.Get(), &bounds_srv, descriptor);
    descriptor.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC trimmed_uav {};
    trimmed_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    trimmed_uav.Format = DXGI_FORMAT_R32_UINT;
    trimmed_uav.Buffer.NumElements = static_cast<UINT>(session->exposure_pair_scratch_sample_count);
    device->CreateUnorderedAccessView(trimmed_resource.Get(), nullptr, &trimmed_uav, descriptor);
    D3D12_RESOURCE_BARRIER bounds_barrier {};
    bounds_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bounds_barrier.Transition.pResource = session->exposure_pair_trim_bounds.Get();
    bounds_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bounds_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bounds_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &bounds_barrier);
    const uint32_t sample_count = static_cast<uint32_t>(session->exposure_pair_scratch_sample_count);
    ID3D12DescriptorHeap *heaps[] {descriptors.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_signature.Get());
    list->SetComputeRoot32BitConstants(0, 1, &sample_count, 0);
    list->SetComputeRootDescriptorTable(1, descriptors->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((sample_count + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER output_barrier {};
    output_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    output_barrier.Transition.pResource = trimmed_resource.Get();
    output_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    output_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    output_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &output_barrier);
    list->CopyResource(readback.Get(), trimmed_resource.Get());
    output_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    output_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &output_barrier);
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 exposure-pair trim command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(
            session->device_core.get(), fence_value, error_buffer, error_buffer_size,
            "D3D12 exposure-pair trim fence timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    void *mapped = nullptr;
    const D3D12_RANGE read_range {0, static_cast<SIZE_T>(word_bytes)};
    if (FAILED(readback->Map(0, &read_range, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 exposure-pair trim readback");
        return PANO_GPU_UNAVAILABLE;
    }
    const auto *const words = static_cast<const uint32_t *>(mapped);
    auto *const bytes = static_cast<uint8_t *>(trimmed);
    for (uint64_t index = 0; index < session->exposure_pair_scratch_sample_count; ++index)
        bytes[index] = static_cast<uint8_t>(words[index] != 0);
    readback->Unmap(0, nullptr);
    session->exposure_pair_accepted = std::move(trimmed_resource);
    session->exposure_pair_trimmed = true;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_reduce_exposure_pair(
    pano_gpu_session *const session, pano_gpu_exposure_pair_reduction *const reduction,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (reduction == nullptr || reduction->size != sizeof(*reduction) ||
        reduction->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-pair reduction result");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    reduction->rejection_reason = PANO_GPU_EXPOSURE_PAIR_NONFINITE;
    reduction->valid_count = 0;
    reduction->inlier_count = 0;
    reduction->difference = std::numeric_limits<float>::quiet_NaN();
    reduction->mad = std::numeric_limits<float>::quiet_NaN();
    reduction->weight = 0.0F;
    reduction->downloaded_bytes = 0;
    if (session == nullptr || !session->exposure_pair_trimmed ||
        !session->exposure_pair_sorted || session->exposure_pair_scratch_sample_count == 0 ||
        session->exposure_pair_sortable_capacity == 0)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 exposure-pair reduction state");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    const uint64_t sample_count = session->exposure_pair_scratch_sample_count;
    const uint64_t capacity = session->exposure_pair_sortable_capacity;
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue ||
        !session->device_core->fence || !session->exposure_pair_log_ratios ||
        !session->exposure_pair_accepted || !session->exposure_pair_sortable_ratios ||
        !session->exposure_pair_trim_bounds || sample_count > std::numeric_limits<UINT>::max() ||
        capacity > std::numeric_limits<UINT>::max())
    {
        write_error(error_buffer, error_buffer_size, "D3D12 exposure-pair reduction resources are not ready");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 3};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC root_description {
        2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&root_signature))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair reduction root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    const auto create_pipeline = [&](const BYTE *const shader, const size_t shader_bytes,
                                     Microsoft::WRL::ComPtr<ID3D12PipelineState> *const pipeline) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC description {};
        description.pRootSignature = root_signature.Get();
        description.CS = {shader, shader_bytes};
        return device->CreateComputePipelineState(&description, IID_PPV_ARGS(pipeline->GetAddressOf()));
    };
    Microsoft::WRL::ComPtr<ID3D12PipelineState> summary_pipeline, deviation_pipeline, sort_pipeline, result_pipeline;
    if (FAILED(create_pipeline(
            pano_gpu_exposure_pair_reduce_summary_shader,
            sizeof(pano_gpu_exposure_pair_reduce_summary_shader), &summary_pipeline)) ||
        FAILED(create_pipeline(
            pano_gpu_exposure_pair_reduce_deviations_shader,
            sizeof(pano_gpu_exposure_pair_reduce_deviations_shader), &deviation_pipeline)) ||
        FAILED(create_pipeline(
            pano_gpu_exposure_pair_sort_shader, sizeof(pano_gpu_exposure_pair_sort_shader),
            &sort_pipeline)) ||
        FAILED(create_pipeline(
            pano_gpu_exposure_pair_reduce_result_shader,
            sizeof(pano_gpu_exposure_pair_reduce_result_shader), &result_pipeline)))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair reduction pipelines");
        return PANO_GPU_UNAVAILABLE;
    }
    const auto buffer = [](const uint64_t bytes, const D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC value {};
        value.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        value.Width = bytes;
        value.Height = 1;
        value.DepthOrArraySize = 1;
        value.MipLevels = 1;
        value.SampleDesc.Count = 1;
        value.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        value.Flags = flags;
        return value;
    };
    D3D12_HEAP_PROPERTIES default_heap {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    const uint64_t packet_bytes = 2 * 4 * sizeof(uint32_t);
    const D3D12_RESOURCE_DESC summary_description =
        buffer(8 * sizeof(uint32_t), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC deviation_description =
        buffer(capacity * sizeof(float), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC result_description =
        buffer(packet_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC readback_description = buffer(packet_bytes, D3D12_RESOURCE_FLAG_NONE);
    Microsoft::WRL::ComPtr<ID3D12Resource> summary_resource, deviation_resource, result_resource, readback;
    if (FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &summary_description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&summary_resource))) ||
        FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &deviation_description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&deviation_resource))) ||
        FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &result_description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&result_resource))) ||
        FAILED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair reduction resources");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 4;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> summary_heap, deviation_heap, sort_heap, result_heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&summary_heap))) ||
        FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&deviation_heap))) ||
        FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&sort_heap))) ||
        FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&result_heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), summary_pipeline.Get(),
            IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 exposure-pair reduction dispatch");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto srv = [&](ID3D12Resource *const resource, const UINT elements, const UINT stride,
                         D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view {};
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.Buffer.NumElements = elements;
        view.Buffer.StructureByteStride = stride;
        device->CreateShaderResourceView(resource, &view, descriptor);
    };
    const auto uav = [&](ID3D12Resource *const resource, const UINT elements, const UINT stride,
                         D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view {};
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.Buffer.NumElements = elements;
        view.Buffer.StructureByteStride = stride;
        device->CreateUnorderedAccessView(resource, nullptr, &view, descriptor);
    };
    const auto fill_heap = [&](ID3D12DescriptorHeap *const heap, ID3D12Resource *const first,
                               const UINT first_elements, const UINT first_stride,
                               ID3D12Resource *const second, const UINT second_elements,
                               const UINT second_stride, ID3D12Resource *const third,
                               const UINT third_elements, const UINT third_stride,
                               ID3D12Resource *const output, const UINT output_elements,
                               const UINT output_stride) {
        auto descriptor = heap->GetCPUDescriptorHandleForHeapStart();
        srv(first, first_elements, first_stride, descriptor);
        descriptor.ptr += increment;
        srv(second, second_elements, second_stride, descriptor);
        descriptor.ptr += increment;
        srv(third, third_elements, third_stride, descriptor);
        descriptor.ptr += increment;
        uav(output, output_elements, output_stride, descriptor);
    };
    fill_heap(
        summary_heap.Get(), session->exposure_pair_sortable_ratios.Get(), static_cast<UINT>(capacity),
        sizeof(float), session->exposure_pair_accepted.Get(), static_cast<UINT>(sample_count),
        sizeof(uint32_t), session->exposure_pair_trim_bounds.Get(), 1, 2 * sizeof(float),
        summary_resource.Get(), 2, 4 * sizeof(uint32_t));
    fill_heap(
        deviation_heap.Get(), session->exposure_pair_log_ratios.Get(), static_cast<UINT>(sample_count),
        sizeof(float), session->exposure_pair_accepted.Get(), static_cast<UINT>(sample_count),
        sizeof(uint32_t), summary_resource.Get(), 2, 4 * sizeof(uint32_t), deviation_resource.Get(),
        static_cast<UINT>(capacity), sizeof(float));
    fill_heap(
        sort_heap.Get(), session->exposure_pair_log_ratios.Get(), static_cast<UINT>(sample_count),
        sizeof(float), session->exposure_pair_accepted.Get(), static_cast<UINT>(sample_count),
        sizeof(uint32_t), summary_resource.Get(), 2, 4 * sizeof(uint32_t), deviation_resource.Get(),
        static_cast<UINT>(capacity), sizeof(float));
    fill_heap(
        result_heap.Get(), deviation_resource.Get(), static_cast<UINT>(capacity), sizeof(float),
        summary_resource.Get(), 2, 4 * sizeof(uint32_t), summary_resource.Get(), 2,
        4 * sizeof(uint32_t), result_resource.Get(), 2, 4 * sizeof(uint32_t));
    const uint32_t base_constants[] {
        static_cast<uint32_t>(sample_count), static_cast<uint32_t>(capacity), 0};
    const auto bind = [&](ID3D12DescriptorHeap *const heap, ID3D12PipelineState *const pipeline) {
        ID3D12DescriptorHeap *heaps[] {heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetPipelineState(pipeline);
        list->SetComputeRootSignature(root_signature.Get());
        list->SetComputeRoot32BitConstants(0, 3, base_constants, 0);
        list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    };
    bind(summary_heap.Get(), summary_pipeline.Get());
    list->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER transition {};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition.pResource = summary_resource.Get();
    transition.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &transition);
    bind(deviation_heap.Get(), deviation_pipeline.Get());
    list->Dispatch((static_cast<UINT>(capacity) + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER uav_barrier {};
    uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_barrier.UAV.pResource = deviation_resource.Get();
    list->ResourceBarrier(1, &uav_barrier);
    bind(sort_heap.Get(), sort_pipeline.Get());
    for (uint32_t sequence = 2; sequence <= static_cast<uint32_t>(capacity); sequence *= 2)
    {
        for (uint32_t stride = sequence / 2; stride > 0; stride /= 2)
        {
            const uint32_t constants[] {static_cast<uint32_t>(capacity), sequence, stride};
            list->SetComputeRoot32BitConstants(0, 3, constants, 0);
            list->Dispatch((static_cast<UINT>(capacity) + 63) / 64, 1, 1);
            list->ResourceBarrier(1, &uav_barrier);
        }
        if (sequence == static_cast<uint32_t>(capacity))
            break;
    }
    transition.Transition.pResource = deviation_resource.Get();
    transition.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &transition);
    bind(result_heap.Get(), result_pipeline.Get());
    list->Dispatch(1, 1, 1);
    transition.Transition.pResource = result_resource.Get();
    transition.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &transition);
    list->CopyResource(readback.Get(), result_resource.Get());
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 exposure-pair reduction command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value = session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(
            session->device_core.get(), fence_value, error_buffer, error_buffer_size,
            "D3D12 exposure-pair reduction fence timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    void *mapped = nullptr;
    const D3D12_RANGE range {0, static_cast<SIZE_T>(packet_bytes)};
    if (FAILED(readback->Map(0, &range, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 exposure-pair reduction packet");
        return PANO_GPU_UNAVAILABLE;
    }
    const auto *const words = static_cast<const uint32_t *>(mapped);
    reduction->rejection_reason = words[0];
    reduction->valid_count = words[1];
    reduction->inlier_count = words[2];
    std::memcpy(&reduction->difference, words + 3, sizeof(float));
    std::memcpy(&reduction->mad, words + 4, sizeof(float));
    std::memcpy(&reduction->weight, words + 5, sizeof(float));
    reduction->downloaded_bytes = packet_bytes;
    readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_dispatch_uint8_exposure_proxies(
    const pano_gpu_session *const session, const pano_gpu_exposure_proxy_request *const request,
    void *const proxies, const uint64_t proxy_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return dispatch_typed_exposure_proxies(
        session, request, proxies, proxy_bytes, error_buffer, error_buffer_size, PANO_GPU_SAMPLE_UINT8);
}

pano_gpu_result pano_gpu_test_dispatch_uint16_exposure_proxies(
    const pano_gpu_session *const session, const pano_gpu_exposure_proxy_request *const request,
    void *const proxies, const uint64_t proxy_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return dispatch_typed_exposure_proxies(
        session, request, proxies, proxy_bytes, error_buffer, error_buffer_size, PANO_GPU_SAMPLE_UINT16);
}

pano_gpu_result pano_gpu_test_dispatch_float32_exposure_proxies(
    const pano_gpu_session *const session, const pano_gpu_exposure_proxy_request *const request,
    void *const proxies, const uint64_t proxy_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return dispatch_typed_exposure_proxies(
        session, request, proxies, proxy_bytes, error_buffer, error_buffer_size, PANO_GPU_SAMPLE_FLOAT32);
}

pano_gpu_result pano_gpu_test_dispatch_one_frame_equirect_local_exposure(
    const pano_gpu_session *const session, const pano_gpu_one_frame_composite_request *const request,
    const float global_gain, void *const local_field, const uint64_t local_field_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->frame_index >= session->frame_count ||
        request->output_width == 0 || request->output_height == 0 || request->rectilinear_output > 1 ||
        !std::isfinite(request->latitude_span_degrees) || !std::isfinite(request->horizontal_fov_degrees) ||
        !std::isfinite(request->vertical_fov_degrees) || request->latitude_span_degrees <= 0.0F ||
        request->horizontal_fov_degrees <= 0.0F || request->vertical_fov_degrees <= 0.0F ||
        (request->rectilinear_output != 0 && (!std::isfinite(request->output_vertical_fov_degrees) ||
                                             request->output_vertical_fov_degrees <= 0.0F ||
                                             request->output_vertical_fov_degrees >= 180.0F)) ||
        !std::isfinite(global_gain) || global_gain <= 0.0F || local_field == nullptr)
    { write_error(error_buffer, error_buffer_size, "invalid D3D12 equirectangular local-exposure request"); return PANO_GPU_INVALID_ARGUMENT; }
    const uint32_t field_width = (request->output_width - 1) / 4 + 1;
    const uint32_t field_height = (request->output_height - 1) / 4 + 1;
    uint64_t field_pixels = 0, field_bytes = 0;
    if (!checked_multiply(field_width, field_height, &field_pixels) || !checked_multiply(field_pixels, sizeof(float), &field_bytes) || local_field_bytes != field_bytes)
    { write_error(error_buffer, error_buffer_size, "invalid D3D12 equirectangular local-exposure field"); return PANO_GPU_INVALID_ARGUMENT; }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows"); return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue || !session->device_core->fence)
    { write_error(error_buffer, error_buffer_size, "invalid D3D12 equirectangular local-exposure session"); return PANO_GPU_INVALID_ARGUMENT; }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const auto buffer = [](const uint64_t bytes, const D3D12_RESOURCE_FLAGS flags) { D3D12_RESOURCE_DESC value {}; value.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; value.Width = bytes; value.Height = 1; value.DepthOrArraySize = 1; value.MipLevels = 1; value.SampleDesc.Count = 1; value.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; value.Flags = flags; return value; };
    D3D12_HEAP_PROPERTIES default_heap {}; default_heap.Type = D3D12_HEAP_TYPE_DEFAULT; D3D12_HEAP_PROPERTIES readback_heap {}; readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    Microsoft::WRL::ComPtr<ID3D12Resource> output, readback;
    const D3D12_RESOURCE_DESC output_desc = buffer(field_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC readback_desc = buffer(field_bytes, D3D12_RESOURCE_FLAG_NONE);
    if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &output_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output))) || FAILED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
    { write_error(error_buffer, error_buffer_size, "cannot create D3D12 equirectangular local-exposure resources"); return PANO_GPU_UNAVAILABLE; }
    D3D12_DESCRIPTOR_RANGE ranges[1] {{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0}}; D3D12_ROOT_PARAMETER parameters[2] {}; parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[0].Constants = {0, 0, 36}; parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[1].DescriptorTable = {1, ranges}; D3D12_ROOT_SIGNATURE_DESC root_desc {2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE}; Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors; Microsoft::WRL::ComPtr<ID3D12RootSignature> root; Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)) || FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)))) { write_error(error_buffer, error_buffer_size, "cannot create D3D12 equirectangular local-exposure root signature"); return PANO_GPU_UNAVAILABLE; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc {}; pipeline_desc.pRootSignature = root.Get(); pipeline_desc.CS = {pano_gpu_equirect_local_exposure_shader, sizeof(pano_gpu_equirect_local_exposure_shader)}; if (FAILED(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)))) { write_error(error_buffer, error_buffer_size, "cannot create D3D12 equirectangular local-exposure pipeline"); return PANO_GPU_UNAVAILABLE; }
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc {}; heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap_desc.NumDescriptors = 1; heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap; Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator; Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list; if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap))) || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) || FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list)))) { write_error(error_buffer, error_buffer_size, "cannot create D3D12 equirectangular local-exposure dispatch"); return PANO_GPU_UNAVAILABLE; }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {}; uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; uav.Format = DXGI_FORMAT_UNKNOWN; uav.Buffer.NumElements = static_cast<UINT>(field_pixels); uav.Buffer.StructureByteStride = sizeof(float); device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, heap->GetCPUDescriptorHandleForHeapStart());
    uint32_t constants[36] {field_width, field_height, request->output_width, request->output_height, session->source_width, session->source_height}; std::memcpy(&constants[8], &request->latitude_span_degrees, sizeof(float)); std::memcpy(&constants[12], request->world_to_camera, 3 * sizeof(float)); std::memcpy(&constants[16], request->world_to_camera + 3, 3 * sizeof(float)); std::memcpy(&constants[20], request->world_to_camera + 6, 3 * sizeof(float)); const float camera[4] {static_cast<float>(session->source_width), static_cast<float>(session->source_height), session->source_width / (2.0F * std::tan(request->horizontal_fov_degrees * 0.00872664625997165F)), session->source_height / (2.0F * std::tan(request->vertical_fov_degrees * 0.00872664625997165F))}; std::memcpy(&constants[24], camera, sizeof(camera)); const float log_gain = std::log(global_gain); std::memcpy(&constants[28], &log_gain, sizeof(log_gain)); const float rectilinear_output = request->rectilinear_output != 0 ? 1.0F : 0.0F; std::memcpy(&constants[32], &rectilinear_output, sizeof(rectilinear_output)); std::memcpy(&constants[33], &request->output_vertical_fov_degrees, sizeof(request->output_vertical_fov_degrees)); ID3D12DescriptorHeap *heaps[] = {heap.Get()}; list->SetDescriptorHeaps(1, heaps); list->SetComputeRootSignature(root.Get()); list->SetComputeRoot32BitConstants(0, 36, constants, 0); list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart()); list->Dispatch((field_width + 7) / 8, (field_height + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER barrier {}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition.pResource = output.Get(); barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; list->ResourceBarrier(1, &barrier); list->CopyResource(readback.Get(), output.Get()); if (FAILED(list->Close())) { write_error(error_buffer, error_buffer_size, "cannot close D3D12 equirectangular local-exposure dispatch"); return PANO_GPU_UNAVAILABLE; } ID3D12CommandList *lists[] = {list.Get()}; session->device_core->queue->ExecuteCommandLists(1, lists); const uint64_t fence = session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1; if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence)) || wait_for_fence(session->device_core.get(), fence, error_buffer, error_buffer_size, "D3D12 equirectangular local-exposure fence timed out") != PANO_GPU_SUCCESS) return PANO_GPU_UNAVAILABLE; const D3D12_RANGE range {0, static_cast<SIZE_T>(field_bytes)}; void *mapped = nullptr; if (FAILED(readback->Map(0, &range, &mapped))) { write_error(error_buffer, error_buffer_size, "cannot map D3D12 equirectangular local-exposure readback"); return PANO_GPU_UNAVAILABLE; } std::memcpy(local_field, mapped, static_cast<size_t>(field_bytes)); readback->Unmap(0, nullptr); return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_dispatch_one_frame_rectilinear_local_exposure(
    const pano_gpu_session *const session, const pano_gpu_one_frame_composite_request *const request,
    const float global_gain, void *const local_field, const uint64_t local_field_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (request == nullptr || request->rectilinear_output == 0)
    { write_error(error_buffer, error_buffer_size, "invalid D3D12 rectilinear local-exposure request"); return PANO_GPU_INVALID_ARGUMENT; }
    return pano_gpu_test_dispatch_one_frame_equirect_local_exposure(
        session, request, global_gain, local_field, local_field_bytes, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_test_dispatch_feather_weights(
    const pano_gpu_session *const session, const pano_gpu_feather_accumulation_request *const request,
    const void *const candidate_validity, const uint64_t candidate_validity_bytes,
    const void *const candidate_edge_distance, const uint64_t candidate_edge_distance_bytes,
    void *const feather_weight, const uint64_t feather_weight_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    uint64_t weight_bytes = 0;
    if (session == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->pixel_count == 0 || request->source_width == 0 ||
        request->source_height == 0 || request->reserved != 0 || candidate_validity == nullptr ||
        candidate_edge_distance == nullptr || feather_weight == nullptr ||
        !checked_multiply(request->pixel_count, sizeof(float), &weight_bytes) ||
        candidate_validity_bytes != request->pixel_count || candidate_edge_distance_bytes != weight_bytes ||
        feather_weight_bytes != weight_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 feather-weight dispatch request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    const uint8_t *const validity = static_cast<const uint8_t *>(candidate_validity);
    const float *const edge_distances = static_cast<const float *>(candidate_edge_distance);
    for (uint32_t index = 0; index < request->pixel_count; ++index)
    {
        if (validity[index] > 1 || !std::isfinite(edge_distances[index]) || edge_distances[index] < 0.0F)
        {
            write_error(error_buffer, error_buffer_size, "invalid D3D12 feather-weight inputs");
            return PANO_GPU_INVALID_ARGUMENT;
        }
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue ||
        !session->device_core->fence)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 feather-weight session");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2},
    };
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 3};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC root_description {2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)))
    {
        write_error(error_buffer, error_buffer_size, "cannot serialize D3D12 feather-weight root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    if (FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root_signature))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 feather-weight root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root_signature.Get();
    pipeline_description.CS = {pano_gpu_feather_weight_shader, sizeof(pano_gpu_feather_weight_shader)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    const HRESULT pipeline_result = device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline));
    if (FAILED(pipeline_result))
    {
        write_device_error(error_buffer, error_buffer_size, "cannot create D3D12 feather-weight pipeline", pipeline_result, device);
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES upload_heap {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const auto make_upload = [&](const uint64_t bytes, const void *const contents,
                                 Microsoft::WRL::ComPtr<ID3D12Resource> *const resource) {
        D3D12_RESOURCE_DESC buffer {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(resource->ReleaseAndGetAddressOf()))))
            return false;
        void *mapped = nullptr;
        if (FAILED((*resource)->Map(0, nullptr, &mapped)))
            return false;
        std::memcpy(mapped, contents, static_cast<size_t>(bytes));
        (*resource)->Unmap(0, nullptr);
        return true;
    };
    std::vector<uint32_t> packed_validity;
    try
    {
        packed_validity.assign((request->pixel_count + 31) / 32, 0);
    }
    catch (const std::bad_alloc &)
    {
        write_error(error_buffer, error_buffer_size, "out of memory packing D3D12 feather-weight validity");
        return PANO_GPU_OUT_OF_MEMORY;
    }
    for (uint32_t index = 0; index < request->pixel_count; ++index)
    {
        if (validity[index] != 0)
            packed_validity[index / 32] |= 1U << (index & 31);
    }
    Microsoft::WRL::ComPtr<ID3D12Resource> validity_resource;
    Microsoft::WRL::ComPtr<ID3D12Resource> edge_resource;
    if (!make_upload(packed_validity.size() * sizeof(uint32_t), packed_validity.data(), &validity_resource) ||
        !make_upload(weight_bytes, candidate_edge_distance, &edge_resource))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 feather-weight input resources");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES default_heap {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC output_buffer {};
    output_buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    output_buffer.Width = weight_bytes;
    output_buffer.Height = 1;
    output_buffer.DepthOrArraySize = 1;
    output_buffer.MipLevels = 1;
    output_buffer.SampleDesc.Count = 1;
    output_buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    output_buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D12Resource> weight_resource;
    if (FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &output_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&weight_resource))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 feather-weight output resource");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readback_buffer = output_buffer;
    readback_buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_resource;
    if (FAILED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback_resource))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 feather-weight readback resource");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 3;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&descriptor_heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 feather-weight dispatch resources");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor = descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC validity_srv {};
    validity_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    validity_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    validity_srv.Format = DXGI_FORMAT_R32_TYPELESS;
    validity_srv.Buffer.NumElements = static_cast<UINT>(packed_validity.size());
    validity_srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(validity_resource.Get(), &validity_srv, descriptor);
    descriptor.ptr += descriptor_size;
    D3D12_SHADER_RESOURCE_VIEW_DESC edge_srv {};
    edge_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    edge_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    edge_srv.Format = DXGI_FORMAT_UNKNOWN;
    edge_srv.Buffer.NumElements = request->pixel_count;
    edge_srv.Buffer.StructureByteStride = sizeof(float);
    device->CreateShaderResourceView(edge_resource.Get(), &edge_srv, descriptor);
    descriptor.ptr += descriptor_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC weight_uav {};
    weight_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    weight_uav.Format = DXGI_FORMAT_UNKNOWN;
    weight_uav.Buffer.NumElements = request->pixel_count;
    weight_uav.Buffer.StructureByteStride = sizeof(float);
    device->CreateUnorderedAccessView(weight_resource.Get(), nullptr, &weight_uav, descriptor);
    ID3D12DescriptorHeap *heaps[] = {descriptor_heap.Get()};
    const uint32_t constants[] = {request->pixel_count, request->source_width, request->source_height};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_signature.Get());
    list->SetComputeRoot32BitConstants(0, 3, constants, 0);
    list->SetComputeRootDescriptorTable(1, descriptor_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((request->pixel_count + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = weight_resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
    list->CopyResource(readback_resource.Get(), weight_resource.Get());
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 feather-weight command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] = {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value = session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)))
    {
        write_error(error_buffer, error_buffer_size, "cannot signal D3D12 feather-weight fence");
        return PANO_GPU_UNAVAILABLE;
    }
    const pano_gpu_result wait_result = wait_for_fence(
        session->device_core.get(), fence_value, error_buffer, error_buffer_size, "D3D12 feather-weight fence timed out");
    if (wait_result != PANO_GPU_SUCCESS)
        return wait_result;
    const D3D12_RANGE range {0, static_cast<SIZE_T>(weight_bytes)};
    void *mapped = nullptr;
    if (FAILED(readback_resource->Map(0, &range, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 feather-weight readback");
        return PANO_GPU_UNAVAILABLE;
    }
    std::memcpy(feather_weight, mapped, static_cast<size_t>(weight_bytes));
    readback_resource->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_dispatch_feather_accumulation(
    const pano_gpu_session *const session, const pano_gpu_feather_accumulation_request *const request,
    const void *const candidate_rgb, const uint64_t candidate_rgb_bytes, const void *const candidate_weight,
    const uint64_t candidate_weight_bytes, const void *const accumulator_rgb,
    const uint64_t accumulator_rgb_bytes, const void *const accumulator_weight,
    const uint64_t accumulator_weight_bytes, void *const result_rgb, const uint64_t result_rgb_bytes,
    void *const result_weight, const uint64_t result_weight_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    uint64_t rgb_bytes = 0;
    uint64_t weight_bytes = 0;
    if (session == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->pixel_count == 0 || request->source_width == 0 ||
        request->source_height == 0 || request->reserved != 0 || candidate_rgb == nullptr ||
        candidate_weight == nullptr || accumulator_rgb == nullptr || accumulator_weight == nullptr ||
        result_rgb == nullptr || result_weight == nullptr ||
        !checked_multiply(request->pixel_count, 3 * sizeof(float), &rgb_bytes) ||
        !checked_multiply(request->pixel_count, sizeof(float), &weight_bytes) || candidate_rgb_bytes != rgb_bytes ||
        candidate_weight_bytes != weight_bytes || accumulator_rgb_bytes != rgb_bytes ||
        accumulator_weight_bytes != weight_bytes || result_rgb_bytes != rgb_bytes || result_weight_bytes != weight_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 feather-accumulation dispatch request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    const float *const candidate_weights = static_cast<const float *>(candidate_weight);
    const float *const accumulator_weights = static_cast<const float *>(accumulator_weight);
    for (uint32_t index = 0; index < request->pixel_count; ++index)
    {
        if (!std::isfinite(candidate_weights[index]) || candidate_weights[index] < 0.0F ||
            !std::isfinite(accumulator_weights[index]) || accumulator_weights[index] < 0.0F)
        {
            write_error(error_buffer, error_buffer_size, "invalid D3D12 feather-accumulation weights");
            return PANO_GPU_INVALID_ARGUMENT;
        }
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue ||
        !session->device_core->fence)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 feather-accumulation session");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 4},
    };
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 1};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC root_description {2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)))
    {
        write_error(error_buffer, error_buffer_size, "cannot serialize D3D12 feather-accumulation root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    if (FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root_signature))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 feather-accumulation root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root_signature.Get();
    pipeline_description.CS = {pano_gpu_feather_accumulate_shader, sizeof(pano_gpu_feather_accumulate_shader)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    const HRESULT pipeline_result = device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline));
    if (FAILED(pipeline_result))
    {
        write_device_error(
            error_buffer, error_buffer_size, "cannot create D3D12 feather-accumulation pipeline", pipeline_result, device);
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES upload_heap {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const auto make_upload = [&](const uint64_t bytes, const void *const contents,
                                 Microsoft::WRL::ComPtr<ID3D12Resource> *const resource) {
        D3D12_RESOURCE_DESC buffer {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(resource->ReleaseAndGetAddressOf()))))
            return false;
        void *mapped = nullptr;
        if (FAILED((*resource)->Map(0, nullptr, &mapped)))
            return false;
        std::memcpy(mapped, contents, static_cast<size_t>(bytes));
        (*resource)->Unmap(0, nullptr);
        return true;
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> candidate_rgb_resource;
    Microsoft::WRL::ComPtr<ID3D12Resource> candidate_weight_resource;
    Microsoft::WRL::ComPtr<ID3D12Resource> accumulator_rgb_resource;
    Microsoft::WRL::ComPtr<ID3D12Resource> accumulator_weight_resource;
    if (!make_upload(rgb_bytes, candidate_rgb, &candidate_rgb_resource) ||
        !make_upload(weight_bytes, candidate_weight, &candidate_weight_resource) ||
        !make_upload(rgb_bytes, accumulator_rgb, &accumulator_rgb_resource) ||
        !make_upload(weight_bytes, accumulator_weight, &accumulator_weight_resource))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 feather-accumulation input resources");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES default_heap {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const auto make_output = [&](const uint64_t bytes, Microsoft::WRL::ComPtr<ID3D12Resource> *const resource) {
        D3D12_RESOURCE_DESC buffer {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(resource->ReleaseAndGetAddressOf())));
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> result_rgb_resource;
    Microsoft::WRL::ComPtr<ID3D12Resource> result_weight_resource;
    if (!make_output(rgb_bytes, &result_rgb_resource) || !make_output(weight_bytes, &result_weight_resource))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 feather-accumulation output resources");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    const auto make_readback = [&](const uint64_t bytes, Microsoft::WRL::ComPtr<ID3D12Resource> *const resource) {
        D3D12_RESOURCE_DESC buffer {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(resource->ReleaseAndGetAddressOf())));
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> result_rgb_readback;
    Microsoft::WRL::ComPtr<ID3D12Resource> result_weight_readback;
    if (!make_readback(rgb_bytes, &result_rgb_readback) || !make_readback(weight_bytes, &result_weight_readback))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 feather-accumulation readback resources");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 6;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&descriptor_heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 feather-accumulation dispatch resources");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor = descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    const auto create_structured_srv = [&](ID3D12Resource *const resource, const UINT stride) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view {};
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.Buffer.NumElements = request->pixel_count;
        view.Buffer.StructureByteStride = stride;
        device->CreateShaderResourceView(resource, &view, descriptor);
        descriptor.ptr += descriptor_size;
    };
    create_structured_srv(candidate_rgb_resource.Get(), 3 * sizeof(float));
    create_structured_srv(candidate_weight_resource.Get(), sizeof(float));
    create_structured_srv(accumulator_rgb_resource.Get(), 3 * sizeof(float));
    create_structured_srv(accumulator_weight_resource.Get(), sizeof(float));
    D3D12_UNORDERED_ACCESS_VIEW_DESC rgb_uav {};
    rgb_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    rgb_uav.Format = DXGI_FORMAT_UNKNOWN;
    rgb_uav.Buffer.NumElements = request->pixel_count;
    rgb_uav.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateUnorderedAccessView(result_rgb_resource.Get(), nullptr, &rgb_uav, descriptor);
    descriptor.ptr += descriptor_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC weight_uav = rgb_uav;
    weight_uav.Buffer.StructureByteStride = sizeof(float);
    device->CreateUnorderedAccessView(result_weight_resource.Get(), nullptr, &weight_uav, descriptor);
    ID3D12DescriptorHeap *heaps[] = {descriptor_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_signature.Get());
    list->SetComputeRoot32BitConstants(0, 1, &request->pixel_count, 0);
    list->SetComputeRootDescriptorTable(1, descriptor_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((request->pixel_count + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER barriers[2] {};
    ID3D12Resource *outputs[] = {result_rgb_resource.Get(), result_weight_resource.Get()};
    for (size_t index = 0; index < 2; ++index)
    {
        barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[index].Transition.pResource = outputs[index];
        barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[index].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    list->ResourceBarrier(2, barriers);
    list->CopyResource(result_rgb_readback.Get(), result_rgb_resource.Get());
    list->CopyResource(result_weight_readback.Get(), result_weight_resource.Get());
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 feather-accumulation command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] = {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value = session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)))
    {
        write_error(error_buffer, error_buffer_size, "cannot signal D3D12 feather-accumulation fence");
        return PANO_GPU_UNAVAILABLE;
    }
    const pano_gpu_result wait_result = wait_for_fence(
        session->device_core.get(), fence_value, error_buffer, error_buffer_size,
        "D3D12 feather-accumulation fence timed out");
    if (wait_result != PANO_GPU_SUCCESS)
        return wait_result;
    const auto copy_readback = [&](ID3D12Resource *const resource, const uint64_t bytes, void *const destination) {
        const D3D12_RANGE range {0, static_cast<SIZE_T>(bytes)};
        void *mapped = nullptr;
        if (FAILED(resource->Map(0, &range, &mapped)))
            return false;
        std::memcpy(destination, mapped, static_cast<size_t>(bytes));
        resource->Unmap(0, nullptr);
        return true;
    };
    if (!copy_readback(result_rgb_readback.Get(), rgb_bytes, result_rgb) ||
        !copy_readback(result_weight_readback.Get(), weight_bytes, result_weight))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 feather-accumulation readback");
        return PANO_GPU_UNAVAILABLE;
    }
    return PANO_GPU_SUCCESS;
#endif
}

static pano_gpu_result dispatch_feather_accumulation_chain(
    pano_gpu_output *const output, const pano_gpu_session *const session,
    const pano_gpu_feather_accumulation_request *const request,
    const void *const *const candidate_rgb, const uint64_t *const candidate_rgb_bytes,
    const void *const *const candidate_weight, const uint64_t *const candidate_weight_bytes,
    const uint32_t frame_count,
    void *const result_rgb, const uint64_t result_rgb_bytes, void *const result_weight,
    const uint64_t result_weight_bytes, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    uint64_t rgb_bytes = 0;
    uint64_t weight_bytes = 0;
    if (session == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->pixel_count == 0 || request->source_width == 0 ||
        request->source_height == 0 || request->reserved != 0 || candidate_rgb == nullptr ||
        candidate_rgb_bytes == nullptr || candidate_weight == nullptr || candidate_weight_bytes == nullptr ||
        frame_count == 0 ||
        !checked_multiply(request->pixel_count, 3 * sizeof(float), &rgb_bytes) ||
        !checked_multiply(request->pixel_count, sizeof(float), &weight_bytes) ||
        (output == nullptr &&
         (result_rgb == nullptr || result_weight == nullptr || result_rgb_bytes != rgb_bytes ||
          result_weight_bytes != weight_bytes)))
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 two-frame feather-accumulation request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    for (uint32_t frame = 0; frame < frame_count; ++frame)
    {
        if (candidate_rgb[frame] == nullptr || candidate_weight[frame] == nullptr ||
            candidate_rgb_bytes[frame] != rgb_bytes || candidate_weight_bytes[frame] != weight_bytes)
        {
            write_error(error_buffer, error_buffer_size, "invalid D3D12 feather-accumulation input layout");
            return PANO_GPU_INVALID_ARGUMENT;
        }
        const float *const weights = static_cast<const float *>(candidate_weight[frame]);
        for (uint32_t index = 0; index < request->pixel_count; ++index)
            if (!std::isfinite(weights[index]) || weights[index] < 0.0F)
            {
                write_error(error_buffer, error_buffer_size, "invalid D3D12 feather-accumulation weights");
                return PANO_GPU_INVALID_ARGUMENT;
            }
    }
    if (output != nullptr)
    {
        const uint32_t storage_rows = output->output_band_rows == 0 ? output->output_height : output->band_row_count;
        if (output->session != session || output->output_width != request->source_width ||
            storage_rows != request->source_height || output->linear_bytes < rgb_bytes ||
            output->coverage_bytes < request->pixel_count)
        {
            write_error(error_buffer, error_buffer_size, "invalid D3D12 feather output layout");
            return PANO_GPU_INVALID_ARGUMENT;
        }
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue ||
        !session->device_core->fence)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 two-frame feather-accumulation session");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const auto buffer_desc = [](const uint64_t bytes, const D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC buffer {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer.Flags = flags;
        return buffer;
    };
    D3D12_HEAP_PROPERTIES upload_heap {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const auto make_upload = [&](const uint64_t bytes, const void *const contents,
                                 Microsoft::WRL::ComPtr<ID3D12Resource> *const resource) {
        const D3D12_RESOURCE_DESC buffer = buffer_desc(bytes, D3D12_RESOURCE_FLAG_NONE);
        if (FAILED(device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(resource->ReleaseAndGetAddressOf()))))
            return false;
        void *mapped = nullptr;
        if (FAILED((*resource)->Map(0, nullptr, &mapped)))
            return false;
        if (contents == nullptr)
            std::memset(mapped, 0, static_cast<size_t>(bytes));
        else
            std::memcpy(mapped, contents, static_cast<size_t>(bytes));
        (*resource)->Unmap(0, nullptr);
        return true;
    };
    using resource_array = std::unique_ptr<Microsoft::WRL::ComPtr<ID3D12Resource>[]>;
    resource_array candidate_rgb_resources {new (std::nothrow)
                                                Microsoft::WRL::ComPtr<ID3D12Resource>[frame_count]};
    resource_array candidate_weight_resources {new (std::nothrow)
                                                   Microsoft::WRL::ComPtr<ID3D12Resource>[frame_count]};
    if (!candidate_rgb_resources || !candidate_weight_resources)
    {
        write_error(error_buffer, error_buffer_size, "cannot allocate feather-accumulation resource owners");
        return PANO_GPU_OUT_OF_MEMORY;
    }
    Microsoft::WRL::ComPtr<ID3D12Resource> zero_rgb;
    Microsoft::WRL::ComPtr<ID3D12Resource> zero_weight;
    for (uint32_t frame = 0; frame < frame_count; ++frame)
        if (!make_upload(rgb_bytes, candidate_rgb[frame], &candidate_rgb_resources[frame]) ||
            !make_upload(weight_bytes, candidate_weight[frame], &candidate_weight_resources[frame]))
        {
            write_error(error_buffer, error_buffer_size, "cannot create D3D12 feather-accumulation inputs");
            return PANO_GPU_UNAVAILABLE;
        }
    if (!make_upload(rgb_bytes, nullptr, &zero_rgb) || !make_upload(weight_bytes, nullptr, &zero_weight))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 two-frame feather-accumulation inputs");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES default_heap {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const auto make_output = [&](const uint64_t bytes, Microsoft::WRL::ComPtr<ID3D12Resource> *const resource) {
        const D3D12_RESOURCE_DESC buffer = buffer_desc(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        return SUCCEEDED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(resource->ReleaseAndGetAddressOf())));
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> accumulator_rgb[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> accumulator_weight[2];
    if (!make_output(rgb_bytes, &accumulator_rgb[0]) || !make_output(weight_bytes, &accumulator_weight[0]) ||
        !make_output(rgb_bytes, &accumulator_rgb[1]) || !make_output(weight_bytes, &accumulator_weight[1]))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 two-frame feather-accumulation outputs");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    const auto make_readback = [&](const uint64_t bytes, Microsoft::WRL::ComPtr<ID3D12Resource> *const resource) {
        const D3D12_RESOURCE_DESC buffer = buffer_desc(bytes, D3D12_RESOURCE_FLAG_NONE);
        return SUCCEEDED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(resource->ReleaseAndGetAddressOf())));
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> rgb_readback;
    Microsoft::WRL::ComPtr<ID3D12Resource> weight_readback;
    if (output == nullptr &&
        (!make_readback(rgb_bytes, &rgb_readback) || !make_readback(weight_bytes, &weight_readback)))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 two-frame feather-accumulation readbacks");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 4},
    };
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 1};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC root_description {2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)))
    {
        write_error(error_buffer, error_buffer_size, "cannot serialize D3D12 two-frame feather root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    if (FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root_signature))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 two-frame feather root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root_signature.Get();
    pipeline_description.CS = {pano_gpu_feather_accumulate_shader, sizeof(pano_gpu_feather_accumulate_shader)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    const HRESULT pipeline_result = device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline));
    if (FAILED(pipeline_result))
    {
        write_device_error(error_buffer, error_buffer_size, "cannot create D3D12 two-frame feather pipeline", pipeline_result, device);
        return PANO_GPU_UNAVAILABLE;
    }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> normalize_root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> normalize_pipeline;
    if (output != nullptr)
    {
        if (!output->linear || !output->coverage)
        {
            write_error(error_buffer, error_buffer_size, "D3D12 feather output storage is not allocated");
            return PANO_GPU_INVALID_ARGUMENT;
        }
        D3D12_DESCRIPTOR_RANGE normalize_ranges[2] {
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 2},
        };
        D3D12_ROOT_PARAMETER normalize_parameters[2] {};
        normalize_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        normalize_parameters[0].Constants = {0, 0, 1};
        normalize_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        normalize_parameters[1].DescriptorTable = {2, normalize_ranges};
        const D3D12_ROOT_SIGNATURE_DESC normalize_description {
            2, normalize_parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        Microsoft::WRL::ComPtr<ID3DBlob> normalize_serialized;
        if (FAILED(D3D12SerializeRootSignature(
                &normalize_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &normalize_serialized, &errors)) ||
            FAILED(device->CreateRootSignature(
                0, normalize_serialized->GetBufferPointer(), normalize_serialized->GetBufferSize(),
                IID_PPV_ARGS(&normalize_root))))
        {
            write_error(error_buffer, error_buffer_size, "cannot create D3D12 output feather-normalization root signature");
            return PANO_GPU_UNAVAILABLE;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC normalize_pipeline_description {};
        normalize_pipeline_description.pRootSignature = normalize_root.Get();
        normalize_pipeline_description.CS = {
            pano_gpu_feather_normalize_output_shader, sizeof(pano_gpu_feather_normalize_output_shader)};
        if (FAILED(device->CreateComputePipelineState(
                &normalize_pipeline_description, IID_PPV_ARGS(&normalize_pipeline))))
        {
            write_error(error_buffer, error_buffer_size, "cannot create D3D12 output feather-normalization pipeline");
            return PANO_GPU_UNAVAILABLE;
        }
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = frame_count * 6 + (output == nullptr ? 0 : 4);
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&descriptor_heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 two-frame feather dispatch resources");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto cpu = [&](const UINT slot) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptor_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(slot) * increment;
        return handle;
    };
    const auto gpu = [&](const UINT slot) {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = descriptor_heap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(slot) * increment;
        return handle;
    };
    const auto create_structured_srv = [&](ID3D12Resource *const resource, const UINT stride, const UINT slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view {};
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.Buffer.NumElements = request->pixel_count;
        view.Buffer.StructureByteStride = stride;
        device->CreateShaderResourceView(resource, &view, cpu(slot));
    };
    D3D12_UNORDERED_ACCESS_VIEW_DESC rgb_uav {};
    rgb_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    rgb_uav.Format = DXGI_FORMAT_UNKNOWN;
    rgb_uav.Buffer.NumElements = request->pixel_count;
    rgb_uav.Buffer.StructureByteStride = 3 * sizeof(float);
    D3D12_UNORDERED_ACCESS_VIEW_DESC weight_uav = rgb_uav;
    weight_uav.Buffer.StructureByteStride = sizeof(float);
    const auto bind_pass = [&](const UINT slot, ID3D12Resource *const candidate_rgb_resource,
                               ID3D12Resource *const candidate_weight_resource,
                               ID3D12Resource *const accumulator_rgb_resource,
                               ID3D12Resource *const accumulator_weight_resource,
                               ID3D12Resource *const result_rgb_resource,
                               ID3D12Resource *const result_weight_resource) {
        create_structured_srv(candidate_rgb_resource, 3 * sizeof(float), slot);
        create_structured_srv(candidate_weight_resource, sizeof(float), slot + 1);
        create_structured_srv(accumulator_rgb_resource, 3 * sizeof(float), slot + 2);
        create_structured_srv(accumulator_weight_resource, sizeof(float), slot + 3);
        device->CreateUnorderedAccessView(result_rgb_resource, nullptr, &rgb_uav, cpu(slot + 4));
        device->CreateUnorderedAccessView(result_weight_resource, nullptr, &weight_uav, cpu(slot + 5));
    };
    for (uint32_t frame = 0; frame < frame_count; ++frame)
        bind_pass(
            frame * 6, candidate_rgb_resources[frame].Get(), candidate_weight_resources[frame].Get(),
            frame == 0 ? zero_rgb.Get() : accumulator_rgb[(frame + 1) % 2].Get(),
            frame == 0 ? zero_weight.Get() : accumulator_weight[(frame + 1) % 2].Get(),
            accumulator_rgb[frame % 2].Get(), accumulator_weight[frame % 2].Get());
    const UINT normalize_slot = frame_count * 6;
    if (output != nullptr)
    {
        const UINT final_accumulator = (frame_count - 1) % 2;
        create_structured_srv(accumulator_rgb[final_accumulator].Get(), 3 * sizeof(float), normalize_slot);
        create_structured_srv(accumulator_weight[final_accumulator].Get(), sizeof(float), normalize_slot + 1);
        device->CreateUnorderedAccessView(output->linear.Get(), nullptr, &rgb_uav, cpu(normalize_slot + 2));
        D3D12_UNORDERED_ACCESS_VIEW_DESC coverage_uav {};
        coverage_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        coverage_uav.Format = DXGI_FORMAT_R8_UINT;
        coverage_uav.Buffer.NumElements = request->pixel_count;
        device->CreateUnorderedAccessView(output->coverage.Get(), nullptr, &coverage_uav, cpu(normalize_slot + 3));
    }
    ID3D12DescriptorHeap *heaps[] = {descriptor_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_signature.Get());
    list->SetComputeRoot32BitConstants(0, 1, &request->pixel_count, 0);
    for (uint32_t frame = 0; frame < frame_count; ++frame)
    {
        if (frame >= 2)
        {
            D3D12_RESOURCE_BARRIER reuse_barriers[2] {};
            ID3D12Resource *reuse_resources[] = {
                accumulator_rgb[frame % 2].Get(), accumulator_weight[frame % 2].Get()};
            for (UINT index = 0; index < 2; ++index)
            {
                reuse_barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                reuse_barriers[index].Transition.pResource = reuse_resources[index];
                reuse_barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                reuse_barriers[index].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                reuse_barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            list->ResourceBarrier(2, reuse_barriers);
        }
        list->SetComputeRootDescriptorTable(1, gpu(frame * 6));
        list->Dispatch((request->pixel_count + 63) / 64, 1, 1);
        if (frame + 1 < frame_count)
        {
            D3D12_RESOURCE_BARRIER intermediate_barriers[4] {};
            ID3D12Resource *intermediate_resources[] = {
                accumulator_rgb[frame % 2].Get(), accumulator_weight[frame % 2].Get()};
            for (UINT index = 0; index < 2; ++index)
            {
                intermediate_barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                intermediate_barriers[index].UAV.pResource = intermediate_resources[index];
                intermediate_barriers[index + 2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                intermediate_barriers[index + 2].Transition.pResource = intermediate_resources[index];
                intermediate_barriers[index + 2].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                intermediate_barriers[index + 2].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                intermediate_barriers[index + 2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            list->ResourceBarrier(4, intermediate_barriers);
        }
    }
    ID3D12Resource *final_resources[] = {
        accumulator_rgb[(frame_count - 1) % 2].Get(), accumulator_weight[(frame_count - 1) % 2].Get()};
    D3D12_RESOURCE_BARRIER final_barriers[4] {};
    for (UINT index = 0; index < 2; ++index)
    {
        final_barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        final_barriers[index].UAV.pResource = final_resources[index];
        final_barriers[index + 2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        final_barriers[index + 2].Transition.pResource = final_resources[index];
        final_barriers[index + 2].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        final_barriers[index + 2].Transition.StateAfter = output == nullptr
            ? D3D12_RESOURCE_STATE_COPY_SOURCE
            : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        final_barriers[index + 2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    list->ResourceBarrier(4, final_barriers);
    if (output == nullptr)
    {
        list->CopyResource(rgb_readback.Get(), final_resources[0]);
        list->CopyResource(weight_readback.Get(), final_resources[1]);
    }
    else
    {
        D3D12_RESOURCE_BARRIER output_to_uav[2] {};
        ID3D12Resource *output_resources[] = {output->linear.Get(), output->coverage.Get()};
        for (UINT index = 0; index < 2; ++index)
        {
            output_to_uav[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            output_to_uav[index].Transition.pResource = output_resources[index];
            output_to_uav[index].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            output_to_uav[index].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            output_to_uav[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        list->ResourceBarrier(2, output_to_uav);
        list->SetPipelineState(normalize_pipeline.Get());
        list->SetComputeRootSignature(normalize_root.Get());
        list->SetComputeRoot32BitConstants(0, 1, &request->pixel_count, 0);
        list->SetComputeRootDescriptorTable(1, gpu(normalize_slot));
        list->Dispatch((request->pixel_count + 63) / 64, 1, 1);
        D3D12_RESOURCE_BARRIER output_ordering[2] {};
        for (UINT index = 0; index < 2; ++index)
        {
            output_ordering[index].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            output_ordering[index].UAV.pResource = output_resources[index];
        }
        list->ResourceBarrier(2, output_ordering);
        for (UINT index = 0; index < 2; ++index)
            std::swap(
                output_to_uav[index].Transition.StateBefore,
                output_to_uav[index].Transition.StateAfter);
        list->ResourceBarrier(2, output_to_uav);
    }
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 two-frame feather command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] = {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value = session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)))
    {
        write_error(error_buffer, error_buffer_size, "cannot signal D3D12 two-frame feather fence");
        return PANO_GPU_UNAVAILABLE;
    }
    const pano_gpu_result wait_result = wait_for_fence(
        session->device_core.get(), fence_value, error_buffer, error_buffer_size, "D3D12 two-frame feather fence timed out");
    if (wait_result != PANO_GPU_SUCCESS)
        return wait_result;
    if (output != nullptr)
        return PANO_GPU_SUCCESS;
    const auto copy_readback = [&](ID3D12Resource *const resource, const uint64_t bytes, void *const destination) {
        const D3D12_RANGE range {0, static_cast<SIZE_T>(bytes)};
        void *mapped = nullptr;
        if (FAILED(resource->Map(0, &range, &mapped)))
            return false;
        std::memcpy(destination, mapped, static_cast<size_t>(bytes));
        resource->Unmap(0, nullptr);
        return true;
    };
    if (!copy_readback(rgb_readback.Get(), rgb_bytes, result_rgb) ||
        !copy_readback(weight_readback.Get(), weight_bytes, result_weight))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 two-frame feather readback");
        return PANO_GPU_UNAVAILABLE;
    }
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_dispatch_feather_accumulation_chain(
    const pano_gpu_session *const session, const pano_gpu_feather_accumulation_request *const request,
    const void *const *const candidate_rgb, const uint64_t *const candidate_rgb_bytes,
    const void *const *const candidate_weight, const uint64_t *const candidate_weight_bytes,
    const uint32_t frame_count, void *const result_rgb, const uint64_t result_rgb_bytes,
    void *const result_weight, const uint64_t result_weight_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    return dispatch_feather_accumulation_chain(
        nullptr, session, request, candidate_rgb, candidate_rgb_bytes, candidate_weight, candidate_weight_bytes,
        frame_count, result_rgb, result_rgb_bytes, result_weight, result_weight_bytes, error_buffer,
        error_buffer_size);
}

pano_gpu_result pano_gpu_test_dispatch_output_feather_accumulation(
    pano_gpu_output *const output, const pano_gpu_feather_accumulation_request *const request,
    const void *const *const candidate_rgb, const uint64_t *const candidate_rgb_bytes,
    const void *const *const candidate_weight, const uint64_t *const candidate_weight_bytes,
    const uint32_t frame_count, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (output == nullptr || output->session == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 feather output handle");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return dispatch_feather_accumulation_chain(
        output, output->session, request, candidate_rgb, candidate_rgb_bytes, candidate_weight,
        candidate_weight_bytes, frame_count, nullptr, 0, nullptr, 0, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_test_dispatch_two_frame_feather_accumulation(
    const pano_gpu_session *const session, const pano_gpu_feather_accumulation_request *const request,
    const void *const first_candidate_rgb, const uint64_t first_candidate_rgb_bytes,
    const void *const first_candidate_weight, const uint64_t first_candidate_weight_bytes,
    const void *const second_candidate_rgb, const uint64_t second_candidate_rgb_bytes,
    const void *const second_candidate_weight, const uint64_t second_candidate_weight_bytes,
    void *const result_rgb, const uint64_t result_rgb_bytes, void *const result_weight,
    const uint64_t result_weight_bytes, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    const void *const rgb[] = {first_candidate_rgb, second_candidate_rgb};
    const uint64_t rgb_bytes[] = {first_candidate_rgb_bytes, second_candidate_rgb_bytes};
    const void *const weight[] = {first_candidate_weight, second_candidate_weight};
    const uint64_t weight_bytes[] = {first_candidate_weight_bytes, second_candidate_weight_bytes};
    return dispatch_feather_accumulation_chain(
        nullptr, session, request, rgb, rgb_bytes, weight, weight_bytes, 2, result_rgb, result_rgb_bytes, result_weight,
        result_weight_bytes, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_test_dispatch_three_frame_feather_accumulation(
    const pano_gpu_session *const session, const pano_gpu_feather_accumulation_request *const request,
    const void *const first_candidate_rgb, const uint64_t first_candidate_rgb_bytes,
    const void *const first_candidate_weight, const uint64_t first_candidate_weight_bytes,
    const void *const second_candidate_rgb, const uint64_t second_candidate_rgb_bytes,
    const void *const second_candidate_weight, const uint64_t second_candidate_weight_bytes,
    const void *const third_candidate_rgb, const uint64_t third_candidate_rgb_bytes,
    const void *const third_candidate_weight, const uint64_t third_candidate_weight_bytes,
    void *const result_rgb, const uint64_t result_rgb_bytes, void *const result_weight,
    const uint64_t result_weight_bytes, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    const void *const rgb[] = {first_candidate_rgb, second_candidate_rgb, third_candidate_rgb};
    const uint64_t rgb_bytes[] = {first_candidate_rgb_bytes, second_candidate_rgb_bytes, third_candidate_rgb_bytes};
    const void *const weight[] = {first_candidate_weight, second_candidate_weight, third_candidate_weight};
    const uint64_t weight_bytes[] = {
        first_candidate_weight_bytes, second_candidate_weight_bytes, third_candidate_weight_bytes};
    return dispatch_feather_accumulation_chain(
        nullptr, session, request, rgb, rgb_bytes, weight, weight_bytes, 3, result_rgb, result_rgb_bytes, result_weight,
        result_weight_bytes, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_test_dispatch_feather_normalize(
    const pano_gpu_session *const session, const pano_gpu_feather_accumulation_request *const request,
    const void *const accumulator_rgb, const uint64_t accumulator_rgb_bytes, const void *const accumulator_weight,
    const uint64_t accumulator_weight_bytes, void *const normalized_rgb, const uint64_t normalized_rgb_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    uint64_t rgb_bytes = 0;
    uint64_t weight_bytes = 0;
    if (session == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->pixel_count == 0 || request->source_width == 0 ||
        request->source_height == 0 || request->reserved != 0 || accumulator_rgb == nullptr ||
        accumulator_weight == nullptr || normalized_rgb == nullptr ||
        !checked_multiply(request->pixel_count, 3 * sizeof(float), &rgb_bytes) ||
        !checked_multiply(request->pixel_count, sizeof(float), &weight_bytes) || accumulator_rgb_bytes != rgb_bytes ||
        accumulator_weight_bytes != weight_bytes || normalized_rgb_bytes != rgb_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 feather-normalization request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    const float *const weights = static_cast<const float *>(accumulator_weight);
    for (uint32_t index = 0; index < request->pixel_count; ++index)
        if (!std::isfinite(weights[index]) || weights[index] < 0.0F)
        {
            write_error(error_buffer, error_buffer_size, "invalid D3D12 feather-normalization weights");
            return PANO_GPU_INVALID_ARGUMENT;
        }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue ||
        !session->device_core->fence)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 feather-normalization session");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const auto buffer_desc = [](const uint64_t bytes, const D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC buffer {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer.Flags = flags;
        return buffer;
    };
    D3D12_HEAP_PROPERTIES upload_heap {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const auto make_upload = [&](const uint64_t bytes, const void *const contents,
                                 Microsoft::WRL::ComPtr<ID3D12Resource> *const resource) {
        const D3D12_RESOURCE_DESC buffer = buffer_desc(bytes, D3D12_RESOURCE_FLAG_NONE);
        if (FAILED(device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(resource->ReleaseAndGetAddressOf()))))
            return false;
        void *mapped = nullptr;
        if (FAILED((*resource)->Map(0, nullptr, &mapped)))
            return false;
        std::memcpy(mapped, contents, static_cast<size_t>(bytes));
        (*resource)->Unmap(0, nullptr);
        return true;
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> rgb_resource;
    Microsoft::WRL::ComPtr<ID3D12Resource> weight_resource;
    if (!make_upload(rgb_bytes, accumulator_rgb, &rgb_resource) ||
        !make_upload(weight_bytes, accumulator_weight, &weight_resource))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 feather-normalization inputs");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES default_heap {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const D3D12_RESOURCE_DESC output_desc = buffer_desc(rgb_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    Microsoft::WRL::ComPtr<ID3D12Resource> output_resource;
    if (FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &output_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&output_resource))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 feather-normalization output");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    const D3D12_RESOURCE_DESC readback_desc = buffer_desc(rgb_bytes, D3D12_RESOURCE_FLAG_NONE);
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_resource;
    if (FAILED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback_resource))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 feather-normalization readback");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2},
    };
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 1};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC root_description {2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)))
    {
        write_error(error_buffer, error_buffer_size, "cannot serialize D3D12 feather-normalization root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    if (FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root_signature))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 feather-normalization root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root_signature.Get();
    pipeline_description.CS = {pano_gpu_feather_normalize_shader, sizeof(pano_gpu_feather_normalize_shader)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    const HRESULT pipeline_result = device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline));
    if (FAILED(pipeline_result))
    {
        write_device_error(error_buffer, error_buffer_size, "cannot create D3D12 feather-normalization pipeline", pipeline_result, device);
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 3;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&descriptor_heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 feather-normalization dispatch resources");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor = descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    const auto create_srv = [&](ID3D12Resource *const resource, const UINT stride) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view {};
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.Buffer.NumElements = request->pixel_count;
        view.Buffer.StructureByteStride = stride;
        device->CreateShaderResourceView(resource, &view, descriptor);
        descriptor.ptr += increment;
    };
    create_srv(rgb_resource.Get(), 3 * sizeof(float));
    create_srv(weight_resource.Get(), sizeof(float));
    D3D12_UNORDERED_ACCESS_VIEW_DESC output_uav {};
    output_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    output_uav.Format = DXGI_FORMAT_UNKNOWN;
    output_uav.Buffer.NumElements = request->pixel_count;
    output_uav.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateUnorderedAccessView(output_resource.Get(), nullptr, &output_uav, descriptor);
    ID3D12DescriptorHeap *heaps[] = {descriptor_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_signature.Get());
    list->SetComputeRoot32BitConstants(0, 1, &request->pixel_count, 0);
    list->SetComputeRootDescriptorTable(1, descriptor_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((request->pixel_count + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = output_resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
    list->CopyResource(readback_resource.Get(), output_resource.Get());
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 feather-normalization command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] = {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value = session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)))
    {
        write_error(error_buffer, error_buffer_size, "cannot signal D3D12 feather-normalization fence");
        return PANO_GPU_UNAVAILABLE;
    }
    const pano_gpu_result wait_result = wait_for_fence(
        session->device_core.get(), fence_value, error_buffer, error_buffer_size, "D3D12 feather-normalization fence timed out");
    if (wait_result != PANO_GPU_SUCCESS)
        return wait_result;
    const D3D12_RANGE range {0, static_cast<SIZE_T>(rgb_bytes)};
    void *mapped = nullptr;
    if (FAILED(readback_resource->Map(0, &range, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 feather-normalization readback");
        return PANO_GPU_UNAVAILABLE;
    }
    std::memcpy(normalized_rgb, mapped, static_cast<size_t>(rgb_bytes));
    readback_resource->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_dispatch_global_gain(
    const pano_gpu_session *const session, const uint32_t pixel_count, const float global_gain,
    const void *const candidate_rgb, const uint64_t candidate_rgb_bytes, void *const adjusted_rgb,
    const uint64_t adjusted_rgb_bytes, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    uint64_t rgb_bytes = 0;
    if (session == nullptr || pixel_count == 0 || !std::isfinite(global_gain) || global_gain <= 0.0F ||
        candidate_rgb == nullptr || adjusted_rgb == nullptr ||
        !checked_multiply(pixel_count, 3 * sizeof(float), &rgb_bytes) || candidate_rgb_bytes != rgb_bytes ||
        adjusted_rgb_bytes != rgb_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 global-gain request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue || !session->device_core->fence)
    { write_error(error_buffer, error_buffer_size, "invalid D3D12 global-gain session"); return PANO_GPU_INVALID_ARGUMENT; }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const auto desc = [](uint64_t bytes, D3D12_RESOURCE_FLAGS flags) { D3D12_RESOURCE_DESC value {}; value.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; value.Width = bytes; value.Height = 1; value.DepthOrArraySize = 1; value.MipLevels = 1; value.SampleDesc.Count = 1; value.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; value.Flags = flags; return value; };
    D3D12_HEAP_PROPERTIES upload_heap {}; upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    Microsoft::WRL::ComPtr<ID3D12Resource> input;
    const D3D12_RESOURCE_DESC input_desc = desc(rgb_bytes, D3D12_RESOURCE_FLAG_NONE);
    if (FAILED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &input_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&input))))
    { write_error(error_buffer, error_buffer_size, "cannot create D3D12 global-gain input"); return PANO_GPU_UNAVAILABLE; }
    void *mapped = nullptr;
    if (FAILED(input->Map(0, nullptr, &mapped))) { write_error(error_buffer, error_buffer_size, "cannot map D3D12 global-gain input"); return PANO_GPU_UNAVAILABLE; }
    std::memcpy(mapped, candidate_rgb, static_cast<size_t>(rgb_bytes)); input->Unmap(0, nullptr);
    D3D12_HEAP_PROPERTIES default_heap {}; default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> output;
    const D3D12_RESOURCE_DESC output_desc = desc(rgb_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &output_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output))))
    { write_error(error_buffer, error_buffer_size, "cannot create D3D12 global-gain output"); return PANO_GPU_UNAVAILABLE; }
    D3D12_HEAP_PROPERTIES readback_heap {}; readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &input_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
    { write_error(error_buffer, error_buffer_size, "cannot create D3D12 global-gain readback"); return PANO_GPU_UNAVAILABLE; }
    D3D12_DESCRIPTOR_RANGE ranges[2] {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER parameters[2] {}; parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[0].Constants = {0, 0, 2}; parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC root_desc {2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE}; Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors))) { write_error(error_buffer, error_buffer_size, "cannot serialize D3D12 global-gain root signature"); return PANO_GPU_UNAVAILABLE; }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)))) { write_error(error_buffer, error_buffer_size, "cannot create D3D12 global-gain root signature"); return PANO_GPU_UNAVAILABLE; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc {}; pipeline_desc.pRootSignature = root.Get(); pipeline_desc.CS = {pano_gpu_global_gain_shader, sizeof(pano_gpu_global_gain_shader)}; Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)))) { write_error(error_buffer, error_buffer_size, "cannot create D3D12 global-gain pipeline"); return PANO_GPU_UNAVAILABLE; }
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc {}; heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap_desc.NumDescriptors = 2; heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap; Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator; Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap))) || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) || FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list)))) { write_error(error_buffer, error_buffer_size, "cannot create D3D12 global-gain dispatch resources"); return PANO_GPU_UNAVAILABLE; }
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv {}; srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Format = DXGI_FORMAT_UNKNOWN; srv.Buffer.NumElements = pixel_count; srv.Buffer.StructureByteStride = 3 * sizeof(float); device->CreateShaderResourceView(input.Get(), &srv, handle); handle.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {}; uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; uav.Format = DXGI_FORMAT_UNKNOWN; uav.Buffer.NumElements = pixel_count; uav.Buffer.StructureByteStride = 3 * sizeof(float); device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, handle);
    uint32_t constants[] = {pixel_count, 0}; std::memcpy(&constants[1], &global_gain, sizeof(global_gain)); ID3D12DescriptorHeap *heaps[] = {heap.Get()}; list->SetDescriptorHeaps(1, heaps); list->SetComputeRootSignature(root.Get()); list->SetComputeRoot32BitConstants(0, 2, constants, 0); list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart()); list->Dispatch((pixel_count + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER barrier {}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition.pResource = output.Get(); barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; list->ResourceBarrier(1, &barrier); list->CopyResource(readback.Get(), output.Get());
    if (FAILED(list->Close())) { write_error(error_buffer, error_buffer_size, "cannot close D3D12 global-gain command list"); return PANO_GPU_UNAVAILABLE; } ID3D12CommandList *lists[] = {list.Get()}; session->device_core->queue->ExecuteCommandLists(1, lists); const uint64_t fence = session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence))) { write_error(error_buffer, error_buffer_size, "cannot signal D3D12 global-gain fence"); return PANO_GPU_UNAVAILABLE; }
    if (wait_for_fence(session->device_core.get(), fence, error_buffer, error_buffer_size, "D3D12 global-gain fence timed out") != PANO_GPU_SUCCESS) return PANO_GPU_UNAVAILABLE;
    const D3D12_RANGE range {0, static_cast<SIZE_T>(rgb_bytes)}; mapped = nullptr; if (FAILED(readback->Map(0, &range, &mapped))) { write_error(error_buffer, error_buffer_size, "cannot map D3D12 global-gain readback"); return PANO_GPU_UNAVAILABLE; } std::memcpy(adjusted_rgb, mapped, static_cast<size_t>(rgb_bytes)); readback->Unmap(0, nullptr); return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_dispatch_local_exposure(
    const pano_gpu_session *const session, const uint32_t output_width, const uint32_t output_height,
    const uint32_t row_start, const uint32_t row_count, const void *const candidate_rgb,
    const uint64_t candidate_rgb_bytes, const void *const local_field, const uint64_t local_field_bytes,
    void *const adjusted_rgb, const uint64_t adjusted_rgb_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    uint64_t pixels = 0, rgb_bytes = 0, field_pixels = 0, expected_field_bytes = 0;
    const uint32_t field_width = output_width == 0 ? 0 : (output_width - 1) / 4 + 1;
    const uint32_t field_height = output_height == 0 ? 0 : (output_height - 1) / 4 + 1;
    if (session == nullptr || output_width == 0 || output_height == 0 || row_count == 0 || row_start >= output_height || row_count > output_height - row_start || candidate_rgb == nullptr || local_field == nullptr || adjusted_rgb == nullptr || !checked_multiply(output_width, row_count, &pixels) || !checked_multiply(pixels, 3 * sizeof(float), &rgb_bytes) || !checked_multiply(field_width, field_height, &field_pixels) || !checked_multiply(field_pixels, sizeof(float), &expected_field_bytes) || candidate_rgb_bytes != rgb_bytes || adjusted_rgb_bytes != rgb_bytes || local_field_bytes != expected_field_bytes)
    { write_error(error_buffer, error_buffer_size, "invalid D3D12 local-exposure request"); return PANO_GPU_INVALID_ARGUMENT; }
    const float *const field_values = static_cast<const float *>(local_field);
    for (uint64_t index = 0; index < field_pixels; ++index)
        if (!std::isfinite(field_values[index]))
        { write_error(error_buffer, error_buffer_size, "invalid D3D12 local-exposure field"); return PANO_GPU_INVALID_ARGUMENT; }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows"); return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue || !session->device_core->fence) { write_error(error_buffer, error_buffer_size, "invalid D3D12 local-exposure session"); return PANO_GPU_INVALID_ARGUMENT; }
    ID3D12Device *const device = session->device_core->d3d_device.Get(); const auto desc = [](uint64_t bytes, D3D12_RESOURCE_FLAGS flags) { D3D12_RESOURCE_DESC d {}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = flags; return d; }; D3D12_HEAP_PROPERTIES upload {}; upload.Type = D3D12_HEAP_TYPE_UPLOAD; D3D12_HEAP_PROPERTIES def {}; def.Type = D3D12_HEAP_TYPE_DEFAULT; D3D12_HEAP_PROPERTIES read {}; read.Type = D3D12_HEAP_TYPE_READBACK; const D3D12_RESOURCE_DESC rgb_desc = desc(rgb_bytes, D3D12_RESOURCE_FLAG_NONE), field_desc = desc(expected_field_bytes, D3D12_RESOURCE_FLAG_NONE), out_desc = desc(rgb_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS); Microsoft::WRL::ComPtr<ID3D12Resource> rgb, field, out, back;
    if (FAILED(device->CreateCommittedResource(&upload,D3D12_HEAP_FLAG_NONE,&rgb_desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&rgb))) || FAILED(device->CreateCommittedResource(&upload,D3D12_HEAP_FLAG_NONE,&field_desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&field))) || FAILED(device->CreateCommittedResource(&def,D3D12_HEAP_FLAG_NONE,&out_desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&out))) || FAILED(device->CreateCommittedResource(&read,D3D12_HEAP_FLAG_NONE,&rgb_desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&back)))) { write_error(error_buffer,error_buffer_size,"cannot create D3D12 local-exposure resources"); return PANO_GPU_UNAVAILABLE; }
    void *mapped = nullptr; if (FAILED(rgb->Map(0,nullptr,&mapped))) return PANO_GPU_UNAVAILABLE; std::memcpy(mapped,candidate_rgb,static_cast<size_t>(rgb_bytes)); rgb->Unmap(0,nullptr); if (FAILED(field->Map(0,nullptr,&mapped))) return PANO_GPU_UNAVAILABLE; std::memcpy(mapped,local_field,static_cast<size_t>(expected_field_bytes)); field->Unmap(0,nullptr);
    D3D12_DESCRIPTOR_RANGE ranges[2] {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,2}}; D3D12_ROOT_PARAMETER p[2] {}; p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[0].Constants={0,0,8};p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;p[1].DescriptorTable={2,ranges};D3D12_ROOT_SIGNATURE_DESC rs {2,p,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};Microsoft::WRL::ComPtr<ID3DBlob> blob,errors;Microsoft::WRL::ComPtr<ID3D12RootSignature> root;Microsoft::WRL::ComPtr<ID3D12PipelineState> pipe;if(FAILED(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1_0,&blob,&errors))||FAILED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))))return PANO_GPU_UNAVAILABLE;D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};pd.pRootSignature=root.Get();pd.CS={pano_gpu_local_exposure_shader,sizeof(pano_gpu_local_exposure_shader)};if(FAILED(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipe))))return PANO_GPU_UNAVAILABLE;
    D3D12_DESCRIPTOR_HEAP_DESC hd {};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=3;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)))||FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)))||FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),pipe.Get(),IID_PPV_ARGS(&list))))return PANO_GPU_UNAVAILABLE;const UINT inc=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);auto h=heap->GetCPUDescriptorHandleForHeapStart();D3D12_SHADER_RESOURCE_VIEW_DESC srv {};srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Format=DXGI_FORMAT_UNKNOWN;srv.Buffer.NumElements=static_cast<UINT>(pixels);srv.Buffer.StructureByteStride=12;device->CreateShaderResourceView(rgb.Get(),&srv,h);h.ptr+=inc;srv.Buffer.NumElements=static_cast<UINT>(field_pixels);srv.Buffer.StructureByteStride=4;device->CreateShaderResourceView(field.Get(),&srv,h);h.ptr+=inc;D3D12_UNORDERED_ACCESS_VIEW_DESC u {};u.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;u.Format=DXGI_FORMAT_UNKNOWN;u.Buffer.NumElements=static_cast<UINT>(pixels);u.Buffer.StructureByteStride=12;device->CreateUnorderedAccessView(out.Get(),nullptr,&u,h);uint32_t c[8]{output_width,output_height,row_start,row_count,field_width,field_height};ID3D12DescriptorHeap *hs[]{heap.Get()};list->SetDescriptorHeaps(1,hs);list->SetComputeRootSignature(root.Get());list->SetComputeRoot32BitConstants(0,8,c,0);list->SetComputeRootDescriptorTable(1,heap->GetGPUDescriptorHandleForHeapStart());list->Dispatch((static_cast<UINT>(pixels)+63)/64,1,1);D3D12_RESOURCE_BARRIER b {};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=out.Get();b.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;list->ResourceBarrier(1,&b);list->CopyResource(back.Get(),out.Get());if(FAILED(list->Close()))return PANO_GPU_UNAVAILABLE;ID3D12CommandList *ls[]{list.Get()};session->device_core->queue->ExecuteCommandLists(1,ls);const uint64_t f=session->device_core->next_fence_value.fetch_add(1,std::memory_order_relaxed)+1;if(FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(),f))||wait_for_fence(session->device_core.get(),f,error_buffer,error_buffer_size,"D3D12 local-exposure fence timed out")!=PANO_GPU_SUCCESS)return PANO_GPU_UNAVAILABLE;const D3D12_RANGE r{0,static_cast<SIZE_T>(rgb_bytes)};if(FAILED(back->Map(0,&r,&mapped)))return PANO_GPU_UNAVAILABLE;std::memcpy(adjusted_rgb,mapped,static_cast<size_t>(rgb_bytes));back->Unmap(0,nullptr);return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_dispatch_mark_incomplete(
    const pano_gpu_session *const session, const uint32_t pixel_count, const void *const selected_rgb,
    const uint64_t selected_rgb_bytes, const void *const selected_weight, const uint64_t selected_weight_bytes,
    void *const marked_rgb, const uint64_t marked_rgb_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    uint64_t rgb_bytes = 0;
    uint64_t weight_bytes = 0;
    if (session == nullptr || pixel_count == 0 || !checked_multiply(pixel_count, 3 * sizeof(float), &rgb_bytes) ||
        !checked_multiply(pixel_count, sizeof(float), &weight_bytes) || selected_rgb == nullptr ||
        selected_rgb_bytes != rgb_bytes || selected_weight == nullptr || selected_weight_bytes != weight_bytes ||
        marked_rgb == nullptr || marked_rgb_bytes != rgb_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 incomplete-marking buffers");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue ||
        !session->device_core->fence)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 incomplete-marking session");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    D3D12_DESCRIPTOR_RANGE ranges[2] {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
                                      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2}};
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 1};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC root_desc {2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc {};
    pipeline_desc.pRootSignature = root.Get();
    pipeline_desc.CS = {pano_gpu_mark_incomplete_shader, sizeof(pano_gpu_mark_incomplete_shader)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline))))
        return PANO_GPU_UNAVAILABLE;
    const auto buffer_desc = [](const uint64_t bytes, const D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = flags;
        return desc;
    };
    D3D12_HEAP_PROPERTIES upload_heap {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const auto make_upload = [&](const uint64_t bytes, const void *const values,
                                 Microsoft::WRL::ComPtr<ID3D12Resource> *const resource) {
        const D3D12_RESOURCE_DESC desc = buffer_desc(bytes, D3D12_RESOURCE_FLAG_NONE);
        if (FAILED(device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(resource->ReleaseAndGetAddressOf()))))
            return false;
        void *mapped = nullptr;
        if (FAILED((*resource)->Map(0, nullptr, &mapped)))
            return false;
        std::memcpy(mapped, values, static_cast<size_t>(bytes));
        (*resource)->Unmap(0, nullptr);
        return true;
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> rgb_input;
    Microsoft::WRL::ComPtr<ID3D12Resource> weight_input;
    if (!make_upload(rgb_bytes, selected_rgb, &rgb_input) || !make_upload(weight_bytes, selected_weight, &weight_input))
        return PANO_GPU_UNAVAILABLE;
    D3D12_HEAP_PROPERTIES default_heap {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const D3D12_RESOURCE_DESC output_desc = buffer_desc(rgb_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    Microsoft::WRL::ComPtr<ID3D12Resource> output;
    if (FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &output_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&output))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    const D3D12_RESOURCE_DESC readback_desc = buffer_desc(rgb_bytes, D3D12_RESOURCE_FLAG_NONE);
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback))))
        return PANO_GPU_UNAVAILABLE;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 3;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
        return PANO_GPU_UNAVAILABLE;
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor = heap->GetCPUDescriptorHandleForHeapStart();
    const auto create_srv = [&](ID3D12Resource *const resource, const UINT stride) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view {};
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.Buffer.NumElements = pixel_count;
        view.Buffer.StructureByteStride = stride;
        device->CreateShaderResourceView(resource, &view, descriptor);
        descriptor.ptr += increment;
    };
    create_srv(rgb_input.Get(), 3 * sizeof(float));
    create_srv(weight_input.Get(), sizeof(float));
    D3D12_UNORDERED_ACCESS_VIEW_DESC output_view {};
    output_view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    output_view.Format = DXGI_FORMAT_UNKNOWN;
    output_view.Buffer.NumElements = pixel_count;
    output_view.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateUnorderedAccessView(output.Get(), nullptr, &output_view, descriptor);
    ID3D12DescriptorHeap *heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRoot32BitConstants(0, 1, &pixel_count, 0);
    list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((pixel_count + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = output.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
    list->CopyResource(readback.Get(), output.Get());
    if (FAILED(list->Close()))
        return PANO_GPU_UNAVAILABLE;
    ID3D12CommandList *lists[] = {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence = session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence)) ||
        wait_for_fence(session->device_core.get(), fence, error_buffer, error_buffer_size,
                       "D3D12 incomplete-marking fence timed out") != PANO_GPU_SUCCESS)
        return PANO_GPU_UNAVAILABLE;
    const D3D12_RANGE range {0, static_cast<SIZE_T>(rgb_bytes)};
    void *mapped = nullptr;
    if (FAILED(readback->Map(0, &range, &mapped)))
        return PANO_GPU_UNAVAILABLE;
    std::memcpy(marked_rgb, mapped, static_cast<size_t>(rgb_bytes));
    readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_dispatch_hard_selection(
    const pano_gpu_session *const session, const pano_gpu_hard_selection_request *const request,
    const void *const candidate_rgb, const uint64_t candidate_rgb_bytes, const void *const candidate_validity,
    const uint64_t candidate_validity_bytes, const void *const candidate_edge_distance,
    const uint64_t candidate_edge_distance_bytes, const void *const prior_rgb, const uint64_t prior_rgb_bytes,
    const void *const prior_weight, const uint64_t prior_weight_bytes, void *const selected_rgb,
    const uint64_t selected_rgb_bytes, void *const selected_weight, const uint64_t selected_weight_bytes,
    void *const coverage, const uint64_t coverage_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    pano_gpu_hard_selection_result_layout layout {};
    layout.size = sizeof(layout);
    layout.abi_version = PANO_GPU_ABI_VERSION;
    const pano_gpu_result validation_result = pano_gpu_test_validate_hard_selection_request(
        session, request, candidate_rgb, candidate_rgb_bytes, candidate_validity, candidate_validity_bytes,
        candidate_edge_distance, candidate_edge_distance_bytes, prior_rgb, prior_rgb_bytes, prior_weight,
        prior_weight_bytes, &layout, error_buffer, error_buffer_size);
    if (validation_result != PANO_GPU_SUCCESS)
        return validation_result;
    if (selected_rgb == nullptr || selected_rgb_bytes != layout.selected_rgb_bytes || selected_weight == nullptr ||
        selected_weight_bytes != layout.selected_weight_bytes || coverage == nullptr || coverage_bytes != layout.coverage_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 hard-selection result buffers");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->d3d_device || !session->device_core->queue ||
        !session->device_core->fence)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 hard-selection session");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 5},
    };
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 1};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC root_description {2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)))
    {
        write_error(error_buffer, error_buffer_size, "cannot serialize D3D12 hard-selection root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    if (FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root_signature))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 hard-selection root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root_signature.Get();
    pipeline_description.CS = {pano_gpu_hard_selection_shader, sizeof(pano_gpu_hard_selection_shader)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    const HRESULT pipeline_result = device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline));
    if (FAILED(pipeline_result))
    {
        write_device_error(error_buffer, error_buffer_size, "cannot create D3D12 hard-selection pipeline", pipeline_result, device);
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES upload_heap {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const auto make_upload = [&](const uint64_t bytes, const void *const contents, Microsoft::WRL::ComPtr<ID3D12Resource> *const resource) {
        D3D12_RESOURCE_DESC buffer {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(resource->ReleaseAndGetAddressOf()))))
            return false;
        void *mapped = nullptr;
        if (FAILED((*resource)->Map(0, nullptr, &mapped)))
            return false;
        std::memcpy(mapped, contents, static_cast<size_t>(bytes));
        (*resource)->Unmap(0, nullptr);
        return true;
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> candidate_rgb_resource;
    Microsoft::WRL::ComPtr<ID3D12Resource> candidate_validity_resource;
    Microsoft::WRL::ComPtr<ID3D12Resource> candidate_edge_resource;
    Microsoft::WRL::ComPtr<ID3D12Resource> prior_rgb_resource;
    Microsoft::WRL::ComPtr<ID3D12Resource> prior_weight_resource;
    std::vector<uint32_t> packed_validity((request->pixel_count + 31) / 32, 0);
    const uint8_t *const candidate_validity_values = static_cast<const uint8_t *>(candidate_validity);
    for (uint32_t index = 0; index < request->pixel_count; ++index)
    {
        if (candidate_validity_values[index] != 0)
            packed_validity[index / 32] |= 1U << (index & 31);
    }
    if (!make_upload(layout.candidate_rgb_bytes, candidate_rgb, &candidate_rgb_resource) ||
        !make_upload(packed_validity.size() * sizeof(uint32_t), packed_validity.data(), &candidate_validity_resource) ||
        !make_upload(layout.candidate_edge_distance_bytes, candidate_edge_distance, &candidate_edge_resource) ||
        !make_upload(layout.prior_rgb_bytes, prior_rgb, &prior_rgb_resource) ||
        !make_upload(layout.prior_weight_bytes, prior_weight, &prior_weight_resource))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 hard-selection input resources");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES default_heap {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const auto make_output = [&](const uint64_t bytes, Microsoft::WRL::ComPtr<ID3D12Resource> *const resource) {
        D3D12_RESOURCE_DESC buffer {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(resource->ReleaseAndGetAddressOf())));
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> selected_rgb_resource;
    Microsoft::WRL::ComPtr<ID3D12Resource> selected_weight_resource;
    Microsoft::WRL::ComPtr<ID3D12Resource> coverage_resource;
    if (!make_output(layout.selected_rgb_bytes, &selected_rgb_resource) ||
        !make_output(layout.selected_weight_bytes, &selected_weight_resource) ||
        !make_output(layout.coverage_bytes, &coverage_resource))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 hard-selection output resources");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    const auto make_readback = [&](const uint64_t bytes, Microsoft::WRL::ComPtr<ID3D12Resource> *const resource) {
        D3D12_RESOURCE_DESC buffer {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(resource->ReleaseAndGetAddressOf())));
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> selected_rgb_readback;
    Microsoft::WRL::ComPtr<ID3D12Resource> selected_weight_readback;
    Microsoft::WRL::ComPtr<ID3D12Resource> coverage_readback;
    if (!make_readback(layout.selected_rgb_bytes, &selected_rgb_readback) ||
        !make_readback(layout.selected_weight_bytes, &selected_weight_readback) ||
        !make_readback(layout.coverage_bytes, &coverage_readback))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 hard-selection readback resources");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 8;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&descriptor_heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 hard-selection dispatch resources");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor = descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    const auto create_structured_srv = [&](ID3D12Resource *const resource, const UINT stride) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view {};
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.Buffer.NumElements = request->pixel_count;
        view.Buffer.StructureByteStride = stride;
        device->CreateShaderResourceView(resource, &view, descriptor);
        descriptor.ptr += descriptor_size;
    };
    create_structured_srv(candidate_rgb_resource.Get(), 3 * sizeof(float));
    D3D12_SHADER_RESOURCE_VIEW_DESC validity_srv {};
    validity_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    validity_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    validity_srv.Format = DXGI_FORMAT_R32_TYPELESS;
    validity_srv.Buffer.NumElements = static_cast<UINT>(packed_validity.size());
    validity_srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(candidate_validity_resource.Get(), &validity_srv, descriptor);
    descriptor.ptr += descriptor_size;
    create_structured_srv(candidate_edge_resource.Get(), sizeof(float));
    create_structured_srv(prior_rgb_resource.Get(), 3 * sizeof(float));
    create_structured_srv(prior_weight_resource.Get(), sizeof(float));
    D3D12_UNORDERED_ACCESS_VIEW_DESC rgb_uav {};
    rgb_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    rgb_uav.Format = DXGI_FORMAT_UNKNOWN;
    rgb_uav.Buffer.NumElements = request->pixel_count;
    rgb_uav.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateUnorderedAccessView(selected_rgb_resource.Get(), nullptr, &rgb_uav, descriptor);
    descriptor.ptr += descriptor_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC weight_uav = rgb_uav;
    weight_uav.Buffer.StructureByteStride = sizeof(float);
    device->CreateUnorderedAccessView(selected_weight_resource.Get(), nullptr, &weight_uav, descriptor);
    descriptor.ptr += descriptor_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC coverage_uav {};
    coverage_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    coverage_uav.Format = DXGI_FORMAT_R8_UINT;
    coverage_uav.Buffer.NumElements = request->pixel_count;
    device->CreateUnorderedAccessView(coverage_resource.Get(), nullptr, &coverage_uav, descriptor);
    ID3D12DescriptorHeap *heaps[] = {descriptor_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_signature.Get());
    list->SetComputeRoot32BitConstants(0, 1, &request->pixel_count, 0);
    list->SetComputeRootDescriptorTable(1, descriptor_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((request->pixel_count + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER barriers[3] {};
    ID3D12Resource *outputs[] = {selected_rgb_resource.Get(), selected_weight_resource.Get(), coverage_resource.Get()};
    for (size_t index = 0; index < 3; ++index)
    {
        barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[index].Transition.pResource = outputs[index];
        barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[index].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    list->ResourceBarrier(3, barriers);
    list->CopyResource(selected_rgb_readback.Get(), selected_rgb_resource.Get());
    list->CopyResource(selected_weight_readback.Get(), selected_weight_resource.Get());
    list->CopyResource(coverage_readback.Get(), coverage_resource.Get());
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 hard-selection command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] = {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value = session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)))
    {
        write_error(error_buffer, error_buffer_size, "cannot signal D3D12 hard-selection fence");
        return PANO_GPU_UNAVAILABLE;
    }
    const pano_gpu_result wait_result = wait_for_fence(
        session->device_core.get(), fence_value, error_buffer, error_buffer_size, "D3D12 hard-selection fence timed out");
    if (wait_result != PANO_GPU_SUCCESS)
        return wait_result;
    const auto copy_readback = [&](ID3D12Resource *const resource, const uint64_t bytes, void *const destination) {
        const D3D12_RANGE range {0, static_cast<SIZE_T>(bytes)};
        void *mapped = nullptr;
        if (FAILED(resource->Map(0, &range, &mapped)))
            return false;
        std::memcpy(destination, mapped, static_cast<size_t>(bytes));
        resource->Unmap(0, nullptr);
        return true;
    };
    if (!copy_readback(selected_rgb_readback.Get(), layout.selected_rgb_bytes, selected_rgb) ||
        !copy_readback(selected_weight_readback.Get(), layout.selected_weight_bytes, selected_weight) ||
        !copy_readback(coverage_readback.Get(), layout.coverage_bytes, coverage))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 hard-selection readback");
        return PANO_GPU_UNAVAILABLE;
    }
    return PANO_GPU_SUCCESS;
#endif
}

static pano_gpu_result dispatch_one_frame_typed_hard_selection(
    const pano_gpu_session *const session, const pano_gpu_one_frame_composite_request *const request,
    const void *const prior_rgb, const uint64_t prior_rgb_bytes, const void *const prior_weight,
    const uint64_t prior_weight_bytes, void *const selected_rgb, const uint64_t selected_rgb_bytes,
    void *const selected_weight, const uint64_t selected_weight_bytes, void *const coverage,
    const uint64_t coverage_bytes, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    pano_gpu_one_frame_composite_result_layout layout {};
    layout.size = sizeof(layout);
    layout.abi_version = PANO_GPU_ABI_VERSION;
    const pano_gpu_result validation_result = pano_gpu_test_validate_one_frame_composite_request(
        session, request, &layout, error_buffer, error_buffer_size);
    if (validation_result != PANO_GPU_SUCCESS)
        return validation_result;
    if (session == nullptr || (session->source_sample_type != PANO_GPU_SAMPLE_UINT8 &&
                               session->source_sample_type != PANO_GPU_SAMPLE_UINT16 &&
                               session->source_sample_type != PANO_GPU_SAMPLE_FLOAT32) || prior_rgb == nullptr ||
        prior_rgb_bytes != layout.linear_rgb_bytes || prior_weight == nullptr ||
        prior_weight_bytes != layout.candidate_edge_distance_bytes || selected_rgb == nullptr ||
        selected_rgb_bytes != layout.linear_rgb_bytes || selected_weight == nullptr ||
        selected_weight_bytes != layout.candidate_edge_distance_bytes || coverage == nullptr ||
        coverage_bytes != layout.coverage_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 typed hard-selection buffers");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    const float *const prior_weights = static_cast<const float *>(prior_weight);
    for (uint64_t index = 0; index < layout.pixel_count; ++index)
    {
        if (!std::isfinite(prior_weights[index]) || prior_weights[index] < 0.0F)
        {
            write_error(error_buffer, error_buffer_size, "invalid D3D12 typed hard-selection prior weights");
            return PANO_GPU_INVALID_ARGUMENT;
        }
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const uint32_t pixel_count = static_cast<uint32_t>(layout.pixel_count);
    const bool is_uint16 = session->source_sample_type == PANO_GPU_SAMPLE_UINT16;
    const bool is_float32 = session->source_sample_type == PANO_GPU_SAMPLE_FLOAT32;
    const uint32_t source_element_bytes = is_float32 ? sizeof(float) : (is_uint16 ? sizeof(uint16_t) : sizeof(uint8_t));
    D3D12_DESCRIPTOR_RANGE candidate_ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 1},
    };
    D3D12_ROOT_PARAMETER candidate_parameters[2] {};
    candidate_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    candidate_parameters[0].Constants = {0, 0, 36};
    candidate_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    candidate_parameters[1].DescriptorTable = {2, candidate_ranges};
    D3D12_ROOT_SIGNATURE_DESC candidate_root_description {
        2, candidate_parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    D3D12_DESCRIPTOR_RANGE selection_ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 5},
    };
    D3D12_ROOT_PARAMETER selection_parameters[2] {};
    selection_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    selection_parameters[0].Constants = {0, 0, 1};
    selection_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    selection_parameters[1].DescriptorTable = {2, selection_ranges};
    D3D12_ROOT_SIGNATURE_DESC selection_root_description {
        2, selection_parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> candidate_serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> selection_serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(
            &candidate_root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &candidate_serialized, &errors)) ||
        FAILED(D3D12SerializeRootSignature(
            &selection_root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &selection_serialized, &errors)))
    {
        write_error(error_buffer, error_buffer_size, "cannot serialize D3D12 typed hard-selection root signatures");
        return PANO_GPU_UNAVAILABLE;
    }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> candidate_root_signature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> selection_root_signature;
    if (FAILED(device->CreateRootSignature(
            0, candidate_serialized->GetBufferPointer(), candidate_serialized->GetBufferSize(),
            IID_PPV_ARGS(&candidate_root_signature))) ||
        FAILED(device->CreateRootSignature(
            0, selection_serialized->GetBufferPointer(), selection_serialized->GetBufferSize(),
            IID_PPV_ARGS(&selection_root_signature))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 typed hard-selection root signatures");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC candidate_pipeline_description {};
    candidate_pipeline_description.pRootSignature = candidate_root_signature.Get();
    candidate_pipeline_description.CS = is_float32
        ? D3D12_SHADER_BYTECODE {pano_gpu_float32_candidate_shader, sizeof(pano_gpu_float32_candidate_shader)}
        : (is_uint16 ? D3D12_SHADER_BYTECODE {pano_gpu_uint16_candidate_shader, sizeof(pano_gpu_uint16_candidate_shader)}
                     : D3D12_SHADER_BYTECODE {pano_gpu_uint8_candidate_shader, sizeof(pano_gpu_uint8_candidate_shader)});
    D3D12_COMPUTE_PIPELINE_STATE_DESC selection_pipeline_description {};
    selection_pipeline_description.pRootSignature = selection_root_signature.Get();
    selection_pipeline_description.CS = {pano_gpu_hard_selection_shader, sizeof(pano_gpu_hard_selection_shader)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> candidate_pipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> selection_pipeline;
    if (FAILED(device->CreateComputePipelineState(&candidate_pipeline_description, IID_PPV_ARGS(&candidate_pipeline))) ||
        FAILED(device->CreateComputePipelineState(&selection_pipeline_description, IID_PPV_ARGS(&selection_pipeline))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 typed hard-selection pipelines");
        return PANO_GPU_UNAVAILABLE;
    }
    const auto resource_description = [](const uint64_t bytes, const D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC result {};
        result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        result.Width = bytes;
        result.Height = 1;
        result.DepthOrArraySize = 1;
        result.MipLevels = 1;
        result.SampleDesc.Count = 1;
        result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        result.Flags = flags;
        return result;
    };
    D3D12_HEAP_PROPERTIES default_heap {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const auto make_default = [&](const uint64_t bytes, Microsoft::WRL::ComPtr<ID3D12Resource> *const resource) {
        const D3D12_RESOURCE_DESC description = resource_description(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        return SUCCEEDED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(resource->ReleaseAndGetAddressOf())));
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> candidates;
    Microsoft::WRL::ComPtr<ID3D12Resource> validity_bits;
    Microsoft::WRL::ComPtr<ID3D12Resource> edge_distances;
    Microsoft::WRL::ComPtr<ID3D12Resource> selected_rgb_resource;
    Microsoft::WRL::ComPtr<ID3D12Resource> selected_weight_resource;
    Microsoft::WRL::ComPtr<ID3D12Resource> coverage_resource;
    const uint64_t validity_bytes = ((layout.pixel_count + 31) / 32) * sizeof(uint32_t);
    if (!make_default(layout.linear_rgb_bytes, &candidates) || !make_default(validity_bytes, &validity_bits) ||
        !make_default(layout.candidate_edge_distance_bytes, &edge_distances) ||
        !make_default(layout.linear_rgb_bytes, &selected_rgb_resource) ||
        !make_default(layout.candidate_edge_distance_bytes, &selected_weight_resource) ||
        !make_default(layout.coverage_bytes, &coverage_resource))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 typed hard-selection GPU buffers");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES upload_heap {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const auto make_upload = [&](const uint64_t bytes, const void *const contents, Microsoft::WRL::ComPtr<ID3D12Resource> *const resource) {
        const D3D12_RESOURCE_DESC description = resource_description(bytes, D3D12_RESOURCE_FLAG_NONE);
        if (FAILED(device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(resource->ReleaseAndGetAddressOf()))))
            return false;
        void *mapped = nullptr;
        if (FAILED((*resource)->Map(0, nullptr, &mapped)))
            return false;
        std::memcpy(mapped, contents, static_cast<size_t>(bytes));
        (*resource)->Unmap(0, nullptr);
        return true;
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> prior_rgb_resource;
    Microsoft::WRL::ComPtr<ID3D12Resource> prior_weight_resource;
    if (!make_upload(layout.linear_rgb_bytes, prior_rgb, &prior_rgb_resource) ||
        !make_upload(layout.candidate_edge_distance_bytes, prior_weight, &prior_weight_resource))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 typed hard-selection prior buffers");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    const auto make_readback = [&](const uint64_t bytes, Microsoft::WRL::ComPtr<ID3D12Resource> *const resource) {
        const D3D12_RESOURCE_DESC description = resource_description(bytes, D3D12_RESOURCE_FLAG_NONE);
        return SUCCEEDED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(resource->ReleaseAndGetAddressOf())));
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> selected_rgb_readback;
    Microsoft::WRL::ComPtr<ID3D12Resource> selected_weight_readback;
    Microsoft::WRL::ComPtr<ID3D12Resource> coverage_readback;
    if (!make_readback(layout.linear_rgb_bytes, &selected_rgb_readback) ||
        !make_readback(layout.candidate_edge_distance_bytes, &selected_weight_readback) ||
        !make_readback(layout.coverage_bytes, &coverage_readback))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 typed hard-selection readback buffers");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 12;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&descriptor_heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), candidate_pipeline.Get(), IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 typed hard-selection command resources");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor = descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC source_srv {};
    source_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    source_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    source_srv.Format = is_float32 ? DXGI_FORMAT_R32_FLOAT : (is_uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R8_UINT);
    source_srv.Buffer.NumElements = static_cast<UINT>(session->source_bytes / source_element_bytes);
    device->CreateShaderResourceView(session->source.Get(), &source_srv, descriptor);
    descriptor.ptr += descriptor_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC candidate_uav {};
    candidate_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    candidate_uav.Format = DXGI_FORMAT_UNKNOWN;
    candidate_uav.Buffer.NumElements = static_cast<UINT>(layout.pixel_count);
    candidate_uav.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateUnorderedAccessView(candidates.Get(), nullptr, &candidate_uav, descriptor);
    descriptor.ptr += descriptor_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC validity_uav {};
    validity_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    validity_uav.Format = DXGI_FORMAT_R32_TYPELESS;
    validity_uav.Buffer.NumElements = static_cast<UINT>(validity_bytes / sizeof(uint32_t));
    validity_uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    const D3D12_CPU_DESCRIPTOR_HANDLE validity_cpu_handle = descriptor;
    device->CreateUnorderedAccessView(validity_bits.Get(), nullptr, &validity_uav, descriptor);
    descriptor.ptr += descriptor_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC edge_uav {};
    edge_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    edge_uav.Format = DXGI_FORMAT_UNKNOWN;
    edge_uav.Buffer.NumElements = static_cast<UINT>(layout.pixel_count);
    edge_uav.Buffer.StructureByteStride = sizeof(float);
    device->CreateUnorderedAccessView(edge_distances.Get(), nullptr, &edge_uav, descriptor);
    descriptor.ptr += descriptor_size;
    const auto create_structured_srv = [&](ID3D12Resource *const resource, const UINT stride) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view {};
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.Buffer.NumElements = static_cast<UINT>(layout.pixel_count);
        view.Buffer.StructureByteStride = stride;
        device->CreateShaderResourceView(resource, &view, descriptor);
        descriptor.ptr += descriptor_size;
    };
    create_structured_srv(candidates.Get(), 3 * sizeof(float));
    D3D12_SHADER_RESOURCE_VIEW_DESC validity_srv {};
    validity_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    validity_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    validity_srv.Format = DXGI_FORMAT_R32_TYPELESS;
    validity_srv.Buffer.NumElements = static_cast<UINT>(validity_bytes / sizeof(uint32_t));
    validity_srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(validity_bits.Get(), &validity_srv, descriptor);
    descriptor.ptr += descriptor_size;
    create_structured_srv(edge_distances.Get(), sizeof(float));
    create_structured_srv(prior_rgb_resource.Get(), 3 * sizeof(float));
    create_structured_srv(prior_weight_resource.Get(), sizeof(float));
    device->CreateUnorderedAccessView(selected_rgb_resource.Get(), nullptr, &candidate_uav, descriptor);
    descriptor.ptr += descriptor_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC weight_uav = edge_uav;
    device->CreateUnorderedAccessView(selected_weight_resource.Get(), nullptr, &weight_uav, descriptor);
    descriptor.ptr += descriptor_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC coverage_uav {};
    coverage_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    coverage_uav.Format = DXGI_FORMAT_R8_UINT;
    coverage_uav.Buffer.NumElements = static_cast<UINT>(layout.pixel_count);
    device->CreateUnorderedAccessView(coverage_resource.Get(), nullptr, &coverage_uav, descriptor);
    ID3D12DescriptorHeap *heaps[] = {descriptor_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    const UINT clear_values[4] {};
    D3D12_GPU_DESCRIPTOR_HANDLE validity_gpu_handle = descriptor_heap->GetGPUDescriptorHandleForHeapStart();
    validity_gpu_handle.ptr += 2 * descriptor_size;
    list->ClearUnorderedAccessViewUint(validity_gpu_handle, validity_cpu_handle, validity_bits.Get(), clear_values, 0, nullptr);
    uint32_t candidate_constants[36] {
        request->output_width, request->output_height, request->row_start, request->row_count,
        session->source_width, session->source_height, session->source_row_stride_bytes / source_element_bytes,
        static_cast<uint32_t>(static_cast<uint64_t>(request->frame_index) * session->source_frame_bytes / source_element_bytes),
    };
    std::memcpy(&candidate_constants[8], &request->latitude_span_degrees, sizeof(candidate_constants[8]));
    std::memcpy(&candidate_constants[12], request->world_to_camera, 3 * sizeof(float));
    std::memcpy(&candidate_constants[16], request->world_to_camera + 3, 3 * sizeof(float));
    std::memcpy(&candidate_constants[20], request->world_to_camera + 6, 3 * sizeof(float));
    const float source_camera[4] {
        static_cast<float>(session->source_width), static_cast<float>(session->source_height),
        session->source_width / (2.0F * std::tan(request->horizontal_fov_degrees * 0.00872664625997165F)),
        session->source_height / (2.0F * std::tan(request->vertical_fov_degrees * 0.00872664625997165F)),
    };
    std::memcpy(&candidate_constants[24], source_camera, sizeof(source_camera));
    const float unit_global_gain = 1.0F;
    std::memcpy(&candidate_constants[28], &unit_global_gain, sizeof(unit_global_gain));
    const float rectilinear_output = request->rectilinear_output != 0 ? 1.0F : 0.0F;
    std::memcpy(&candidate_constants[32], &rectilinear_output, sizeof(rectilinear_output));
    std::memcpy(&candidate_constants[33], &request->output_vertical_fov_degrees, sizeof(request->output_vertical_fov_degrees));
    candidate_constants[34] = session->transfer_function;
    list->SetComputeRootSignature(candidate_root_signature.Get());
    list->SetComputeRoot32BitConstants(0, 36, candidate_constants, 0);
    list->SetComputeRootDescriptorTable(1, descriptor_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((request->output_width + 7) / 8, (request->row_count + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER candidate_barriers[3] {};
    ID3D12Resource *candidate_resources[] = {candidates.Get(), validity_bits.Get(), edge_distances.Get()};
    for (size_t index = 0; index < 3; ++index)
    {
        candidate_barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        candidate_barriers[index].Transition.pResource = candidate_resources[index];
        candidate_barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        candidate_barriers[index].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        candidate_barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    list->ResourceBarrier(3, candidate_barriers);
    list->SetPipelineState(selection_pipeline.Get());
    list->SetComputeRootSignature(selection_root_signature.Get());
    list->SetComputeRoot32BitConstants(0, 1, &pixel_count, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE selection_table = descriptor_heap->GetGPUDescriptorHandleForHeapStart();
    selection_table.ptr += 4 * descriptor_size;
    list->SetComputeRootDescriptorTable(1, selection_table);
    list->Dispatch((pixel_count + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER selected_barriers[3] {};
    ID3D12Resource *selected_resources[] = {
        selected_rgb_resource.Get(), selected_weight_resource.Get(), coverage_resource.Get()};
    for (size_t index = 0; index < 3; ++index)
    {
        selected_barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        selected_barriers[index].Transition.pResource = selected_resources[index];
        selected_barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        selected_barriers[index].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        selected_barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    list->ResourceBarrier(3, selected_barriers);
    list->CopyResource(selected_rgb_readback.Get(), selected_rgb_resource.Get());
    list->CopyResource(selected_weight_readback.Get(), selected_weight_resource.Get());
    list->CopyResource(coverage_readback.Get(), coverage_resource.Get());
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 typed hard-selection command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] = {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value = session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)) ||
        wait_for_fence(session->device_core.get(), fence_value, error_buffer, error_buffer_size,
                       "D3D12 typed hard-selection fence timed out") != PANO_GPU_SUCCESS)
    {
        return PANO_GPU_UNAVAILABLE;
    }
    const auto copy_readback = [&](ID3D12Resource *const resource, const uint64_t bytes, void *const destination) {
        const D3D12_RANGE range {0, static_cast<SIZE_T>(bytes)};
        void *mapped = nullptr;
        if (FAILED(resource->Map(0, &range, &mapped)))
            return false;
        std::memcpy(destination, mapped, static_cast<size_t>(bytes));
        resource->Unmap(0, nullptr);
        return true;
    };
    if (!copy_readback(selected_rgb_readback.Get(), layout.linear_rgb_bytes, selected_rgb) ||
        !copy_readback(selected_weight_readback.Get(), layout.candidate_edge_distance_bytes, selected_weight) ||
        !copy_readback(coverage_readback.Get(), layout.coverage_bytes, coverage))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 typed hard-selection readback");
        return PANO_GPU_UNAVAILABLE;
    }
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_dispatch_one_frame_uint8_hard_selection(
    const pano_gpu_session *const session, const pano_gpu_one_frame_composite_request *const request,
    const void *const prior_rgb, const uint64_t prior_rgb_bytes, const void *const prior_weight,
    const uint64_t prior_weight_bytes, void *const selected_rgb, const uint64_t selected_rgb_bytes,
    void *const selected_weight, const uint64_t selected_weight_bytes, void *const coverage,
    const uint64_t coverage_bytes, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || session->source_sample_type != PANO_GPU_SAMPLE_UINT8)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 uint8 hard-selection session");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return dispatch_one_frame_typed_hard_selection(
        session, request, prior_rgb, prior_rgb_bytes, prior_weight, prior_weight_bytes, selected_rgb,
        selected_rgb_bytes, selected_weight, selected_weight_bytes, coverage, coverage_bytes, error_buffer,
        error_buffer_size);
}

pano_gpu_result pano_gpu_test_dispatch_one_frame_uint16_hard_selection(
    const pano_gpu_session *const session, const pano_gpu_one_frame_composite_request *const request,
    const void *const prior_rgb, const uint64_t prior_rgb_bytes, const void *const prior_weight,
    const uint64_t prior_weight_bytes, void *const selected_rgb, const uint64_t selected_rgb_bytes,
    void *const selected_weight, const uint64_t selected_weight_bytes, void *const coverage,
    const uint64_t coverage_bytes, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || session->source_sample_type != PANO_GPU_SAMPLE_UINT16)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 uint16 hard-selection session");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return dispatch_one_frame_typed_hard_selection(
        session, request, prior_rgb, prior_rgb_bytes, prior_weight, prior_weight_bytes, selected_rgb,
        selected_rgb_bytes, selected_weight, selected_weight_bytes, coverage, coverage_bytes, error_buffer,
        error_buffer_size);
}

pano_gpu_result pano_gpu_test_dispatch_one_frame_float32_hard_selection(
    const pano_gpu_session *const session, const pano_gpu_one_frame_composite_request *const request,
    const void *const prior_rgb, const uint64_t prior_rgb_bytes, const void *const prior_weight,
    const uint64_t prior_weight_bytes, void *const selected_rgb, const uint64_t selected_rgb_bytes,
    void *const selected_weight, const uint64_t selected_weight_bytes, void *const coverage,
    const uint64_t coverage_bytes, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || session->source_sample_type != PANO_GPU_SAMPLE_FLOAT32)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 float32 hard-selection session");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return dispatch_one_frame_typed_hard_selection(
        session, request, prior_rgb, prior_rgb_bytes, prior_weight, prior_weight_bytes, selected_rgb,
        selected_rgb_bytes, selected_weight, selected_weight_bytes, coverage, coverage_bytes, error_buffer,
        error_buffer_size);
}

static pano_gpu_result dispatch_one_frame_typed_candidates(
    const pano_gpu_session *const session, const pano_gpu_one_frame_composite_request *const request,
    void *const candidate_rgb, const uint64_t candidate_rgb_bytes, void *const validity,
    const uint64_t validity_bytes, void *const candidate_edge_distance, const uint64_t candidate_edge_distance_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    pano_gpu_one_frame_composite_result_layout layout {};
    layout.size = sizeof(layout);
    layout.abi_version = PANO_GPU_ABI_VERSION;
    const pano_gpu_result validation_result = pano_gpu_test_validate_one_frame_composite_request(
        session, request, &layout, error_buffer, error_buffer_size);
    if (validation_result != PANO_GPU_SUCCESS)
        return validation_result;
    if (session == nullptr || (session->source_sample_type != PANO_GPU_SAMPLE_UINT8 &&
                               session->source_sample_type != PANO_GPU_SAMPLE_UINT16 &&
                               session->source_sample_type != PANO_GPU_SAMPLE_FLOAT32) || candidate_rgb == nullptr ||
        candidate_rgb_bytes != layout.linear_rgb_bytes || validity == nullptr || validity_bytes != layout.coverage_bytes ||
        candidate_edge_distance == nullptr || candidate_edge_distance_bytes != layout.candidate_edge_distance_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 typed candidate result buffer");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    const bool is_uint16 = session->source_sample_type == PANO_GPU_SAMPLE_UINT16;
    const bool is_float32 = session->source_sample_type == PANO_GPU_SAMPLE_FLOAT32;
    const uint32_t source_element_bytes = is_float32 ? sizeof(float) : (is_uint16 ? sizeof(uint16_t) : sizeof(uint8_t));
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 1},
    };
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 36};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC root_description {2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)))
    {
        write_error(error_buffer, error_buffer_size, "cannot serialize D3D12 typed candidate root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    if (FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root_signature))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 typed candidate root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root_signature.Get();
    pipeline_description.CS = is_float32
        ? D3D12_SHADER_BYTECODE {pano_gpu_float32_candidate_shader, sizeof(pano_gpu_float32_candidate_shader)}
        : (is_uint16 ? D3D12_SHADER_BYTECODE {pano_gpu_uint16_candidate_shader, sizeof(pano_gpu_uint16_candidate_shader)}
                     : D3D12_SHADER_BYTECODE {pano_gpu_uint8_candidate_shader, sizeof(pano_gpu_uint8_candidate_shader)});
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    const HRESULT pipeline_result = device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline));
    if (FAILED(pipeline_result))
    {
        write_device_error(error_buffer, error_buffer_size, "cannot create D3D12 typed candidate pipeline", pipeline_result, device);
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES default_heap {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC candidate_buffer {};
    candidate_buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    candidate_buffer.Width = layout.linear_rgb_bytes;
    candidate_buffer.Height = 1;
    candidate_buffer.DepthOrArraySize = 1;
    candidate_buffer.MipLevels = 1;
    candidate_buffer.SampleDesc.Count = 1;
    candidate_buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    candidate_buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D12Resource> candidates;
    if (FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &candidate_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&candidates))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 typed candidate output");
        return PANO_GPU_UNAVAILABLE;
    }
    const uint64_t validity_word_bytes = ((layout.pixel_count + 31) / 32) * sizeof(uint32_t);
    D3D12_RESOURCE_DESC validity_buffer = candidate_buffer;
    validity_buffer.Width = validity_word_bytes;
    Microsoft::WRL::ComPtr<ID3D12Resource> validity_output;
    if (FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &validity_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&validity_output))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 typed candidate validity output");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_RESOURCE_DESC edge_distance_buffer = candidate_buffer;
    edge_distance_buffer.Width = layout.candidate_edge_distance_bytes;
    Microsoft::WRL::ComPtr<ID3D12Resource> edge_distance_output;
    if (FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &edge_distance_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&edge_distance_output))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 typed candidate edge-distance output");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC candidate_readback_buffer = candidate_buffer;
    candidate_readback_buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
    Microsoft::WRL::ComPtr<ID3D12Resource> candidate_readback;
    Microsoft::WRL::ComPtr<ID3D12Resource> validity_readback;
    Microsoft::WRL::ComPtr<ID3D12Resource> edge_distance_readback;
    if (FAILED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &candidate_readback_buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&candidate_readback))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 typed candidate readback");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_RESOURCE_DESC edge_distance_readback_buffer = edge_distance_buffer;
    edge_distance_readback_buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &edge_distance_readback_buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&edge_distance_readback))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 typed candidate edge-distance readback");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_RESOURCE_DESC validity_readback_buffer = validity_buffer;
    validity_readback_buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &validity_readback_buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&validity_readback))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 typed candidate validity readback");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 4;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&descriptor_heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 typed candidate dispatch resources");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor = descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC source_srv {};
    source_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    source_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    source_srv.Format = is_float32 ? DXGI_FORMAT_R32_FLOAT : (is_uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R8_UINT);
    source_srv.Buffer.NumElements = static_cast<UINT>(session->source_bytes / source_element_bytes);
    device->CreateShaderResourceView(session->source.Get(), &source_srv, descriptor);
    descriptor.ptr += descriptor_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC candidate_uav {};
    candidate_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    candidate_uav.Format = DXGI_FORMAT_UNKNOWN;
    candidate_uav.Buffer.NumElements = static_cast<UINT>(layout.pixel_count);
    candidate_uav.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateUnorderedAccessView(candidates.Get(), nullptr, &candidate_uav, descriptor);
    descriptor.ptr += descriptor_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC validity_uav {};
    validity_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    validity_uav.Format = DXGI_FORMAT_R32_TYPELESS;
    validity_uav.Buffer.NumElements = static_cast<UINT>(validity_word_bytes / sizeof(uint32_t));
    validity_uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    const D3D12_CPU_DESCRIPTOR_HANDLE validity_cpu_handle = descriptor;
    device->CreateUnorderedAccessView(validity_output.Get(), nullptr, &validity_uav, descriptor);
    descriptor.ptr += descriptor_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC edge_distance_uav {};
    edge_distance_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    edge_distance_uav.Format = DXGI_FORMAT_UNKNOWN;
    edge_distance_uav.Buffer.NumElements = static_cast<UINT>(layout.pixel_count);
    edge_distance_uav.Buffer.StructureByteStride = sizeof(float);
    device->CreateUnorderedAccessView(edge_distance_output.Get(), nullptr, &edge_distance_uav, descriptor);
    uint32_t constants[36] {
        request->output_width, request->output_height, request->row_start, request->row_count,
        session->source_width, session->source_height, session->source_row_stride_bytes / source_element_bytes,
        static_cast<uint32_t>(static_cast<uint64_t>(request->frame_index) * session->source_frame_bytes / source_element_bytes),
    };
    std::memcpy(&constants[8], &request->latitude_span_degrees, sizeof(constants[8]));
    std::memcpy(&constants[12], request->world_to_camera, 3 * sizeof(float));
    std::memcpy(&constants[16], request->world_to_camera + 3, 3 * sizeof(float));
    std::memcpy(&constants[20], request->world_to_camera + 6, 3 * sizeof(float));
    const float source_camera[4] {
        static_cast<float>(session->source_width), static_cast<float>(session->source_height),
        session->source_width / (2.0F * std::tan(request->horizontal_fov_degrees * 0.00872664625997165F)),
        session->source_height / (2.0F * std::tan(request->vertical_fov_degrees * 0.00872664625997165F)),
    };
    std::memcpy(&constants[24], source_camera, sizeof(source_camera));
    const float unit_global_gain = 1.0F;
    std::memcpy(&constants[28], &unit_global_gain, sizeof(unit_global_gain));
    const float rectilinear_output = request->rectilinear_output != 0 ? 1.0F : 0.0F;
    std::memcpy(&constants[32], &rectilinear_output, sizeof(rectilinear_output));
    std::memcpy(&constants[33], &request->output_vertical_fov_degrees, sizeof(request->output_vertical_fov_degrees));
    constants[34] = session->transfer_function;
    ID3D12DescriptorHeap *heaps[] = {descriptor_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    const UINT clear_values[4] {};
    D3D12_GPU_DESCRIPTOR_HANDLE validity_gpu_handle = descriptor_heap->GetGPUDescriptorHandleForHeapStart();
    validity_gpu_handle.ptr += 2 * descriptor_size;
    list->ClearUnorderedAccessViewUint(validity_gpu_handle, validity_cpu_handle, validity_output.Get(), clear_values, 0, nullptr);
    list->SetComputeRootSignature(root_signature.Get());
    list->SetComputeRoot32BitConstants(0, 36, constants, 0);
    list->SetComputeRootDescriptorTable(1, descriptor_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((request->output_width + 7) / 8, (request->row_count + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER barriers[3] {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = candidates.Get();
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = validity_output.Get();
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[2].Transition.pResource = edge_distance_output.Get();
    barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(3, barriers);
    list->CopyResource(candidate_readback.Get(), candidates.Get());
    list->CopyResource(validity_readback.Get(), validity_output.Get());
    list->CopyResource(edge_distance_readback.Get(), edge_distance_output.Get());
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 typed candidate command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] = {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value = session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)))
    {
        write_error(error_buffer, error_buffer_size, "cannot signal D3D12 typed candidate fence");
        return PANO_GPU_UNAVAILABLE;
    }
    const pano_gpu_result wait_result = wait_for_fence(
        session->device_core.get(), fence_value, error_buffer, error_buffer_size, "D3D12 typed candidate fence timed out");
    if (wait_result != PANO_GPU_SUCCESS)
        return wait_result;
    const D3D12_RANGE candidate_range {0, static_cast<SIZE_T>(layout.linear_rgb_bytes)};
    void *mapped = nullptr;
    if (FAILED(candidate_readback->Map(0, &candidate_range, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 typed candidate readback");
        return PANO_GPU_UNAVAILABLE;
    }
    std::memcpy(candidate_rgb, mapped, static_cast<size_t>(layout.linear_rgb_bytes));
    candidate_readback->Unmap(0, nullptr);
    const D3D12_RANGE validity_range {0, static_cast<SIZE_T>(validity_word_bytes)};
    if (FAILED(validity_readback->Map(0, &validity_range, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 typed candidate validity readback");
        return PANO_GPU_UNAVAILABLE;
    }
    const uint32_t *const words = static_cast<const uint32_t *>(mapped);
    uint8_t *const values = static_cast<uint8_t *>(validity);
    for (uint64_t index = 0; index < layout.pixel_count; ++index)
        values[index] = static_cast<uint8_t>((words[index / 32] >> (index & 31)) & 1U);
    validity_readback->Unmap(0, nullptr);
    const D3D12_RANGE edge_distance_range {0, static_cast<SIZE_T>(layout.candidate_edge_distance_bytes)};
    if (FAILED(edge_distance_readback->Map(0, &edge_distance_range, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 typed candidate edge-distance readback");
        return PANO_GPU_UNAVAILABLE;
    }
    std::memcpy(candidate_edge_distance, mapped, static_cast<size_t>(layout.candidate_edge_distance_bytes));
    edge_distance_readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_dispatch_one_frame_uint8_candidates(
    const pano_gpu_session *const session, const pano_gpu_one_frame_composite_request *const request,
    void *const candidate_rgb, const uint64_t candidate_rgb_bytes, void *const validity,
    const uint64_t validity_bytes, void *const candidate_edge_distance, const uint64_t candidate_edge_distance_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || session->source_sample_type != PANO_GPU_SAMPLE_UINT8)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 uint8 candidate session");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return dispatch_one_frame_typed_candidates(
        session, request, candidate_rgb, candidate_rgb_bytes, validity, validity_bytes, candidate_edge_distance,
        candidate_edge_distance_bytes, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_test_dispatch_one_frame_uint16_candidates(
    const pano_gpu_session *const session, const pano_gpu_one_frame_composite_request *const request,
    void *const candidate_rgb, const uint64_t candidate_rgb_bytes, void *const validity,
    const uint64_t validity_bytes, void *const candidate_edge_distance, const uint64_t candidate_edge_distance_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || session->source_sample_type != PANO_GPU_SAMPLE_UINT16)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 uint16 candidate session");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return dispatch_one_frame_typed_candidates(
        session, request, candidate_rgb, candidate_rgb_bytes, validity, validity_bytes, candidate_edge_distance,
        candidate_edge_distance_bytes, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_test_dispatch_one_frame_float32_candidates(
    const pano_gpu_session *const session, const pano_gpu_one_frame_composite_request *const request,
    void *const candidate_rgb, const uint64_t candidate_rgb_bytes, void *const validity,
    const uint64_t validity_bytes, void *const candidate_edge_distance, const uint64_t candidate_edge_distance_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || session->source_sample_type != PANO_GPU_SAMPLE_FLOAT32)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 float32 candidate session");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return dispatch_one_frame_typed_candidates(
        session, request, candidate_rgb, candidate_rgb_bytes, validity, validity_bytes, candidate_edge_distance,
        candidate_edge_distance_bytes, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_test_validate_uint8_sample_request(
    const pano_gpu_session *const session, const pano_gpu_uint8_sample_request *const request,
    const void *const coordinates, const uint64_t coordinate_bytes, pano_gpu_uint8_sample_result_layout *const layout,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (layout == nullptr || layout->size != sizeof(*layout) || layout->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 uint8 sampling result layout");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    std::memset(layout, 0, sizeof(*layout));
    layout->size = sizeof(*layout);
    layout->abi_version = PANO_GPU_ABI_VERSION;
    if (session == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->coordinate_count == 0 ||
        request->frame_index >= session->frame_count || session->source_sample_type != PANO_GPU_SAMPLE_UINT8 ||
        coordinates == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 uint8 sampling request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    uint64_t expected_coordinate_bytes = 0;
    uint64_t sampled_rgb_bytes = 0;
    if (!checked_multiply(request->coordinate_count, 2 * sizeof(float), &expected_coordinate_bytes) ||
        !checked_multiply(request->coordinate_count, 3 * sizeof(float), &sampled_rgb_bytes) ||
        coordinate_bytes != expected_coordinate_bytes || session->source_bytes > std::numeric_limits<uint32_t>::max() ||
        session->source_frame_bytes > std::numeric_limits<uint32_t>::max() ||
        static_cast<uint64_t>(request->frame_index) * session->source_frame_bytes >
            std::numeric_limits<uint32_t>::max())
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 uint8 sampling coordinate buffer");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    const float *const coordinate_values = static_cast<const float *>(coordinates);
    for (uint32_t index = 0; index < request->coordinate_count; ++index)
    {
        const float x = coordinate_values[2 * index];
        const float y = coordinate_values[2 * index + 1];
        if (!std::isfinite(x) || !std::isfinite(y))
        {
            write_error(error_buffer, error_buffer_size, "invalid D3D12 uint8 sampling coordinate");
            return PANO_GPU_INVALID_ARGUMENT;
        }
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->fence || !session->source ||
        request->frame_index >= session->frame_upload_fences.size())
    {
        write_error(error_buffer, error_buffer_size, "D3D12 uint8 sampling source is not ready");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    const uint64_t frame_fence = session->frame_upload_fences[request->frame_index];
    if (frame_fence == 0 || session->device_core->fence->GetCompletedValue() < frame_fence)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 uint8 sampling source upload is unfinished");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    layout->coordinate_bytes = expected_coordinate_bytes;
    layout->sampled_rgb_bytes = sampled_rgb_bytes;
    return PANO_GPU_SUCCESS;
#endif
}

static pano_gpu_result sample_integer(
    const pano_gpu_session *const session, const pano_gpu_uint8_sample_request *const request,
    const void *const coordinates, const uint64_t coordinate_bytes, void *const sampled_rgb,
    const uint64_t sampled_rgb_bytes, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    pano_gpu_uint8_sample_result_layout layout {};
    layout.size = sizeof(layout);
    layout.abi_version = PANO_GPU_ABI_VERSION;
    pano_gpu_result validation_result = PANO_GPU_INVALID_ARGUMENT;
    if (session != nullptr && session->source_sample_type == PANO_GPU_SAMPLE_UINT16)
    {
        validation_result = pano_gpu_test_validate_uint16_sample_request(
            session, request, coordinates, coordinate_bytes, &layout, error_buffer, error_buffer_size);
    }
    else if (session != nullptr && session->source_sample_type == PANO_GPU_SAMPLE_FLOAT32)
    {
        validation_result = pano_gpu_test_validate_float32_sample_request(
            session, request, coordinates, coordinate_bytes, &layout, error_buffer, error_buffer_size);
    }
    else
    {
        validation_result = pano_gpu_test_validate_uint8_sample_request(
            session, request, coordinates, coordinate_bytes, &layout, error_buffer, error_buffer_size);
    }
    if (validation_result != PANO_GPU_SUCCESS)
        return validation_result;
    if (sampled_rgb == nullptr || sampled_rgb_bytes != layout.sampled_rgb_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 uint8 sampling result buffer");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    ID3D12Device *const device = session->device_core->d3d_device.Get();
    D3D12_DESCRIPTOR_RANGE ranges[2] {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2},
    };
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 4};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC root_description {2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)))
    {
        write_error(error_buffer, error_buffer_size, "cannot serialize D3D12 uint8 sampling root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    if (FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root_signature))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 uint8 sampling root signature");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description {};
    pipeline_description.pRootSignature = root_signature.Get();
    const bool is_uint16 = session->source_sample_type == PANO_GPU_SAMPLE_UINT16;
    const bool is_float32 = session->source_sample_type == PANO_GPU_SAMPLE_FLOAT32;
    const uint32_t source_element_bytes = is_float32 ? sizeof(float) : (is_uint16 ? sizeof(uint16_t) : sizeof(uint8_t));
    pipeline_description.CS = is_float32
        ? D3D12_SHADER_BYTECODE {pano_gpu_float32_sample_shader, sizeof(pano_gpu_float32_sample_shader)}
        : (is_uint16 ? D3D12_SHADER_BYTECODE {pano_gpu_uint16_sample_shader, sizeof(pano_gpu_uint16_sample_shader)}
                    : D3D12_SHADER_BYTECODE {pano_gpu_uint8_sample_shader, sizeof(pano_gpu_uint8_sample_shader)});
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    const HRESULT pipeline_result = device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline));
    if (FAILED(pipeline_result))
    {
        write_device_error(error_buffer, error_buffer_size, "cannot create D3D12 uint8 sampling pipeline", pipeline_result, device);
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_RESOURCE_DESC coordinate_buffer {};
    coordinate_buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    coordinate_buffer.Width = coordinate_bytes;
    coordinate_buffer.Height = 1;
    coordinate_buffer.DepthOrArraySize = 1;
    coordinate_buffer.MipLevels = 1;
    coordinate_buffer.SampleDesc.Count = 1;
    coordinate_buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES upload_heap {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    Microsoft::WRL::ComPtr<ID3D12Resource> coordinate_resource;
    if (FAILED(device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &coordinate_buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&coordinate_resource))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 uint8 sampling coordinate resource");
        return PANO_GPU_UNAVAILABLE;
    }
    void *mapped_coordinates = nullptr;
    if (FAILED(coordinate_resource->Map(0, nullptr, &mapped_coordinates)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 uint8 sampling coordinate resource");
        return PANO_GPU_UNAVAILABLE;
    }
    std::memcpy(mapped_coordinates, coordinates, static_cast<size_t>(coordinate_bytes));
    coordinate_resource->Unmap(0, nullptr);
    D3D12_RESOURCE_DESC output_buffer = coordinate_buffer;
    output_buffer.Width = layout.sampled_rgb_bytes;
    output_buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES default_heap {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> output;
    if (FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &output_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&output))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 uint8 sampling output");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readback_buffer = output_buffer;
    readback_buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 uint8 sampling readback");
        return PANO_GPU_UNAVAILABLE;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_description {};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 3;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&descriptor_heap))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 uint8 sampling dispatch resources");
        return PANO_GPU_UNAVAILABLE;
    }
    const UINT descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor = descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC source_srv {};
    source_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    source_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    source_srv.Format = is_float32 ? DXGI_FORMAT_R32_FLOAT : (is_uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R8_UINT);
    source_srv.Buffer.NumElements = static_cast<UINT>(session->source_bytes / source_element_bytes);
    device->CreateShaderResourceView(session->source.Get(), &source_srv, descriptor);
    descriptor.ptr += descriptor_size;
    D3D12_SHADER_RESOURCE_VIEW_DESC coordinate_srv {};
    coordinate_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    coordinate_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    coordinate_srv.Format = DXGI_FORMAT_UNKNOWN;
    coordinate_srv.Buffer.NumElements = request->coordinate_count;
    coordinate_srv.Buffer.StructureByteStride = 2 * sizeof(float);
    device->CreateShaderResourceView(coordinate_resource.Get(), &coordinate_srv, descriptor);
    descriptor.ptr += descriptor_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC output_uav {};
    output_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    output_uav.Format = DXGI_FORMAT_UNKNOWN;
    output_uav.Buffer.NumElements = request->coordinate_count;
    output_uav.Buffer.StructureByteStride = 3 * sizeof(float);
    device->CreateUnorderedAccessView(output.Get(), nullptr, &output_uav, descriptor);
    const uint32_t constants[4] {
        session->source_width, session->source_height, session->source_row_stride_bytes / source_element_bytes,
        static_cast<uint32_t>(static_cast<uint64_t>(request->frame_index) * session->source_frame_bytes /
                              source_element_bytes),
    };
    ID3D12DescriptorHeap *heaps[] = {descriptor_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_signature.Get());
    list->SetComputeRoot32BitConstants(0, 4, constants, 0);
    list->SetComputeRootDescriptorTable(1, descriptor_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((request->coordinate_count + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER to_copy {};
    to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy.Transition.pResource = output.Get();
    to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &to_copy);
    list->CopyResource(readback.Get(), output.Get());
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 uint8 sampling command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] = {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value = session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)))
    {
        write_error(error_buffer, error_buffer_size, "cannot signal D3D12 uint8 sampling fence");
        return PANO_GPU_UNAVAILABLE;
    }
    const pano_gpu_result wait_result = wait_for_fence(
        session->device_core.get(), fence_value, error_buffer, error_buffer_size, "D3D12 uint8 sampling fence timed out");
    if (wait_result != PANO_GPU_SUCCESS)
        return wait_result;
    const D3D12_RANGE read_range {0, static_cast<SIZE_T>(sampled_rgb_bytes)};
    void *mapped = nullptr;
    if (FAILED(readback->Map(0, &read_range, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 uint8 sampling readback");
        return PANO_GPU_UNAVAILABLE;
    }
    std::memcpy(sampled_rgb, mapped, static_cast<size_t>(sampled_rgb_bytes));
    readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_sample_uint16(
    const pano_gpu_session *const session, const pano_gpu_uint16_sample_request *const request,
    const void *const coordinates, const uint64_t coordinate_bytes, void *const sampled_rgb,
    const uint64_t sampled_rgb_bytes, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || session->source_sample_type != PANO_GPU_SAMPLE_UINT16)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 uint16 sampling session");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return sample_integer(
        session, request, coordinates, coordinate_bytes, sampled_rgb, sampled_rgb_bytes, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_test_sample_uint8(
    const pano_gpu_session *const session, const pano_gpu_uint8_sample_request *const request,
    const void *const coordinates, const uint64_t coordinate_bytes, void *const sampled_rgb,
    const uint64_t sampled_rgb_bytes, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || session->source_sample_type != PANO_GPU_SAMPLE_UINT8)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 uint8 sampling session");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return sample_integer(
        session, request, coordinates, coordinate_bytes, sampled_rgb, sampled_rgb_bytes, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_test_sample_float32(
    const pano_gpu_session *const session, const pano_gpu_float32_sample_request *const request,
    const void *const coordinates, const uint64_t coordinate_bytes, void *const sampled_rgb,
    const uint64_t sampled_rgb_bytes, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || session->source_sample_type != PANO_GPU_SAMPLE_FLOAT32)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 float32 sampling session");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    return sample_integer(
        session, request, coordinates, coordinate_bytes, sampled_rgb, sampled_rgb_bytes, error_buffer, error_buffer_size);
}

pano_gpu_result pano_gpu_test_validate_uint16_sample_request(
    const pano_gpu_session *const session, const pano_gpu_uint16_sample_request *const request,
    const void *const coordinates, const uint64_t coordinate_bytes, pano_gpu_uint16_sample_result_layout *const layout,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (layout == nullptr || layout->size != sizeof(*layout) || layout->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 uint16 sampling result layout");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    std::memset(layout, 0, sizeof(*layout));
    layout->size = sizeof(*layout);
    layout->abi_version = PANO_GPU_ABI_VERSION;
    if (session == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->coordinate_count == 0 ||
        request->frame_index >= session->frame_count || session->source_sample_type != PANO_GPU_SAMPLE_UINT16 ||
        coordinates == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 uint16 sampling request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    uint64_t expected_coordinate_bytes = 0;
    uint64_t sampled_rgb_bytes = 0;
    if (!checked_multiply(request->coordinate_count, 2 * sizeof(float), &expected_coordinate_bytes) ||
        !checked_multiply(request->coordinate_count, 3 * sizeof(float), &sampled_rgb_bytes) ||
        coordinate_bytes != expected_coordinate_bytes || session->source_bytes > std::numeric_limits<uint32_t>::max() ||
        session->source_frame_bytes > std::numeric_limits<uint32_t>::max() ||
        static_cast<uint64_t>(request->frame_index) * session->source_frame_bytes >
            std::numeric_limits<uint32_t>::max())
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 uint16 sampling coordinate buffer");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    const float *const coordinate_values = static_cast<const float *>(coordinates);
    for (uint32_t index = 0; index < request->coordinate_count; ++index)
    {
        if (!std::isfinite(coordinate_values[2 * index]) || !std::isfinite(coordinate_values[2 * index + 1]))
        {
            write_error(error_buffer, error_buffer_size, "invalid D3D12 uint16 sampling coordinate");
            return PANO_GPU_INVALID_ARGUMENT;
        }
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->fence || !session->source ||
        request->frame_index >= session->frame_upload_fences.size())
    {
        write_error(error_buffer, error_buffer_size, "D3D12 uint16 sampling source is not ready");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    const uint64_t frame_fence = session->frame_upload_fences[request->frame_index];
    if (frame_fence == 0 || session->device_core->fence->GetCompletedValue() < frame_fence)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 uint16 sampling source upload is unfinished");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    layout->coordinate_bytes = expected_coordinate_bytes;
    layout->sampled_rgb_bytes = sampled_rgb_bytes;
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_validate_float32_sample_request(
    const pano_gpu_session *const session, const pano_gpu_float32_sample_request *const request,
    const void *const coordinates, const uint64_t coordinate_bytes, pano_gpu_float32_sample_result_layout *const layout,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (layout == nullptr || layout->size != sizeof(*layout) || layout->abi_version != PANO_GPU_ABI_VERSION)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 float32 sampling result layout");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    std::memset(layout, 0, sizeof(*layout));
    layout->size = sizeof(*layout);
    layout->abi_version = PANO_GPU_ABI_VERSION;
    if (session == nullptr || request == nullptr || request->size != sizeof(*request) ||
        request->abi_version != PANO_GPU_ABI_VERSION || request->coordinate_count == 0 ||
        request->frame_index >= session->frame_count || session->source_sample_type != PANO_GPU_SAMPLE_FLOAT32 ||
        coordinates == nullptr)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 float32 sampling request");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    uint64_t expected_coordinate_bytes = 0;
    uint64_t sampled_rgb_bytes = 0;
    if (!checked_multiply(request->coordinate_count, 2 * sizeof(float), &expected_coordinate_bytes) ||
        !checked_multiply(request->coordinate_count, 3 * sizeof(float), &sampled_rgb_bytes) ||
        coordinate_bytes != expected_coordinate_bytes || session->source_bytes > std::numeric_limits<uint32_t>::max() ||
        session->source_frame_bytes > std::numeric_limits<uint32_t>::max() ||
        static_cast<uint64_t>(request->frame_index) * session->source_frame_bytes >
            std::numeric_limits<uint32_t>::max())
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 float32 sampling coordinate buffer");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    const float *const coordinate_values = static_cast<const float *>(coordinates);
    for (uint32_t index = 0; index < request->coordinate_count; ++index)
    {
        if (!std::isfinite(coordinate_values[2 * index]) || !std::isfinite(coordinate_values[2 * index + 1]))
        {
            write_error(error_buffer, error_buffer_size, "invalid D3D12 float32 sampling coordinate");
            return PANO_GPU_INVALID_ARGUMENT;
        }
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->device_core || !session->device_core->fence || !session->source ||
        request->frame_index >= session->frame_upload_fences.size())
    {
        write_error(error_buffer, error_buffer_size, "D3D12 float32 sampling source is not ready");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    const uint64_t frame_fence = session->frame_upload_fences[request->frame_index];
    if (frame_fence == 0 || session->device_core->fence->GetCompletedValue() < frame_fence)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 float32 sampling source upload is unfinished");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    layout->coordinate_bytes = expected_coordinate_bytes;
    layout->sampled_rgb_bytes = sampled_rgb_bytes;
    return PANO_GPU_SUCCESS;
#endif
}

void pano_gpu_test_fail_next_source_allocation(void) noexcept
{
    fail_next_source_allocation.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_rotation_allocation(void) noexcept
{
    fail_next_rotation_allocation.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_encoding_metadata_allocation(void) noexcept
{
    fail_next_encoding_metadata_allocation.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_upload_slot_allocation(void) noexcept
{
    fail_next_upload_slot_allocation.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_second_upload_slot_allocation(void) noexcept
{
    fail_next_second_upload_slot_allocation.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_encoding_metadata_upload(void) noexcept
{
    fail_next_encoding_metadata_upload.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_fail_next_fence_signal(void) noexcept
{
    fail_next_fence_signal.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_cancel_after_next_upload_slot_wait(void) noexcept
{
    cancel_after_next_upload_slot_wait.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_cancel_after_next_upload_finish_wait(void) noexcept
{
    cancel_after_next_upload_finish_wait.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_cancel_after_next_output_download_wait(void) noexcept
{
    cancel_after_next_output_download_wait.store(true, std::memory_order_relaxed);
}

void pano_gpu_test_stale_after_next_preview_wait(void) noexcept
{
    stale_after_next_preview_wait.store(true, std::memory_order_relaxed);
}

uint64_t pano_gpu_test_session_source_bytes(const pano_gpu_session *const session) noexcept
{
    return session == nullptr ? 0 : session->source_bytes;
}

uint64_t pano_gpu_test_session_rotation_bytes(const pano_gpu_session *const session) noexcept
{
    return session == nullptr ? 0 : session->rotation_bytes;
}

uint64_t pano_gpu_test_session_encoding_metadata_bytes(const pano_gpu_session *const session) noexcept
{
    return session == nullptr ? 0 : session->encoding_metadata_bytes;
}

uint64_t pano_gpu_test_session_upload_slot_bytes(const pano_gpu_session *const session) noexcept
{
    return session == nullptr ? 0 : session->upload_slot_bytes;
}

uint64_t pano_gpu_test_session_second_upload_slot_bytes(const pano_gpu_session *const session) noexcept
{
    return session == nullptr ? 0 : session->second_upload_slot_bytes;
}

uint64_t pano_gpu_test_session_first_upload_slot_fence(const pano_gpu_session *const session) noexcept
{
    return session == nullptr ? 0 : session->first_upload_slot_fence;
}

uint64_t pano_gpu_test_session_second_upload_slot_fence(const pano_gpu_session *const session) noexcept
{
    return session == nullptr ? 0 : session->second_upload_slot_fence;
}

pano_gpu_result pano_gpu_test_read_session_rotations(
    const pano_gpu_session *const session, void *const rotations, const uint64_t rotation_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || !session->device_core || rotations == nullptr ||
        rotation_bytes != session->requested_rotation_bytes || !session->rotations_uploaded)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 rotation readback");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->rotations)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 rotation storage is not allocated");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = session->rotation_bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(session->device_core->d3d_device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback))) ||
        FAILED(session->device_core->d3d_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(session->device_core->d3d_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 rotation readback resources");
        return PANO_GPU_UNAVAILABLE;
    }
    list->CopyBufferRegion(readback.Get(), 0, session->rotations.Get(), 0, rotation_bytes);
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 rotation readback command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] = {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)))
    {
        write_error(error_buffer, error_buffer_size, "cannot signal D3D12 rotation readback fence");
        return PANO_GPU_UNAVAILABLE;
    }
    HANDLE event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_handle == nullptr ||
        FAILED(session->device_core->fence->SetEventOnCompletion(fence_value, event_handle)) ||
        WaitForSingleObject(event_handle, 10000) != WAIT_OBJECT_0)
    {
        if (event_handle != nullptr)
            CloseHandle(event_handle);
        write_error(error_buffer, error_buffer_size, "D3D12 rotation readback fence timed out");
        return PANO_GPU_UNAVAILABLE;
    }
    CloseHandle(event_handle);
    const D3D12_RANGE range {0, static_cast<SIZE_T>(rotation_bytes)};
    void *mapped = nullptr;
    if (FAILED(readback->Map(0, &range, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 rotation readback resource");
        return PANO_GPU_UNAVAILABLE;
    }
    std::memcpy(rotations, mapped, static_cast<size_t>(rotation_bytes));
    readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_read_session_encoding_metadata(
    const pano_gpu_session *const session, void *const encoding_metadata,
    const uint64_t encoding_metadata_bytes, char *const error_buffer,
    const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || !session->device_core || encoding_metadata == nullptr ||
        encoding_metadata_bytes != session->requested_encoding_metadata_bytes ||
        !session->encoding_metadata_uploaded)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 encoding metadata readback");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->encoding_metadata)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 encoding metadata storage is not allocated");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = session->encoding_metadata_bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(session->device_core->d3d_device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback))) ||
        FAILED(session->device_core->d3d_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(session->device_core->d3d_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 encoding metadata readback resources");
        return PANO_GPU_UNAVAILABLE;
    }
    list->CopyBufferRegion(
        readback.Get(), 0, session->encoding_metadata.Get(), 0, encoding_metadata_bytes);
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 encoding metadata readback command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] = {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)))
    {
        write_error(error_buffer, error_buffer_size, "cannot signal D3D12 encoding metadata readback fence");
        return PANO_GPU_UNAVAILABLE;
    }
    HANDLE event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_handle == nullptr ||
        FAILED(session->device_core->fence->SetEventOnCompletion(fence_value, event_handle)) ||
        WaitForSingleObject(event_handle, 10000) != WAIT_OBJECT_0)
    {
        if (event_handle != nullptr)
            CloseHandle(event_handle);
        write_error(error_buffer, error_buffer_size, "D3D12 encoding metadata readback fence timed out");
        return PANO_GPU_UNAVAILABLE;
    }
    CloseHandle(event_handle);
    const D3D12_RANGE range {0, static_cast<SIZE_T>(encoding_metadata_bytes)};
    void *mapped = nullptr;
    if (FAILED(readback->Map(0, &range, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 encoding metadata readback resource");
        return PANO_GPU_UNAVAILABLE;
    }
    std::memcpy(encoding_metadata, mapped, static_cast<size_t>(encoding_metadata_bytes));
    readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_read_session_frame(
    const pano_gpu_session *const session, const uint32_t frame_index, void *const data,
    const uint64_t data_bytes, char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    if (session == nullptr || !session->device_core || data == nullptr ||
        frame_index >= session->frame_count || data_bytes != session->source_frame_bytes)
    {
        write_error(error_buffer, error_buffer_size, "invalid D3D12 source readback");
        return PANO_GPU_INVALID_ARGUMENT;
    }
#if !defined(_WIN32)
    write_error(error_buffer, error_buffer_size, "D3D12 is available only on Windows");
    return PANO_GPU_UNAVAILABLE;
#else
    if (!session->source)
    {
        write_error(error_buffer, error_buffer_size, "D3D12 source storage is not allocated");
        return PANO_GPU_INVALID_ARGUMENT;
    }
    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = session->upload_slot_bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(session->device_core->d3d_device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback))) ||
        FAILED(session->device_core->d3d_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(session->device_core->d3d_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
    {
        write_error(error_buffer, error_buffer_size, "cannot create D3D12 source readback resources");
        return PANO_GPU_UNAVAILABLE;
    }
    const uint64_t source_offset = static_cast<uint64_t>(frame_index) * session->source_frame_bytes;
    list->CopyBufferRegion(readback.Get(), 0, session->source.Get(), source_offset, data_bytes);
    if (FAILED(list->Close()))
    {
        write_error(error_buffer, error_buffer_size, "cannot close D3D12 source readback command list");
        return PANO_GPU_UNAVAILABLE;
    }
    ID3D12CommandList *lists[] = {list.Get()};
    session->device_core->queue->ExecuteCommandLists(1, lists);
    const uint64_t fence_value =
        session->device_core->next_fence_value.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FAILED(session->device_core->queue->Signal(session->device_core->fence.Get(), fence_value)))
    {
        write_error(error_buffer, error_buffer_size, "cannot signal D3D12 source readback fence");
        return PANO_GPU_UNAVAILABLE;
    }
    HANDLE event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_handle == nullptr ||
        FAILED(session->device_core->fence->SetEventOnCompletion(fence_value, event_handle)) ||
        WaitForSingleObject(event_handle, 10000) != WAIT_OBJECT_0)
    {
        if (event_handle != nullptr)
            CloseHandle(event_handle);
        write_error(error_buffer, error_buffer_size, "D3D12 source readback fence timed out");
        return PANO_GPU_UNAVAILABLE;
    }
    CloseHandle(event_handle);
    const D3D12_RANGE range {0, static_cast<SIZE_T>(data_bytes)};
    void *mapped = nullptr;
    if (FAILED(readback->Map(0, &range, &mapped)))
    {
        write_error(error_buffer, error_buffer_size, "cannot map D3D12 source readback resource");
        return PANO_GPU_UNAVAILABLE;
    }
    std::memcpy(data, mapped, static_cast<size_t>(data_bytes));
    readback->Unmap(0, nullptr);
    return PANO_GPU_SUCCESS;
#endif
}

pano_gpu_result pano_gpu_test_read_session_frame_zero(
    const pano_gpu_session *const session, void *const data, const uint64_t data_bytes,
    char *const error_buffer, const uint32_t error_buffer_size) noexcept
{
    return pano_gpu_test_read_session_frame(session, 0, data, data_bytes, error_buffer, error_buffer_size);
}
#endif
