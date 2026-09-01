#include "pano_app.h"
#include "pano_gpu.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace pano::app {
namespace {
constexpr std::uint64_t alignment = 65536U;

bool checked_multiply(const std::uint64_t left, const std::uint64_t right,
                      std::uint64_t &result) {
  if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left)
    return false;
  result = left * right;
  return true;
}

bool align_bytes(const std::uint64_t value, std::uint64_t &result) {
  if (value > std::numeric_limits<std::uint64_t>::max() - (alignment - 1U))
    return false;
  result = (value + alignment - 1U) & ~(alignment - 1U);
  return true;
}

bool gpu_ok(const pano_gpu_result result, const std::array<char, 512> &detail,
            const char *fallback, std::string &error) {
  if (result == PANO_GPU_SUCCESS)
    return true;
  error = detail[0] == '\0' ? fallback : detail.data();
  return false;
}

unsigned sample_type(const ImageEncoding &encoding) {
  if (encoding.sample_type == "uint8")
    return PANO_GPU_SAMPLE_UINT8;
  if (encoding.sample_type == "uint16")
    return PANO_GPU_SAMPLE_UINT16;
  if (encoding.sample_type == "float32")
    return PANO_GPU_SAMPLE_FLOAT32;
  return 0;
}

unsigned transfer_function(const ImageEncoding &encoding) {
  if (encoding.transfer_function == "srgb")
    return PANO_GPU_TRANSFER_SRGB;
  if (encoding.transfer_function == "pq")
    return PANO_GPU_TRANSFER_PQ;
  if (encoding.transfer_function == "linear")
    return PANO_GPU_TRANSFER_LINEAR;
  return 0;
}

std::array<float, 9> world_to_camera(const FrameSummary &frame) {
  std::array<float, 9> result{};
  if (frame.camera_basis_row_major.has_value()) {
    for (unsigned row = 0; row < 3; ++row)
      for (unsigned column = 0; column < 3; ++column)
        result[row * 3U + column] = static_cast<float>(
            (*frame.camera_basis_row_major)[column * 3U + row]);
    return result;
  }
  constexpr double radians = 3.14159265358979323846 / 180.0;
  const double yaw = frame.yaw_deg * radians;
  const double pitch = -frame.pitch_deg * radians;
  const double roll = frame.roll_deg * radians;
  const double cy = std::cos(yaw), sy = std::sin(yaw);
  const double cp = std::cos(pitch), sp = std::sin(pitch);
  const double cr = std::cos(roll), sr = std::sin(roll);
  const std::array<double, 9> camera_to_world{cy * cr + sy * sp * sr,
                                              -cy * sr + sy * sp * cr,
                                              sy * cp,
                                              cp * sr,
                                              cp * cr,
                                              -sp,
                                              -sy * cr + cy * sp * sr,
                                              sy * sr + cy * sp * cr,
                                              cy * cp};
  for (unsigned row = 0; row < 3; ++row)
    for (unsigned column = 0; column < 3; ++column)
      result[row * 3U + column] =
          static_cast<float>(camera_to_world[column * 3U + row]);
  return result;
}
} // namespace

class NativePreview {
public:
  pano_gpu_session *session = nullptr;
  pano_gpu_preview *preview = nullptr;
  pano_gpu_device *device = nullptr;
  NativePreviewDiagnostics diagnostics;
  std::vector<std::uint8_t> masks;
  RenderPlan plan;
  ImageInfo source;
  pano_gpu_memory_plan memory{};
  std::vector<float> gains;
  std::vector<pano_gpu_exposure_equation> exposure_equations;
  bool exposure_graph_measured = false;
  unsigned exposure_edge_count = 0;
  std::uint64_t preview_cache_bytes = 0;

  ~NativePreview() {
    pano_gpu_preview_destroy(&preview);
    pano_gpu_session_destroy(&session);
  }
};

namespace {
bool cancelled(const NativePreviewOptions &options);
void report_progress(const NativePreviewOptions &options, unsigned completed,
                     const char *phase);
bool plan_preview_memory(pano_gpu_device *device, const ImageInfo &source,
                         unsigned frame_count, unsigned output_width,
                         unsigned output_height,
                         std::uint64_t preview_cache_bytes,
                         unsigned output_sample_bytes,
                         bool needs_sdr_conversion,
                         std::optional<unsigned> requested_mib,
                         pano_gpu_memory_plan &plan, std::string &error);
bool compose_preview(pano_gpu_session *session, const RenderPlan &plan,
                     const ImageInfo &source,
                     const pano_gpu_memory_plan &memory, unsigned width,
                     unsigned height, const NativePreviewOptions &options,
                     const std::vector<float> &gains,
                     std::vector<std::uint8_t> &pixels, std::string &error);
bool make_overview(const std::vector<std::uint8_t> &source,
                   unsigned source_width, unsigned source_height,
                   unsigned output_width, unsigned output_height,
                   std::vector<std::uint8_t> &output, std::string &error);
bool make_masks(const RenderPlan &plan, const ImageInfo &source, unsigned width,
                unsigned height, const NativePreviewOptions &options,
                std::vector<std::uint8_t> &masks, std::string &error);
bool replace_preview_pixels(NativePreview &preview,
                            const std::vector<float> &gains,
                            const NativePreviewOptions &options,
                            NativeExposureResult &result, std::string &error);
} // namespace

bool create_native_preview(pano_gpu_device *const device,
                           const RenderPlan &plan,
                           const NativePreviewOptions &options,
                           NativePreview **const preview, std::string &error) {
  if (preview == nullptr || *preview != nullptr || device == nullptr ||
      options.viewport_width == 0U || plan.session.frames.empty() ||
      plan.session.frames.size() > std::numeric_limits<std::uint32_t>::max()) {
    error = "invalid native preview request";
    return false;
  }
  if (cancelled(options)) {
    error = "native preview creation cancelled";
    return false;
  }
  if (plan.session.frames.size() < 2U) {
    error = "native D3D12 preview requires at least two captured frames";
    return false;
  }
  try {
    report_progress(options, 0U, "Preparing preview");
    ImageInfo source;
    CodecErrorCategory category{};
    if (!inspect_image(plan.session.frames.front().filename, source, category,
                       error))
      return false;
    if (source.channels != 3U || sample_type(source.encoding) == 0U ||
        transfer_function(source.encoding) == 0U) {
      error = "native preview source encoding is unsupported";
      return false;
    }
    report_progress(options, 5U, "Inspecting source images");

    std::uint64_t preview_width64 = 0;
    if (!checked_multiply(options.viewport_width, 4U, preview_width64) ||
        preview_width64 > std::numeric_limits<unsigned>::max()) {
      error = "native preview dimensions overflow";
      return false;
    }
    unsigned preview_width = static_cast<unsigned>(preview_width64);
    unsigned preview_height = 0;
    if (plan.session.capture_mode == "full_sphere") {
      preview_width = std::max(2U, preview_width - preview_width % 2U);
      preview_height = preview_width / 2U;
    } else {
      preview_height = std::max(
          1U, static_cast<unsigned>(std::round(
                  preview_width * plan.session.vertical_fov_deg / 360.0)));
    }
    const auto overview_height =
        std::max(1U, static_cast<unsigned>(std::round(
                         static_cast<double>(options.viewport_width) *
                         preview_height / preview_width)));

    std::uint64_t preview_pixels = 0;
    std::uint64_t overview_pixels = 0;
    std::uint64_t preview_cache = 0;
    std::uint64_t term = 0;
    const auto frame_count = static_cast<unsigned>(plan.session.frames.size());
    if (!checked_multiply(preview_width, preview_height, preview_pixels) ||
        !checked_multiply(options.viewport_width, overview_height,
                          overview_pixels) ||
        !checked_multiply(preview_pixels, 3U + frame_count, preview_cache) ||
        !checked_multiply(overview_pixels, 6U + frame_count, term) ||
        preview_cache > std::numeric_limits<std::uint64_t>::max() - term ||
        preview_cache + term >
            std::numeric_limits<std::uint64_t>::max() - frame_count) {
      error = "native preview cache accounting overflows";
      return false;
    }
    preview_cache += term + frame_count;

    pano_gpu_memory_plan memory{};
    if (!plan_preview_memory(device, source, frame_count, preview_width,
                             preview_height, preview_cache, 1U, true,
                             plan.gpu_memory_mib, memory, error))
      return false;
    std::unique_ptr<NativePreview> owner(new NativePreview);
    std::array<char, 512> gpu_error{};
    pano_gpu_device_diagnostics device_info{};
    device_info.size = sizeof(device_info);
    device_info.abi_version = PANO_GPU_ABI_VERSION;
    if (!gpu_ok(pano_gpu_device_query_diagnostics(
                    device, &device_info, gpu_error.data(),
                    static_cast<std::uint32_t>(gpu_error.size())),
                gpu_error, "cannot query native preview device", error))
      return false;

    const std::uint64_t bytes_per_sample =
        source.encoding.sample_type == "float32"
            ? 4U
            : (source.encoding.sample_type == "uint16" ? 2U : 1U);
    std::uint64_t row_stride = 0;
    if (!checked_multiply(source.width, 3U * bytes_per_sample, row_stride) ||
        row_stride > std::numeric_limits<std::uint32_t>::max()) {
      error = "native preview source row stride overflows";
      return false;
    }
    std::vector<float> rotations;
    rotations.reserve(plan.session.frames.size() * 9U);
    for (const auto &frame : plan.session.frames) {
      const auto rotation = world_to_camera(frame);
      rotations.insert(rotations.end(), rotation.begin(), rotation.end());
    }
    pano_gpu_session_create_options session_options{};
    session_options.size = sizeof(session_options);
    session_options.abi_version = PANO_GPU_ABI_VERSION;
    session_options.frame_count = frame_count;
    session_options.source_width = source.width;
    session_options.source_height = source.height;
    session_options.source_sample_type = sample_type(source.encoding);
    session_options.transfer_function = transfer_function(source.encoding);
    session_options.source_row_stride_bytes =
        static_cast<std::uint32_t>(row_stride);
    session_options.device_luid = device_info.adapter.luid;
    session_options.rotations = rotations.data();
    session_options.rotations_bytes = rotations.size() * sizeof(float);
    if (!gpu_ok(pano_gpu_session_create(
                    device, &session_options, &owner->session, gpu_error.data(),
                    static_cast<std::uint32_t>(gpu_error.size())),
                gpu_error, "cannot create native preview session", error))
      return false;

    if (!gpu_ok(pano_gpu_session_allocate_rotations(
                    owner->session, gpu_error.data(),
                    static_cast<std::uint32_t>(gpu_error.size())),
                gpu_error, "cannot allocate native preview rotations", error) ||
        !gpu_ok(pano_gpu_session_upload_rotations(
                    owner->session, rotations.data(),
                    rotations.size() * sizeof(float), gpu_error.data(),
                    static_cast<std::uint32_t>(gpu_error.size())),
                gpu_error, "cannot upload native preview rotations", error))
      return false;
    report_progress(options, native_preview_load_progress_begin,
                    "Preparing source images");

    std::vector<std::string> paths;
    paths.reserve(plan.session.frames.size());
    for (const auto &frame : plan.session.frames)
      paths.push_back(frame.filename);
    if (!decode_and_upload_images(
            owner->session, paths, source, options.cancellation,
            options.gpu_cancellation, category, error, options.progress,
            options.progress_user_data, native_preview_load_progress_begin,
            native_preview_compose_progress_begin))
      return false;

    std::vector<std::uint8_t> pixels;
    std::vector<std::uint8_t> overview;
    owner->gains.assign(frame_count, 1.0F);
    if (!compose_preview(owner->session, plan, source, memory, preview_width,
                         preview_height, options, owner->gains, pixels,
                         error) ||
        !make_overview(pixels, preview_width, preview_height,
                       options.viewport_width, overview_height, overview,
                       error))
      return false;
    report_progress(options, native_preview_overview_progress_end,
                    "Building preview overview");
    if (!make_masks(plan, source, options.viewport_width, overview_height,
                    options, owner->masks, error))
      return false;

    pano_gpu_preview_create_options preview_options{};
    preview_options.size = sizeof(preview_options);
    preview_options.abi_version = PANO_GPU_ABI_VERSION;
    preview_options.frame_count = frame_count;
    preview_options.preview_width = preview_width;
    preview_options.preview_height = preview_height;
    preview_options.overview_width = options.viewport_width;
    preview_options.overview_height = overview_height;
    preview_options.mask_width = options.viewport_width;
    preview_options.mask_height = overview_height;
    preview_options.preview_rgb8 = pixels.data();
    preview_options.preview_rgb8_bytes = pixels.size();
    preview_options.overview_rgb8 = overview.data();
    preview_options.overview_rgb8_bytes = overview.size();
    preview_options.compact_masks = owner->masks.data();
    preview_options.compact_mask_bytes = owner->masks.size();
    if (!gpu_ok(pano_gpu_preview_create(
                    owner->session, &preview_options, &owner->preview,
                    gpu_error.data(),
                    static_cast<std::uint32_t>(gpu_error.size())),
                gpu_error, "cannot retain native preview", error))
      return false;
    report_progress(options, native_preview_retain_progress_end,
                    "Retaining preview");
    owner->diagnostics = {frame_count,     preview_width,
                          preview_height,  options.viewport_width,
                          overview_height, options.viewport_width,
                          overview_height};
    owner->plan = plan;
    owner->source = source;
    owner->memory = memory;
    owner->device = device;
    owner->preview_cache_bytes = preview_cache;
    *preview = owner.release();
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate native preview";
    return false;
  }
}

bool query_native_preview(const NativePreview *const preview,
                          NativePreviewDiagnostics &diagnostics,
                          std::string &error) {
  if (preview == nullptr || preview->preview == nullptr) {
    error = "native preview is not available";
    return false;
  }
  diagnostics = preview->diagnostics;
  error.clear();
  return true;
}

bool update_native_preview_render_plan(NativePreview *const preview,
                                       const RenderPlan &plan,
                                       std::string &error) {
  if (preview == nullptr || preview->session == nullptr) {
    error = "native preview is not available";
    return false;
  }
  const auto &current = preview->plan.session;
  const auto &candidate = plan.session;
  const bool encoding_matches =
      current.image_encoding.sample_type ==
          candidate.image_encoding.sample_type &&
      current.image_encoding.color_primaries ==
          candidate.image_encoding.color_primaries &&
      current.image_encoding.transfer_function ==
          candidate.image_encoding.transfer_function &&
      current.image_encoding.reference_white_nits ==
          candidate.image_encoding.reference_white_nits;
  bool frames_match = current.frames.size() == candidate.frames.size();
  for (std::size_t index = 0; frames_match && index < current.frames.size();
       ++index) {
    const auto &left = current.frames[index];
    const auto &right = candidate.frames[index];
    frames_match =
        left.index == right.index && left.filename == right.filename &&
        left.yaw_deg == right.yaw_deg && left.pitch_deg == right.pitch_deg &&
        left.roll_deg == right.roll_deg && left.status == right.status &&
        left.camera_basis_row_major == right.camera_basis_row_major;
  }
  if (current.schema_version != candidate.schema_version ||
      current.session_id != candidate.session_id ||
      current.capture_mode != candidate.capture_mode ||
      current.projection != candidate.projection ||
      current.horizontal_fov_deg != candidate.horizontal_fov_deg ||
      current.vertical_fov_deg != candidate.vertical_fov_deg ||
      current.overlap_fraction != candidate.overlap_fraction ||
      current.completed != candidate.completed || !encoding_matches ||
      !frames_match || preview->plan.use_gpu != plan.use_gpu) {
    error = "native render plan does not match the retained session";
    return false;
  }
  try {
    preview->plan = plan;
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot update native render plan";
    return false;
  }
}

bool rebuild_native_preview(NativePreview *const preview,
                            const RenderPlan &plan,
                            const NativePreviewOptions &options,
                            std::string &error) {
  if (preview == nullptr || preview->session == nullptr ||
      preview->preview == nullptr) {
    error = "native preview is not available";
    return false;
  }
  try {
    RenderPlan previous = preview->plan;
    if (!update_native_preview_render_plan(preview, plan, error))
      return false;
    report_progress(options, native_preview_compose_progress_begin,
                    "Compositing preview");
    NativeExposureResult rebuilt;
    if (!replace_preview_pixels(*preview, preview->gains, options, rebuilt,
                                error)) {
      preview->plan = std::move(previous);
      return false;
    }
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot rebuild native preview";
    return false;
  }
}

pano_gpu_preview *native_preview_handle(NativePreview *const preview) noexcept {
  return preview == nullptr ? nullptr : preview->preview;
}

const std::vector<std::uint8_t> &
native_preview_masks(const NativePreview *const preview) noexcept {
  static const std::vector<std::uint8_t> empty;
  return preview == nullptr ? empty : preview->masks;
}

void destroy_native_preview(NativePreview **const preview) noexcept {
  if (preview == nullptr || *preview == nullptr)
    return;
  delete *preview;
  *preview = nullptr;
}

namespace {
bool plan_preview_memory(pano_gpu_device *device, const ImageInfo &source,
                         const unsigned frame_count,
                         const unsigned output_width,
                         const unsigned output_height,
                         const std::uint64_t preview_cache_bytes,
                         const unsigned output_sample_bytes,
                         const bool needs_sdr_conversion,
                         const std::optional<unsigned> requested_mib,
                         pano_gpu_memory_plan &plan, std::string &error) {
  std::array<char, 512> gpu_error{};
  pano_gpu_device_diagnostics device_info{};
  device_info.size = sizeof(device_info);
  device_info.abi_version = PANO_GPU_ABI_VERSION;
  if (!gpu_ok(pano_gpu_device_query_diagnostics(
                  device, &device_info, gpu_error.data(),
                  static_cast<std::uint32_t>(gpu_error.size())),
              gpu_error, "cannot query D3D12 device", error))
    return false;
  const std::uint64_t bytes_per_sample =
      source.encoding.sample_type == "float32"
          ? 4U
          : (source.encoding.sample_type == "uint16" ? 2U : 1U);
  std::uint64_t source_frame_bytes = 0;
  std::uint64_t aligned_source_frame = 0;
  std::uint64_t rotation_bytes = 0;
  if (!checked_multiply(source.width, source.height, source_frame_bytes) ||
      !checked_multiply(source_frame_bytes, 3U * bytes_per_sample,
                        source_frame_bytes) ||
      !align_bytes(source_frame_bytes, aligned_source_frame) ||
      !checked_multiply(frame_count, 9U * sizeof(float), rotation_bytes) ||
      !align_bytes(rotation_bytes, rotation_bytes)) {
    error = "preview memory accounting overflows";
    return false;
  }
  pano_gpu_memory_request request{};
  request.size = sizeof(request);
  request.abi_version = PANO_GPU_ABI_VERSION;
  request.frame_count = frame_count;
  request.source_width = source.width;
  request.source_height = source.height;
  request.source_sample_bytes = static_cast<std::uint32_t>(bytes_per_sample);
  request.output_width = output_width;
  request.output_height = output_height;
  request.output_sample_bytes = output_sample_bytes;
  request.needs_sdr_conversion = needs_sdr_conversion ? 1U : 0U;
  request.free_bytes = device_info.usable_local_bytes;
  request.total_bytes = std::max<std::uint64_t>(
      1U, std::max(device_info.adapter.dedicated_bytes,
                   device_info.adapter.local_budget_bytes));
  request.requested_budget_bytes =
      requested_mib.has_value()
          ? static_cast<std::uint64_t>(*requested_mib) * 1024U * 1024U
          : 0U;
  request.preview_cache_bytes = preview_cache_bytes;
  request.session_workspace_bytes = rotation_bytes;
  request.output_workspace_bytes_per_pixel = std::max<std::uint64_t>(
      62U + 21U * frame_count, needs_sdr_conversion ? 52U : 25U);
  request.output_workspace_fixed_bytes =
      4096U * sizeof(std::uint32_t) + (4U * frame_count + 16U) * alignment;
  request.upload_bytes = aligned_source_frame * 2U;
  request.readback_bytes_per_pixel = 12U;
  request.descriptor_count = frame_count + 4U;
  plan = {};
  plan.size = sizeof(plan);
  plan.abi_version = PANO_GPU_ABI_VERSION;
  gpu_error.fill('\0');
  return gpu_ok(
      pano_gpu_plan_memory(&request, &plan, gpu_error.data(),
                           static_cast<std::uint32_t>(gpu_error.size())),
      gpu_error, "cannot admit D3D12 preview memory", error);
}

bool compose_preview(pano_gpu_session *session, const RenderPlan &plan,
                     const ImageInfo &source,
                     const pano_gpu_memory_plan &memory, const unsigned width,
                     const unsigned height,
                     const NativePreviewOptions &progress_options,
                     const std::vector<float> &gains,
                     std::vector<std::uint8_t> &pixels, std::string &error) {
  if (gains.size() != plan.session.frames.size() ||
      std::any_of(gains.begin(), gains.end(), [](const float gain) {
        return !std::isfinite(gain) || gain <= 0.0F;
      })) {
    error = "invalid native preview exposure gains";
    return false;
  }
  std::array<char, 512> gpu_error{};
  pano_gpu_output *output = nullptr;
  struct OutputCleanup {
    pano_gpu_output **output;
    ~OutputCleanup() { pano_gpu_output_destroy(output); }
  } cleanup{&output};
  pano_gpu_output_create_options options{};
  options.size = sizeof(options);
  options.abi_version = PANO_GPU_ABI_VERSION;
  options.output_width = width;
  options.output_height = height;
  options.output_sample_bytes = 1;
  options.output_band_rows = memory.output_band_rows;
  options.descriptor_count = memory.descriptor_count;
  options.output_workspace_bytes = memory.output_workspace_bytes;
  if (!gpu_ok(pano_gpu_output_create_empty(
                  session, &options, &output, gpu_error.data(),
                  static_cast<std::uint32_t>(gpu_error.size())),
              gpu_error, "cannot create D3D12 preview output", error) ||
      !gpu_ok(pano_gpu_output_allocate_linear(
                  output, gpu_error.data(),
                  static_cast<std::uint32_t>(gpu_error.size())),
              gpu_error, "cannot allocate D3D12 preview color", error) ||
      !gpu_ok(pano_gpu_output_allocate_coverage(
                  output, gpu_error.data(),
                  static_cast<std::uint32_t>(gpu_error.size())),
              gpu_error, "cannot allocate D3D12 preview coverage", error))
    return false;

  std::vector<pano_gpu_one_frame_composite_request> frames(
      plan.session.frames.size());
  const unsigned native_sample = sample_type(source.encoding);
  const float latitude_span =
      static_cast<float>(plan.session.capture_mode == "full_sphere"
                             ? 180.0
                             : plan.session.vertical_fov_deg);
  for (std::size_t index = 0; index < frames.size(); ++index) {
    auto &request = frames[index];
    request.size = sizeof(request);
    request.abi_version = PANO_GPU_ABI_VERSION;
    request.frame_index = static_cast<std::uint32_t>(index);
    request.source_sample_type = native_sample;
    request.output_width = width;
    request.output_height = height;
    request.latitude_span_degrees = latitude_span;
    request.horizontal_fov_degrees =
        static_cast<float>(plan.session.horizontal_fov_deg);
    request.vertical_fov_degrees =
        static_cast<float>(plan.session.vertical_fov_deg);
    const auto rotation = world_to_camera(plan.session.frames[index]);
    std::copy(rotation.begin(), rotation.end(), request.world_to_camera);
  }
  pano_gpu_ordered_hard_composite_request ordered{};
  ordered.size = sizeof(ordered);
  ordered.abi_version = PANO_GPU_ABI_VERSION;
  ordered.frame_request_count = static_cast<std::uint32_t>(frames.size());
  ordered.frame_requests = frames.data();
  pano_gpu_composite_inputs inputs{};
  inputs.size = sizeof(inputs);
  inputs.abi_version = PANO_GPU_ABI_VERSION;
  inputs.mark_incomplete = plan.allow_incomplete ? 1U : 0U;
  inputs.global_gains = gains.data();
  inputs.global_gain_bytes = gains.size() * sizeof(float);
  const auto compose = [&]() {
    gpu_error.fill('\0');
    const auto result = plan.blend == "hard"
                            ? pano_gpu_output_compose_hard_with_inputs(
                                  output, &ordered, &inputs, gpu_error.data(),
                                  static_cast<std::uint32_t>(gpu_error.size()))
                            : pano_gpu_output_compose_feather_with_inputs(
                                  output, &ordered, &inputs, gpu_error.data(),
                                  static_cast<std::uint32_t>(gpu_error.size()));
    return gpu_ok(result, gpu_error, "cannot compose D3D12 preview", error);
  };
  const bool pq = source.encoding.transfer_function == "pq";
  const unsigned workspace_rows =
      memory.output_band_rows == 0U ? height : memory.output_band_rows;
  const bool histogram_required = plan.auto_contrast || !plan.allow_incomplete;
  const unsigned band_count =
      std::max(1U, (height + workspace_rows - 1U) / workspace_rows);
  const unsigned total_steps = band_count * (plan.auto_contrast ? 2U : 1U);
  unsigned completed_steps = 0U;
  const auto report_composition = [&]() {
    const unsigned span = native_preview_compose_progress_end -
                          native_preview_compose_progress_begin;
    report_progress(progress_options,
                    native_preview_compose_progress_begin +
                        span * completed_steps / total_steps,
                    plan.auto_contrast && completed_steps <= band_count
                        ? "Calculating preview contrast"
                        : "Compositing preview");
  };
  report_composition();
  if (histogram_required &&
      !gpu_ok(pano_gpu_output_prepare_auto_contrast_histogram(
                  output, gpu_error.data(),
                  static_cast<std::uint32_t>(gpu_error.size())),
              gpu_error, "cannot prepare preview auto contrast", error))
    return false;
  if (plan.auto_contrast) {
    for (unsigned row = 0; row < height; row += workspace_rows) {
      const unsigned rows = std::min(workspace_rows, height - row);
      for (auto &frame : frames) {
        frame.row_start = row;
        frame.row_count = rows;
      }
      if (!compose())
        return false;
      if (pq) {
        if (!gpu_ok(
                pano_gpu_output_tone_map_rec2020(
                    output,
                    static_cast<float>(source.encoding.reference_white_nits),
                    gpu_error.data(),
                    static_cast<std::uint32_t>(gpu_error.size())),
                gpu_error, "cannot tone-map D3D12 preview", error) ||
            !gpu_ok(pano_gpu_output_convert_tone_mapped_rec2020_to_linear_srgb(
                        output, gpu_error.data(),
                        static_cast<std::uint32_t>(gpu_error.size())),
                    gpu_error, "cannot convert D3D12 preview primaries",
                    error) ||
            !gpu_ok(
                pano_gpu_output_accumulate_auto_contrast_histogram_converted_srgb(
                    output, gpu_error.data(),
                    static_cast<std::uint32_t>(gpu_error.size())),
                gpu_error, "cannot accumulate D3D12 preview histogram", error))
          return false;
      } else if (!gpu_ok(
                     pano_gpu_output_accumulate_auto_contrast_histogram_srgb(
                         output, gpu_error.data(),
                         static_cast<std::uint32_t>(gpu_error.size())),
                     gpu_error, "cannot accumulate D3D12 preview histogram",
                     error)) {
        return false;
      }
      ++completed_steps;
      report_composition();
    }
    pano_gpu_auto_contrast_levels levels{};
    levels.size = sizeof(levels);
    levels.abi_version = PANO_GPU_ABI_VERSION;
    if (!gpu_ok(pano_gpu_output_select_auto_contrast_levels(
                    output, &levels, gpu_error.data(),
                    static_cast<std::uint32_t>(gpu_error.size())),
                gpu_error, "cannot select D3D12 preview contrast", error) ||
        (!plan.allow_incomplete &&
         levels.processed_pixels !=
             static_cast<std::uint64_t>(width) * height)) {
      if (error.empty())
        error = "capture does not cover every preview pixel";
      return false;
    }
  }

  std::uint64_t pixel_bytes = 0;
  if (!checked_multiply(width, height, pixel_bytes) ||
      !checked_multiply(pixel_bytes, 3U, pixel_bytes) ||
      pixel_bytes > std::numeric_limits<std::size_t>::max()) {
    error = "preview host buffer overflows";
    return false;
  }
  pixels.resize(static_cast<std::size_t>(pixel_bytes));
  for (unsigned row = 0; row < height; row += workspace_rows) {
    const unsigned rows = std::min(workspace_rows, height - row);
    for (auto &frame : frames) {
      frame.row_start = row;
      frame.row_count = rows;
    }
    if (!compose())
      return false;
    if (histogram_required && !plan.auto_contrast) {
      if (!gpu_ok(pano_gpu_output_accumulate_auto_contrast_histogram_srgb(
                      output, gpu_error.data(),
                      static_cast<std::uint32_t>(gpu_error.size())),
                  gpu_error, "cannot validate D3D12 preview coverage", error))
        return false;
    }
    if (pq) {
      if (!gpu_ok(pano_gpu_output_tone_map_rec2020(
                      output,
                      static_cast<float>(source.encoding.reference_white_nits),
                      gpu_error.data(),
                      static_cast<std::uint32_t>(gpu_error.size())),
                  gpu_error, "cannot tone-map D3D12 preview", error) ||
          !gpu_ok(pano_gpu_output_convert_tone_mapped_rec2020_to_linear_srgb(
                      output, gpu_error.data(),
                      static_cast<std::uint32_t>(gpu_error.size())),
                  gpu_error, "cannot convert D3D12 preview primaries", error) ||
          !gpu_ok(pano_gpu_output_apply_auto_contrast_converted_srgb(
                      output, plan.auto_contrast ? 1U : 0U, gpu_error.data(),
                      static_cast<std::uint32_t>(gpu_error.size())),
                  gpu_error, "cannot apply D3D12 preview contrast", error))
        return false;
    } else if (!gpu_ok(
                   pano_gpu_output_apply_auto_contrast_srgb(
                       output, plan.auto_contrast ? 1U : 0U, gpu_error.data(),
                       static_cast<std::uint32_t>(gpu_error.size())),
                   gpu_error, "cannot apply D3D12 preview contrast", error)) {
      return false;
    }
    if (!gpu_ok(pano_gpu_output_quantize_normalized_srgb8(
                    output, gpu_error.data(),
                    static_cast<std::uint32_t>(gpu_error.size())),
                gpu_error, "cannot quantize D3D12 preview", error))
      return false;
    pano_gpu_output_download_request download{};
    download.size = sizeof(download);
    download.abi_version = PANO_GPU_ABI_VERSION;
    download.output_width = width;
    download.row_start = row;
    download.row_count = rows;
    download.data = pixels.data() + static_cast<std::size_t>(row) * width * 3U;
    download.data_bytes = static_cast<std::uint64_t>(rows) * width * 3U;
    if (!gpu_ok(pano_gpu_output_download_srgb8(
                    output, &download, progress_options.gpu_cancellation,
                    gpu_error.data(),
                    static_cast<std::uint32_t>(gpu_error.size())),
                gpu_error, "cannot download D3D12 preview", error))
      return false;
    ++completed_steps;
    report_composition();
  }
  if (histogram_required && !plan.auto_contrast) {
    pano_gpu_auto_contrast_levels levels{};
    levels.size = sizeof(levels);
    levels.abi_version = PANO_GPU_ABI_VERSION;
    if (!gpu_ok(pano_gpu_output_select_auto_contrast_levels(
                    output, &levels, gpu_error.data(),
                    static_cast<std::uint32_t>(gpu_error.size())),
                gpu_error, "cannot validate D3D12 preview coverage", error) ||
        levels.processed_pixels != static_cast<std::uint64_t>(width) * height) {
      if (error.empty())
        error = "capture does not cover every preview pixel";
      return false;
    }
  }
  return true;
}

bool cancelled(const NativePreviewOptions &options) {
  return (options.cancellation.callback != nullptr &&
          options.cancellation.callback(options.cancellation.user_data)) ||
         (options.gpu_cancellation != nullptr &&
          pano_gpu_cancellation_token_is_cancelled(options.gpu_cancellation) !=
              0);
}

void report_progress(const NativePreviewOptions &options,
                     const unsigned completed, const char *const phase) {
  if (options.progress != nullptr)
    options.progress(options.progress_user_data, std::min(100U, completed),
                     100U, phase);
}

bool make_overview(const std::vector<std::uint8_t> &source,
                   const unsigned source_width, const unsigned source_height,
                   const unsigned output_width, const unsigned output_height,
                   std::vector<std::uint8_t> &output, std::string &error) {
  std::uint64_t bytes = 0;
  if (source_width == 0U || source_height == 0U || output_width == 0U ||
      output_height == 0U ||
      !checked_multiply(source_width, source_height, bytes) ||
      !checked_multiply(bytes, 3U, bytes) || bytes != source.size() ||
      !checked_multiply(output_width, output_height, bytes) ||
      !checked_multiply(bytes, 3U, bytes) ||
      bytes > std::numeric_limits<std::size_t>::max()) {
    error = "invalid native preview overview dimensions";
    return false;
  }
  output.assign(static_cast<std::size_t>(bytes), 0U);
  for (unsigned y = 0; y < output_height; ++y) {
    const auto top = static_cast<unsigned>(static_cast<std::uint64_t>(y) *
                                           source_height / output_height);
    const auto bottom = std::max(
        top + 1U, static_cast<unsigned>(static_cast<std::uint64_t>(y + 1U) *
                                        source_height / output_height));
    for (unsigned x = 0; x < output_width; ++x) {
      const auto left = static_cast<unsigned>(static_cast<std::uint64_t>(x) *
                                              source_width / output_width);
      const auto right = std::max(
          left + 1U, static_cast<unsigned>(static_cast<std::uint64_t>(x + 1U) *
                                           source_width / output_width));
      std::array<std::uint64_t, 3> sum{};
      std::uint64_t count = 0;
      for (unsigned source_y = top; source_y < std::min(bottom, source_height);
           ++source_y) {
        for (unsigned source_x = left; source_x < std::min(right, source_width);
             ++source_x) {
          const auto offset =
              (static_cast<std::size_t>(source_y) * source_width + source_x) *
              3U;
          for (unsigned channel = 0; channel < 3; ++channel)
            sum[channel] += source[offset + channel];
          ++count;
        }
      }
      const auto offset = (static_cast<std::size_t>(y) * output_width + x) * 3U;
      for (unsigned channel = 0; channel < 3; ++channel)
        output[offset + channel] =
            static_cast<std::uint8_t>((sum[channel] + count / 2U) / count);
    }
  }
  return true;
}

bool make_masks(const RenderPlan &plan, const ImageInfo &source,
                const unsigned width, const unsigned height,
                const NativePreviewOptions &options,
                std::vector<std::uint8_t> &masks, std::string &error) {
  std::uint64_t pixel_count = 0;
  std::uint64_t mask_bytes = 0;
  std::uint64_t ray_components = 0;
  if (!checked_multiply(width, height, pixel_count) ||
      pixel_count > std::numeric_limits<unsigned>::max() ||
      !checked_multiply(pixel_count, plan.session.frames.size(), mask_bytes) ||
      mask_bytes > std::numeric_limits<std::size_t>::max() ||
      !checked_multiply(pixel_count, 3U, ray_components) ||
      ray_components > std::numeric_limits<std::size_t>::max()) {
    error = "native preview mask dimensions overflow";
    return false;
  }
  try {
    std::vector<float> rays(static_cast<std::size_t>(ray_components));
    CpuRayRequest ray_request;
    ray_request.output_width = width;
    ray_request.output_height = height;
    ray_request.row_count = height;
    ray_request.latitude_span_degrees =
        static_cast<float>(plan.session.capture_mode == "full_sphere"
                               ? 180.0
                               : plan.session.vertical_fov_deg);
    if (!generate_cpu_world_rays(ray_request, rays.data(),
                                 rays.size() * sizeof(float), error))
      return false;
    masks.assign(static_cast<std::size_t>(mask_bytes), 0U);
    std::vector<float> coordinates(static_cast<std::size_t>(pixel_count) * 2U);
    std::vector<float> edges(static_cast<std::size_t>(pixel_count));
    for (std::size_t index = 0; index < plan.session.frames.size(); ++index) {
      if (cancelled(options)) {
        error = "native preview mask generation cancelled";
        return false;
      }
      CpuProjectionRequest projection;
      projection.pixel_count = static_cast<unsigned>(pixel_count);
      projection.source_width = source.width;
      projection.source_height = source.height;
      projection.horizontal_fov_degrees =
          static_cast<float>(plan.session.horizontal_fov_deg);
      projection.vertical_fov_degrees =
          static_cast<float>(plan.session.vertical_fov_deg);
      projection.world_to_camera = world_to_camera(plan.session.frames[index]);
      auto *validity = masks.data() + index * pixel_count;
      if (!project_cpu_world_rays(
              projection, rays.data(), rays.size() * sizeof(float),
              coordinates.data(), coordinates.size() * sizeof(float), validity,
              pixel_count, edges.data(), edges.size() * sizeof(float), error))
        return false;
      const unsigned span = native_preview_masks_progress_end -
                            native_preview_overview_progress_end;
      report_progress(options,
                      native_preview_overview_progress_end +
                          static_cast<unsigned>(span * (index + 1U) /
                                                plan.session.frames.size()),
                      "Building preview masks");
    }
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate native preview masks";
    return false;
  }
}

int report_exposure_pair_progress(void *const user_data,
                                  const std::uint32_t completed,
                                  const std::uint32_t total) {
  const auto &options = *static_cast<const NativePreviewOptions *>(user_data);
  if (total != 0U) {
    const auto progress =
        25U + static_cast<unsigned>(14ULL * completed / total);
    report_progress(options, progress, "Measuring exposure overlaps");
  }
  return cancelled(options) ? 0 : 1;
}

bool measure_exposure_graph(NativePreview &preview,
                            const NativePreviewOptions &options,
                            std::string &error) {
  if (preview.exposure_graph_measured) {
    report_progress(options, 40U, "Exposure samples ready");
    return true;
  }
  if (cancelled(options)) {
    error = "native exposure measurement cancelled";
    return false;
  }
  report_progress(options, 5U, "Sampling poses");
  std::array<char, 512> gpu_error{};
  const auto frame_count = preview.diagnostics.frame_count;
  pano_gpu_exposure_proxy_request proxy{};
  proxy.size = sizeof(proxy);
  proxy.abi_version = PANO_GPU_ABI_VERSION;
  proxy.frame_count = frame_count;
  proxy.source_width = preview.source.width;
  proxy.source_height = preview.source.height;
  if (!gpu_ok(pano_gpu_session_build_exposure_proxies(
                  preview.session, &proxy, gpu_error.data(),
                  static_cast<std::uint32_t>(gpu_error.size())),
              gpu_error, "cannot build native exposure proxies", error))
    return false;
  report_progress(options, 15U, "Sampling poses");
  std::uint32_t pair_count = 0;
  if (!gpu_ok(pano_gpu_exposure_pair_count(
                  frame_count, &pair_count, gpu_error.data(),
                  static_cast<std::uint32_t>(gpu_error.size())),
              gpu_error, "cannot count native exposure pairs", error) ||
      !gpu_ok(pano_gpu_session_prepare_exposure_graph(
                  preview.session, pair_count, gpu_error.data(),
                  static_cast<std::uint32_t>(gpu_error.size())),
              gpu_error, "cannot prepare native exposure graph", error) ||
      !gpu_ok(pano_gpu_session_enumerate_exposure_pairs(
                  preview.session, gpu_error.data(),
                  static_cast<std::uint32_t>(gpu_error.size())),
              gpu_error, "cannot enumerate native exposure pairs", error))
    return false;
  report_progress(options, 25U, "Measuring exposure overlaps");
  pano_gpu_exposure_pair_request request{};
  request.size = sizeof(request);
  request.abi_version = PANO_GPU_ABI_VERSION;
  request.first_frame_index = 0;
  request.second_frame_index = 1;
  request.sample_width = 256U;
  request.sample_height = 128U;
  request.latitude_span_degrees =
      static_cast<float>(preview.plan.session.capture_mode == "full_sphere"
                             ? 180.0
                             : preview.plan.session.vertical_fov_deg);
  request.horizontal_fov_degrees =
      static_cast<float>(preview.plan.session.horizontal_fov_deg);
  request.vertical_fov_degrees =
      static_cast<float>(preview.plan.session.vertical_fov_deg);
  if (!gpu_ok(pano_gpu_session_reduce_reference_exposure_graph_progress(
                  preview.session, &request, report_exposure_pair_progress,
                  const_cast<NativePreviewOptions *>(&options),
                  gpu_error.data(),
                  static_cast<std::uint32_t>(gpu_error.size())),
              gpu_error, "cannot reduce native exposure graph", error) ||
      cancelled(options)) {
    if (error.empty())
      error = "native exposure measurement cancelled";
    return false;
  }
  pano_gpu_exposure_graph_diagnostics diagnostics{};
  diagnostics.size = sizeof(diagnostics);
  diagnostics.abi_version = PANO_GPU_ABI_VERSION;
  if (!gpu_ok(pano_gpu_session_query_exposure_graph(
                  preview.session, &diagnostics, gpu_error.data(),
                  static_cast<std::uint32_t>(gpu_error.size())),
              gpu_error, "cannot query native exposure graph", error))
    return false;
  try {
    preview.exposure_equations.resize(diagnostics.equation_count);
  } catch (const std::bad_alloc &) {
    error = "cannot allocate native exposure equations";
    return false;
  }
  if (!gpu_ok(pano_gpu_session_copy_exposure_equations(
                  preview.session, preview.exposure_equations.data(),
                  preview.exposure_equations.size() *
                      sizeof(pano_gpu_exposure_equation),
                  gpu_error.data(),
                  static_cast<std::uint32_t>(gpu_error.size())),
              gpu_error, "cannot copy native exposure equations", error))
    return false;
  preview.exposure_graph_measured = true;
  report_progress(options, 40U, "Exposure samples ready");
  return true;
}

bool solve_automatic_exposure(NativePreview &preview, const unsigned target,
                              const NativePreviewOptions &options,
                              std::vector<float> &gains, std::string &error) {
  if (!measure_exposure_graph(preview, options, error))
    return false;
  report_progress(options, 45U, "Propagating exposure");
  try {
    std::vector<CpuExposureEquation> equations;
    equations.reserve(preview.exposure_equations.size());
    for (const auto &equation : preview.exposure_equations)
      equations.push_back(CpuExposureEquation{
          equation.left_frame_index, equation.right_frame_index,
          equation.difference, equation.weight});
    if (!solve_reference_exposure_gains(preview.diagnostics.frame_count, target,
                                        equations, preview.gains, gains, error))
      return false;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate native exposure solution";
    return false;
  }
  preview.exposure_edge_count =
      static_cast<unsigned>(preview.exposure_equations.size());
  return true;
}

bool replace_preview_pixels(NativePreview &preview,
                            const std::vector<float> &gains,
                            const NativePreviewOptions &options,
                            NativeExposureResult &result, std::string &error) {
  std::vector<std::uint8_t> pixels;
  std::vector<std::uint8_t> overview;
  const auto &dimensions = preview.diagnostics;
  if (!compose_preview(preview.session, preview.plan, preview.source,
                       preview.memory, dimensions.preview_width,
                       dimensions.preview_height, options, gains, pixels,
                       error) ||
      cancelled(options) ||
      !make_overview(pixels, dimensions.preview_width,
                     dimensions.preview_height, dimensions.overview_width,
                     dimensions.overview_height, overview, error)) {
    if (error.empty())
      error = "native exposure recomposition cancelled";
    return false;
  }
  report_progress(options, native_preview_overview_progress_end,
                  "Building preview overview");
  pano_gpu_preview_create_options create{};
  create.size = sizeof(create);
  create.abi_version = PANO_GPU_ABI_VERSION;
  create.frame_count = dimensions.frame_count;
  create.preview_width = dimensions.preview_width;
  create.preview_height = dimensions.preview_height;
  create.overview_width = dimensions.overview_width;
  create.overview_height = dimensions.overview_height;
  create.mask_width = dimensions.mask_width;
  create.mask_height = dimensions.mask_height;
  create.preview_rgb8 = pixels.data();
  create.preview_rgb8_bytes = pixels.size();
  create.overview_rgb8 = overview.data();
  create.overview_rgb8_bytes = overview.size();
  create.compact_masks = preview.masks.data();
  create.compact_mask_bytes = preview.masks.size();
  pano_gpu_preview *replacement = nullptr;
  std::array<char, 512> gpu_error{};
  if (!gpu_ok(pano_gpu_preview_create(
                  preview.session, &create, &replacement, gpu_error.data(),
                  static_cast<std::uint32_t>(gpu_error.size())),
              gpu_error, "cannot retain recomputed native preview", error))
    return false;
  pano_gpu_preview_destroy(&preview.preview);
  preview.preview = replacement;
  preview.gains = gains;
  result.gains = gains;
  result.edge_count = preview.exposure_edge_count;
  result.warning = dimensions.frame_count > 1U &&
                   result.edge_count < dimensions.frame_count - 1U;
  report_progress(options, native_preview_retain_progress_end,
                  "Retaining preview");
  error.clear();
  return true;
}

bool cancelled(const NativeRenderOptions &options) {
  return (options.cancellation.callback != nullptr &&
          options.cancellation.callback(options.cancellation.user_data)) ||
         (options.gpu_cancellation != nullptr &&
          pano_gpu_cancellation_token_is_cancelled(options.gpu_cancellation) !=
              0);
}

void render_progress(const NativeRenderOptions &options,
                     const unsigned completed, const unsigned total,
                     const char *phase) {
  if (options.progress != nullptr)
    options.progress(options.progress_user_data, completed, total, phase);
}

struct RenderProgressRange {
  const NativeRenderOptions *outer = nullptr;
  unsigned begin = 0;
  unsigned end = 100;
};

void report_render_range(void *const user_data, const unsigned completed,
                         const unsigned total, const char *const phase) {
  const auto &range = *static_cast<const RenderProgressRange *>(user_data);
  if (range.outer == nullptr || total == 0U)
    return;
  const auto span = static_cast<std::uint64_t>(range.end - range.begin);
  const auto scaled = static_cast<unsigned>(
      std::min<std::uint64_t>(span, span * completed / total));
  render_progress(*range.outer, range.begin + scaled, 100U, phase);
}

NativeRenderOptions ranged_render_options(const NativeRenderOptions &options,
                                          RenderProgressRange &range,
                                          const unsigned begin,
                                          const unsigned end) {
  range = {&options, begin, end};
  NativeRenderOptions ranged = options;
  ranged.progress = report_render_range;
  ranged.progress_user_data = &range;
  return ranged;
}

bool render_dimensions(const NativePreview &preview, unsigned &width,
                       unsigned &height, std::string &error,
                       const bool maximum = false) {
  if (!maximum && preview.plan.output_width.has_value()) {
    width = *preview.plan.output_width;
  } else {
    constexpr double pi = 3.14159265358979323846;
    const double focal =
        preview.source.width /
        (2.0 * std::tan(preview.plan.session.horizontal_fov_deg * pi / 360.0));
    const double scaled = std::round(2.0 * pi * focal *
                                     (maximum ? 1.0 : preview.plan.resolution));
    if (!std::isfinite(scaled) ||
        scaled > std::numeric_limits<unsigned>::max()) {
      error = "native render dimensions overflow";
      return false;
    }
    width = std::max(2U, static_cast<unsigned>(scaled));
  }
  if (preview.plan.session.capture_mode == "full_sphere") {
    width = std::max(2U, width - width % 2U);
    height = width / 2U;
  } else {
    height = std::max(
        1U, static_cast<unsigned>(std::round(
                width * preview.plan.session.vertical_fov_deg / 360.0)));
  }
  return true;
}

ImageContainer output_container(const std::string &path) {
  const auto position = path.find_last_of('.');
  if (position != std::string::npos) {
    std::string extension = path.substr(position);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](const unsigned char value) {
                     return static_cast<char>(std::tolower(value));
                   });
    if (extension == ".png")
      return ImageContainer::png;
    if (extension == ".exr")
      return ImageContainer::exr;
  }
  return ImageContainer::jpeg;
}

bool stream_gpu_output(NativePreview &preview, ImageWriter *writer,
                       ImageWriter *coverage_writer, const unsigned width,
                       const unsigned height, const bool rectilinear,
                       const float output_vertical_fov,
                       const pano_gpu_memory_plan &memory,
                       const NativeRenderOptions &options, std::string &error) {
  std::array<char, 512> gpu_error{};
  pano_gpu_output *output = nullptr;
  struct OutputCleanup {
    pano_gpu_output **output;
    ~OutputCleanup() { pano_gpu_output_destroy(output); }
  } cleanup{&output};
  const bool sdr = output_container(preview.plan.outputs.panorama.final_path) !=
                   ImageContainer::exr;
  pano_gpu_output_create_options create{};
  create.size = sizeof(create);
  create.abi_version = PANO_GPU_ABI_VERSION;
  create.output_width = width;
  create.output_height = height;
  create.output_sample_bytes = sdr ? 1U : 4U;
  create.output_band_rows = memory.output_band_rows;
  create.descriptor_count = memory.descriptor_count;
  create.output_workspace_bytes = memory.output_workspace_bytes;
  if (!gpu_ok(pano_gpu_output_create_empty(
                  preview.session, &create, &output, gpu_error.data(),
                  static_cast<std::uint32_t>(gpu_error.size())),
              gpu_error, "cannot create native render output", error) ||
      !gpu_ok(pano_gpu_output_allocate_linear(
                  output, gpu_error.data(),
                  static_cast<std::uint32_t>(gpu_error.size())),
              gpu_error, "cannot allocate native render color", error) ||
      !gpu_ok(pano_gpu_output_allocate_coverage(
                  output, gpu_error.data(),
                  static_cast<std::uint32_t>(gpu_error.size())),
              gpu_error, "cannot allocate native render coverage", error))
    return false;
  std::vector<pano_gpu_one_frame_composite_request> frames(
      preview.plan.session.frames.size());
  for (std::size_t index = 0; index < frames.size(); ++index) {
    auto &frame = frames[index];
    frame.size = sizeof(frame);
    frame.abi_version = PANO_GPU_ABI_VERSION;
    frame.frame_index = static_cast<std::uint32_t>(index);
    frame.source_sample_type = sample_type(preview.source.encoding);
    frame.output_width = width;
    frame.output_height = height;
    frame.latitude_span_degrees =
        static_cast<float>(preview.plan.session.capture_mode == "full_sphere"
                               ? 180.0
                               : preview.plan.session.vertical_fov_deg);
    frame.horizontal_fov_degrees =
        static_cast<float>(preview.plan.session.horizontal_fov_deg);
    frame.vertical_fov_degrees =
        static_cast<float>(preview.plan.session.vertical_fov_deg);
    const auto rotation = world_to_camera(preview.plan.session.frames[index]);
    std::copy(rotation.begin(), rotation.end(), frame.world_to_camera);
    frame.rectilinear_output = rectilinear ? 1U : 0U;
    frame.output_vertical_fov_degrees = output_vertical_fov;
  }
  pano_gpu_ordered_hard_composite_request ordered{};
  ordered.size = sizeof(ordered);
  ordered.abi_version = PANO_GPU_ABI_VERSION;
  ordered.frame_request_count = static_cast<std::uint32_t>(frames.size());
  ordered.frame_requests = frames.data();
  pano_gpu_composite_inputs inputs{};
  inputs.size = sizeof(inputs);
  inputs.abi_version = PANO_GPU_ABI_VERSION;
  inputs.mark_incomplete = preview.plan.allow_incomplete ? 1U : 0U;
  inputs.global_gains = preview.gains.data();
  inputs.global_gain_bytes = preview.gains.size() * sizeof(float);
  const auto compose = [&]() {
    const auto result = preview.plan.blend == "hard"
                            ? pano_gpu_output_compose_hard_with_inputs(
                                  output, &ordered, &inputs, gpu_error.data(),
                                  static_cast<std::uint32_t>(gpu_error.size()))
                            : pano_gpu_output_compose_feather_with_inputs(
                                  output, &ordered, &inputs, gpu_error.data(),
                                  static_cast<std::uint32_t>(gpu_error.size()));
    return gpu_ok(result, gpu_error, "cannot compose native render band",
                  error);
  };
  const bool auto_contrast = sdr && preview.plan.auto_contrast;
  if (auto_contrast &&
      !gpu_ok(pano_gpu_output_prepare_auto_contrast_histogram(
                  output, gpu_error.data(),
                  static_cast<std::uint32_t>(gpu_error.size())),
              gpu_error, "cannot prepare native render contrast", error))
    return false;
  const unsigned band_rows =
      memory.output_band_rows == 0U ? height : memory.output_band_rows;
  const bool pq = preview.source.encoding.transfer_function == "pq";
  const unsigned total_progress = height * (auto_contrast ? 2U : 1U);
  if (auto_contrast) {
    for (unsigned row = 0; row < height; row += band_rows) {
      if (cancelled(options)) {
        error = "native render cancelled";
        return false;
      }
      const unsigned rows = std::min(band_rows, height - row);
      for (auto &frame : frames) {
        frame.row_start = row;
        frame.row_count = rows;
      }
      if (!compose())
        return false;
      if (pq) {
        if (!gpu_ok(pano_gpu_output_tone_map_rec2020(
                        output,
                        static_cast<float>(
                            preview.source.encoding.reference_white_nits),
                        gpu_error.data(),
                        static_cast<std::uint32_t>(gpu_error.size())),
                    gpu_error, "cannot tone-map native render", error) ||
            !gpu_ok(pano_gpu_output_convert_tone_mapped_rec2020_to_linear_srgb(
                        output, gpu_error.data(),
                        static_cast<std::uint32_t>(gpu_error.size())),
                    gpu_error, "cannot convert native render primaries",
                    error) ||
            !gpu_ok(
                pano_gpu_output_accumulate_auto_contrast_histogram_converted_srgb(
                    output, gpu_error.data(),
                    static_cast<std::uint32_t>(gpu_error.size())),
                gpu_error, "cannot accumulate native render contrast", error))
          return false;
      } else if (!gpu_ok(
                     pano_gpu_output_accumulate_auto_contrast_histogram_srgb(
                         output, gpu_error.data(),
                         static_cast<std::uint32_t>(gpu_error.size())),
                     gpu_error, "cannot accumulate native render contrast",
                     error)) {
        return false;
      }
      render_progress(options, row + rows, total_progress, "contrast");
    }
    pano_gpu_auto_contrast_levels levels{};
    levels.size = sizeof(levels);
    levels.abi_version = PANO_GPU_ABI_VERSION;
    if (!gpu_ok(pano_gpu_output_select_auto_contrast_levels(
                    output, &levels, gpu_error.data(),
                    static_cast<std::uint32_t>(gpu_error.size())),
                gpu_error, "cannot select native render contrast", error) ||
        (!preview.plan.allow_incomplete &&
         levels.processed_pixels !=
             static_cast<std::uint64_t>(width) * height)) {
      if (error.empty())
        error = "capture does not cover every output pixel";
      return false;
    }
  }
  const std::uint64_t sample_bytes = sdr ? 1U : sizeof(float);
  std::uint64_t band_bytes = 0;
  if (!checked_multiply(band_rows, width, band_bytes) ||
      !checked_multiply(band_bytes, 3U * sample_bytes, band_bytes) ||
      band_bytes > std::numeric_limits<std::size_t>::max()) {
    error = "native render band buffer overflows";
    return false;
  }
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(band_bytes));
  std::vector<std::uint8_t> coverage(
      coverage_writer == nullptr && preview.plan.allow_incomplete
          ? 0U
          : static_cast<std::size_t>(band_rows) * width);
  CodecErrorCategory category{};
  for (unsigned row = 0; row < height; row += band_rows) {
    if (cancelled(options)) {
      error = "native render cancelled";
      return false;
    }
    const unsigned rows = std::min(band_rows, height - row);
    for (auto &frame : frames) {
      frame.row_start = row;
      frame.row_count = rows;
    }
    if (!compose())
      return false;
    if (sdr) {
      if (pq) {
        if (!gpu_ok(pano_gpu_output_tone_map_rec2020(
                        output,
                        static_cast<float>(
                            preview.source.encoding.reference_white_nits),
                        gpu_error.data(),
                        static_cast<std::uint32_t>(gpu_error.size())),
                    gpu_error, "cannot tone-map native render", error) ||
            !gpu_ok(pano_gpu_output_convert_tone_mapped_rec2020_to_linear_srgb(
                        output, gpu_error.data(),
                        static_cast<std::uint32_t>(gpu_error.size())),
                    gpu_error, "cannot convert native render primaries",
                    error) ||
            !gpu_ok(pano_gpu_output_apply_auto_contrast_converted_srgb(
                        output, auto_contrast ? 1U : 0U, gpu_error.data(),
                        static_cast<std::uint32_t>(gpu_error.size())),
                    gpu_error, "cannot apply native render contrast", error))
          return false;
      } else if (!gpu_ok(pano_gpu_output_apply_auto_contrast_srgb(
                             output, auto_contrast ? 1U : 0U, gpu_error.data(),
                             static_cast<std::uint32_t>(gpu_error.size())),
                         gpu_error, "cannot apply native render contrast",
                         error)) {
        return false;
      }
      if (!gpu_ok(pano_gpu_output_quantize_normalized_srgb8(
                      output, gpu_error.data(),
                      static_cast<std::uint32_t>(gpu_error.size())),
                  gpu_error, "cannot quantize native render", error))
        return false;
    } else if (!gpu_ok(pano_gpu_output_copy_linear_float(
                           output, gpu_error.data(),
                           static_cast<std::uint32_t>(gpu_error.size())),
                       gpu_error, "cannot copy native float render", error)) {
      return false;
    }
    pano_gpu_output_download_request download{};
    download.size = sizeof(download);
    download.abi_version = PANO_GPU_ABI_VERSION;
    download.output_width = width;
    download.row_start = row;
    download.row_count = rows;
    download.data = pixels.data();
    download.data_bytes =
        static_cast<std::uint64_t>(rows) * width * 3U * sample_bytes;
    const auto download_result =
        sdr ? pano_gpu_output_download_srgb8(
                  output, &download, options.gpu_cancellation, gpu_error.data(),
                  static_cast<std::uint32_t>(gpu_error.size()))
            : pano_gpu_output_download_float(
                  output, &download, options.gpu_cancellation, gpu_error.data(),
                  static_cast<std::uint32_t>(gpu_error.size()));
    if (!gpu_ok(download_result, gpu_error,
                "cannot download native render band", error) ||
        !write_image_rows(writer, pixels.data(), rows,
                          static_cast<std::uint64_t>(width) * 3U * sample_bytes,
                          options.cancellation, category, error))
      return false;
    if (!coverage.empty()) {
      download.data = coverage.data();
      download.data_bytes = static_cast<std::uint64_t>(rows) * width;
      if (!gpu_ok(pano_gpu_output_download_coverage(
                      output, &download, options.gpu_cancellation,
                      gpu_error.data(),
                      static_cast<std::uint32_t>(gpu_error.size())),
                  gpu_error, "cannot download native render coverage", error) ||
          (!preview.plan.allow_incomplete &&
           std::find(coverage.begin(),
                     coverage.begin() + static_cast<std::size_t>(rows) * width,
                     std::uint8_t{0}) !=
               coverage.begin() + static_cast<std::size_t>(rows) * width)) {
        if (error.empty())
          error = "capture does not cover every output pixel";
        return false;
      }
      if (coverage_writer != nullptr &&
          !write_image_rows(coverage_writer, coverage.data(), rows, width,
                            options.cancellation, category, error))
        return false;
    }
    render_progress(options, (auto_contrast ? height : 0U) + row + rows,
                    total_progress, rectilinear ? "thumbnail" : "render");
  }
  return true;
}
} // namespace

bool apply_native_automatic_exposure(NativePreview *const preview,
                                     const unsigned target,
                                     const NativePreviewOptions &options,
                                     NativeExposureResult &result,
                                     std::string &error) {
  if (preview == nullptr || target >= preview->diagnostics.frame_count) {
    error = "invalid native automatic exposure target";
    return false;
  }
  report_progress(options, 0U, "Sampling poses");
  std::vector<float> gains;
  if (!solve_automatic_exposure(*preview, target, options, gains, error))
    return false;
  try {
    NativeExposureResult updated;
    updated.anchor_frame = target;
    if (!replace_preview_pixels(*preview, gains, options, updated, error))
      return false;
    result = std::move(updated);
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate native automatic exposure gains";
    return false;
  }
}

bool apply_native_manual_exposure_match(NativePreview *const preview,
                                        const unsigned target,
                                        const std::vector<unsigned> &selected,
                                        const NativePreviewOptions &options,
                                        NativeExposureResult &result,
                                        std::string &error) {
  if (preview == nullptr || target >= preview->diagnostics.frame_count ||
      selected.empty() ||
      std::any_of(selected.begin(), selected.end(), [&](const unsigned frame) {
        return frame >= preview->diagnostics.frame_count || frame == target;
      })) {
    error = "invalid native manual exposure selection";
    return false;
  }
  report_progress(options, 0U, "Sampling poses");
  if (!measure_exposure_graph(*preview, options, error))
    return false;
  try {
    std::vector<double> shifts;
    for (const unsigned selected_frame : selected) {
      for (const auto &equation : preview->exposure_equations) {
        double raw_shift = 0.0;
        if (equation.left_frame_index == target &&
            equation.right_frame_index == selected_frame)
          raw_shift = equation.difference;
        else if (equation.right_frame_index == target &&
                 equation.left_frame_index == selected_frame)
          raw_shift = -equation.difference;
        else
          continue;
        shifts.push_back(raw_shift + std::log(preview->gains[target]) -
                         std::log(preview->gains[selected_frame]));
      }
    }
    if (shifts.empty()) {
      error = "target pose must overlap at least one selected pose";
      return false;
    }
    std::sort(shifts.begin(), shifts.end());
    const auto middle = shifts.size() / 2U;
    const double shift = shifts.size() % 2U == 0U
                             ? 0.5 * (shifts[middle - 1U] + shifts[middle])
                             : shifts[middle];
    const float multiplier = static_cast<float>(std::exp(shift));
    if (!std::isfinite(multiplier) || multiplier <= 0.0F) {
      error = "native manual exposure gain is invalid";
      return false;
    }
    auto gains = preview->gains;
    for (const unsigned frame : selected)
      gains[frame] *= multiplier;
    NativeExposureResult updated;
    updated.anchor_frame = target;
    preview->exposure_edge_count =
        static_cast<unsigned>(preview->exposure_equations.size());
    if (!replace_preview_pixels(*preview, gains, options, updated, error))
      return false;
    result = std::move(updated);
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate native manual exposure state";
    return false;
  }
}

bool discard_native_exposure_edits(NativePreview *const preview,
                                   const NativePreviewOptions &options,
                                   NativeExposureResult &result,
                                   std::string &error) {
  if (preview == nullptr) {
    error = "native preview is not available";
    return false;
  }
  report_progress(options, 0U, "Resetting exposure preview");
  try {
    std::vector<float> gains(preview->diagnostics.frame_count, 1.0F);
    NativeExposureResult updated;
    if (!replace_preview_pixels(*preview, gains, options, updated, error))
      return false;
    result = std::move(updated);
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate native exposure reset";
    return false;
  }
}

bool query_native_render_dimensions(const NativePreview *const preview,
                                    unsigned &width, unsigned &height,
                                    std::string &error) {
  if (preview == nullptr) {
    error = "native preview is not available";
    return false;
  }
  return render_dimensions(*preview, width, height, error);
}

bool query_native_maximum_render_width(const NativePreview *const preview,
                                       unsigned &width, std::string &error) {
  if (preview == nullptr) {
    error = "native preview is not available";
    return false;
  }
  unsigned height = 0U;
  return render_dimensions(*preview, width, height, error, true);
}

bool render_native_session(NativePreview *const preview,
                           const NativeRenderOptions &options,
                           NativeRenderResult &result, std::string &error) {
  if (preview == nullptr || preview->session == nullptr ||
      preview->device == nullptr) {
    error = "native render session is not available";
    return false;
  }
  if (cancelled(options)) {
    error = "native render cancelled";
    return false;
  }
  render_progress(options, 0U, 100U, "Preparing output");
  OutputStage *panorama_stage = nullptr;
  OutputStage *coverage_stage = nullptr;
  OutputStage *thumbnail_stage = nullptr;
  ImageWriter *panorama_writer = nullptr;
  ImageWriter *coverage_writer = nullptr;
  ImageWriter *thumbnail_writer = nullptr;
  struct Cleanup {
    OutputStage **panorama_stage;
    OutputStage **coverage_stage;
    OutputStage **thumbnail_stage;
    ImageWriter **panorama_writer;
    ImageWriter **coverage_writer;
    ImageWriter **thumbnail_writer;
    ~Cleanup() {
      abort_image_writer(thumbnail_writer);
      abort_image_writer(coverage_writer);
      abort_image_writer(panorama_writer);
      abort_output_stage(thumbnail_stage);
      abort_output_stage(coverage_stage);
      abort_output_stage(panorama_stage);
    }
  } cleanup{&panorama_stage,  &coverage_stage,  &thumbnail_stage,
            &panorama_writer, &coverage_writer, &thumbnail_writer};
  try {
    unsigned width = 0;
    unsigned height = 0;
    if (!render_dimensions(*preview, width, height, error))
      return false;
    CodecErrorCategory category{};
    if (!create_output_stage(preview->plan.outputs.panorama.final_path,
                             &panorama_stage, category, error))
      return false;
    if (preview->plan.outputs.coverage.has_value() &&
        !create_output_stage(preview->plan.outputs.coverage->final_path,
                             &coverage_stage, category, error))
      return false;
    if (preview->plan.outputs.thumbnail.has_value() &&
        !create_output_stage(preview->plan.outputs.thumbnail->final_path,
                             &thumbnail_stage, category, error))
      return false;
    render_progress(options, 5U, 100U, "Preparing output");

    const ImageContainer container =
        output_container(preview->plan.outputs.panorama.final_path);
    ImageWriterOptions panorama_options;
    panorama_options.path = output_stage_path(panorama_stage);
    panorama_options.container = container;
    panorama_options.width = width;
    panorama_options.height = height;
    panorama_options.sample_type =
        container == ImageContainer::exr ? "float32" : "uint8";
    panorama_options.encoding =
        container == ImageContainer::exr
            ? ImageEncoding{"float32", "rec2020", "linear",
                            preview->source.encoding.reference_white_nits}
            : ImageEncoding{};
    panorama_options.jpeg_quality = preview->plan.jpeg_quality;
    if (!create_image_writer(panorama_options, &panorama_writer, category,
                             error))
      return false;
    if (coverage_stage != nullptr) {
      ImageWriterOptions coverage_options;
      coverage_options.path = output_stage_path(coverage_stage);
      coverage_options.container = ImageContainer::png;
      coverage_options.width = width;
      coverage_options.height = height;
      coverage_options.channels = 1;
      if (!create_image_writer(coverage_options, &coverage_writer, category,
                               error))
        return false;
    }
    pano_gpu_memory_plan memory{};
    const bool sdr = container != ImageContainer::exr;
    const bool has_thumbnail = thumbnail_stage != nullptr;
    RenderProgressRange panorama_range;
    const NativeRenderOptions panorama_render_options = ranged_render_options(
        options, panorama_range, 5U, has_thumbnail ? 75U : 95U);
    if (!plan_preview_memory(preview->device, preview->source,
                             preview->diagnostics.frame_count, width, height,
                             preview->preview_cache_bytes, sdr ? 1U : 4U, sdr,
                             preview->plan.gpu_memory_mib, memory, error) ||
        !stream_gpu_output(*preview, panorama_writer, coverage_writer, width,
                           height, false, 0.0F, memory, panorama_render_options,
                           error))
      return false;
    if (!finish_image_writer(&panorama_writer, options.cancellation, category,
                             error) ||
        (coverage_writer != nullptr &&
         !finish_image_writer(&coverage_writer, options.cancellation, category,
                              error)))
      return false;

    if (thumbnail_stage != nullptr) {
      ImageWriterOptions thumbnail_options = panorama_options;
      thumbnail_options.path = output_stage_path(thumbnail_stage);
      thumbnail_options.width = preview->source.width;
      thumbnail_options.height = preview->source.height;
      if (!create_image_writer(thumbnail_options, &thumbnail_writer, category,
                               error))
        return false;
      pano_gpu_memory_plan thumbnail_memory{};
      if (!plan_preview_memory(preview->device, preview->source,
                               preview->diagnostics.frame_count,
                               preview->source.width, preview->source.height,
                               preview->preview_cache_bytes, sdr ? 1U : 4U, sdr,
                               preview->plan.gpu_memory_mib, thumbnail_memory,
                               error))
        return false;
      constexpr double pi = 3.14159265358979323846;
      const float vertical_fov = static_cast<float>(
          2.0 *
          std::atan((static_cast<double>(preview->source.height) /
                     preview->source.width) *
                    std::tan(pi / 4.0)) *
          180.0 / pi);
      RenderProgressRange thumbnail_range;
      const NativeRenderOptions thumbnail_render_options =
          ranged_render_options(options, thumbnail_range, 75U, 95U);
      if (!stream_gpu_output(*preview, thumbnail_writer, nullptr,
                             preview->source.width, preview->source.height,
                             true, vertical_fov, thumbnail_memory,
                             thumbnail_render_options, error) ||
          !finish_image_writer(&thumbnail_writer, options.cancellation,
                               category, error))
        return false;
    }
    if (cancelled(options)) {
      error = "native render cancelled";
      return false;
    }
    std::vector<OutputStage *> stages;
    if (coverage_stage != nullptr)
      stages.push_back(coverage_stage);
    if (thumbnail_stage != nullptr)
      stages.push_back(thumbnail_stage);
    stages.push_back(panorama_stage);
    render_progress(options, 95U, 100U, "Publishing output");
    if (!publish_output_stages(stages, {}, category, error))
      return false;
    NativeRenderResult completed;
    completed.width = width;
    completed.height = height;
    completed.published_paths.reserve(stages.size());
    for (const OutputStage *stage : stages) {
      if (stage == coverage_stage)
        completed.published_paths.push_back(
            preview->plan.outputs.coverage->final_path);
      else if (stage == thumbnail_stage)
        completed.published_paths.push_back(
            preview->plan.outputs.thumbnail->final_path);
      else
        completed.published_paths.push_back(
            preview->plan.outputs.panorama.final_path);
    }
    result = std::move(completed);
    render_progress(options, 100U, 100U, "Output ready");
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate native render state";
    return false;
  } catch (...) {
    error = "unexpected native render failure";
    return false;
  }
}

} // namespace pano::app
