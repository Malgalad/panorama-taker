#include "pano_app.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <utility>

namespace pano::app {
namespace {
namespace fs = std::filesystem;

std::string trimmed(std::string value) {
  const auto content = [](const unsigned char character) {
    return std::isspace(character) == 0;
  };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), content));
  value.erase(std::find_if(value.rbegin(), value.rend(), content).base(),
              value.end());
  return value;
}

std::string format_extension(const std::string &format) {
  return format == "jpeg" ? ".jpg" : "." + format;
}
} // namespace

GuiOptionEnablement
gui_option_enablement(const GuiRenderRequestState &state) noexcept {
  GuiOptionEnablement result;
  result.jpeg_quality = state.format == "jpeg";
  result.gpu_strict = state.gpu;
  return result;
}

std::optional<std::string>
gui_output_format_from_filename(const std::string &filename) {
  std::string extension = fs::u8path(filename).extension().u8string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](const unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  if (extension == ".jpg" || extension == ".jpeg")
    return "jpeg";
  if (extension == ".png")
    return "png";
  if (extension == ".exr")
    return "exr";
  return std::nullopt;
}

bool snapshot_gui_render_request(const GuiRenderRequestState &state,
                                 RenderOptions &options, std::string &error) {
  error.clear();
  if (state.session.empty()) {
    error = "choose a session file";
    return false;
  }
  if (state.output_directory.empty()) {
    error = "choose an output directory";
    return false;
  }
  if (state.format != "jpeg" && state.format != "png" &&
      state.format != "exr") {
    error = "format must be jpeg, png, or exr";
    return false;
  }
  if (state.format == "jpeg" &&
      (state.jpeg_quality < 1U || state.jpeg_quality > 100U)) {
    error = "JPEG quality must be between 1 and 100";
    return false;
  }
  if (state.resolution_percent < 1U || state.resolution_percent > 100U) {
    error = "resolution must be between 1 and 100 percent";
    return false;
  }
  if (state.width == 0U) {
    error = "explicit width must be greater than zero";
    return false;
  }
  if (state.blend != "hard" && state.blend != "feather") {
    error = "blend must be hard or feather";
    return false;
  }
  if (state.memory_mib < 1U || state.memory_mib > 8192U) {
    error = "memory budget must be between 1 and 8192 MiB";
    return false;
  }
  if (state.gpu_memory_mib.has_value() &&
      (*state.gpu_memory_mib < 1U || *state.gpu_memory_mib > 8192U)) {
    error = "GPU memory budget must be between 1 and 8192 MiB";
    return false;
  }
  if (!state.gpu && state.gpu_memory_mib.has_value()) {
    error = "GPU memory budget requires GPU rendering";
    return false;
  }
  if (!std::isfinite(state.final_exposure_ev) ||
      state.final_exposure_ev < -2.0 || state.final_exposure_ev > 2.0) {
    error = "final exposure must be between -2 and +2 EV";
    return false;
  }

  std::string output_name = trimmed(state.output_name);
  if (output_name.empty() || output_name == "panorama.png") {
    const std::string identity =
        state.session_id.empty() ? fs::u8path(state.session).stem().u8string()
                                 : state.session_id;
    output_name = "panorama-" + identity;
  }
  fs::path output =
      fs::u8path(state.output_directory) / fs::u8path(output_name);
  output.replace_extension(format_extension(state.format));

  RenderOptions snapshot;
  snapshot.session = state.session;
  snapshot.image_dir = state.image_dir;
  snapshot.output = output.u8string();
  snapshot.width = state.width;
  snapshot.resolution = static_cast<double>(state.resolution_percent) / 100.0;
  snapshot.format = state.format;
  snapshot.jpeg_quality = state.jpeg_quality;
  snapshot.blend = state.blend;
  snapshot.thumbnail = state.thumbnail;
  snapshot.coverage = state.coverage;
  snapshot.memory_mib = state.memory_mib;
  snapshot.workers = state.workers;
  snapshot.gpu = state.gpu;
  snapshot.gpu_memory_mib = state.gpu_memory_mib;
  snapshot.gpu_strict = state.gpu && state.gpu_strict;
  snapshot.allow_incomplete = state.allow_incomplete;
  snapshot.auto_contrast = state.auto_contrast;
  snapshot.final_exposure_ev = state.final_exposure_ev;
  options = std::move(snapshot);
  return true;
}

bool snapshot_gui_preview_request(const GuiRenderRequestState &state,
                                  const std::string &temporary_directory,
                                  RenderOptions &options, std::string &error) {
  if (temporary_directory.empty()) {
    error = "preview temporary directory is unavailable";
    return false;
  }
  GuiRenderRequestState preview = state;
  preview.output_directory = temporary_directory;
  preview.output_name = "preview.png";
  preview.width.reset();
  preview.resolution_percent = 100U;
  preview.format = "png";
  preview.jpeg_quality = 95U;
  preview.thumbnail = false;
  preview.coverage = false;
  return snapshot_gui_render_request(preview, options, error);
}

std::uint64_t begin_gui_validation(GuiValidationState &state) noexcept {
  state.plan.reset();
  state.error.clear();
  return ++state.generation;
}

bool complete_gui_validation(GuiValidationState &state,
                             const std::uint64_t generation,
                             std::optional<RenderPlan> plan,
                             std::string error) noexcept {
  if (generation != state.generation)
    return false;
  state.plan = std::move(plan);
  state.error = std::move(error);
  return true;
}

std::vector<std::string> gui_existing_output_paths(const RenderPlan &plan) {
  std::vector<std::string> paths;
  if (plan.outputs.panorama.exists)
    paths.push_back(plan.outputs.panorama.final_path);
  if (plan.outputs.coverage.has_value() && plan.outputs.coverage->exists)
    paths.push_back(plan.outputs.coverage->final_path);
  if (plan.outputs.thumbnail.has_value() && plan.outputs.thumbnail->exists)
    paths.push_back(plan.outputs.thumbnail->final_path);
  return paths;
}

void navigate_gui_stage(GuiWorkflowState &state,
                        const GuiStage stage) noexcept {
  state.stage = stage;
}

GuiInvalidation apply_gui_change(GuiWorkflowState &state,
                                 const GuiChange change) noexcept {
  GuiInvalidation invalidation;
  switch (change) {
  case GuiChange::game_directory:
    invalidation = {true, false, true, false};
    state.session_selected = false;
    break;
  case GuiChange::session:
  case GuiChange::screenshots_directory:
    invalidation = {false, true, true, false};
    break;
  case GuiChange::gpu_budget_increase:
  case GuiChange::output_options:
    invalidation = {false, true, false, false};
    break;
  case GuiChange::gpu_budget_decrease:
  case GuiChange::preview_options:
    invalidation = {false, true, true, state.preview_ready};
    break;
  }
  if (invalidation.reset_session || invalidation.revalidate) {
    ++state.validation_generation;
    state.validation_ready = false;
  }
  if (invalidation.discard_preview) {
    ++state.preview_generation;
    state.preview_ready = false;
  }
  return invalidation;
}

bool begin_gui_operation(GuiWorkflowState &state, const GuiOperation operation,
                         std::uint64_t &generation,
                         std::string &error) noexcept {
  if (operation == GuiOperation::idle ||
      state.operation != GuiOperation::idle) {
    error = "GUI operation is already active";
    return false;
  }
  state.operation = operation;
  generation = ++state.operation_generation;
  error.clear();
  return true;
}

bool complete_gui_operation(GuiWorkflowState &state,
                            const std::uint64_t generation) noexcept {
  if (state.operation == GuiOperation::idle ||
      generation != state.operation_generation)
    return false;
  state.operation = GuiOperation::idle;
  return true;
}

void cancel_gui_operation(GuiWorkflowState &state) noexcept {
  ++state.operation_generation;
  state.operation = GuiOperation::idle;
}

GuiPresentationState derive_gui_presentation(
    const GuiWorkflowState &state, const bool exposure_available,
    const bool exposure_target_selected, const bool exposure_sources_selected,
    const bool exposure_edits_applied, const unsigned operation_progress,
    const bool output_complete) noexcept {
  GuiPresentationState result;
  result.busy = state.operation != GuiOperation::idle;
  result.input_enabled = !result.busy;
  result.preview_enabled = state.validation_ready && !result.busy;
  result.preview_ready = state.preview_ready;
  result.exposure_enabled =
      state.preview_ready && exposure_available && !result.busy;
  result.automatic_exposure_enabled =
      result.exposure_enabled && exposure_target_selected;
  result.match_exposure_enabled =
      result.automatic_exposure_enabled && exposure_sources_selected;
  result.discard_exposure_enabled =
      result.exposure_enabled && exposure_edits_applied;
  result.output_enabled = state.preview_ready && !result.busy;
  result.render_enabled =
      state.validation_ready && state.preview_ready && !result.busy;
  result.rendering = state.operation == GuiOperation::render;
  if (state.operation == GuiOperation::preview ||
      state.operation == GuiOperation::exposure)
    result.preview_progress = operation_progress;
  if (result.rendering)
    result.output_progress = operation_progress;
  result.output_complete = output_complete && !result.rendering;
  return result;
}

GuiBackendDecision select_gui_backend(const bool request_gpu,
                                      const bool require_gpu,
                                      const bool d3d12_available,
                                      const bool cpu_available) noexcept {
  if (!request_gpu)
    return cpu_available ? GuiBackendDecision::cpu_forced
                         : GuiBackendDecision::unavailable;
  if (d3d12_available)
    return GuiBackendDecision::d3d12;
  if (require_gpu)
    return GuiBackendDecision::strict_d3d12_rejection;
  return cpu_available ? GuiBackendDecision::cpu_fallback
                       : GuiBackendDecision::unavailable;
}

bool calculate_gui_preview_crop(const unsigned source_width,
                                const unsigned source_height,
                                const unsigned viewport_width,
                                const unsigned viewport_height,
                                const double pointer_x, const double pointer_y,
                                GuiPreviewViewState &state,
                                std::string &error) {
  if (source_width == 0 || source_height == 0 || viewport_width == 0 ||
      viewport_height == 0 || viewport_width > source_width ||
      viewport_height > source_height || !std::isfinite(pointer_x) ||
      !std::isfinite(pointer_y)) {
    error = "invalid preview crop dimensions";
    return false;
  }
  const auto center = [](const double pointer, const unsigned extent) {
    return static_cast<unsigned>(std::nearbyint(std::clamp(pointer, 0.0, 1.0) *
                                                static_cast<double>(extent)));
  };
  const unsigned center_x = center(pointer_x, source_width);
  const unsigned center_y = center(pointer_y, source_height);
  const unsigned left = std::min(
      center_x > viewport_width / 2U ? center_x - viewport_width / 2U : 0U,
      source_width - viewport_width);
  const unsigned top = std::min(
      center_y > viewport_height / 2U ? center_y - viewport_height / 2U : 0U,
      source_height - viewport_height);
  state = {false, {left, top, viewport_width, viewport_height}};
  error.clear();
  return true;
}

void reset_gui_preview_view(GuiPreviewViewState &state) noexcept { state = {}; }

bool gui_preview_hit_test(const GuiPreviewHitRequest &request,
                          const std::vector<std::uint8_t> &compact_masks,
                          std::vector<unsigned> &candidates,
                          std::string &error) {
  std::uint64_t expected = static_cast<std::uint64_t>(request.frame_count) *
                           request.mask_width * request.mask_height;
  if (request.source_width == 0 || request.source_height == 0 ||
      request.mask_width == 0 || request.mask_height == 0 ||
      request.frame_count == 0 || expected != compact_masks.size() ||
      !std::isfinite(request.pointer_x) || !std::isfinite(request.pointer_y) ||
      (request.target.has_value() && *request.target >= request.frame_count)) {
    error = "invalid preview hit-test request";
    return false;
  }
  const GuiPreviewCrop crop =
      request.view.overview
          ? GuiPreviewCrop{0, 0, request.source_width, request.source_height}
          : request.view.crop;
  if (crop.width == 0 || crop.height == 0 ||
      crop.width > request.source_width ||
      crop.height > request.source_height ||
      crop.left > request.source_width - crop.width ||
      crop.top > request.source_height - crop.height) {
    error = "invalid preview hit-test crop";
    return false;
  }
  const auto coordinate = [](const double pointer, const unsigned extent) {
    return std::min(extent - 1U, static_cast<unsigned>(
                                     std::clamp(pointer, 0.0, 1.0) * extent));
  };
  const unsigned source_x =
      crop.left + coordinate(request.pointer_x, crop.width);
  const unsigned source_y =
      crop.top + coordinate(request.pointer_y, crop.height);
  const unsigned mask_x = std::min(
      request.mask_width - 1U,
      static_cast<unsigned>(static_cast<std::uint64_t>(source_x) *
                            request.mask_width / request.source_width));
  const unsigned mask_y = std::min(
      request.mask_height - 1U,
      static_cast<unsigned>(static_cast<std::uint64_t>(source_y) *
                            request.mask_height / request.source_height));
  candidates.clear();
  for (unsigned frame = 0; frame < request.frame_count; ++frame) {
    const std::size_t offset =
        (static_cast<std::size_t>(frame) * request.mask_height + mask_y) *
            request.mask_width +
        mask_x;
    if (compact_masks[offset] == 0 ||
        (!request.target_mode && request.target == frame) ||
        (request.target_mode &&
         std::find(request.selected.begin(), request.selected.end(), frame) !=
             request.selected.end()))
      continue;
    candidates.push_back(frame);
    if (request.target_mode)
      break;
  }
  error.clear();
  return true;
}

bool begin_gui_exposure_operation(GuiExposureState &state,
                                  const GuiExposureOperation operation,
                                  const unsigned frame_count,
                                  const std::optional<unsigned> target,
                                  std::vector<unsigned> selected,
                                  std::uint64_t &generation,
                                  std::string &error) {
  if (frame_count == 0 || (target.has_value() && *target >= frame_count) ||
      (operation == GuiExposureOperation::manual_match &&
       (!target.has_value() || selected.empty()))) {
    error = "invalid exposure operation selection";
    return false;
  }
  std::sort(selected.begin(), selected.end());
  if (std::adjacent_find(selected.begin(), selected.end()) != selected.end() ||
      std::any_of(selected.begin(), selected.end(), [&](const unsigned frame) {
        return frame >= frame_count || target == frame;
      })) {
    error = "invalid exposure source selection";
    return false;
  }
  if (state.edits.gains.size() != frame_count)
    state.edits.gains.assign(frame_count, 1.0F);
  state.edits.target = target;
  state.edits.selected = std::move(selected);
  state.operation = operation;
  state.busy = true;
  state.progress_percent = 0;
  state.report.reset();
  state.warning.clear();
  generation = ++state.generation;
  error.clear();
  return true;
}

bool update_gui_exposure_progress(GuiExposureState &state,
                                  const std::uint64_t generation,
                                  const unsigned progress_percent) noexcept {
  if (!state.busy || generation != state.generation || progress_percent > 100U)
    return false;
  state.progress_percent = progress_percent;
  return true;
}

bool complete_gui_exposure_operation(GuiExposureState &state,
                                     const std::uint64_t generation,
                                     std::optional<CpuExposureReport> report,
                                     std::string warning) noexcept {
  if (!state.busy || generation != state.generation)
    return false;
  if (report.has_value() && report->gains.size() == state.edits.gains.size())
    state.edits.gains = report->gains;
  state.report = std::move(report);
  state.warning = std::move(warning);
  state.progress_percent = 100U;
  state.busy = false;
  return true;
}

void cancel_gui_exposure_operation(GuiExposureState &state) noexcept {
  ++state.generation;
  state.busy = false;
  state.progress_percent = 0;
}

bool apply_gui_exposure_match(GuiExposureState &state, const float gain,
                              std::string &error) {
  if (state.busy) {
    error = "exposure recomputation is active";
    return false;
  }
  state.report.reset();
  state.warning.clear();
  return apply_cpu_exposure_match(state.edits, gain, error);
}

bool discard_gui_exposure_edits(GuiExposureState &state, std::string &error) {
  if (state.busy) {
    error = "exposure recomputation is active";
    return false;
  }
  state.report.reset();
  state.warning.clear();
  return discard_cpu_exposure_edits(state.edits, error);
}
} // namespace pano::app
