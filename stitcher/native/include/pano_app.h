#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

struct pano_gpu_cancellation_token;
struct pano_gpu_device;
struct pano_gpu_preview;
struct pano_gpu_session;

namespace pano::app {

class ImageWriter;
class OutputStage;
class CpuRenderStorage;
class CpuWorkerPool;
class CpuExposureCache;
class CpuRenderCoordinator;
class NativePreview;
class CpuNativePreview;

enum class ExitCode : int {
  success = 0,
  invalid_input = 2,
  unavailable = 3,
  failure = 4,
};

enum class CodecErrorCategory {
  none,
  invalid_request,
  io,
  malformed,
  unsupported,
  cancelled,
  unavailable,
  allocation,
  codec_failure,
};

enum class ImageContainer { jpeg, png, exr };

struct RenderOptions {
  std::string session;
  std::string image_dir;
  std::string output;
  std::optional<unsigned> width;
  double resolution = 1.0;
  std::string format = "jpeg";
  unsigned jpeg_quality = 95;
  std::string blend = "hard";
  bool thumbnail = false;
  bool coverage = false;
  unsigned memory_mib = 1024;
  unsigned workers = 0;
  bool gpu = true;
  std::optional<unsigned> gpu_memory_mib;
  bool gpu_strict = false;
  bool allow_incomplete = false;
  bool auto_contrast = true;
  bool automatic_exposure = false;
  std::optional<unsigned> exposure_target;
  std::vector<unsigned> exposure_sources;
};

struct ImageEncoding {
  std::string sample_type = "uint8";
  std::string color_primaries = "srgb";
  std::string transfer_function = "srgb";
  double reference_white_nits = 100.0;
};

struct ImageInfo {
  ImageContainer container = ImageContainer::png;
  unsigned width = 0;
  unsigned height = 0;
  unsigned channels = 0;
  ImageEncoding encoding;
  std::optional<std::string> jpeg_subsampling;
  std::optional<std::array<std::uint8_t, 4>> png_cicp;
  std::optional<std::array<std::int32_t, 4>> exr_data_window;
  std::optional<std::array<std::int32_t, 4>> exr_display_window;
  std::optional<std::string> exr_compression;
};

struct ImageWriterOptions {
  std::string path;
  ImageContainer container = ImageContainer::png;
  unsigned width = 0;
  unsigned height = 0;
  unsigned channels = 3;
  std::string sample_type = "uint8";
  ImageEncoding encoding;
  unsigned jpeg_quality = 95;
};

struct CpuRenderPlanRequest {
  unsigned source_width = 0;
  unsigned source_height = 0;
  unsigned output_width = 0;
  unsigned output_height = 0;
  std::uint64_t memory_budget_bytes = 0;
  unsigned worker_count = 0;
};

struct CpuRenderPlan {
  unsigned worker_count = 0;
  unsigned strip_height = 0;
  std::uint64_t source_working_set_bytes = 0;
  std::uint64_t available_strip_bytes = 0;
  std::uint64_t bytes_per_worker_row = 0;
  std::uint64_t scratch_bytes = 0;
};

struct CpuStorageFaultCheck {
  bool (*callback)(void *, const char *) = nullptr;
  void *user_data = nullptr;
};

struct CpuRenderStorageOptions {
  std::string directory;
  CpuRenderPlan plan;
  unsigned output_width = 0;
  unsigned output_height = 0;
  CpuStorageFaultCheck fault;
};

struct CpuRenderStorageDiagnostics {
  std::uint64_t mapped_scratch_bytes = 0;
  std::uint64_t worker_strip_bytes = 0;
  std::uint64_t live_bytes = 0;
  std::uint64_t peak_live_bytes = 0;
};

enum class CpuOutputProjection { equirectangular, rectilinear };
enum class CpuSampleType { uint8, uint16, float32 };
enum class CpuTransferFunction { srgb, pq, linear };
enum class CpuColorPrimaries { srgb, rec2020 };
enum class CpuOutputBandSample { srgb8, linear_float32 };

struct CpuRayRequest {
  CpuOutputProjection projection = CpuOutputProjection::equirectangular;
  unsigned output_width = 0;
  unsigned output_height = 0;
  unsigned row_start = 0;
  unsigned row_count = 0;
  float latitude_span_degrees = 180.0F;
  float rectilinear_vertical_fov_degrees = 90.0F;
};

struct CpuProjectionRequest {
  unsigned pixel_count = 0;
  unsigned source_width = 0;
  unsigned source_height = 0;
  float horizontal_fov_degrees = 0.0F;
  float vertical_fov_degrees = 0.0F;
  std::array<float, 9> world_to_camera{};
};

struct CpuSampleRequest {
  CpuSampleType sample_type = CpuSampleType::uint8;
  CpuTransferFunction transfer_function = CpuTransferFunction::srgb;
  unsigned source_width = 0;
  unsigned source_height = 0;
  std::uint64_t source_row_stride_bytes = 0;
  unsigned pixel_count = 0;
};

using CpuTaskCallback = bool (*)(void *, unsigned, unsigned);
using CpuFrameAcquireCallback = bool (*)(void *, unsigned);
using CpuFrameComposeCallback = bool (*)(void *, unsigned);
using CpuFrameReleaseCallback = void (*)(void *, unsigned);

enum class CpuRenderPhase { allocation, decode, compose, encode, publish };
using CpuRenderPhaseCallback = bool (*)(void *, CpuRenderPhase);
using CpuRenderCleanupCallback = void (*)(void *);

struct CpuRenderPipelineCallbacks {
  CpuRenderPhaseCallback run = nullptr;
  CpuRenderCleanupCallback cleanup = nullptr;
  void *user_data = nullptr;
};

struct CpuFrameCallbacks {
  CpuFrameAcquireCallback acquire = nullptr;
  CpuFrameComposeCallback compose = nullptr;
  CpuFrameReleaseCallback release = nullptr;
  void *user_data = nullptr;
};

struct CpuLocalGainRequest {
  unsigned output_width = 0;
  unsigned output_height = 0;
  unsigned row_start = 0;
  unsigned row_count = 0;
  unsigned field_width = 0;
  unsigned field_height = 0;
};

struct CpuFramePair {
  unsigned left = 0;
  unsigned right = 0;
};

struct CpuExposureProxyRequest {
  CpuSampleRequest source;
  unsigned proxy_width = 0;
  unsigned proxy_height = 0;
};

struct CpuExposurePairRequest {
  unsigned sample_width = 0;
  unsigned sample_height = 0;
  unsigned proxy_width = 0;
  unsigned proxy_height = 0;
  float latitude_span_degrees = 180.0F;
  float horizontal_fov_degrees = 0.0F;
  float vertical_fov_degrees = 0.0F;
  std::array<float, 9> left_world_to_camera{};
  std::array<float, 9> right_world_to_camera{};
};

enum class CpuExposurePairRejection {
  accepted,
  insufficient_valid,
  insufficient_inliers,
  non_finite,
  excessive_mad,
};

struct CpuExposurePairReduction {
  CpuExposurePairRejection rejection = CpuExposurePairRejection::non_finite;
  unsigned valid_count = 0;
  unsigned inlier_count = 0;
  float difference = 0.0F;
  float mad = 0.0F;
  float weight = 0.0F;
};

struct CpuExposurePairMeasurement {
  CpuFramePair pair;
  CpuExposurePairReduction reduction;
  unsigned geometric_count = 0;
};

struct CpuExposureEquation {
  unsigned left = 0;
  unsigned right = 0;
  double difference = 0.0;
  double weight = 0.0;
};

struct CpuExposureSolveResult {
  unsigned anchor_frame = 0;
  unsigned edge_count = 0;
  std::vector<float> log_gains;
};

struct CpuExposureReport {
  unsigned anchor_frame = 0;
  unsigned edge_count = 0;
  std::vector<float> gains;
  bool warning = false;
};

struct CpuExposureEdits {
  std::vector<float> gains;
  std::optional<unsigned> target;
  std::vector<unsigned> selected;
};

struct CpuAutoContrastLevels {
  float black = 0.0F;
  float white = 1.0F;
  std::uint64_t processed_pixels = 0;
  bool valid = false;
};

struct CpuSdrConversionRequest {
  unsigned pixel_count = 0;
  CpuTransferFunction source_transfer = CpuTransferFunction::srgb;
  CpuColorPrimaries source_primaries = CpuColorPrimaries::srgb;
  float reference_white_nits = 100.0F;
  bool apply_auto_contrast = false;
  CpuAutoContrastLevels levels;
};

struct GuiLayoutMetrics {
  int margin = 0;
  int gap = 0;
  int row_height = 0;
  int label_width = 0;
  int button_width = 0;
  int content_width = 0;
};

struct CancellationCheck {
  bool (*callback)(void *) = nullptr;
  void *user_data = nullptr;
};

struct PublicationFaultCheck {
  bool (*callback)(void *, const char *) = nullptr;
  void *user_data = nullptr;
};

struct FrameSummary {
  unsigned index = 0;
  std::string filename;
  double yaw_deg = 0.0;
  double pitch_deg = 0.0;
  double roll_deg = 0.0;
  std::string status;
  std::optional<std::array<double, 9>> camera_basis_row_major;
};

struct SessionSummary {
  unsigned schema_version = 0;
  std::string session_id;
  std::string capture_mode;
  std::string projection = "rectilinear";
  double horizontal_fov_deg = 0.0;
  double vertical_fov_deg = 0.0;
  double overlap_fraction = 0.0;
  std::vector<FrameSummary> frames;
  bool completed = false;
  ImageEncoding image_encoding;
};

struct GuiSessionRecord {
  std::string path;
  SessionSummary session;
  std::vector<std::string> image_paths;
  std::string error;
};

enum class GuiSessionStatus { complete, incomplete, invalid, stitched };

struct GuiRefreshState {
  std::uint64_t generation = 0;
  std::vector<GuiSessionRecord> records;
};

struct GuiRenderRequestState {
  std::string session;
  std::string session_id;
  std::string image_dir;
  std::string output_directory;
  std::string output_name = "panorama.jpg";
  std::optional<unsigned> width;
  unsigned resolution_percent = 100;
  std::string format = "jpeg";
  unsigned jpeg_quality = 95;
  std::string blend = "feather";
  bool thumbnail = false;
  bool coverage = false;
  unsigned memory_mib = 1024;
  unsigned workers = 0;
  bool gpu = true;
  std::optional<unsigned> gpu_memory_mib;
  bool gpu_strict = false;
  bool allow_incomplete = false;
  bool auto_contrast = true;
};

struct GuiOptionEnablement {
  bool jpeg_quality = true;
  bool cpu_memory = true;
  bool workers = true;
  bool gpu_strict = true;
};

struct GuiPreviewCrop {
  unsigned left = 0;
  unsigned top = 0;
  unsigned width = 0;
  unsigned height = 0;
};

struct GuiPreviewViewState {
  bool overview = true;
  GuiPreviewCrop crop;
};

struct GuiPreviewHitRequest {
  unsigned source_width = 0;
  unsigned source_height = 0;
  unsigned mask_width = 0;
  unsigned mask_height = 0;
  unsigned frame_count = 0;
  double pointer_x = 0.0;
  double pointer_y = 0.0;
  GuiPreviewViewState view;
  std::optional<unsigned> target;
  bool target_mode = false;
  std::vector<unsigned> selected;
};

struct OutputTarget {
  std::string final_path;
  std::string stage_pattern;
  bool exists = false;
};

struct OutputPlan {
  OutputTarget panorama;
  std::optional<OutputTarget> coverage;
  std::optional<OutputTarget> thumbnail;
};

struct RenderPlan {
  SessionSummary session;
  OutputPlan outputs;
  std::optional<unsigned> output_width;
  std::optional<unsigned> output_height;
  double resolution = 1.0;
  std::string projection = "equirectangular";
  std::string blend;
  unsigned jpeg_quality = 95;
  unsigned memory_mib = 1024;
  unsigned workers = 0;
  bool use_gpu = true;
  std::optional<unsigned> gpu_memory_mib;
  bool gpu_strict = false;
  bool allow_incomplete = false;
  bool auto_contrast = true;
  bool automatic_exposure = false;
  std::optional<unsigned> exposure_target;
  std::vector<unsigned> exposure_sources;
};

struct GuiValidationState {
  std::uint64_t generation = 0;
  std::optional<RenderPlan> plan;
  std::string error;
};

enum class GuiStage { input, preview, output };

enum class GuiOperation { idle, validation, preview, exposure, render };

enum class GuiChange {
  game_directory,
  session,
  screenshots_directory,
  gpu_budget_increase,
  gpu_budget_decrease,
  preview_options,
  output_options
};

struct GuiInvalidation {
  bool reset_session = false;
  bool revalidate = false;
  bool discard_preview = false;
  bool rebuild_preview = false;
};

struct GuiWorkflowState {
  GuiStage stage = GuiStage::input;
  GuiOperation operation = GuiOperation::idle;
  std::uint64_t operation_generation = 0;
  std::uint64_t validation_generation = 0;
  std::uint64_t preview_generation = 0;
  bool session_selected = false;
  bool validation_ready = false;
  bool preview_ready = false;
};

struct GuiPresentationState {
  bool busy = false;
  bool input_enabled = true;
  bool preview_enabled = false;
  bool preview_ready = false;
  bool exposure_enabled = false;
  bool automatic_exposure_enabled = false;
  bool match_exposure_enabled = false;
  bool discard_exposure_enabled = false;
  bool output_enabled = false;
  bool render_enabled = false;
  bool rendering = false;
  unsigned preview_progress = 0;
  unsigned output_progress = 0;
  bool output_complete = false;
};

enum class GuiBackendDecision {
  d3d12,
  cpu_forced,
  cpu_fallback,
  strict_d3d12_rejection,
  unavailable
};

enum class GuiExposureOperation { automatic, manual_match };

struct GuiExposureState {
  std::uint64_t generation = 0;
  bool busy = false;
  unsigned progress_percent = 0;
  GuiExposureOperation operation = GuiExposureOperation::automatic;
  CpuExposureEdits edits;
  std::optional<CpuExposureReport> report;
  std::string warning;
};

using NativeProgressCallback = void (*)(void *, unsigned, unsigned,
                                        const char *);

constexpr unsigned native_preview_load_progress_begin = 10U;
constexpr unsigned native_preview_compose_progress_begin = 50U;
constexpr unsigned native_preview_compose_progress_end = 85U;
constexpr unsigned native_preview_overview_progress_end = 90U;
constexpr unsigned native_preview_masks_progress_end = 94U;
constexpr unsigned native_preview_retain_progress_end = 95U;

struct NativePreviewOptions {
  unsigned viewport_width = 0;
  CancellationCheck cancellation;
  const pano_gpu_cancellation_token *gpu_cancellation = nullptr;
  NativeProgressCallback progress = nullptr;
  void *progress_user_data = nullptr;
};

struct NativePreviewDiagnostics {
  unsigned frame_count = 0;
  unsigned preview_width = 0;
  unsigned preview_height = 0;
  unsigned overview_width = 0;
  unsigned overview_height = 0;
  unsigned mask_width = 0;
  unsigned mask_height = 0;
};

struct NativeExposureResult {
  unsigned anchor_frame = 0;
  unsigned edge_count = 0;
  std::vector<float> gains;
  bool warning = false;
};

using NativeRenderProgressCallback = NativeProgressCallback;

struct NativeRenderOptions {
  CancellationCheck cancellation;
  const pano_gpu_cancellation_token *gpu_cancellation = nullptr;
  NativeRenderProgressCallback progress = nullptr;
  void *progress_user_data = nullptr;
};

struct NativeRenderResult {
  unsigned width = 0;
  unsigned height = 0;
  std::vector<std::string> published_paths;
};

struct StitchedSessionEntry {
  std::string key;
  std::string output_name;
};

struct SessionTagEntry {
  std::string key;
  std::string tag;
};

struct ApplicationSettings {
  std::string game_directory;
  std::string image_directory;
  std::string output_directory;
  std::vector<StitchedSessionEntry> stitched_sessions;
  std::vector<SessionTagEntry> session_tags;
  unsigned gpu_memory_mib = 0;
  bool debug_coverage = false;
  bool auto_contrast = true;
};

struct DeletionResult {
  unsigned deleted = 0;
  unsigned missing = 0;
};

bool parse_render_options(const std::vector<std::string> &arguments,
                          RenderOptions &options, std::string &error);
bool load_session(const std::string &path,
                  const std::optional<std::string> &image_directory,
                  SessionSummary &session, std::string &error);
bool plan_outputs(const RenderOptions &options, OutputPlan &plan,
                  std::string &error);
bool make_render_plan(const RenderOptions &options, RenderPlan &plan,
                      std::string &error);
bool inspect_image(const std::string &path, ImageInfo &info,
                   CodecErrorCategory &category, std::string &error);
bool decode_image(const std::string &path, const ImageInfo &info, void *output,
                  std::uint64_t row_stride_bytes, std::uint64_t output_bytes,
                  const CancellationCheck &cancellation,
                  CodecErrorCategory &category, std::string &error);
bool decode_and_upload_images(
    pano_gpu_session *session, const std::vector<std::string> &paths,
    const ImageInfo &expected_info, const CancellationCheck &cancellation,
    const pano_gpu_cancellation_token *gpu_cancellation,
    CodecErrorCategory &category, std::string &error,
    NativeProgressCallback progress = nullptr,
    void *progress_user_data = nullptr, unsigned progress_begin = 0U,
    unsigned progress_end = 100U);
bool create_image_writer(const ImageWriterOptions &options,
                         ImageWriter **writer, CodecErrorCategory &category,
                         std::string &error);
bool write_image_rows(ImageWriter *writer, const void *rows, unsigned row_count,
                      std::uint64_t row_stride_bytes,
                      const CancellationCheck &cancellation,
                      CodecErrorCategory &category, std::string &error);
bool finish_image_writer(ImageWriter **writer,
                         const CancellationCheck &cancellation,
                         CodecErrorCategory &category, std::string &error);
void abort_image_writer(ImageWriter **writer) noexcept;
bool create_output_stage(const std::string &destination, OutputStage **stage,
                         CodecErrorCategory &category, std::string &error);
const std::string &output_stage_path(const OutputStage *stage) noexcept;
bool publish_output_stages(const std::vector<OutputStage *> &stages,
                           const PublicationFaultCheck &fault,
                           CodecErrorCategory &category, std::string &error);
void abort_output_stage(OutputStage **stage) noexcept;
bool recover_stale_output_stages(const std::vector<std::string> &destinations,
                                 CodecErrorCategory &category,
                                 std::string &error);
bool plan_cpu_render(const CpuRenderPlanRequest &request, CpuRenderPlan &plan,
                     std::string &error);
bool create_cpu_render_storage(const CpuRenderStorageOptions &options,
                               CpuRenderStorage **storage, std::string &error);
bool query_cpu_render_storage(const CpuRenderStorage *storage,
                              CpuRenderStorageDiagnostics &diagnostics,
                              std::string &error);
float *cpu_render_color_scratch(CpuRenderStorage *storage) noexcept;
float *cpu_render_weight_scratch(CpuRenderStorage *storage) noexcept;
void *cpu_render_worker_strip(CpuRenderStorage *storage,
                              unsigned worker_index) noexcept;
void destroy_cpu_render_storage(CpuRenderStorage **storage) noexcept;
bool create_cpu_worker_pool(unsigned worker_count, CpuWorkerPool **pool,
                            std::string &error);
bool run_cpu_tasks(CpuWorkerPool *pool, unsigned task_count,
                   CpuTaskCallback callback, void *user_data,
                   const CancellationCheck &cancellation, std::string &error);
void destroy_cpu_worker_pool(CpuWorkerPool **pool) noexcept;
bool generate_cpu_world_rays(const CpuRayRequest &request, float *world_rays,
                             std::uint64_t world_ray_bytes, std::string &error);
bool project_cpu_world_rays(const CpuProjectionRequest &request,
                            const float *world_rays,
                            std::uint64_t world_ray_bytes, float *coordinates,
                            std::uint64_t coordinate_bytes,
                            std::uint8_t *validity,
                            std::uint64_t validity_bytes, float *edge_distances,
                            std::uint64_t edge_distance_bytes,
                            std::string &error);
bool sample_cpu_bilinear(const CpuSampleRequest &request, const void *source,
                         std::uint64_t source_bytes, const float *coordinates,
                         std::uint64_t coordinate_bytes,
                         const std::uint8_t *validity,
                         std::uint64_t validity_bytes, float *candidate_rgb,
                         std::uint64_t candidate_rgb_bytes, std::string &error);
bool select_cpu_hard(unsigned pixel_count, const float *candidate_rgb,
                     const std::uint8_t *candidate_validity,
                     const float *candidate_edge_distance, float *color,
                     float *weight, std::uint8_t *coverage, std::string &error);
bool iterate_cpu_frames(unsigned frame_count,
                        const CpuFrameCallbacks &callbacks,
                        const CancellationCheck &cancellation,
                        std::string &error);
bool accumulate_cpu_feather(unsigned pixel_count, unsigned source_width,
                            unsigned source_height, const float *candidate_rgb,
                            const std::uint8_t *candidate_validity,
                            const float *candidate_edge_distance, float *color,
                            float *weight, std::string &error);
bool normalize_cpu_feather(unsigned pixel_count, float *color,
                           const float *weight, std::uint8_t *coverage,
                           std::string &error);
bool mark_cpu_incomplete(unsigned pixel_count, float *color,
                         const float *weight, std::string &error);
bool apply_cpu_global_gain(unsigned pixel_count, float gain, float *color,
                           std::string &error);
bool apply_cpu_local_gain(const CpuLocalGainRequest &request,
                          const float *local_log_gain, float *color,
                          std::string &error);
bool build_cpu_exposure_proxy(const CpuExposureProxyRequest &request,
                              const void *source, std::uint64_t source_bytes,
                              float *proxy, std::uint64_t proxy_bytes,
                              std::string &error);
bool enumerate_cpu_exposure_pairs(unsigned frame_count, CpuFramePair *pairs,
                                  unsigned pair_capacity, unsigned &pair_count,
                                  std::string &error);
bool project_cpu_exposure_pair(const CpuExposurePairRequest &request,
                               float *paired_coordinates, std::uint8_t *overlap,
                               std::string &error);
bool sample_cpu_exposure_pair(const CpuExposurePairRequest &request,
                              const float *left_proxy, const float *right_proxy,
                              const float *paired_coordinates,
                              float *sampled_pairs, std::string &error);
bool classify_cpu_exposure_samples(unsigned sample_count,
                                   CpuTransferFunction transfer_function,
                                   const float *sampled_pairs,
                                   const std::uint8_t *geometric_overlap,
                                   float *pair_luminance,
                                   std::uint8_t *accepted, std::string &error);
bool calculate_cpu_exposure_gradients(unsigned sample_width,
                                      unsigned sample_height,
                                      const float *pair_luminance,
                                      float *gradients, std::string &error);
bool filter_cpu_exposure_gradients(unsigned sample_count,
                                   const float *gradients,
                                   std::uint8_t *accepted,
                                   std::array<float, 2> &limits,
                                   std::string &error);
bool reduce_cpu_exposure_pair(unsigned sample_count,
                              const float *pair_luminance,
                              const std::uint8_t *accepted,
                              CpuExposurePairReduction &reduction,
                              std::string &error);
bool build_cpu_exposure_solve_graph(
    unsigned frame_count, const CpuExposurePairMeasurement *measurements,
    unsigned measurement_count, std::vector<CpuExposureEquation> &equations,
    std::string &error);
bool solve_cpu_exposure_graph(unsigned frame_count,
                              const std::vector<CpuExposureEquation> &equations,
                              CpuExposureSolveResult &result,
                              std::string &error);
bool solve_reference_exposure_gains(
    unsigned frame_count, unsigned target,
    const std::vector<CpuExposureEquation> &equations,
    const std::vector<float> &current_gains, std::vector<float> &gains,
    std::string &error);
bool make_cpu_exposure_report(const CpuExposureSolveResult &solved,
                              const std::vector<float> &manual_gains,
                              CpuExposureReport &report, std::string &error);
bool apply_cpu_exposure_match(CpuExposureEdits &edits, float gain,
                              std::string &error);
bool discard_cpu_exposure_edits(CpuExposureEdits &edits, std::string &error);
bool create_cpu_exposure_cache(CpuExposureCache **cache, std::string &error);
bool query_cpu_exposure_cache(const CpuExposureCache *cache,
                              const std::string &identity,
                              CpuExposureReport &report, bool &hit,
                              std::string &error);
bool store_cpu_exposure_cache(CpuExposureCache *cache,
                              const std::string &identity,
                              const CpuExposureReport &report,
                              std::string &error);
void invalidate_cpu_exposure_cache(CpuExposureCache *cache) noexcept;
void destroy_cpu_exposure_cache(CpuExposureCache **cache) noexcept;
bool accumulate_cpu_auto_contrast_histogram(
    const CpuSdrConversionRequest &request, const float *linear_rgb,
    const std::uint8_t *coverage, std::array<std::uint64_t, 4096> &histogram,
    std::string &error);
bool select_cpu_auto_contrast_levels(
    const std::array<std::uint64_t, 4096> &histogram,
    CpuAutoContrastLevels &levels, std::string &error);
bool convert_cpu_sdr8_band(const CpuSdrConversionRequest &request,
                           const float *linear_rgb, std::uint8_t *srgb8,
                           std::string &error);
bool copy_cpu_float_band(unsigned pixel_count, const float *linear_rgb,
                         float *output_rgb, std::string &error);
bool convert_and_write_cpu_band(
    ImageWriter *writer, CpuOutputBandSample output_sample,
    const CpuSdrConversionRequest &request, unsigned width, unsigned row_count,
    const float *linear_rgb, void *conversion_scratch,
    std::uint64_t conversion_scratch_bytes,
    const CancellationCheck &cancellation, CodecErrorCategory &category,
    std::string &error);
bool create_cpu_render_coordinator(CpuRenderCoordinator **coordinator,
                                   std::string &error);
bool run_cpu_render_pipeline(CpuRenderCoordinator *coordinator,
                             const CpuRenderPipelineCallbacks &callbacks,
                             const CancellationCheck &cancellation,
                             std::string &error);
void destroy_cpu_render_coordinator(
    CpuRenderCoordinator **coordinator) noexcept;
bool calculate_gui_layout_metrics(unsigned dpi, int client_width,
                                  GuiLayoutMetrics &metrics,
                                  std::string &error);
bool discover_gui_sessions(const std::string &game_directory,
                           std::vector<GuiSessionRecord> &records,
                           std::string &error);
GuiSessionStatus gui_session_status(const GuiSessionRecord &record,
                                    bool stitched) noexcept;
std::string gui_session_local_label(const std::string &session_id);
std::uint64_t begin_gui_session_refresh(GuiRefreshState &state) noexcept;
bool complete_gui_session_refresh(
    GuiRefreshState &state, std::uint64_t generation,
    std::vector<GuiSessionRecord> records) noexcept;
GuiOptionEnablement
gui_option_enablement(const GuiRenderRequestState &state) noexcept;
bool snapshot_gui_render_request(const GuiRenderRequestState &state,
                                 RenderOptions &options, std::string &error);
std::uint64_t begin_gui_validation(GuiValidationState &state) noexcept;
bool complete_gui_validation(GuiValidationState &state,
                             std::uint64_t generation,
                             std::optional<RenderPlan> plan,
                             std::string error) noexcept;
std::vector<std::string> gui_existing_output_paths(const RenderPlan &plan);
void navigate_gui_stage(GuiWorkflowState &state, GuiStage stage) noexcept;
GuiInvalidation apply_gui_change(GuiWorkflowState &state,
                                 GuiChange change) noexcept;
bool begin_gui_operation(GuiWorkflowState &state, GuiOperation operation,
                         std::uint64_t &generation,
                         std::string &error) noexcept;
bool complete_gui_operation(GuiWorkflowState &state,
                            std::uint64_t generation) noexcept;
void cancel_gui_operation(GuiWorkflowState &state) noexcept;
GuiPresentationState derive_gui_presentation(const GuiWorkflowState &state,
                                             bool exposure_available,
                                             bool exposure_target_selected,
                                             bool exposure_sources_selected,
                                             bool exposure_edits_applied,
                                             unsigned operation_progress,
                                             bool output_complete) noexcept;
GuiBackendDecision select_gui_backend(bool request_gpu, bool require_gpu,
                                      bool d3d12_available,
                                      bool cpu_available) noexcept;
bool calculate_gui_preview_crop(unsigned source_width, unsigned source_height,
                                unsigned viewport_width,
                                unsigned viewport_height, double pointer_x,
                                double pointer_y, GuiPreviewViewState &state,
                                std::string &error);
void reset_gui_preview_view(GuiPreviewViewState &state) noexcept;
bool gui_preview_hit_test(const GuiPreviewHitRequest &request,
                          const std::vector<std::uint8_t> &compact_masks,
                          std::vector<unsigned> &candidates,
                          std::string &error);
bool begin_gui_exposure_operation(GuiExposureState &state,
                                  GuiExposureOperation operation,
                                  unsigned frame_count,
                                  std::optional<unsigned> target,
                                  std::vector<unsigned> selected,
                                  std::uint64_t &generation,
                                  std::string &error);
bool update_gui_exposure_progress(GuiExposureState &state,
                                  std::uint64_t generation,
                                  unsigned progress_percent) noexcept;
bool complete_gui_exposure_operation(GuiExposureState &state,
                                     std::uint64_t generation,
                                     std::optional<CpuExposureReport> report,
                                     std::string warning) noexcept;
void cancel_gui_exposure_operation(GuiExposureState &state) noexcept;
bool apply_gui_exposure_match(GuiExposureState &state, float gain,
                              std::string &error);
bool discard_gui_exposure_edits(GuiExposureState &state, std::string &error);
bool create_native_preview(pano_gpu_device *device, const RenderPlan &plan,
                           const NativePreviewOptions &options,
                           NativePreview **preview, std::string &error);
bool rebuild_native_preview(NativePreview *preview, const RenderPlan &plan,
                            const NativePreviewOptions &options,
                            std::string &error);
bool query_native_preview(const NativePreview *preview,
                          NativePreviewDiagnostics &diagnostics,
                          std::string &error);
bool query_native_render_dimensions(const NativePreview *preview,
                                    unsigned &width, unsigned &height,
                                    std::string &error);
bool query_native_maximum_render_width(const NativePreview *preview,
                                       unsigned &width, std::string &error);
bool update_native_preview_render_plan(NativePreview *preview,
                                       const RenderPlan &plan,
                                       std::string &error);
pano_gpu_preview *native_preview_handle(NativePreview *preview) noexcept;
const std::vector<std::uint8_t> &
native_preview_masks(const NativePreview *preview) noexcept;
bool apply_native_automatic_exposure(NativePreview *preview, unsigned target,
                                     const NativePreviewOptions &options,
                                     NativeExposureResult &result,
                                     std::string &error);
bool apply_native_manual_exposure_match(NativePreview *preview, unsigned target,
                                        const std::vector<unsigned> &selected,
                                        const NativePreviewOptions &options,
                                        NativeExposureResult &result,
                                        std::string &error);
bool discard_native_exposure_edits(NativePreview *preview,
                                   const NativePreviewOptions &options,
                                   NativeExposureResult &result,
                                   std::string &error);
bool render_native_session(NativePreview *preview,
                           const NativeRenderOptions &options,
                           NativeRenderResult &result, std::string &error);
bool create_cpu_native_preview(const RenderPlan &plan,
                               const NativePreviewOptions &options,
                               CpuNativePreview **preview, std::string &error);
bool query_cpu_native_preview(const CpuNativePreview *preview,
                              NativePreviewDiagnostics &diagnostics,
                              std::string &error);
bool query_cpu_native_render_dimensions(const CpuNativePreview *preview,
                                        unsigned &width, unsigned &height,
                                        std::string &error);
bool query_cpu_native_maximum_render_width(const CpuNativePreview *preview,
                                           unsigned &width, std::string &error);
const std::vector<std::uint8_t> &
cpu_native_preview_pixels(const CpuNativePreview *preview) noexcept;
bool update_cpu_native_preview_render_plan(CpuNativePreview *preview,
                                           const RenderPlan &plan,
                                           std::string &error);
bool apply_cpu_native_automatic_exposure(
    CpuNativePreview *preview, unsigned target,
    const NativePreviewOptions &options, NativeExposureResult &result,
    std::string &error);
bool apply_cpu_native_manual_exposure_match(
    CpuNativePreview *preview, unsigned target,
    const std::vector<unsigned> &selected,
    const NativePreviewOptions &options, NativeExposureResult &result,
    std::string &error);
bool discard_cpu_native_exposure_edits(
    CpuNativePreview *preview, const NativePreviewOptions &options,
    NativeExposureResult &result, std::string &error);
bool render_cpu_native_session(CpuNativePreview *preview,
                               const NativeRenderOptions &options,
                               NativeRenderResult &result, std::string &error);
void destroy_cpu_native_preview(CpuNativePreview **preview) noexcept;
bool load_application_settings(const std::string &path,
                               ApplicationSettings &settings,
                               std::string &error);
bool save_application_settings(const std::string &path,
                               const ApplicationSettings &settings,
                               std::string &error);
bool parse_application_gpu_memory_mib(std::string_view text, unsigned &value,
                                      std::string &error);
std::string application_history_key(const std::string &game_directory,
                                    const std::string &session_id);
void mark_application_session_stitched(ApplicationSettings &settings,
                                       const std::string &game_directory,
                                       const std::string &session_id,
                                       const std::string &output_name);
std::optional<std::string>
application_stitched_name(const ApplicationSettings &settings,
                          const std::string &game_directory,
                          const std::string &session_id);
bool set_application_session_tag(ApplicationSettings &settings,
                                 const std::string &game_directory,
                                 const std::string &session_id,
                                 const std::string &tag, std::string &error);
bool set_and_save_application_session_tag(
    ApplicationSettings &settings, const std::string &game_directory,
    const std::string &session_id, const std::string &tag,
    const std::optional<std::string> &settings_path, std::string &error);
std::optional<std::string>
application_session_tag(const ApplicationSettings &settings,
                        const std::string &game_directory,
                        const std::string &session_id);
std::vector<std::string>
application_deletion_targets(const GuiSessionRecord &record,
                             bool include_images);
bool delete_application_files(const std::vector<std::string> &paths,
                              DeletionResult &result, std::string &error);
void destroy_native_preview(NativePreview **preview) noexcept;

int run(const std::vector<std::string> &arguments, std::ostream &output,
        std::ostream &error);

} // namespace pano::app
