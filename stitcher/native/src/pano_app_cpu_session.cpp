#include "pano_app.h"
#include "pano_gpu.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace pano::app {
namespace {
namespace fs = std::filesystem;

constexpr std::uint64_t cpu_budget_bytes = 2048ULL * 1024ULL * 1024ULL;
constexpr double pi = 3.14159265358979323846;

struct CpuCancellationBridge {
  CancellationCheck cancellation;
  const pano_gpu_cancellation_token *gpu_cancellation = nullptr;
};

bool cpu_cancelled(void *const user_data) {
  const auto &bridge = *static_cast<const CpuCancellationBridge *>(user_data);
  return (bridge.cancellation.callback != nullptr &&
          bridge.cancellation.callback(bridge.cancellation.user_data)) ||
         (bridge.gpu_cancellation != nullptr &&
          pano_gpu_cancellation_token_is_cancelled(
              bridge.gpu_cancellation) != 0);
}

CancellationCheck combined_cancellation(CpuCancellationBridge &bridge) {
  return {cpu_cancelled, &bridge};
}

bool cancelled(const NativePreviewOptions &options) {
  return (options.cancellation.callback != nullptr &&
          options.cancellation.callback(options.cancellation.user_data)) ||
         (options.gpu_cancellation != nullptr &&
          pano_gpu_cancellation_token_is_cancelled(options.gpu_cancellation) !=
              0);
}

bool cancelled(const NativeRenderOptions &options) {
  return (options.cancellation.callback != nullptr &&
          options.cancellation.callback(options.cancellation.user_data)) ||
         (options.gpu_cancellation != nullptr &&
          pano_gpu_cancellation_token_is_cancelled(options.gpu_cancellation) !=
              0);
}

void progress(const NativeRenderOptions &options, const unsigned completed,
              const unsigned total, const char *phase) {
  if (options.progress != nullptr)
    options.progress(options.progress_user_data, completed, total, phase);
}

struct ProgressRange {
  const NativeRenderOptions *outer = nullptr;
  unsigned begin = 0U;
  unsigned end = 100U;
};

void report_progress_range(void *const user_data, const unsigned completed,
                           const unsigned total, const char *const phase) {
  const auto &range = *static_cast<const ProgressRange *>(user_data);
  if (range.outer == nullptr || total == 0U)
    return;
  const auto span = static_cast<std::uint64_t>(range.end - range.begin);
  const auto scaled = static_cast<unsigned>(
      std::min<std::uint64_t>(span, span * completed / total));
  progress(*range.outer, range.begin + scaled, 100U, phase);
}

NativeRenderOptions ranged_options(const NativeRenderOptions &options,
                                   ProgressRange &range, const unsigned begin,
                                   const unsigned end) {
  range = {&options, begin, end};
  NativeRenderOptions ranged = options;
  ranged.progress = report_progress_range;
  ranged.progress_user_data = &range;
  return ranged;
}

CpuSampleType sample_type(const ImageEncoding &encoding) {
  if (encoding.sample_type == "uint16")
    return CpuSampleType::uint16;
  if (encoding.sample_type == "float32")
    return CpuSampleType::float32;
  return CpuSampleType::uint8;
}

unsigned sample_bytes(const ImageEncoding &encoding) {
  if (encoding.sample_type == "uint16")
    return 2U;
  if (encoding.sample_type == "float32")
    return 4U;
  return 1U;
}

CpuTransferFunction transfer(const ImageEncoding &encoding) {
  if (encoding.transfer_function == "pq")
    return CpuTransferFunction::pq;
  if (encoding.transfer_function == "linear")
    return CpuTransferFunction::linear;
  return CpuTransferFunction::srgb;
}

CpuColorPrimaries primaries(const ImageEncoding &encoding) {
  return encoding.color_primaries == "rec2020" ? CpuColorPrimaries::rec2020
                                               : CpuColorPrimaries::srgb;
}

ImageContainer output_container(const std::string &path) {
  const auto extension = fs::u8path(path).extension().u8string();
  if (extension == ".png" || extension == ".PNG")
    return ImageContainer::png;
  if (extension == ".exr" || extension == ".EXR")
    return ImageContainer::exr;
  return ImageContainer::jpeg;
}

std::array<float, 9> world_to_camera(const FrameSummary &frame) {
  std::array<float, 9> result{};
  if (frame.camera_basis_row_major.has_value()) {
    for (unsigned row = 0; row < 3U; ++row)
      for (unsigned column = 0; column < 3U; ++column)
        result[row * 3U + column] = static_cast<float>(
            (*frame.camera_basis_row_major)[column * 3U + row]);
    return result;
  }
  const double yaw = frame.yaw_deg * pi / 180.0;
  const double pitch = -frame.pitch_deg * pi / 180.0;
  const double roll = frame.roll_deg * pi / 180.0;
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
  for (unsigned row = 0; row < 3U; ++row)
    for (unsigned column = 0; column < 3U; ++column)
      result[row * 3U + column] =
          static_cast<float>(camera_to_world[column * 3U + row]);
  return result;
}

bool render_dimensions(const RenderPlan &plan, const ImageInfo &source,
                       unsigned &width, unsigned &height, std::string &error,
                       const bool maximum = false) {
  if (!maximum && plan.output_width.has_value()) {
    width = *plan.output_width;
  } else {
    const double focal =
        source.width /
        (2.0 * std::tan(plan.session.horizontal_fov_deg * pi / 360.0));
    const double scaled =
        std::round(2.0 * pi * focal * (maximum ? 1.0 : plan.resolution));
    if (!std::isfinite(scaled) || scaled < 1.0 ||
        scaled > std::numeric_limits<unsigned>::max()) {
      error = "CPU render dimensions overflow";
      return false;
    }
    width = std::max(2U, static_cast<unsigned>(scaled));
  }
  if (plan.session.capture_mode == "full_sphere") {
    width = std::max(2U, width - width % 2U);
    height = width / 2U;
  } else {
    height = std::max(1U, static_cast<unsigned>(std::round(
                              width * plan.session.vertical_fov_deg / 360.0)));
  }
  error.clear();
  return true;
}

bool compatible_session(const RenderPlan &left, const RenderPlan &right) {
  const auto &a = left.session;
  const auto &b = right.session;
  if (a.schema_version != b.schema_version || a.session_id != b.session_id ||
      a.capture_mode != b.capture_mode || a.projection != b.projection ||
      a.horizontal_fov_deg != b.horizontal_fov_deg ||
      a.vertical_fov_deg != b.vertical_fov_deg ||
      a.overlap_fraction != b.overlap_fraction || a.completed != b.completed ||
      a.image_encoding.sample_type != b.image_encoding.sample_type ||
      a.image_encoding.color_primaries != b.image_encoding.color_primaries ||
      a.image_encoding.transfer_function !=
          b.image_encoding.transfer_function ||
      a.image_encoding.reference_white_nits !=
          b.image_encoding.reference_white_nits ||
      a.frames.size() != b.frames.size())
    return false;
  for (std::size_t index = 0; index < a.frames.size(); ++index) {
    const auto &x = a.frames[index];
    const auto &y = b.frames[index];
    if (x.index != y.index || x.filename != y.filename ||
        x.yaw_deg != y.yaw_deg || x.pitch_deg != y.pitch_deg ||
        x.roll_deg != y.roll_deg || x.status != y.status ||
        x.camera_basis_row_major != y.camera_basis_row_major)
      return false;
  }
  return true;
}

struct Band {
  std::vector<float> rays;
  std::vector<float> coordinates;
  std::vector<std::uint8_t> validity;
  std::vector<float> edge;
  std::vector<float> candidate;
  std::vector<float> color;
  std::vector<float> weight;
  std::vector<std::uint8_t> coverage;
};

bool compose_band(const RenderPlan &plan, const ImageInfo &source,
                  const unsigned width, const unsigned height,
                  const unsigned row_start, const unsigned row_count,
                  const bool rectilinear, const float vertical_fov,
                  const std::vector<float> &gains,
                  const CancellationCheck &cancellation, Band &band,
                  std::string &error) {
  const auto is_cancelled = [&] {
    return cancellation.callback != nullptr &&
           cancellation.callback(cancellation.user_data);
  };
  if (is_cancelled()) {
    error = "CPU render cancelled";
    return false;
  }
  const std::uint64_t pixel_count64 =
      static_cast<std::uint64_t>(width) * row_count;
  if (pixel_count64 == 0U ||
      pixel_count64 > std::numeric_limits<unsigned>::max()) {
    error = "CPU output band is too large";
    return false;
  }
  const unsigned pixel_count = static_cast<unsigned>(pixel_count64);
  try {
    band.rays.resize(pixel_count64 * 3U);
    band.coordinates.resize(pixel_count64 * 2U);
    band.validity.resize(pixel_count);
    band.edge.resize(pixel_count);
    band.candidate.resize(pixel_count64 * 3U);
    band.color.assign(pixel_count64 * 3U, 0.0F);
    band.weight.assign(pixel_count, 0.0F);
    band.coverage.assign(pixel_count, 0U);
    CpuRayRequest rays;
    rays.projection = rectilinear ? CpuOutputProjection::rectilinear
                                  : CpuOutputProjection::equirectangular;
    rays.output_width = width;
    rays.output_height = height;
    rays.row_start = row_start;
    rays.row_count = row_count;
    rays.latitude_span_degrees =
        static_cast<float>(plan.session.capture_mode == "full_sphere"
                               ? 180.0
                               : plan.session.vertical_fov_deg);
    rays.rectilinear_vertical_fov_degrees = vertical_fov;
    if (!generate_cpu_world_rays(rays, band.rays.data(),
                                 band.rays.size() * sizeof(float), error))
      return false;
    if (is_cancelled()) {
      error = "CPU render cancelled";
      return false;
    }
    const unsigned bytes = sample_bytes(source.encoding);
    const std::uint64_t row_stride =
        static_cast<std::uint64_t>(source.width) * 3U * bytes;
    const std::uint64_t source_bytes = row_stride * source.height;
    if (source_bytes > std::numeric_limits<std::size_t>::max()) {
      error = "CPU source image is too large";
      return false;
    }
    std::vector<std::uint8_t> decoded(static_cast<std::size_t>(source_bytes));
    CodecErrorCategory category{};
    for (std::size_t frame_index = 0; frame_index < plan.session.frames.size();
         ++frame_index) {
      if (is_cancelled()) {
        error = "CPU render cancelled";
        return false;
      }
      ImageInfo actual;
      if (!inspect_image(plan.session.frames[frame_index].filename, actual,
                         category, error) ||
          actual.width != source.width || actual.height != source.height ||
          actual.channels != 3U ||
          actual.encoding.sample_type != source.encoding.sample_type ||
          actual.encoding.color_primaries != source.encoding.color_primaries ||
          actual.encoding.transfer_function !=
              source.encoding.transfer_function) {
        if (error.empty())
          error = "CPU source images do not match";
        return false;
      }
      if (!decode_image(plan.session.frames[frame_index].filename, source,
                        decoded.data(), row_stride, source_bytes, cancellation,
                        category, error))
        return false;
      CpuProjectionRequest projection;
      projection.pixel_count = pixel_count;
      projection.source_width = source.width;
      projection.source_height = source.height;
      projection.horizontal_fov_degrees =
          static_cast<float>(plan.session.horizontal_fov_deg);
      projection.vertical_fov_degrees =
          static_cast<float>(plan.session.vertical_fov_deg);
      projection.world_to_camera =
          world_to_camera(plan.session.frames[frame_index]);
      if (!project_cpu_world_rays(
              projection, band.rays.data(), band.rays.size() * sizeof(float),
              band.coordinates.data(), band.coordinates.size() * sizeof(float),
              band.validity.data(), band.validity.size(), band.edge.data(),
              band.edge.size() * sizeof(float), error))
        return false;
      CpuSampleRequest sampling;
      sampling.sample_type = sample_type(source.encoding);
      sampling.transfer_function = transfer(source.encoding);
      sampling.source_width = source.width;
      sampling.source_height = source.height;
      sampling.source_row_stride_bytes = row_stride;
      sampling.pixel_count = pixel_count;
      if (!sample_cpu_bilinear(
              sampling, decoded.data(), decoded.size(), band.coordinates.data(),
              band.coordinates.size() * sizeof(float), band.validity.data(),
              band.validity.size(), band.candidate.data(),
              band.candidate.size() * sizeof(float), error))
        return false;
      const float gain = frame_index < gains.size() ? gains[frame_index] : 1.0F;
      if (!apply_cpu_global_gain(pixel_count, gain, band.candidate.data(),
                                 error))
        return false;
      if (plan.blend == "hard") {
        if (!select_cpu_hard(pixel_count, band.candidate.data(),
                             band.validity.data(), band.edge.data(),
                             band.color.data(), band.weight.data(),
                             band.coverage.data(), error))
          return false;
      } else if (!accumulate_cpu_feather(pixel_count, source.width,
                                         source.height, band.candidate.data(),
                                         band.validity.data(), band.edge.data(),
                                         band.color.data(), band.weight.data(),
                                         error)) {
        return false;
      }
    }
    if (plan.blend != "hard" &&
        !normalize_cpu_feather(pixel_count, band.color.data(),
                               band.weight.data(), band.coverage.data(), error))
      return false;
    if (!plan.allow_incomplete &&
        std::find(band.coverage.begin(), band.coverage.end(), 0U) !=
            band.coverage.end()) {
      error = "capture does not cover every output pixel";
      return false;
    }
    if (plan.allow_incomplete &&
        !mark_cpu_incomplete(pixel_count, band.color.data(), band.weight.data(),
                             error))
      return false;
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate CPU output band";
    return false;
  }
}

bool plan_band_height(const ImageInfo &source, const unsigned width,
                      const unsigned height, unsigned &rows,
                      std::string &error) {
  CpuRenderPlanRequest request;
  request.source_width = source.width;
  request.source_height = source.height;
  request.output_width = width;
  request.output_height = height;
  request.memory_budget_bytes = cpu_budget_bytes;
  request.worker_count = 1U;
  CpuRenderPlan plan;
  if (!plan_cpu_render(request, plan, error))
    return false;
  rows = std::max(1U, plan.strip_height);
  return true;
}

bool make_conversion_request(const ImageInfo &source, const double exposure_ev,
                             const bool contrast,
                             const CpuAutoContrastLevels &levels,
                             const unsigned pixels,
                             CpuSdrConversionRequest &request) {
  request.pixel_count = pixels;
  request.source_transfer = transfer(source.encoding);
  request.source_primaries = primaries(source.encoding);
  request.reference_white_nits =
      static_cast<float>(source.encoding.reference_white_nits);
  request.exposure_multiplier = static_cast<float>(std::exp2(exposure_ev));
  request.apply_auto_contrast = contrast;
  request.levels = levels;
  return true;
}

} // namespace

class CpuNativePreview {
public:
  RenderPlan plan;
  ImageInfo source;
  NativePreviewDiagnostics diagnostics;
  std::vector<std::uint8_t> pixels;
  std::vector<float> gains;
  std::vector<CpuExposureEquation> exposure_equations;
  unsigned measured_exposure_edge_count = 0U;
  bool exposure_graph_measured = false;
};

namespace {
void preview_progress(const NativePreviewOptions &options,
                      const unsigned completed, const char *const phase) {
  if (options.progress != nullptr)
    options.progress(options.progress_user_data, completed, 100U, phase);
}

bool recompose_cpu_preview(CpuNativePreview &preview,
                           const std::vector<float> &gains,
                           const NativePreviewOptions &options,
                           NativeExposureResult &result,
                           std::string &error) {
  CpuCancellationBridge cancellation_bridge{options.cancellation,
                                            options.gpu_cancellation};
  Band band;
  if (!compose_band(preview.plan, preview.source,
                    preview.diagnostics.preview_width,
                    preview.diagnostics.preview_height, 0U,
                    preview.diagnostics.preview_height, false, 90.0F, gains,
                    combined_cancellation(cancellation_bridge), band, error))
    return false;
  CpuAutoContrastLevels levels;
  CpuSdrConversionRequest conversion;
  const unsigned pixel_count = preview.diagnostics.preview_width *
                               preview.diagnostics.preview_height;
  make_conversion_request(preview.source, preview.plan.final_exposure_ev, false,
                          levels, pixel_count,
                          conversion);
  if (preview.plan.auto_contrast) {
    std::array<std::uint64_t, 4096> histogram{};
    if (!accumulate_cpu_auto_contrast_histogram(
            conversion, band.color.data(), band.coverage.data(), histogram,
            error) ||
        !select_cpu_auto_contrast_levels(histogram, levels, error))
      return false;
    conversion.apply_auto_contrast = true;
    conversion.levels = levels;
  }
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(pixel_count) * 3U);
  if (!convert_cpu_sdr8_band(conversion, band.color.data(), rgb.data(), error))
    return false;
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(pixel_count) * 4U);
  for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
    pixels[pixel * 4U] = rgb[pixel * 3U + 2U];
    pixels[pixel * 4U + 1U] = rgb[pixel * 3U + 1U];
    pixels[pixel * 4U + 2U] = rgb[pixel * 3U];
    pixels[pixel * 4U + 3U] = 255U;
  }
  preview.pixels.swap(pixels);
  preview.gains = gains;
  result.gains = gains;
  result.edge_count = static_cast<unsigned>(preview.exposure_equations.size());
  result.warning = preview.diagnostics.frame_count > 1U &&
                   result.edge_count < preview.diagnostics.frame_count - 1U;
  preview_progress(options, 95U, "Retaining CPU preview");
  error.clear();
  return true;
}

bool measure_cpu_exposure_graph(CpuNativePreview &preview,
                                const NativePreviewOptions &options,
                                std::string &error) {
  if (preview.exposure_graph_measured) {
    preview_progress(options, 40U, "Exposure samples ready");
    return true;
  }
  if (cancelled(options)) {
    error = "CPU exposure measurement cancelled";
    return false;
  }
  const unsigned frame_count = preview.diagnostics.frame_count;
  const unsigned proxy_width = std::min(preview.source.width, 256U);
  const unsigned proxy_height = std::min(preview.source.height, 256U);
  const std::uint64_t source_stride =
      static_cast<std::uint64_t>(preview.source.width) * 3U *
      sample_bytes(preview.source.encoding);
  const std::uint64_t source_bytes = source_stride * preview.source.height;
  const std::size_t proxy_values =
      static_cast<std::size_t>(proxy_width) * proxy_height * 3U;
  try {
    std::vector<std::vector<float>> proxies(
        frame_count, std::vector<float>(proxy_values));
    std::vector<std::uint8_t> decoded(static_cast<std::size_t>(source_bytes));
    CpuExposureProxyRequest proxy_request;
    proxy_request.source.sample_type = sample_type(preview.source.encoding);
    proxy_request.source.transfer_function = transfer(preview.source.encoding);
    proxy_request.source.source_width = preview.source.width;
    proxy_request.source.source_height = preview.source.height;
    proxy_request.source.source_row_stride_bytes = source_stride;
    proxy_request.source.pixel_count =
        preview.source.width * preview.source.height;
    proxy_request.proxy_width = proxy_width;
    proxy_request.proxy_height = proxy_height;
    CodecErrorCategory category{};
    CpuCancellationBridge cancellation_bridge{options.cancellation,
                                              options.gpu_cancellation};
    const CancellationCheck cancellation =
        combined_cancellation(cancellation_bridge);
    for (unsigned frame = 0; frame < frame_count; ++frame) {
      if (cancelled(options)) {
        error = "CPU exposure measurement cancelled";
        return false;
      }
      if (!decode_image(preview.plan.session.frames[frame].filename,
                        preview.source, decoded.data(), source_stride,
                        source_bytes, cancellation, category, error) ||
          !build_cpu_exposure_proxy(
              proxy_request, decoded.data(), decoded.size(),
              proxies[frame].data(),
              proxies[frame].size() * sizeof(float), error))
        return false;
    }
    preview_progress(options, 15U, "Sampling poses");
    const unsigned pair_count = frame_count * (frame_count - 1U) / 2U;
    std::vector<CpuFramePair> pairs(pair_count);
    unsigned enumerated = 0U;
    if (!enumerate_cpu_exposure_pairs(frame_count, pairs.data(), pair_count,
                                      enumerated, error))
      return false;
    constexpr unsigned sample_width = 256U;
    constexpr unsigned sample_height = 128U;
    constexpr unsigned sample_count = sample_width * sample_height;
    std::vector<float> coordinates(sample_count * 4U);
    std::vector<std::uint8_t> overlap(sample_count);
    std::vector<float> samples(sample_count * 6U);
    std::vector<float> luminance(sample_count * 2U);
    std::vector<std::uint8_t> accepted(sample_count);
    std::vector<float> gradients(sample_count * 2U);
    std::vector<CpuExposurePairMeasurement> measurements(pair_count);
    CpuExposurePairRequest request;
    request.sample_width = sample_width;
    request.sample_height = sample_height;
    request.proxy_width = proxy_width;
    request.proxy_height = proxy_height;
    request.latitude_span_degrees = static_cast<float>(
        preview.plan.session.capture_mode == "full_sphere"
            ? 180.0
            : preview.plan.session.vertical_fov_deg);
    request.horizontal_fov_degrees =
        static_cast<float>(preview.plan.session.horizontal_fov_deg);
    request.vertical_fov_degrees =
        static_cast<float>(preview.plan.session.vertical_fov_deg);
    for (unsigned index = 0; index < enumerated; ++index) {
      if (cancelled(options)) {
        error = "CPU exposure measurement cancelled";
        return false;
      }
      const auto pair = pairs[index];
      request.left_world_to_camera =
          world_to_camera(preview.plan.session.frames[pair.left]);
      request.right_world_to_camera =
          world_to_camera(preview.plan.session.frames[pair.right]);
      std::array<float, 2> limits{};
      if (!project_cpu_exposure_pair(request, coordinates.data(),
                                     overlap.data(), error) ||
          !sample_cpu_exposure_pair(request, proxies[pair.left].data(),
                                    proxies[pair.right].data(),
                                    coordinates.data(), samples.data(), error) ||
          !classify_cpu_exposure_samples(
              sample_count, transfer(preview.source.encoding), samples.data(),
              overlap.data(), luminance.data(), accepted.data(), error) ||
          !calculate_cpu_exposure_gradients(sample_width, sample_height,
                                            luminance.data(), gradients.data(),
                                            error) ||
          !filter_cpu_exposure_gradients(sample_count, gradients.data(),
                                         accepted.data(), limits, error))
        return false;
      measurements[index].pair = pair;
      measurements[index].geometric_count = static_cast<unsigned>(
          std::count(overlap.begin(), overlap.end(), std::uint8_t{1}));
      if (!reduce_cpu_exposure_pair(sample_count, luminance.data(),
                                    accepted.data(),
                                    measurements[index].reduction, error))
        return false;
      preview_progress(options, 25U + (index + 1U) * 15U /
                                            std::max(1U, enumerated),
                       "Measuring exposure overlaps");
    }
    if (!build_cpu_exposure_solve_graph(
            frame_count, measurements.data(), enumerated,
            preview.exposure_equations, error))
      return false;
    preview.measured_exposure_edge_count = static_cast<unsigned>(std::count_if(
        measurements.begin(), measurements.end(),
        [](const CpuExposurePairMeasurement &measurement) {
          return measurement.reduction.rejection ==
                 CpuExposurePairRejection::accepted;
        }));
    preview.exposure_graph_measured = true;
    preview_progress(options, 40U, "Exposure samples ready");
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate CPU exposure measurement storage";
    return false;
  }
}
} // namespace

bool create_cpu_native_preview(const RenderPlan &plan,
                               const NativePreviewOptions &options,
                               CpuNativePreview **const preview,
                               std::string &error) {
  if (preview == nullptr || *preview != nullptr ||
      options.viewport_width == 0U || plan.session.frames.empty()) {
    error = "invalid CPU preview request";
    return false;
  }
  if (cancelled(options)) {
    error = "CPU preview creation cancelled";
    return false;
  }
  preview_progress(options, 0U, "Preparing CPU preview");
  try {
    auto owner = std::make_unique<CpuNativePreview>();
    CodecErrorCategory category{};
    if (!inspect_image(plan.session.frames.front().filename, owner->source,
                       category, error) ||
        owner->source.channels != 3U) {
      if (error.empty())
        error = "CPU preview source encoding is unsupported";
      return false;
    }
    unsigned width = options.viewport_width;
    unsigned height =
        plan.session.capture_mode == "full_sphere"
            ? std::max(1U, width / 2U)
            : std::max(1U, static_cast<unsigned>(std::round(
                               width * plan.session.vertical_fov_deg / 360.0)));
    if (plan.session.capture_mode == "full_sphere")
      width = std::max(2U, width - width % 2U);
    Band band;
    owner->gains.assign(plan.session.frames.size(), 1.0F);
    CpuCancellationBridge cancellation_bridge{options.cancellation,
                                              options.gpu_cancellation};
    if (!compose_band(plan, owner->source, width, height, 0U, height, false,
                      90.0F, owner->gains,
                      combined_cancellation(cancellation_bridge), band,
                      error))
      return false;
    CpuAutoContrastLevels levels;
    CpuSdrConversionRequest conversion;
    make_conversion_request(owner->source, plan.final_exposure_ev, false,
                            levels, width * height, conversion);
    if (plan.auto_contrast) {
      std::array<std::uint64_t, 4096> histogram{};
      if (!accumulate_cpu_auto_contrast_histogram(conversion, band.color.data(),
                                                  band.coverage.data(),
                                                  histogram, error) ||
          !select_cpu_auto_contrast_levels(histogram, levels, error))
        return false;
      conversion.apply_auto_contrast = true;
      conversion.levels = levels;
    }
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * height *
                                  3U);
    if (!convert_cpu_sdr8_band(conversion, band.color.data(), rgb.data(),
                               error))
      return false;
    owner->pixels.resize(static_cast<std::size_t>(width) * height * 4U);
    for (std::size_t pixel = 0;
         pixel < static_cast<std::size_t>(width) * height; ++pixel) {
      owner->pixels[pixel * 4U] = rgb[pixel * 3U + 2U];
      owner->pixels[pixel * 4U + 1U] = rgb[pixel * 3U + 1U];
      owner->pixels[pixel * 4U + 2U] = rgb[pixel * 3U];
      owner->pixels[pixel * 4U + 3U] = 255U;
    }
    owner->plan = plan;
    owner->diagnostics.frame_count =
        static_cast<unsigned>(plan.session.frames.size());
    owner->diagnostics.preview_width = width;
    owner->diagnostics.preview_height = height;
    owner->diagnostics.overview_width = width;
    owner->diagnostics.overview_height = height;
    *preview = owner.release();
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate CPU preview";
    return false;
  }
}

bool rebuild_cpu_native_preview(CpuNativePreview *const preview,
                                const RenderPlan &plan,
                                const NativePreviewOptions &options,
                                std::string &error) {
  if (preview == nullptr || preview->pixels.empty()) {
    error = "CPU preview is not available";
    return false;
  }
  try {
    RenderPlan previous = preview->plan;
    if (!update_cpu_native_preview_render_plan(preview, plan, error))
      return false;
    NativeExposureResult rebuilt;
    if (!recompose_cpu_preview(*preview, preview->gains, options, rebuilt,
                               error)) {
      preview->plan = std::move(previous);
      return false;
    }
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot rebuild CPU preview";
    return false;
  }
}

bool query_cpu_native_preview(const CpuNativePreview *const preview,
                              NativePreviewDiagnostics &diagnostics,
                              std::string &error) {
  if (preview == nullptr || preview->pixels.empty()) {
    error = "CPU preview is not available";
    return false;
  }
  diagnostics = preview->diagnostics;
  error.clear();
  return true;
}

const std::vector<std::uint8_t> &
cpu_native_preview_pixels(const CpuNativePreview *const preview) noexcept {
  static const std::vector<std::uint8_t> empty;
  return preview == nullptr ? empty : preview->pixels;
}

bool update_cpu_native_preview_render_plan(CpuNativePreview *const preview,
                                           const RenderPlan &plan,
                                           std::string &error) {
  if (preview == nullptr || !compatible_session(preview->plan, plan)) {
    error = "CPU render plan does not match the retained session";
    return false;
  }
  try {
    preview->plan = plan;
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot update CPU render plan";
    return false;
  }
}

bool apply_cpu_native_automatic_exposure(
    CpuNativePreview *const preview, const unsigned target,
    const NativePreviewOptions &options, NativeExposureResult &result,
    std::string &error) {
  if (preview == nullptr || target >= preview->diagnostics.frame_count) {
    error = "invalid CPU automatic exposure target";
    return false;
  }
  if (!measure_cpu_exposure_graph(*preview, options, error))
    return false;
  preview_progress(options, 45U, "Propagating exposure");
  std::vector<float> gains;
  if (!solve_reference_exposure_gains(
          preview->diagnostics.frame_count, target,
          preview->exposure_equations, preview->measured_exposure_edge_count,
          preview->gains, gains, error) ||
      !recompose_cpu_preview(*preview, gains, options, result, error))
    return false;
  result.anchor_frame = target;
  return true;
}

bool apply_cpu_native_manual_exposure_match(
    CpuNativePreview *const preview, const unsigned target,
    const std::vector<unsigned> &selected,
    const NativePreviewOptions &options, NativeExposureResult &result,
    std::string &error) {
  if (preview == nullptr || target >= preview->diagnostics.frame_count ||
      selected.empty() ||
      std::any_of(selected.begin(), selected.end(), [&](const unsigned frame) {
        return frame >= preview->diagnostics.frame_count || frame == target;
      })) {
    error = "invalid CPU manual exposure selection";
    return false;
  }
  if (!measure_cpu_exposure_graph(*preview, options, error))
    return false;
  try {
    std::vector<double> shifts;
    for (const unsigned selected_frame : selected) {
      for (unsigned index = 0; index < preview->measured_exposure_edge_count;
           ++index) {
        const auto &equation = preview->exposure_equations[index];
        double raw_shift = 0.0;
        if (equation.left == target && equation.right == selected_frame)
          raw_shift = equation.difference;
        else if (equation.right == target && equation.left == selected_frame)
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
    const std::size_t middle = shifts.size() / 2U;
    const double shift = shifts.size() % 2U == 0U
                             ? 0.5 * (shifts[middle - 1U] + shifts[middle])
                             : shifts[middle];
    const float multiplier = static_cast<float>(std::exp(shift));
    if (!std::isfinite(multiplier) || multiplier <= 0.0F) {
      error = "CPU manual exposure gain is invalid";
      return false;
    }
    auto gains = preview->gains;
    for (const unsigned frame : selected)
      gains[frame] *= multiplier;
    if (!recompose_cpu_preview(*preview, gains, options, result, error))
      return false;
    result.anchor_frame = target;
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate CPU manual exposure state";
    return false;
  }
}

bool discard_cpu_native_exposure_edits(
    CpuNativePreview *const preview, const NativePreviewOptions &options,
    NativeExposureResult &result, std::string &error) {
  if (preview == nullptr) {
    error = "CPU preview is not available";
    return false;
  }
  preview_progress(options, 0U, "Resetting CPU exposure preview");
  try {
    std::vector<float> gains(preview->diagnostics.frame_count, 1.0F);
    return recompose_cpu_preview(*preview, gains, options, result, error);
  } catch (const std::bad_alloc &) {
    error = "cannot allocate CPU exposure reset";
    return false;
  }
}

namespace {
bool render_cpu_target(CpuNativePreview &preview, const OutputTarget &target,
                       const std::optional<OutputTarget> &coverage_target,
                       const unsigned width, const unsigned height,
                       const bool rectilinear, const float vertical_fov,
                       const NativeRenderOptions &options,
                       OutputStage **const output_stage,
                       OutputStage **const coverage_stage, std::string &error) {
  CpuCancellationBridge cancellation_bridge{options.cancellation,
                                            options.gpu_cancellation};
  const CancellationCheck cancellation =
      combined_cancellation(cancellation_bridge);
  CodecErrorCategory category{};
  if (!create_output_stage(target.final_path, output_stage, category, error) ||
      (coverage_target.has_value() &&
       !create_output_stage(coverage_target->final_path, coverage_stage,
                            category, error)))
    return false;
  const ImageContainer container = output_container(target.final_path);
  ImageWriter *writer = nullptr;
  ImageWriter *coverage_writer = nullptr;
  struct WriterCleanup {
    ImageWriter **writer;
    ImageWriter **coverage;
    ~WriterCleanup() {
      abort_image_writer(coverage);
      abort_image_writer(writer);
    }
  } cleanup{&writer, &coverage_writer};
  ImageWriterOptions writer_options;
  writer_options.path = output_stage_path(*output_stage);
  writer_options.container = container;
  writer_options.width = width;
  writer_options.height = height;
  writer_options.sample_type =
      container == ImageContainer::exr ? "float32" : "uint8";
  writer_options.encoding =
      container == ImageContainer::exr
          ? ImageEncoding{"float16", "rec2020", "linear",
                          preview.source.encoding.reference_white_nits}
          : ImageEncoding{};
  writer_options.jpeg_quality = preview.plan.jpeg_quality;
  if (!create_image_writer(writer_options, &writer, category, error))
    return false;
  if (coverage_target.has_value()) {
    ImageWriterOptions coverage_options;
    coverage_options.path = output_stage_path(*coverage_stage);
    coverage_options.container = ImageContainer::png;
    coverage_options.width = width;
    coverage_options.height = height;
    coverage_options.channels = 1U;
    if (!create_image_writer(coverage_options, &coverage_writer, category,
                             error))
      return false;
  }
  unsigned band_rows = 0;
  if (!plan_band_height(preview.source, width, height, band_rows, error))
    return false;
  const bool sdr = container != ImageContainer::exr;
  const bool auto_contrast = sdr && preview.plan.auto_contrast;
  CpuAutoContrastLevels levels;
  if (auto_contrast) {
    std::array<std::uint64_t, 4096> histogram{};
    for (unsigned row = 0; row < height; row += band_rows) {
      if (cancelled(options)) {
        error = "CPU render cancelled";
        return false;
      }
      const unsigned rows = std::min(band_rows, height - row);
      Band band;
      if (!compose_band(preview.plan, preview.source, width, height, row, rows,
                        rectilinear, vertical_fov, preview.gains,
                        cancellation, band, error))
        return false;
      CpuSdrConversionRequest conversion;
      make_conversion_request(preview.source, preview.plan.final_exposure_ev,
                              false, levels, width * rows, conversion);
      if (!accumulate_cpu_auto_contrast_histogram(conversion, band.color.data(),
                                                  band.coverage.data(),
                                                  histogram, error))
        return false;
      progress(options, row + rows, height * 2U, "CPU contrast");
    }
    if (!select_cpu_auto_contrast_levels(histogram, levels, error))
      return false;
  }
  for (unsigned row = 0; row < height; row += band_rows) {
    if (cancelled(options)) {
      error = "CPU render cancelled";
      return false;
    }
    const unsigned rows = std::min(band_rows, height - row);
    Band band;
    if (!compose_band(preview.plan, preview.source, width, height, row, rows,
                      rectilinear, vertical_fov, preview.gains,
                      cancellation, band, error))
      return false;
    CpuSdrConversionRequest conversion;
    make_conversion_request(preview.source, preview.plan.final_exposure_ev,
                            auto_contrast, levels, width * rows, conversion);
    std::vector<std::uint8_t> scratch;
    if (sdr)
      scratch.resize(static_cast<std::size_t>(width) * rows * 3U);
    if (!convert_and_write_cpu_band(
            writer,
            sdr ? CpuOutputBandSample::srgb8
                : CpuOutputBandSample::linear_float32,
            conversion, width, rows, band.color.data(),
            sdr ? static_cast<void *>(scratch.data()) : nullptr,
            sdr ? scratch.size() : 0U, cancellation, category, error) ||
        (coverage_writer != nullptr &&
         !write_image_rows(coverage_writer, band.coverage.data(), rows, width,
                           cancellation, category, error)))
      return false;
    progress(options, (auto_contrast ? height : 0U) + row + rows,
             height * (auto_contrast ? 2U : 1U), "CPU render");
  }
  return finish_image_writer(&writer, cancellation, category, error) &&
         (coverage_writer == nullptr ||
          finish_image_writer(&coverage_writer, cancellation, category,
                              error));
}
} // namespace

bool query_cpu_native_render_dimensions(const CpuNativePreview *const preview,
                                        unsigned &width, unsigned &height,
                                        std::string &error) {
  if (preview == nullptr) {
    error = "CPU preview is not available";
    return false;
  }
  return render_dimensions(preview->plan, preview->source, width, height,
                           error);
}

bool query_cpu_native_maximum_render_width(
    const CpuNativePreview *const preview, unsigned &width,
    std::string &error) {
  if (preview == nullptr) {
    error = "CPU preview is not available";
    return false;
  }
  unsigned height = 0U;
  return render_dimensions(preview->plan, preview->source, width, height, error,
                           true);
}

bool render_cpu_native_session(CpuNativePreview *const preview,
                               const NativeRenderOptions &options,
                               NativeRenderResult &result, std::string &error) {
  if (preview == nullptr || preview->pixels.empty()) {
    error = "CPU render session is not available";
    return false;
  }
  if (cancelled(options)) {
    error = "CPU render cancelled";
    return false;
  }
  progress(options, 0U, 100U, "Preparing CPU output");
  OutputStage *panorama = nullptr;
  OutputStage *coverage = nullptr;
  OutputStage *thumbnail = nullptr;
  struct StageCleanup {
    OutputStage **panorama;
    OutputStage **coverage;
    OutputStage **thumbnail;
    ~StageCleanup() {
      abort_output_stage(thumbnail);
      abort_output_stage(coverage);
      abort_output_stage(panorama);
    }
  } cleanup{&panorama, &coverage, &thumbnail};
  try {
    unsigned width = 0;
    unsigned height = 0;
    if (!render_dimensions(preview->plan, preview->source, width, height,
                           error))
      return false;
    const bool has_thumbnail = preview->plan.outputs.thumbnail.has_value();
    ProgressRange panorama_range;
    const NativeRenderOptions panorama_options =
        ranged_options(options, panorama_range, 5U, has_thumbnail ? 75U : 95U);
    progress(options, 5U, 100U, "Preparing CPU output");
    if (!render_cpu_target(*preview, preview->plan.outputs.panorama,
                           preview->plan.outputs.coverage, width, height, false,
                           90.0F, panorama_options, &panorama, &coverage,
                           error))
      return false;
    if (has_thumbnail) {
      const float thumbnail_vertical_fov = static_cast<float>(
          2.0 *
          std::atan((static_cast<double>(preview->source.height) /
                     preview->source.width) *
                    std::tan(pi / 4.0)) *
          180.0 / pi);
      ProgressRange thumbnail_range;
      const NativeRenderOptions thumbnail_options =
          ranged_options(options, thumbnail_range, 75U, 95U);
      if (!render_cpu_target(*preview, *preview->plan.outputs.thumbnail,
                             std::nullopt, preview->source.width,
                             preview->source.height, true,
                             thumbnail_vertical_fov, thumbnail_options,
                             &thumbnail, nullptr, error))
        return false;
    }
    std::vector<OutputStage *> stages;
    if (coverage != nullptr)
      stages.push_back(coverage);
    if (thumbnail != nullptr)
      stages.push_back(thumbnail);
    stages.push_back(panorama);
    CodecErrorCategory category{};
    progress(options, 95U, 100U, "Publishing CPU output");
    if (!publish_output_stages(stages, {}, category, error))
      return false;
    result.width = width;
    result.height = height;
    result.published_paths.clear();
    if (preview->plan.outputs.coverage.has_value())
      result.published_paths.push_back(
          preview->plan.outputs.coverage->final_path);
    if (preview->plan.outputs.thumbnail.has_value())
      result.published_paths.push_back(
          preview->plan.outputs.thumbnail->final_path);
    result.published_paths.push_back(preview->plan.outputs.panorama.final_path);
    progress(options, 100U, 100U, "CPU output ready");
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate CPU render session";
    return false;
  }
}

void destroy_cpu_native_preview(CpuNativePreview **const preview) noexcept {
  if (preview == nullptr || *preview == nullptr)
    return;
  delete *preview;
  *preview = nullptr;
}

} // namespace pano::app
