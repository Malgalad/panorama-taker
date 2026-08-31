#include "pano_app.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

namespace pano::app {
namespace {
bool parse_unsigned(const std::string &text, unsigned &value) {
  if (text.empty() || text.front() == '-')
    return false;
  char *end = nullptr;
  errno = 0;
  const auto parsed = std::strtoul(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0' ||
      parsed > std::numeric_limits<unsigned>::max()) {
    return false;
  }
  value = static_cast<unsigned>(parsed);
  return true;
}

bool parse_positive_double(const std::string &text, double &value) {
  if (text.empty())
    return false;
  char *end = nullptr;
  errno = 0;
  value = std::strtod(text.c_str(), &end);
  return errno == 0 && end != text.c_str() && *end == '\0' &&
         std::isfinite(value) && value > 0.0;
}

bool parse_positive_integer_component(const std::string &text,
                                      long double &value) {
  std::size_t index = 0;
  while (index < text.size() &&
         std::isspace(static_cast<unsigned char>(text[index]))) {
    ++index;
  }
  if (index < text.size() && text[index] == '+')
    ++index;
  const auto digits_begin = index;
  value = 0.0L;
  while (index < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[index]))) {
    value = value * 10.0L + static_cast<unsigned>(text[index] - '0');
    if (!std::isfinite(value))
      return false;
    ++index;
  }
  if (index == digits_begin)
    return false;
  while (index < text.size() &&
         std::isspace(static_cast<unsigned char>(text[index]))) {
    ++index;
  }
  return index == text.size() && value > 0.0L;
}

bool parse_resolution(const std::string &text, double &value) {
  const auto slash = text.find('/');
  if (slash == std::string::npos) {
    return parse_positive_double(text, value) && value <= 1.0;
  }
  if (text.find('/', slash + 1) != std::string::npos)
    return false;
  long double numerator = 0.0L;
  long double denominator = 0.0L;
  if (!parse_positive_integer_component(text.substr(0, slash), numerator) ||
      !parse_positive_integer_component(text.substr(slash + 1), denominator)) {
    return false;
  }
  value = static_cast<double>(numerator / denominator);
  return std::isfinite(value) && value > 0.0 && value <= 1.0;
}

bool take_value(const std::vector<std::string> &arguments, std::size_t &index,
                const std::string &option, const std::string *&value,
                std::string &error) {
  ++index;
  if (index == arguments.size()) {
    error = option + " requires a value";
    return false;
  }
  value = &arguments[index];
  return true;
}
} // namespace

bool parse_render_options(const std::vector<std::string> &arguments,
                          RenderOptions &options, std::string &error) {
  error.clear();
  if (arguments.size() < 2 || arguments.front() != "render" ||
      arguments[1].empty()) {
    error = "render requires SESSION";
    return false;
  }

  options = {};
  options.session = arguments[1];
  bool resolution_was_set = false;
  bool jpeg_quality_was_set = false;
  for (std::size_t index = 2; index < arguments.size(); ++index) {
    const auto &argument = arguments[index];
    const std::string *value = nullptr;
    if (argument == "--output" || argument == "--image-dir" ||
        argument == "--format" || argument == "--blend") {
      if (!take_value(arguments, index, argument, value, error))
        return false;
      if (argument == "--output") {
        options.output = *value;
      } else if (argument == "--image-dir") {
        options.image_dir = *value;
      } else if (argument == "--format") {
        if (*value != "png" && *value != "jpeg" && *value != "exr") {
          error = "format must be png, jpeg, or exr";
          return false;
        }
        options.format = *value;
      } else {
        if (*value != "hard" && *value != "feather") {
          error = "blend must be hard or feather";
          return false;
        }
        options.blend = *value;
      }
      continue;
    }
    if (argument == "--resolution") {
      if (!take_value(arguments, index, argument, value, error) ||
          !parse_resolution(*value, options.resolution)) {
        error = "resolution must be greater than 0 and at most 1";
        return false;
      }
      resolution_was_set = true;
      continue;
    }
    if (argument == "--width" || argument == "--jpeg-quality" ||
        argument == "--memory-budget-mib" || argument == "--workers" ||
        argument == "--gpu-memory-budget-mib" ||
        argument == "--exposure-target" || argument == "--exposure-source") {
      unsigned parsed = 0;
      if (!take_value(arguments, index, argument, value, error) ||
          !parse_unsigned(*value, parsed)) {
        error = argument + " requires a non-negative integer";
        return false;
      }
      if (argument == "--width") {
        if (parsed == 0) {
          error = "width must be greater than 0";
          return false;
        }
        options.width = parsed;
      } else if (argument == "--jpeg-quality") {
        if (parsed < 1 || parsed > 100) {
          error = "JPEG quality must be from 1 to 100";
          return false;
        }
        options.jpeg_quality = parsed;
        jpeg_quality_was_set = true;
      } else if (argument == "--memory-budget-mib") {
        if (parsed < 1 || parsed > 8192) {
          error = "memory budget must be from 1 to 8192 MiB";
          return false;
        }
        options.memory_mib = parsed;
      } else if (argument == "--workers") {
        options.workers = parsed;
      } else if (argument == "--gpu-memory-budget-mib") {
        if (parsed == 0) {
          error = "GPU memory budget must be greater than 0";
          return false;
        }
        options.gpu_memory_mib = parsed;
      } else if (argument == "--exposure-target") {
        options.exposure_target = parsed;
      } else {
        options.exposure_sources.push_back(parsed);
      }
      continue;
    }
    if (argument == "--thumbnail") {
      options.thumbnail = true;
    } else if (argument == "--coverage") {
      options.coverage = true;
    } else if (argument == "--no-gpu") {
      options.gpu = false;
    } else if (argument == "--gpu-strict") {
      options.gpu_strict = true;
    } else if (argument == "--allow-incomplete") {
      options.allow_incomplete = true;
    } else if (argument == "--auto-exposure") {
      options.automatic_exposure = true;
    } else if (argument == "--no-auto-contrast") {
      options.auto_contrast = false;
    } else {
      error = "unknown render option: " + argument;
      return false;
    }
  }

  if (options.output.empty()) {
    error = "render requires --output";
    return false;
  }
  if (options.width.has_value() && resolution_was_set) {
    error = "--width and --resolution cannot be combined";
    return false;
  }
  if (options.format != "jpeg" && jpeg_quality_was_set) {
    error = "--jpeg-quality requires JPEG output";
    return false;
  }
  if (options.gpu_strict && !options.gpu) {
    error = "--gpu-strict requires GPU";
    return false;
  }
  if (options.gpu_memory_mib.has_value() && !options.gpu) {
    error = "--gpu-memory-budget-mib requires GPU";
    return false;
  }
  if (!options.exposure_sources.empty() &&
      !options.exposure_target.has_value()) {
    error = "--exposure-source requires --exposure-target";
    return false;
  }
  if (options.exposure_target.has_value() && options.exposure_sources.empty()) {
    error = "--exposure-target requires at least one --exposure-source";
    return false;
  }
  if (options.automatic_exposure && options.exposure_target.has_value()) {
    error = "--auto-exposure cannot be combined with manual exposure matching";
    return false;
  }
  for (std::size_t index = 0; index < options.exposure_sources.size();
       ++index) {
    const auto source = options.exposure_sources[index];
    if (options.exposure_target.has_value() &&
        source == *options.exposure_target) {
      error = "exposure target cannot also be an exposure source";
      return false;
    }
    if (std::find(options.exposure_sources.begin(),
                  options.exposure_sources.begin() + index,
                  source) != options.exposure_sources.begin() + index) {
      error = "exposure source indices must be unique";
      return false;
    }
  }
  return true;
}
} // namespace pano::app
