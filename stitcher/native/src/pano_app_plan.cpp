#include "pano_app.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>

namespace pano::app {
namespace {
namespace fs = std::filesystem;

std::string extension_for(const std::string &format) {
  if (format == "jpeg")
    return ".jpg";
  return "." + format;
}

bool target_for(const fs::path &path, OutputTarget &target,
                std::string &error) {
  std::error_code status_error;
  const bool exists = fs::exists(path, status_error);
  if (status_error) {
    error = "cannot inspect output path: " + path.u8string();
    return false;
  }
  target = {path.u8string(),
            (path.parent_path() /
             fs::u8path("." + path.filename().u8string() + ".<unique>.partial"))
                .u8string(),
            exists};
  return true;
}

fs::path
with_stem_suffix(const fs::path &path, const std::string &suffix,
                 const std::optional<std::string> &extension = std::nullopt) {
  auto result =
      path.parent_path() / fs::u8path(path.stem().u8string() + suffix);
  result.replace_extension(extension.value_or(path.extension().u8string()));
  return result;
}
} // namespace

bool plan_outputs(const RenderOptions &options, OutputPlan &plan,
                  std::string &error) {
  error.clear();
  if (options.output.empty()) {
    error = "render requires --output";
    return false;
  }
  auto panorama = fs::u8path(options.output);
  panorama.replace_extension(extension_for(options.format));
  OutputPlan parsed;
  if (!target_for(panorama, parsed.panorama, error))
    return false;
  if (options.coverage) {
    OutputTarget coverage;
    if (!target_for(with_stem_suffix(panorama, "-coverage", ".png"), coverage,
                    error))
      return false;
    parsed.coverage = std::move(coverage);
  }
  if (options.thumbnail) {
    OutputTarget thumbnail;
    if (!target_for(with_stem_suffix(panorama, "-thumbnail"), thumbnail, error))
      return false;
    parsed.thumbnail = std::move(thumbnail);
  }
  plan = std::move(parsed);
  return true;
}

bool make_render_plan(const RenderOptions &options, RenderPlan &plan,
                      std::string &error) {
  SessionSummary session;
  const std::optional<std::string> image_directory =
      options.image_dir.empty() ? std::nullopt
                                : std::optional<std::string>(options.image_dir);
  if (!load_session(options.session, image_directory, session, error))
    return false;
  if (!session.completed && !options.allow_incomplete) {
    error = "session is incomplete; use --allow-incomplete to render it";
    return false;
  }
  if (!std::isfinite(options.final_exposure_ev) ||
      options.final_exposure_ev < -2.0 || options.final_exposure_ev > 2.0) {
    error = "final exposure must be from -2 to +2 EV";
    return false;
  }
  const auto frame_count = session.frames.size();
  if (options.exposure_target.has_value() &&
      *options.exposure_target >= frame_count) {
    error = "exposure target index is outside the session frame range";
    return false;
  }
  std::unordered_set<unsigned> unique_sources;
  for (const auto source : options.exposure_sources) {
    if (source >= frame_count) {
      error = "exposure source index is outside the session frame range";
      return false;
    }
    if (options.exposure_target.has_value() &&
        source == *options.exposure_target) {
      error = "exposure target cannot also be an exposure source";
      return false;
    }
    if (!unique_sources.insert(source).second) {
      error = "exposure source indices must be unique";
      return false;
    }
  }

  OutputPlan outputs;
  if (!plan_outputs(options, outputs, error))
    return false;
  RenderPlan parsed;
  parsed.session = std::move(session);
  parsed.outputs = std::move(outputs);
  if (options.width.has_value()) {
    unsigned width = *options.width;
    if (parsed.session.capture_mode == "full_sphere") {
      width = std::max(2U, width - width % 2U);
      parsed.output_width = width;
      parsed.output_height = width / 2U;
    } else {
      parsed.output_width = width;
      parsed.output_height =
          std::max(1U, static_cast<unsigned>(std::round(
                           width * parsed.session.vertical_fov_deg / 360.0)));
    }
  }
  parsed.resolution = options.resolution;
  parsed.blend = options.blend;
  parsed.jpeg_quality = options.jpeg_quality;
  parsed.memory_mib = options.memory_mib;
  parsed.workers = options.workers;
  parsed.use_gpu = options.gpu;
  parsed.gpu_memory_mib = options.gpu_memory_mib;
  parsed.gpu_strict = options.gpu_strict;
  parsed.allow_incomplete = options.allow_incomplete;
  parsed.auto_contrast = options.auto_contrast;
  parsed.final_exposure_ev = options.final_exposure_ev;
  parsed.automatic_exposure = options.automatic_exposure;
  parsed.exposure_target = options.exposure_target;
  parsed.exposure_sources = options.exposure_sources;
  plan = std::move(parsed);
  return true;
}
} // namespace pano::app
