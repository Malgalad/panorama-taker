#pragma once

#include <stdint.h>

#if defined(_WIN32)
#if defined(PANO_GPU_BUILDING_DLL)
#define PANO_GPU_API __declspec(dllexport)
#else
#define PANO_GPU_API __declspec(dllimport)
#endif
#else
#define PANO_GPU_API
#endif

#ifdef __cplusplus
#define PANO_GPU_NOEXCEPT noexcept
#else
#define PANO_GPU_NOEXCEPT
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    PANO_GPU_ABI_VERSION = 10,
};

typedef enum pano_gpu_result
{
    PANO_GPU_SUCCESS = 0,
    PANO_GPU_UNAVAILABLE = 1,
    PANO_GPU_CANCELLED = 2,
    PANO_GPU_INVALID_ARGUMENT = 3,
    PANO_GPU_OUT_OF_MEMORY = 4,
} pano_gpu_result;

typedef enum pano_gpu_sample_type
{
    PANO_GPU_SAMPLE_UINT8 = 1,
    PANO_GPU_SAMPLE_UINT16 = 2,
    PANO_GPU_SAMPLE_FLOAT32 = 3,
} pano_gpu_sample_type;

typedef enum pano_gpu_transfer_function
{
    PANO_GPU_TRANSFER_SRGB = 1,
    PANO_GPU_TRANSFER_PQ = 2,
    PANO_GPU_TRANSFER_LINEAR = 3,
} pano_gpu_transfer_function;

typedef enum pano_gpu_exposure_pair_rejection
{
    PANO_GPU_EXPOSURE_PAIR_ACCEPTED = 0,
    PANO_GPU_EXPOSURE_PAIR_INSUFFICIENT_VALID = 1,
    PANO_GPU_EXPOSURE_PAIR_INSUFFICIENT_INLIERS = 2,
    PANO_GPU_EXPOSURE_PAIR_NONFINITE = 3,
    PANO_GPU_EXPOSURE_PAIR_EXCESSIVE_DISPERSION = 4,
    PANO_GPU_EXPOSURE_PAIR_PENDING = 5,
} pano_gpu_exposure_pair_rejection;

typedef enum pano_gpu_exposure_invalidation_reason
{
    PANO_GPU_EXPOSURE_INVALIDATE_MANUAL_GAINS = 1,
    PANO_GPU_EXPOSURE_INVALIDATE_GEOMETRY = 2,
} pano_gpu_exposure_invalidation_reason;

typedef int (*pano_gpu_progress_callback)(
    void *user_data, uint32_t completed, uint32_t total);

typedef struct pano_gpu_probe_options
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t allow_warp;
} pano_gpu_probe_options;

typedef struct pano_gpu_adapter_info
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t vendor_id;
    uint32_t device_id;
    uint64_t luid;
    uint64_t dedicated_bytes;
    uint64_t local_budget_bytes;
    uint64_t local_usage_bytes;
    char name[128];
} pano_gpu_adapter_info;

typedef struct pano_gpu_memory_request
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t frame_count;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t source_sample_bytes;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t output_sample_bytes;
    uint32_t needs_sdr_conversion;
    uint64_t free_bytes;
    uint64_t total_bytes;
    uint64_t requested_budget_bytes;
    uint64_t preview_cache_bytes;
    uint64_t session_workspace_bytes;
    uint64_t output_workspace_bytes_per_pixel;
    uint64_t output_workspace_fixed_bytes;
    uint64_t upload_bytes;
    uint64_t readback_bytes_per_pixel;
    uint64_t readback_fixed_bytes;
    uint32_t descriptor_count;
    uint32_t reserved;
} pano_gpu_memory_request;

typedef struct pano_gpu_memory_plan
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t output_band_rows;
    uint32_t descriptor_count;
    uint64_t source_bytes;
    uint64_t session_workspace_bytes;
    uint64_t output_workspace_bytes;
    uint64_t upload_bytes;
    uint64_t readback_bytes;
    uint64_t reserve_bytes;
    uint64_t required_bytes;
    uint64_t available_bytes;
} pano_gpu_memory_plan;

typedef struct pano_gpu_diagnostics
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t live_device_count;
    uint32_t live_queue_count;
    uint32_t live_fence_count;
    uint32_t live_session_count;
    uint32_t live_output_count;
} pano_gpu_diagnostics;

typedef struct pano_gpu_device_diagnostics
{
    uint32_t size;
    uint32_t abi_version;
    pano_gpu_adapter_info adapter;
    uint64_t usable_local_bytes;
} pano_gpu_device_diagnostics;

typedef struct pano_gpu_session_create_options
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t frame_count;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t source_sample_type;
    uint32_t transfer_function;
    uint32_t source_row_stride_bytes;
    uint64_t device_luid;
    const void *rotations;
    uint64_t rotations_bytes;
    const void *encoding_metadata;
    uint64_t encoding_metadata_bytes;
} pano_gpu_session_create_options;

typedef struct pano_gpu_session_diagnostics
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t descriptor_count;
    uint64_t planned_source_bytes;
    uint64_t source_bytes;
    uint64_t planned_rotation_bytes;
    uint64_t rotation_bytes;
    uint64_t planned_encoding_metadata_bytes;
    uint64_t encoding_metadata_bytes;
    uint32_t upload_count;
    uint64_t uploaded_bytes;
    uint64_t last_completed_upload_fence;
} pano_gpu_session_diagnostics;

typedef struct pano_gpu_source_upload
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t frame_index;
    uint32_t source_sample_type;
    uint32_t source_row_stride_bytes;
    const void *data;
    uint64_t data_bytes;
} pano_gpu_source_upload;

typedef struct pano_gpu_output_create_options
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t output_sample_bytes;
    uint32_t output_band_rows;
    uint32_t descriptor_count;
    uint64_t output_workspace_bytes;
} pano_gpu_output_create_options;

typedef struct pano_gpu_output_diagnostics
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t is_banded;
    uint32_t output_band_rows;
    uint32_t band_row_start;
    uint32_t band_row_count;
    uint64_t planned_linear_bytes;
    uint64_t linear_bytes;
    uint64_t planned_coverage_bytes;
    uint64_t coverage_bytes;
} pano_gpu_output_diagnostics;

typedef struct pano_gpu_output_download_request
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t output_width;
    uint32_t row_start;
    uint32_t row_count;
    void *data;
    uint64_t data_bytes;
} pano_gpu_output_download_request;

typedef struct pano_gpu_output_transfer_diagnostics
{
    uint32_t size;
    uint32_t abi_version;
    uint64_t readback_bytes;
    uint64_t download_count;
    uint64_t downloaded_bytes;
} pano_gpu_output_transfer_diagnostics;

typedef struct pano_gpu_preview_create_options
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t frame_count;
    uint32_t preview_width;
    uint32_t preview_height;
    uint32_t overview_width;
    uint32_t overview_height;
    uint32_t mask_width;
    uint32_t mask_height;
    const uint8_t *preview_rgb8;
    uint64_t preview_rgb8_bytes;
    const uint8_t *overview_rgb8;
    uint64_t overview_rgb8_bytes;
    const uint8_t *compact_masks;
    uint64_t compact_mask_bytes;
} pano_gpu_preview_create_options;

typedef struct pano_gpu_preview_diagnostics
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t frame_count;
    uint32_t preview_width;
    uint32_t preview_height;
    uint32_t overview_width;
    uint32_t overview_height;
    uint32_t mask_width;
    uint32_t mask_height;
    uint32_t live_preview_count;
    uint64_t preview_rgb8_bytes;
    uint64_t overview_rgb8_bytes;
    uint64_t compact_mask_bytes;
    uint64_t retained_bytes;
} pano_gpu_preview_diagnostics;

typedef struct pano_gpu_preview_render_request
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t crop_left;
    uint32_t crop_top;
    uint32_t crop_width;
    uint32_t crop_height;
    uint32_t use_overview;
    uint8_t *output_rgb8;
    uint64_t output_rgb8_bytes;
} pano_gpu_preview_render_request;

typedef struct pano_gpu_preview_overlay_request
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t crop_left;
    uint32_t crop_top;
    uint32_t crop_width;
    uint32_t crop_height;
    uint32_t use_overview;
    const uint8_t *hovered_frames;
    uint64_t hovered_frame_bytes;
    int32_t target_pose;
    uint32_t target_mode;
    uint32_t show_boundaries;
    uint8_t *output_rgb8;
    uint64_t output_rgb8_bytes;
} pano_gpu_preview_overlay_request;

typedef struct pano_gpu_preview_surface_create_options
{
    uint32_t size;
    uint32_t abi_version;
    uint64_t native_window;
    uint32_t width;
    uint32_t height;
} pano_gpu_preview_surface_create_options;

typedef struct pano_gpu_preview_surface_diagnostics
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t width;
    uint32_t height;
    uint64_t present_count;
    uint64_t resize_count;
    uint32_t occluded;
    uint32_t live_surface_count;
    uint32_t device_lost;
    uint32_t reserved;
} pano_gpu_preview_surface_diagnostics;

typedef struct pano_gpu_preview_surface_present_request
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t crop_left;
    uint32_t crop_top;
    uint32_t crop_width;
    uint32_t crop_height;
    uint32_t use_overview;
} pano_gpu_preview_surface_present_request;

typedef struct pano_gpu_preview_surface_overlay_request
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t crop_left;
    uint32_t crop_top;
    uint32_t crop_width;
    uint32_t crop_height;
    uint32_t use_overview;
    const uint8_t *hovered_frames;
    uint64_t hovered_frame_bytes;
    int32_t target_pose;
    uint32_t target_mode;
    uint32_t show_boundaries;
} pano_gpu_preview_surface_overlay_request;

typedef struct pano_gpu_projection_request
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t row_start;
    uint32_t row_count;
    float latitude_span_degrees;
    float horizontal_fov_degrees;
    float vertical_fov_degrees;
    float world_to_camera[9];
} pano_gpu_projection_request;

typedef struct pano_gpu_projection_result_layout
{
    uint32_t size;
    uint32_t abi_version;
    uint64_t pixel_count;
    uint64_t world_ray_bytes;
    uint64_t camera_ray_bytes;
    uint64_t projected_coordinate_bytes;
    uint64_t validity_bytes;
} pano_gpu_projection_result_layout;

typedef struct pano_gpu_one_frame_composite_request
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t frame_index;
    uint32_t source_sample_type;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t row_start;
    uint32_t row_count;
    float latitude_span_degrees;
    float horizontal_fov_degrees;
    float vertical_fov_degrees;
    float world_to_camera[9];
    uint32_t rectilinear_output;
    float output_vertical_fov_degrees;
} pano_gpu_one_frame_composite_request;

typedef struct pano_gpu_one_frame_composite_result_layout
{
    uint32_t size;
    uint32_t abi_version;
    uint64_t pixel_count;
    uint64_t linear_rgb_bytes;
    uint64_t coverage_bytes;
    uint64_t candidate_edge_distance_bytes;
} pano_gpu_one_frame_composite_result_layout;

typedef struct pano_gpu_hard_selection_request
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t pixel_count;
    uint32_t reserved;
} pano_gpu_hard_selection_request;

typedef struct pano_gpu_hard_selection_result_layout
{
    uint32_t size;
    uint32_t abi_version;
    uint64_t candidate_rgb_bytes;
    uint64_t candidate_validity_bytes;
    uint64_t candidate_edge_distance_bytes;
    uint64_t prior_rgb_bytes;
    uint64_t prior_weight_bytes;
    uint64_t selected_rgb_bytes;
    uint64_t selected_weight_bytes;
    uint64_t coverage_bytes;
} pano_gpu_hard_selection_result_layout;

typedef struct pano_gpu_feather_accumulation_request
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t pixel_count;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t reserved;
} pano_gpu_feather_accumulation_request;

typedef struct pano_gpu_feather_accumulation_result_layout
{
    uint32_t size;
    uint32_t abi_version;
    uint64_t candidate_rgb_bytes;
    uint64_t candidate_validity_bytes;
    uint64_t candidate_edge_distance_bytes;
    uint64_t accumulator_rgb_bytes;
    uint64_t accumulator_weight_bytes;
} pano_gpu_feather_accumulation_result_layout;

typedef struct pano_gpu_exposure_request
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t frame_count;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t local_field_width;
    uint32_t local_field_height;
    uint32_t reserved;
} pano_gpu_exposure_request;

typedef struct pano_gpu_exposure_result_layout
{
    uint32_t size;
    uint32_t abi_version;
    uint64_t global_gain_bytes;
    uint64_t local_field_bytes;
} pano_gpu_exposure_result_layout;

typedef struct pano_gpu_exposure_proxy_request
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t frame_count;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t reserved;
} pano_gpu_exposure_proxy_request;

typedef struct pano_gpu_exposure_proxy_layout
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t proxy_width;
    uint32_t proxy_height;
    uint64_t proxy_frame_offset_bytes;
    uint64_t proxy_frame_bytes;
    uint64_t proxy_total_bytes;
} pano_gpu_exposure_proxy_layout;

typedef struct pano_gpu_exposure_pair_request
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t first_frame_index;
    uint32_t second_frame_index;
    uint32_t sample_width;
    uint32_t sample_height;
    float latitude_span_degrees;
    float horizontal_fov_degrees;
    float vertical_fov_degrees;
    uint32_t reserved;
} pano_gpu_exposure_pair_request;

typedef struct pano_gpu_exposure_pair_layout
{
    uint32_t size;
    uint32_t abi_version;
    uint64_t sample_count;
    uint64_t paired_coordinate_bytes;
    uint64_t overlap_bytes;
} pano_gpu_exposure_pair_layout;

typedef struct pano_gpu_exposure_pair_scratch_diagnostics
{
    uint32_t size;
    uint32_t abi_version;
    uint64_t sample_count;
    uint64_t sortable_capacity;
    uint64_t device_bytes;
    uint64_t readback_bytes;
    uint32_t resource_count;
    uint32_t reserved;
} pano_gpu_exposure_pair_scratch_diagnostics;

typedef struct pano_gpu_exposure_pair_reduction
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t rejection_reason;
    uint32_t valid_count;
    uint32_t inlier_count;
    float difference;
    float mad;
    float weight;
    uint64_t downloaded_bytes;
} pano_gpu_exposure_pair_reduction;

typedef struct pano_gpu_exposure_equation
{
    uint32_t left_frame_index;
    uint32_t right_frame_index;
    double difference;
    double weight;
} pano_gpu_exposure_equation;

typedef struct pano_gpu_exposure_pair_report
{
    uint32_t left_frame_index;
    uint32_t right_frame_index;
    uint32_t rejection_reason;
    uint32_t valid_count;
    uint32_t inlier_count;
    uint32_t geometric_count;
} pano_gpu_exposure_pair_report;

typedef struct pano_gpu_exposure_graph_diagnostics
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t pair_capacity;
    uint32_t pair_report_count;
    uint32_t equation_count;
    uint32_t solve_equation_count;
} pano_gpu_exposure_graph_diagnostics;

typedef struct pano_gpu_exposure_solve_result
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t anchor_frame_index;
    uint32_t edge_count;
    uint32_t frame_count;
    uint32_t reserved;
} pano_gpu_exposure_solve_result;

typedef struct pano_gpu_exposure_report
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t anchor_frame_index;
    uint32_t edge_count;
    uint32_t frame_count;
    uint32_t gains_uploaded;
    uint32_t solve_count;
    uint32_t gain_upload_count;
} pano_gpu_exposure_report;

typedef struct pano_gpu_histogram_request
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t output_width;
    uint32_t output_height;
} pano_gpu_histogram_request;

typedef struct pano_gpu_histogram_layout
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t bin_count;
    uint32_t counter_bytes;
    uint64_t histogram_bytes;
    uint64_t maximum_population;
} pano_gpu_histogram_layout;

typedef struct pano_gpu_histogram_diagnostics
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t clear_count;
    uint32_t accumulated_band_count;
    uint64_t histogram_bytes;
    uint64_t accumulated_pixels;
} pano_gpu_histogram_diagnostics;

typedef struct pano_gpu_auto_contrast_levels
{
    uint32_t size;
    uint32_t abi_version;
    float black;
    float white;
    uint32_t processed_pixels;
    uint32_t reserved;
} pano_gpu_auto_contrast_levels;

typedef struct pano_gpu_ordered_hard_composite_request
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t frame_request_count;
    uint32_t reserved;
    const pano_gpu_one_frame_composite_request *frame_requests;
} pano_gpu_ordered_hard_composite_request;

typedef struct pano_gpu_composite_inputs
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t use_session_exposure_gains;
    uint32_t mark_incomplete;
    const float *global_gains;
    uint64_t global_gain_bytes;
    const float *local_fields;
    uint64_t local_field_bytes;
    uint32_t reserved;
} pano_gpu_composite_inputs;

typedef struct pano_gpu_ordered_hard_composite_result_layout
{
    uint32_t size;
    uint32_t abi_version;
    uint64_t pixel_count;
    uint64_t selected_rgb_bytes;
    uint64_t selected_weight_bytes;
    uint64_t coverage_bytes;
} pano_gpu_ordered_hard_composite_result_layout;

typedef struct pano_gpu_uint8_sample_request
{
    uint32_t size;
    uint32_t abi_version;
    uint32_t frame_index;
    uint32_t coordinate_count;
} pano_gpu_uint8_sample_request;

typedef struct pano_gpu_uint8_sample_result_layout
{
    uint32_t size;
    uint32_t abi_version;
    uint64_t coordinate_bytes;
    uint64_t sampled_rgb_bytes;
} pano_gpu_uint8_sample_result_layout;

typedef pano_gpu_uint8_sample_request pano_gpu_uint16_sample_request;
typedef pano_gpu_uint8_sample_result_layout pano_gpu_uint16_sample_result_layout;
typedef pano_gpu_uint8_sample_request pano_gpu_float32_sample_request;
typedef pano_gpu_uint8_sample_result_layout pano_gpu_float32_sample_result_layout;


typedef struct pano_gpu_cancellation_token pano_gpu_cancellation_token;
typedef struct pano_gpu_device pano_gpu_device;
/* A successful upload remains resident until pano_gpu_session_destroy. */
typedef struct pano_gpu_session pano_gpu_session;
typedef struct pano_gpu_output pano_gpu_output;
typedef struct pano_gpu_preview pano_gpu_preview;
typedef struct pano_gpu_preview_surface pano_gpu_preview_surface;

PANO_GPU_API uint32_t pano_gpu_abi_version(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_probe(char *error_buffer, uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_probe_adapter(
    const pano_gpu_probe_options *options,
    pano_gpu_adapter_info *adapter,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_dispatch_self_test(
    uint32_t allow_warp, char *error_buffer, uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_plan_memory(
    const pano_gpu_memory_request *request,
    pano_gpu_memory_plan *plan,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_query_diagnostics(
    pano_gpu_diagnostics *diagnostics,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_device_create(
    const pano_gpu_probe_options *options,
    pano_gpu_device **device,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_device_dispatch_self_test(
    pano_gpu_device *device,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_device_query_diagnostics(
    const pano_gpu_device *device,
    pano_gpu_device_diagnostics *diagnostics,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_validate_session_create_options(
    const pano_gpu_device *device,
    const pano_gpu_session_create_options *options,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_create(
    pano_gpu_device *device,
    const pano_gpu_session_create_options *options,
    pano_gpu_session **session,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_allocate_source(
    pano_gpu_session *session,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_allocate_rotations(
    pano_gpu_session *session,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_upload_rotations(
    pano_gpu_session *session,
    const void *rotations,
    uint64_t rotation_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_allocate_encoding_metadata(
    pano_gpu_session *session,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_upload_encoding_metadata(
    pano_gpu_session *session,
    const void *encoding_metadata,
    uint64_t encoding_metadata_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_query_diagnostics(
    const pano_gpu_session *session,
    pano_gpu_session_diagnostics *diagnostics,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_prepare_exposure_graph(
    pano_gpu_session *session,
    uint32_t pair_capacity,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_exposure_pair_count(
    uint32_t frame_count,
    uint32_t *pair_count,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_enumerate_exposure_pairs(
    pano_gpu_session *session,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_copy_exposure_pair_reports(
    const pano_gpu_session *session,
    pano_gpu_exposure_pair_report *reports,
    uint64_t report_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_copy_exposure_equations(
    const pano_gpu_session *session,
    pano_gpu_exposure_equation *equations,
    uint64_t equation_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_build_exposure_solve_graph(
    pano_gpu_session *session,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_copy_exposure_solve_equations(
    const pano_gpu_session *session,
    pano_gpu_exposure_equation *equations,
    uint64_t equation_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_solve_exposure_graph(
    pano_gpu_session *session,
    pano_gpu_exposure_solve_result *result,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_copy_exposure_log_gains(
    const pano_gpu_session *session,
    float *log_gains,
    uint64_t log_gain_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_upload_exposure_gains(
    pano_gpu_session *session,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_query_exposure_report(
    const pano_gpu_session *session,
    pano_gpu_exposure_report *report,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_invalidate_exposure(
    pano_gpu_session *session,
    uint32_t reason,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_plan_auto_contrast_histogram(
    const pano_gpu_histogram_request *request,
    pano_gpu_histogram_layout *layout,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_reduce_exposure_graph(
    pano_gpu_session *session,
    const pano_gpu_exposure_pair_request *request_template,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_reduce_reference_exposure_graph(
    pano_gpu_session *session,
    const pano_gpu_exposure_pair_request *request_template,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_reduce_reference_exposure_graph_progress(
    pano_gpu_session *session,
    const pano_gpu_exposure_pair_request *request_template,
    pano_gpu_progress_callback progress,
    void *progress_user_data,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_query_exposure_graph(
    const pano_gpu_session *session,
    pano_gpu_exposure_graph_diagnostics *diagnostics,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_session_clear_exposure_graph(pano_gpu_session *session) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_prepare_exposure_pair_scratch(
    pano_gpu_session *session,
    const pano_gpu_exposure_pair_request *request,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_query_exposure_pair_scratch(
    const pano_gpu_session *session,
    pano_gpu_exposure_pair_scratch_diagnostics *diagnostics,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_session_clear_exposure_pair_scratch(pano_gpu_session *session) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_dispatch_exposure_pair_projection_samples(
    pano_gpu_session *session,
    const pano_gpu_exposure_pair_request *request,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_dispatch_exposure_pair_classification(
    pano_gpu_session *session,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_dispatch_exposure_pair_gradient_limits(
    pano_gpu_session *session,
    uint32_t sample_width,
    uint32_t sample_height,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_dispatch_exposure_pair_filter_ratios(
    pano_gpu_session *session,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_dispatch_exposure_pair_trim(
    pano_gpu_session *session,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_reduce_exposure_pair(
    pano_gpu_session *session,
    pano_gpu_exposure_pair_reduction *reduction,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_validate_source_upload(
    const pano_gpu_session *session,
    const pano_gpu_source_upload *upload,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_allocate_upload_slot(
    pano_gpu_session *session,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_allocate_second_upload_slot(
    pano_gpu_session *session,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_upload_frame_zero(
    pano_gpu_session *session,
    const pano_gpu_source_upload *upload,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_upload_frame(
    pano_gpu_session *session,
    const pano_gpu_source_upload *upload,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_upload_frame_cancellable(
    pano_gpu_session *session,
    const pano_gpu_source_upload *upload,
    const pano_gpu_cancellation_token *token,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_finish_uploads(
    pano_gpu_session *session,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_finish_uploads_cancellable(
    pano_gpu_session *session,
    const pano_gpu_cancellation_token *token,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_session_build_exposure_proxies(
    pano_gpu_session *session,
    const pano_gpu_exposure_proxy_request *request,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_validate_output_create_options(
    const pano_gpu_session *session,
    const pano_gpu_output_create_options *options,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_create_empty(
    pano_gpu_session *session,
    const pano_gpu_output_create_options *options,
    pano_gpu_output **output,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_allocate_linear(
    pano_gpu_output *output,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_allocate_coverage(
    pano_gpu_output *output,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_query_diagnostics(
    const pano_gpu_output *output,
    pano_gpu_output_diagnostics *diagnostics,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_compose_hard(
    pano_gpu_output *output,
    const pano_gpu_ordered_hard_composite_request *request,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_compose_feather(
    pano_gpu_output *output,
    const pano_gpu_ordered_hard_composite_request *request,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_compose_hard_with_inputs(
    pano_gpu_output *output,
    const pano_gpu_ordered_hard_composite_request *request,
    const pano_gpu_composite_inputs *inputs,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_compose_feather_with_inputs(
    pano_gpu_output *output,
    const pano_gpu_ordered_hard_composite_request *request,
    const pano_gpu_composite_inputs *inputs,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_prepare_auto_contrast_histogram(
    pano_gpu_output *output,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_accumulate_auto_contrast_histogram_srgb(
    pano_gpu_output *output,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_accumulate_auto_contrast_histogram_converted_srgb(
    pano_gpu_output *output,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_query_histogram_diagnostics(
    const pano_gpu_output *output,
    pano_gpu_histogram_diagnostics *diagnostics,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_select_auto_contrast_levels(
    pano_gpu_output *output,
    pano_gpu_auto_contrast_levels *levels,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_apply_auto_contrast_srgb(
    pano_gpu_output *output,
    uint32_t apply_levels,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_apply_auto_contrast_converted_srgb(
    pano_gpu_output *output,
    uint32_t apply_levels,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_quantize_normalized_srgb8(
    pano_gpu_output *output,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_tone_map_rec2020(
    pano_gpu_output *output,
    float reference_white_nits,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_convert_tone_mapped_rec2020_to_linear_srgb(
    pano_gpu_output *output,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_copy_linear_float(
    pano_gpu_output *output,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_download_srgb8(
    pano_gpu_output *output,
    const pano_gpu_output_download_request *request,
    const pano_gpu_cancellation_token *token,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_download_float(
    pano_gpu_output *output,
    const pano_gpu_output_download_request *request,
    const pano_gpu_cancellation_token *token,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;

PANO_GPU_API pano_gpu_result pano_gpu_output_download_coverage(
    pano_gpu_output *output,
    const pano_gpu_output_download_request *request,
    const pano_gpu_cancellation_token *token,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_output_query_transfer_diagnostics(
    const pano_gpu_output *output,
    pano_gpu_output_transfer_diagnostics *diagnostics,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_preview_create(
    pano_gpu_session *session,
    const pano_gpu_preview_create_options *options,
    pano_gpu_preview **preview,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_preview_query_diagnostics(
    const pano_gpu_preview *preview,
    pano_gpu_preview_diagnostics *diagnostics,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_preview_render_base(
    pano_gpu_preview *preview,
    const pano_gpu_preview_render_request *request,
    const pano_gpu_cancellation_token *token,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_preview_render_overlay(
    pano_gpu_preview *preview,
    const pano_gpu_preview_overlay_request *request,
    const pano_gpu_cancellation_token *token,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_preview_set_generation(
    pano_gpu_preview *preview,
    uint64_t generation,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_preview_render_overlay_generation(
    pano_gpu_preview *preview,
    const pano_gpu_preview_overlay_request *request,
    uint64_t generation,
    const pano_gpu_cancellation_token *token,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_preview_surface_create(
    pano_gpu_device *device,
    const pano_gpu_preview_surface_create_options *options,
    pano_gpu_preview_surface **surface,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_preview_surface_resize(
    pano_gpu_preview_surface *surface,
    uint32_t width,
    uint32_t height,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_preview_surface_clear_present(
    pano_gpu_preview_surface *surface,
    const float rgba[4],
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_preview_surface_present_base(
    pano_gpu_preview_surface *surface,
    pano_gpu_preview *preview,
    const pano_gpu_preview_surface_present_request *request,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_preview_surface_present_overlay(
    pano_gpu_preview_surface *surface,
    pano_gpu_preview *preview,
    const pano_gpu_preview_surface_overlay_request *request,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_preview_surface_query_diagnostics(
    const pano_gpu_preview_surface *surface,
    pano_gpu_preview_surface_diagnostics *diagnostics,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_cancellation_token_create(
    pano_gpu_cancellation_token **token, char *error_buffer, uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_cancellation_token_cancel(pano_gpu_cancellation_token *token) PANO_GPU_NOEXCEPT;
PANO_GPU_API int32_t pano_gpu_cancellation_token_is_cancelled(const pano_gpu_cancellation_token *token) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_cancellation_token_destroy(pano_gpu_cancellation_token **token) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_device_destroy(pano_gpu_device **device) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_session_destroy(pano_gpu_session **session) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_output_destroy(pano_gpu_output **output) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_preview_destroy(pano_gpu_preview **preview) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_preview_surface_destroy(
    pano_gpu_preview_surface **surface) PANO_GPU_NOEXCEPT;

#if defined(PANO_GPU_TEST_HOOKS)
PANO_GPU_API void pano_gpu_test_fail_next_allocation(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_device_creation(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_pipeline_creation(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_descriptor_creation(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_resource_creation(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_composite_before_dispatch(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_composite_after_dispatch(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_device_removed_before_dispatch(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_device_removed_after_dispatch(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_download_allocation(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_download_submission(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_download_fence_wait(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_download_map(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_fence_wait(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_convert_linear_srgb(
    const pano_gpu_session *session,
    const float *linear_rgb,
    uint32_t pixel_count,
    float *normalized_srgb,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_build_linear_srgb_histogram(
    const pano_gpu_session *session,
    const float *linear_rgb,
    const uint8_t *coverage,
    uint32_t pixel_count,
    uint32_t *histogram,
    uint64_t histogram_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_upload_output_histogram_band(
    pano_gpu_output *output,
    const float *linear_rgb,
    uint64_t linear_rgb_bytes,
    const uint8_t *coverage,
    uint64_t coverage_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_read_output_histogram(
    pano_gpu_output *output,
    uint32_t *histogram,
    uint64_t histogram_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_read_output_normalized_srgb(
    pano_gpu_output *output,
    float *normalized_srgb,
    uint64_t normalized_srgb_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_read_output_srgb8(
    pano_gpu_output *output,
    uint8_t *srgb,
    uint64_t srgb_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_read_output_tone_mapped_rec2020(
    pano_gpu_output *output,
    float *tone_mapped_rec2020,
    uint64_t tone_mapped_rec2020_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_read_output_linear_srgb(
    pano_gpu_output *output,
    float *linear_srgb,
    uint64_t linear_srgb_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_read_output_float(
    pano_gpu_output *output,
    float *linear_rgb,
    uint64_t linear_rgb_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_cancel_after_next_output_download_wait(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_stale_after_next_preview_wait(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_read_preview_retained(
    pano_gpu_preview *preview,
    uint8_t *preview_rgb8,
    uint64_t preview_rgb8_bytes,
    uint8_t *overview_rgb8,
    uint64_t overview_rgb8_bytes,
    uint8_t *compact_masks,
    uint64_t compact_mask_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_replace_exposure_graph(
    pano_gpu_session *session,
    const pano_gpu_exposure_pair_report *reports,
    uint32_t report_count,
    const pano_gpu_exposure_equation *equations,
    uint32_t equation_count,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_session_allocation(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_output_allocation(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_preview_allocation(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API uint32_t pano_gpu_test_live_preview_count(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API int32_t pano_gpu_test_claim_preview_rendering(pano_gpu_preview *preview) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_release_preview_rendering(pano_gpu_preview *preview) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_read_preview_surface(
    pano_gpu_preview_surface *surface,
    uint8_t *rgba8,
    uint64_t rgba8_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API uint64_t pano_gpu_test_preview_surface_sdr_color_space_set_count(
    const pano_gpu_preview_surface *surface) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_preview_surface_device_removed(
    void) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_validate_projection_request(
    const pano_gpu_session *session,
    const pano_gpu_projection_request *request,
    pano_gpu_projection_result_layout *layout,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_rays(
    const pano_gpu_session *session,
    const pano_gpu_projection_request *request,
    void *world_rays,
    uint64_t world_ray_bytes,
    void *projected_coordinates,
    uint64_t projected_coordinate_bytes,
    void *validity,
    uint64_t validity_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_validate_one_frame_composite_request(
    const pano_gpu_session *session,
    const pano_gpu_one_frame_composite_request *request,
    pano_gpu_one_frame_composite_result_layout *layout,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_one_frame_projection(
    const pano_gpu_session *session,
    const pano_gpu_one_frame_composite_request *request,
    void *projected_coordinates,
    uint64_t projected_coordinate_bytes,
    void *validity,
    uint64_t validity_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_one_frame_uint8_candidates(
    const pano_gpu_session *session,
    const pano_gpu_one_frame_composite_request *request,
    void *candidate_rgb,
    uint64_t candidate_rgb_bytes,
    void *validity,
    uint64_t validity_bytes,
    void *candidate_edge_distance,
    uint64_t candidate_edge_distance_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_one_frame_uint16_candidates(
    const pano_gpu_session *session,
    const pano_gpu_one_frame_composite_request *request,
    void *candidate_rgb,
    uint64_t candidate_rgb_bytes,
    void *validity,
    uint64_t validity_bytes,
    void *candidate_edge_distance,
    uint64_t candidate_edge_distance_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_one_frame_float32_candidates(
    const pano_gpu_session *session,
    const pano_gpu_one_frame_composite_request *request,
    void *candidate_rgb,
    uint64_t candidate_rgb_bytes,
    void *validity,
    uint64_t validity_bytes,
    void *candidate_edge_distance,
    uint64_t candidate_edge_distance_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_validate_hard_selection_request(
    const pano_gpu_session *session,
    const pano_gpu_hard_selection_request *request,
    const void *candidate_rgb,
    uint64_t candidate_rgb_bytes,
    const void *candidate_validity,
    uint64_t candidate_validity_bytes,
    const void *candidate_edge_distance,
    uint64_t candidate_edge_distance_bytes,
    const void *prior_rgb,
    uint64_t prior_rgb_bytes,
    const void *prior_weight,
    uint64_t prior_weight_bytes,
    pano_gpu_hard_selection_result_layout *layout,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_validate_feather_accumulation_request(
    const pano_gpu_session *session,
    const pano_gpu_feather_accumulation_request *request,
    const void *candidate_rgb,
    uint64_t candidate_rgb_bytes,
    const void *candidate_validity,
    uint64_t candidate_validity_bytes,
    const void *candidate_edge_distance,
    uint64_t candidate_edge_distance_bytes,
    const void *accumulator_rgb,
    uint64_t accumulator_rgb_bytes,
    const void *accumulator_weight,
    uint64_t accumulator_weight_bytes,
    pano_gpu_feather_accumulation_result_layout *layout,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_validate_exposure_request(
    const pano_gpu_session *session,
    const pano_gpu_exposure_request *request,
    const void *global_gains,
    uint64_t global_gain_bytes,
    const void *local_field,
    uint64_t local_field_bytes,
    pano_gpu_exposure_result_layout *layout,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_plan_exposure_proxies(
    const pano_gpu_session *session,
    const pano_gpu_exposure_proxy_request *request,
    pano_gpu_exposure_proxy_layout *layout,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_uint8_exposure_proxies(
    const pano_gpu_session *session,
    const pano_gpu_exposure_proxy_request *request,
    void *proxies,
    uint64_t proxy_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_uint16_exposure_proxies(
    const pano_gpu_session *session,
    const pano_gpu_exposure_proxy_request *request,
    void *proxies,
    uint64_t proxy_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_float32_exposure_proxies(
    const pano_gpu_session *session,
    const pano_gpu_exposure_proxy_request *request,
    void *proxies,
    uint64_t proxy_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API uint64_t pano_gpu_test_session_exposure_proxy_bytes(const pano_gpu_session *session) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_validate_exposure_pair_request(
    const pano_gpu_session *session,
    const pano_gpu_exposure_pair_request *request,
    const void *paired_coordinates,
    uint64_t paired_coordinate_bytes,
    const void *overlap,
    uint64_t overlap_bytes,
    pano_gpu_exposure_pair_layout *layout,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_exposure_pair_projection(
    const pano_gpu_session *session,
    const pano_gpu_exposure_pair_request *request,
    void *paired_coordinates,
    uint64_t paired_coordinate_bytes,
    void *overlap,
    uint64_t overlap_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_exposure_pair_samples(
    const pano_gpu_session *session,
    const pano_gpu_exposure_pair_request *request,
    const void *paired_coordinates,
    uint64_t paired_coordinate_bytes,
    const void *geometric_overlap,
    uint64_t geometric_overlap_bytes,
    void *sampled_pairs,
    uint64_t sampled_pair_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_read_resident_exposure_pair_projection_samples(
    const pano_gpu_session *session,
    void *paired_coordinates,
    uint64_t paired_coordinate_bytes,
    void *overlap,
    uint64_t overlap_bytes,
    void *sampled_pairs,
    uint64_t sampled_pair_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_read_resident_exposure_pair_classification(
    const pano_gpu_session *session,
    void *pair_luminance,
    uint64_t pair_luminance_bytes,
    void *accepted,
    uint64_t accepted_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_read_resident_exposure_pair_gradient_limits(
    const pano_gpu_session *session,
    void *gradients,
    uint64_t gradient_bytes,
    float *gradient_limits,
    uint64_t gradient_limit_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_read_resident_exposure_pair_filter_ratios(
    const pano_gpu_session *session,
    void *accepted,
    uint64_t accepted_bytes,
    void *log_ratios,
    uint64_t log_ratio_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_read_resident_exposure_pair_trim(
    const pano_gpu_session *session,
    void *sorted_ratios,
    uint64_t sorted_ratio_bytes,
    float *trim_bounds,
    uint64_t trim_bound_bytes,
    void *trimmed,
    uint64_t trimmed_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_classify_exposure_pair_samples(
    const pano_gpu_session *session,
    const pano_gpu_exposure_pair_request *request,
    const void *sampled_pairs,
    uint64_t sampled_pair_bytes,
    const void *geometric_overlap,
    uint64_t geometric_overlap_bytes,
    void *pair_luminance,
    uint64_t pair_luminance_bytes,
    void *accepted,
    uint64_t accepted_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_exposure_pair_gradients(
    const pano_gpu_session *session,
    const pano_gpu_exposure_pair_request *request,
    const void *pair_luminance,
    uint64_t pair_luminance_bytes,
    void *gradients,
    uint64_t gradient_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_compute_exposure_pair_gradient_limits(
    const pano_gpu_session *session,
    const pano_gpu_exposure_pair_request *request,
    const void *gradients,
    uint64_t gradient_bytes,
    float *gradient_limits,
    uint64_t gradient_limit_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_filter_exposure_pair_acceptance(
    const pano_gpu_session *session,
    const pano_gpu_exposure_pair_request *request,
    const void *gradients,
    uint64_t gradient_bytes,
    const float *gradient_limits,
    uint64_t gradient_limit_bytes,
    void *accepted,
    uint64_t accepted_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_build_exposure_pair_ratios(
    pano_gpu_session *session,
    const pano_gpu_exposure_pair_request *request,
    const void *pair_luminance,
    uint64_t pair_luminance_bytes,
    const void *accepted,
    uint64_t accepted_bytes,
    void *log_ratios,
    uint64_t log_ratio_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_prepare_exposure_pair_sort(
    pano_gpu_session *session,
    const pano_gpu_exposure_pair_request *request,
    void *sortable_ratios,
    uint64_t sortable_ratio_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_sort_exposure_pair(
    pano_gpu_session *session,
    void *sorted_ratios,
    uint64_t sorted_ratio_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_extract_exposure_pair_trim_bounds(
    pano_gpu_session *session,
    float *trim_bounds,
    uint64_t trim_bound_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_trim_exposure_pair(
    pano_gpu_session *session,
    void *trimmed,
    uint64_t trimmed_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_reduce_exposure_pair(
    pano_gpu_session *session,
    pano_gpu_exposure_pair_reduction *reduction,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_one_frame_equirect_local_exposure(
    const pano_gpu_session *session,
    const pano_gpu_one_frame_composite_request *request,
    float global_gain,
    void *local_field,
    uint64_t local_field_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_one_frame_rectilinear_local_exposure(
    const pano_gpu_session *session,
    const pano_gpu_one_frame_composite_request *request,
    float global_gain,
    void *local_field,
    uint64_t local_field_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_feather_weights(
    const pano_gpu_session *session,
    const pano_gpu_feather_accumulation_request *request,
    const void *candidate_validity,
    uint64_t candidate_validity_bytes,
    const void *candidate_edge_distance,
    uint64_t candidate_edge_distance_bytes,
    void *feather_weight,
    uint64_t feather_weight_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_feather_accumulation(
    const pano_gpu_session *session,
    const pano_gpu_feather_accumulation_request *request,
    const void *candidate_rgb,
    uint64_t candidate_rgb_bytes,
    const void *candidate_weight,
    uint64_t candidate_weight_bytes,
    const void *accumulator_rgb,
    uint64_t accumulator_rgb_bytes,
    const void *accumulator_weight,
    uint64_t accumulator_weight_bytes,
    void *result_rgb,
    uint64_t result_rgb_bytes,
    void *result_weight,
    uint64_t result_weight_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_two_frame_feather_accumulation(
    const pano_gpu_session *session,
    const pano_gpu_feather_accumulation_request *request,
    const void *first_candidate_rgb,
    uint64_t first_candidate_rgb_bytes,
    const void *first_candidate_weight,
    uint64_t first_candidate_weight_bytes,
    const void *second_candidate_rgb,
    uint64_t second_candidate_rgb_bytes,
    const void *second_candidate_weight,
    uint64_t second_candidate_weight_bytes,
    void *result_rgb,
    uint64_t result_rgb_bytes,
    void *result_weight,
    uint64_t result_weight_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_feather_accumulation_chain(
    const pano_gpu_session *session,
    const pano_gpu_feather_accumulation_request *request,
    const void *const *candidate_rgb,
    const uint64_t *candidate_rgb_bytes,
    const void *const *candidate_weight,
    const uint64_t *candidate_weight_bytes,
    uint32_t frame_count,
    void *result_rgb,
    uint64_t result_rgb_bytes,
    void *result_weight,
    uint64_t result_weight_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_output_feather_accumulation(
    pano_gpu_output *output,
    const pano_gpu_feather_accumulation_request *request,
    const void *const *candidate_rgb,
    const uint64_t *candidate_rgb_bytes,
    const void *const *candidate_weight,
    const uint64_t *candidate_weight_bytes,
    uint32_t frame_count,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_three_frame_feather_accumulation(
    const pano_gpu_session *session,
    const pano_gpu_feather_accumulation_request *request,
    const void *first_candidate_rgb,
    uint64_t first_candidate_rgb_bytes,
    const void *first_candidate_weight,
    uint64_t first_candidate_weight_bytes,
    const void *second_candidate_rgb,
    uint64_t second_candidate_rgb_bytes,
    const void *second_candidate_weight,
    uint64_t second_candidate_weight_bytes,
    const void *third_candidate_rgb,
    uint64_t third_candidate_rgb_bytes,
    const void *third_candidate_weight,
    uint64_t third_candidate_weight_bytes,
    void *result_rgb,
    uint64_t result_rgb_bytes,
    void *result_weight,
    uint64_t result_weight_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_feather_normalize(
    const pano_gpu_session *session,
    const pano_gpu_feather_accumulation_request *request,
    const void *accumulator_rgb,
    uint64_t accumulator_rgb_bytes,
    const void *accumulator_weight,
    uint64_t accumulator_weight_bytes,
    void *normalized_rgb,
    uint64_t normalized_rgb_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_global_gain(
    const pano_gpu_session *session,
    uint32_t pixel_count,
    float global_gain,
    const void *candidate_rgb,
    uint64_t candidate_rgb_bytes,
    void *adjusted_rgb,
    uint64_t adjusted_rgb_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_local_exposure(
    const pano_gpu_session *session,
    uint32_t output_width,
    uint32_t output_height,
    uint32_t row_start,
    uint32_t row_count,
    const void *candidate_rgb,
    uint64_t candidate_rgb_bytes,
    const void *local_field,
    uint64_t local_field_bytes,
    void *adjusted_rgb,
    uint64_t adjusted_rgb_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_mark_incomplete(
    const pano_gpu_session *session,
    uint32_t pixel_count,
    const void *selected_rgb,
    uint64_t selected_rgb_bytes,
    const void *selected_weight,
    uint64_t selected_weight_bytes,
    void *marked_rgb,
    uint64_t marked_rgb_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_hard_selection(
    const pano_gpu_session *session,
    const pano_gpu_hard_selection_request *request,
    const void *candidate_rgb,
    uint64_t candidate_rgb_bytes,
    const void *candidate_validity,
    uint64_t candidate_validity_bytes,
    const void *candidate_edge_distance,
    uint64_t candidate_edge_distance_bytes,
    const void *prior_rgb,
    uint64_t prior_rgb_bytes,
    const void *prior_weight,
    uint64_t prior_weight_bytes,
    void *selected_rgb,
    uint64_t selected_rgb_bytes,
    void *selected_weight,
    uint64_t selected_weight_bytes,
    void *coverage,
    uint64_t coverage_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_one_frame_uint8_hard_selection(
    const pano_gpu_session *session,
    const pano_gpu_one_frame_composite_request *request,
    const void *prior_rgb,
    uint64_t prior_rgb_bytes,
    const void *prior_weight,
    uint64_t prior_weight_bytes,
    void *selected_rgb,
    uint64_t selected_rgb_bytes,
    void *selected_weight,
    uint64_t selected_weight_bytes,
    void *coverage,
    uint64_t coverage_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_one_frame_uint16_hard_selection(
    const pano_gpu_session *session,
    const pano_gpu_one_frame_composite_request *request,
    const void *prior_rgb,
    uint64_t prior_rgb_bytes,
    const void *prior_weight,
    uint64_t prior_weight_bytes,
    void *selected_rgb,
    uint64_t selected_rgb_bytes,
    void *selected_weight,
    uint64_t selected_weight_bytes,
    void *coverage,
    uint64_t coverage_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_one_frame_float32_hard_selection(
    const pano_gpu_session *session,
    const pano_gpu_one_frame_composite_request *request,
    const void *prior_rgb,
    uint64_t prior_rgb_bytes,
    const void *prior_weight,
    uint64_t prior_weight_bytes,
    void *selected_rgb,
    uint64_t selected_rgb_bytes,
    void *selected_weight,
    uint64_t selected_weight_bytes,
    void *coverage,
    uint64_t coverage_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_validate_ordered_hard_composite_request(
    const pano_gpu_session *session,
    const pano_gpu_ordered_hard_composite_request *request,
    pano_gpu_ordered_hard_composite_result_layout *layout,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_validate_output_hard_composite_request(
    const pano_gpu_output *output,
    const pano_gpu_ordered_hard_composite_request *request,
    pano_gpu_ordered_hard_composite_result_layout *layout,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_output_uint8_hard_composite(
    pano_gpu_output *output,
    const pano_gpu_ordered_hard_composite_request *request,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_set_output_band(
    pano_gpu_output *output,
    uint32_t row_start,
    uint32_t row_count,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_read_output_band(
    const pano_gpu_output *output,
    void *linear_rgb,
    uint64_t linear_rgb_bytes,
    void *coverage,
    uint64_t coverage_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_validate_two_frame_uint8_hard_composite_request(
    const pano_gpu_session *session,
    const pano_gpu_ordered_hard_composite_request *request,
    pano_gpu_ordered_hard_composite_result_layout *layout,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;

PANO_GPU_API pano_gpu_result pano_gpu_test_validate_three_frame_uint8_hard_composite_request(
    const pano_gpu_session *session,
    const pano_gpu_ordered_hard_composite_request *request,
    pano_gpu_ordered_hard_composite_result_layout *layout,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;

PANO_GPU_API pano_gpu_result pano_gpu_test_validate_two_frame_uint16_hard_composite_request(
    const pano_gpu_session *session,
    const pano_gpu_ordered_hard_composite_request *request,
    pano_gpu_ordered_hard_composite_result_layout *layout,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;

PANO_GPU_API pano_gpu_result pano_gpu_test_validate_three_frame_uint16_hard_composite_request(
    const pano_gpu_session *session,
    const pano_gpu_ordered_hard_composite_request *request,
    pano_gpu_ordered_hard_composite_result_layout *layout,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;

PANO_GPU_API pano_gpu_result pano_gpu_test_validate_two_frame_float32_hard_composite_request(
    const pano_gpu_session *session,
    const pano_gpu_ordered_hard_composite_request *request,
    pano_gpu_ordered_hard_composite_result_layout *layout,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;

PANO_GPU_API pano_gpu_result pano_gpu_test_validate_three_frame_float32_hard_composite_request(
    const pano_gpu_session *session,
    const pano_gpu_ordered_hard_composite_request *request,
    pano_gpu_ordered_hard_composite_result_layout *layout,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_two_frame_uint8_hard_composite(
    const pano_gpu_session *session,
    const pano_gpu_ordered_hard_composite_request *request,
    void *selected_rgb,
    uint64_t selected_rgb_bytes,
    void *selected_weight,
    uint64_t selected_weight_bytes,
    void *coverage,
    uint64_t coverage_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_two_frame_uint8_hard_composite_with_gains(
    const pano_gpu_session *session,
    const pano_gpu_ordered_hard_composite_request *request,
    const float *global_gains,
    uint64_t global_gain_bytes,
    void *selected_rgb,
    uint64_t selected_rgb_bytes,
    void *selected_weight,
    uint64_t selected_weight_bytes,
    void *coverage,
    uint64_t coverage_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_two_frame_uint8_hard_composite_with_exposure(
    const pano_gpu_session *session,
    const pano_gpu_ordered_hard_composite_request *request,
    const float *global_gains,
    uint64_t global_gain_bytes,
    const float *local_fields,
    uint64_t local_field_bytes,
    void *selected_rgb,
    uint64_t selected_rgb_bytes,
    void *selected_weight,
    uint64_t selected_weight_bytes,
    void *coverage,
    uint64_t coverage_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;

PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_three_frame_uint8_hard_composite(
    const pano_gpu_session *session,
    const pano_gpu_ordered_hard_composite_request *request,
    void *selected_rgb,
    uint64_t selected_rgb_bytes,
    void *selected_weight,
    uint64_t selected_weight_bytes,
    void *coverage,
    uint64_t coverage_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_three_frame_uint8_hard_composite_with_session_gains(
    const pano_gpu_session *session,
    const pano_gpu_ordered_hard_composite_request *request,
    void *selected_rgb,
    uint64_t selected_rgb_bytes,
    void *selected_weight,
    uint64_t selected_weight_bytes,
    void *coverage,
    uint64_t coverage_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;

PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_two_frame_uint16_hard_composite(
    const pano_gpu_session *session,
    const pano_gpu_ordered_hard_composite_request *request,
    void *selected_rgb,
    uint64_t selected_rgb_bytes,
    void *selected_weight,
    uint64_t selected_weight_bytes,
    void *coverage,
    uint64_t coverage_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;

PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_two_frame_float32_hard_composite(
    const pano_gpu_session *session,
    const pano_gpu_ordered_hard_composite_request *request,
    void *selected_rgb,
    uint64_t selected_rgb_bytes,
    void *selected_weight,
    uint64_t selected_weight_bytes,
    void *coverage,
    uint64_t coverage_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;

PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_three_frame_float32_hard_composite(
    const pano_gpu_session *session,
    const pano_gpu_ordered_hard_composite_request *request,
    void *selected_rgb,
    uint64_t selected_rgb_bytes,
    void *selected_weight,
    uint64_t selected_weight_bytes,
    void *coverage,
    uint64_t coverage_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;

PANO_GPU_API pano_gpu_result pano_gpu_test_dispatch_three_frame_uint16_hard_composite(
    const pano_gpu_session *session,
    const pano_gpu_ordered_hard_composite_request *request,
    void *selected_rgb,
    uint64_t selected_rgb_bytes,
    void *selected_weight,
    uint64_t selected_weight_bytes,
    void *coverage,
    uint64_t coverage_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_validate_uint8_sample_request(
    const pano_gpu_session *session,
    const pano_gpu_uint8_sample_request *request,
    const void *coordinates,
    uint64_t coordinate_bytes,
    pano_gpu_uint8_sample_result_layout *layout,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_validate_uint16_sample_request(
    const pano_gpu_session *session,
    const pano_gpu_uint16_sample_request *request,
    const void *coordinates,
    uint64_t coordinate_bytes,
    pano_gpu_uint16_sample_result_layout *layout,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_validate_float32_sample_request(
    const pano_gpu_session *session,
    const pano_gpu_float32_sample_request *request,
    const void *coordinates,
    uint64_t coordinate_bytes,
    pano_gpu_float32_sample_result_layout *layout,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_sample_uint8(
    const pano_gpu_session *session,
    const pano_gpu_uint8_sample_request *request,
    const void *coordinates,
    uint64_t coordinate_bytes,
    void *sampled_rgb,
    uint64_t sampled_rgb_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_sample_uint16(
    const pano_gpu_session *session,
    const pano_gpu_uint16_sample_request *request,
    const void *coordinates,
    uint64_t coordinate_bytes,
    void *sampled_rgb,
    uint64_t sampled_rgb_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_sample_float32(
    const pano_gpu_session *session,
    const pano_gpu_float32_sample_request *request,
    const void *coordinates,
    uint64_t coordinate_bytes,
    void *sampled_rgb,
    uint64_t sampled_rgb_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_source_allocation(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_rotation_allocation(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_encoding_metadata_allocation(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_upload_slot_allocation(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_second_upload_slot_allocation(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_encoding_metadata_upload(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_fail_next_fence_signal(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_cancel_after_next_upload_slot_wait(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API void pano_gpu_test_cancel_after_next_upload_finish_wait(void) PANO_GPU_NOEXCEPT;
PANO_GPU_API uint64_t pano_gpu_test_session_source_bytes(const pano_gpu_session *session) PANO_GPU_NOEXCEPT;
PANO_GPU_API uint64_t pano_gpu_test_session_rotation_bytes(const pano_gpu_session *session) PANO_GPU_NOEXCEPT;
PANO_GPU_API uint64_t pano_gpu_test_session_encoding_metadata_bytes(
    const pano_gpu_session *session) PANO_GPU_NOEXCEPT;
PANO_GPU_API uint64_t pano_gpu_test_session_upload_slot_bytes(
    const pano_gpu_session *session) PANO_GPU_NOEXCEPT;
PANO_GPU_API uint64_t pano_gpu_test_session_second_upload_slot_bytes(
    const pano_gpu_session *session) PANO_GPU_NOEXCEPT;
PANO_GPU_API uint64_t pano_gpu_test_session_first_upload_slot_fence(
    const pano_gpu_session *session) PANO_GPU_NOEXCEPT;
PANO_GPU_API uint64_t pano_gpu_test_session_second_upload_slot_fence(
    const pano_gpu_session *session) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_read_session_rotations(
    const pano_gpu_session *session,
    void *rotations,
    uint64_t rotation_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_read_session_encoding_metadata(
    const pano_gpu_session *session,
    void *encoding_metadata,
    uint64_t encoding_metadata_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_read_session_frame_zero(
    const pano_gpu_session *session,
    void *data,
    uint64_t data_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
PANO_GPU_API pano_gpu_result pano_gpu_test_read_session_frame(
    const pano_gpu_session *session,
    uint32_t frame_index,
    void *data,
    uint64_t data_bytes,
    char *error_buffer,
    uint32_t error_buffer_size) PANO_GPU_NOEXCEPT;
#endif

#ifdef __cplusplus
}
#endif
