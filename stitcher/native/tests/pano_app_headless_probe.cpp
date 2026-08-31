#include "pano_app.h"
#include "pano_gpu.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#define NOMINMAX
#include <windows.h>

namespace {
using Clock = std::chrono::steady_clock;

std::string utf8(const wchar_t *const value) {
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string result(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                          result.data(), size, nullptr, nullptr) <= 0)
    return {};
  result.pop_back();
  return result;
}

bool parse_unsigned(const std::string &value, const unsigned maximum,
                    unsigned &parsed) {
  try {
    std::size_t consumed = 0;
    const unsigned long number = std::stoul(value, &consumed, 10);
    if (consumed != value.size() || number == 0UL || number > maximum)
      return false;
    parsed = static_cast<unsigned>(number);
    return true;
  } catch (...) {
    return false;
  }
}

bool take_probe_option(std::vector<std::string> &arguments,
                       const std::string &name, const unsigned maximum,
                       unsigned &value, std::string &error) {
  for (auto iterator = arguments.begin(); iterator != arguments.end();
       ++iterator) {
    if (*iterator != name) continue;
    const auto next = iterator + 1;
    if (next == arguments.end() || !parse_unsigned(*next, maximum, value)) {
      error = name + " requires an integer from 1 through " +
              std::to_string(maximum);
      return false;
    }
    arguments.erase(iterator, next + 1);
    return true;
  }
  return true;
}

bool output_exists(const pano::app::OutputPlan &outputs) {
  return outputs.panorama.exists ||
         (outputs.coverage.has_value() && outputs.coverage->exists) ||
         (outputs.thumbnail.has_value() && outputs.thumbnail->exists);
}

bool same_live_counts(const pano_gpu_diagnostics &left,
                      const pano_gpu_diagnostics &right) {
  return left.live_device_count == right.live_device_count &&
         left.live_queue_count == right.live_queue_count &&
         left.live_fence_count == right.live_fence_count &&
         left.live_session_count == right.live_session_count &&
         left.live_output_count == right.live_output_count;
}

void progress(void *, const unsigned completed, const unsigned total,
              const char *const phase) {
  std::cout << "progress phase=" << (phase == nullptr ? "render" : phase)
            << " completed=" << completed << " total=" << total << '\n';
}

void write_phase(const char *const phase) {
  std::array<char, 32768> path{};
  const DWORD length = GetEnvironmentVariableA(
      "PANO_HEADLESS_PHASE_FILE", path.data(), static_cast<DWORD>(path.size()));
  if (length != 0U && length < path.size()) {
    std::ofstream stream(path.data(), std::ios::binary | std::ios::trunc);
    stream << phase << '\n';
  }
  std::cout << "phase=" << phase << '\n' << std::flush;
}

void print_help() {
  std::cout
      << "Usage: pano_app_headless_probe render SESSION --output PATH [render "
         "options]\n"
         "       [--probe-preview-width PIXELS] [--probe-hold-ms MILLISECONDS]\n"
         "       [--probe-exposure-target POSE]\n";
}
} // namespace

int wmain(const int argc, wchar_t **const argv) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(std::max(argc - 1, 0)));
  for (int index = 1; index < argc; ++index) {
    auto argument = utf8(argv[index]);
    if (argument.empty() && argv[index][0] != L'\0') {
      std::cerr << "error: command line is not valid UTF-16\n";
      return 1;
    }
    arguments.push_back(std::move(argument));
  }
  if (arguments.empty() || arguments == std::vector<std::string>{"--help"}) {
    print_help();
    return 0;
  }

  unsigned preview_width = 2U;
  unsigned hold_ms = 1U;
  unsigned exposure_target = 1U;
  std::string error;
  if (!take_probe_option(arguments, "--probe-preview-width", 4096U,
                         preview_width, error) ||
      !take_probe_option(arguments, "--probe-hold-ms", 600000U, hold_ms,
                         error) ||
      !take_probe_option(arguments, "--probe-exposure-target", 100000U,
                         exposure_target,
                         error)) {
    std::cerr << "error: " << error << '\n';
    return 1;
  }

  pano::app::RenderOptions render_options;
  pano::app::RenderPlan plan;
  if (!pano::app::parse_render_options(arguments, render_options, error) ||
      !pano::app::make_render_plan(render_options, plan, error)) {
    std::cerr << "error: " << error << '\n';
    return 1;
  }
  if (!render_options.gpu || output_exists(plan.outputs)) {
    std::cerr << "error: headless probe requires D3D12 and new output paths\n";
    return 1;
  }

  std::array<char, 512> gpu_error{};
  pano_gpu_diagnostics before{};
  before.size = sizeof(before);
  before.abi_version = PANO_GPU_ABI_VERSION;
  if (pano_gpu_query_diagnostics(&before, gpu_error.data(),
                                 static_cast<std::uint32_t>(gpu_error.size())) !=
      PANO_GPU_SUCCESS) {
    std::cerr << "error: " << gpu_error.data() << '\n';
    return 2;
  }

  pano_gpu_probe_options probe{};
  probe.size = sizeof(probe);
  probe.abi_version = PANO_GPU_ABI_VERSION;
  pano_gpu_device *device = nullptr;
  pano::app::NativePreview *preview = nullptr;
  struct Cleanup {
    pano::app::NativePreview **preview;
    pano_gpu_device **device;
    ~Cleanup() {
      pano::app::destroy_native_preview(preview);
      pano_gpu_device_destroy(device);
    }
  } cleanup{&preview, &device};

  if (pano_gpu_device_create(&probe, &device, gpu_error.data(),
                             static_cast<std::uint32_t>(gpu_error.size())) !=
      PANO_GPU_SUCCESS) {
    std::cerr << "error: " << gpu_error.data() << '\n';
    return 2;
  }
  pano_gpu_device_diagnostics device_diagnostics{};
  device_diagnostics.size = sizeof(device_diagnostics);
  device_diagnostics.abi_version = PANO_GPU_ABI_VERSION;
  if (pano_gpu_device_query_diagnostics(
          device, &device_diagnostics, gpu_error.data(),
          static_cast<std::uint32_t>(gpu_error.size())) != PANO_GPU_SUCCESS) {
    std::cerr << "error: " << gpu_error.data() << '\n';
    return 2;
  }
  std::cout << "adapter name=" << device_diagnostics.adapter.name
            << " budget_bytes=" << device_diagnostics.adapter.local_budget_bytes
            << " usage_bytes=" << device_diagnostics.adapter.local_usage_bytes
            << " usable_bytes=" << device_diagnostics.usable_local_bytes
            << '\n';

  const auto started = Clock::now();
  pano::app::NativePreviewOptions preview_options;
  preview_options.viewport_width = preview_width;
  if (!pano::app::create_native_preview(device, plan, preview_options, &preview,
                                        error)) {
    std::cerr << "error: " << error << '\n';
    return 3;
  }
  const auto preview_ready = Clock::now();
  pano::app::NativePreviewDiagnostics preview_diagnostics;
  pano_gpu_preview_diagnostics retained{};
  retained.size = sizeof(retained);
  retained.abi_version = PANO_GPU_ABI_VERSION;
  if (!pano::app::query_native_preview(preview, preview_diagnostics, error) ||
      pano_gpu_preview_query_diagnostics(
          pano::app::native_preview_handle(preview), &retained,
          gpu_error.data(), static_cast<std::uint32_t>(gpu_error.size())) !=
          PANO_GPU_SUCCESS) {
    std::cerr << "error: " << (error.empty() ? gpu_error.data() : error)
              << '\n';
    return 3;
  }
  std::cout << "preview width=" << preview_diagnostics.preview_width
            << " height=" << preview_diagnostics.preview_height
            << " retained_bytes=" << retained.retained_bytes
            << " hold_ms=" << hold_ms << '\n';
  write_phase("preview_idle");
  std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
  write_phase("exposure");

  pano::app::NativeExposureResult exposure;
  if (plan.automatic_exposure &&
      !pano::app::apply_native_automatic_exposure(
          preview, exposure_target - 1U, preview_options, exposure, error)) {
    std::cerr << "error: " << error << '\n';
    return 3;
  }
  if (plan.automatic_exposure) {
    std::cout << "exposure_gains=" << std::setprecision(9);
    for (std::size_t index = 0; index < exposure.gains.size(); ++index) {
      if (index != 0U) std::cout << ',';
      std::cout << exposure.gains[index];
    }
    std::cout << '\n';
  }
  if (plan.exposure_target.has_value() &&
      !pano::app::apply_native_manual_exposure_match(
          preview, *plan.exposure_target, plan.exposure_sources,
          preview_options, exposure, error)) {
    std::cerr << "error: " << error << '\n';
    return 3;
  }
  const auto exposure_ready = Clock::now();
  write_phase("render");

  pano::app::NativeRenderOptions native_options;
  native_options.progress = progress;
  pano::app::NativeRenderResult result;
  if (!pano::app::render_native_session(preview, native_options, result,
                                        error)) {
    std::cerr << "error: " << error << '\n';
    return 3;
  }
  const auto rendered = Clock::now();
  write_phase("teardown");
  pano::app::destroy_native_preview(&preview);
  pano_gpu_device_destroy(&device);
  pano_gpu_diagnostics after{};
  after.size = sizeof(after);
  after.abi_version = PANO_GPU_ABI_VERSION;
  if (pano_gpu_query_diagnostics(&after, gpu_error.data(),
                                 static_cast<std::uint32_t>(gpu_error.size())) !=
          PANO_GPU_SUCCESS ||
      !same_live_counts(before, after)) {
    std::cerr << "error: native D3D12 resources survived probe teardown\n";
    return 4;
  }
  const auto milliseconds = [](const Clock::duration duration) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration)
        .count();
  };
  std::cout << "result width=" << result.width << " height=" << result.height
            << " outputs=" << result.published_paths.size()
            << " preview_ms=" << milliseconds(preview_ready - started)
            << " exposure_ms=" << milliseconds(exposure_ready - preview_ready)
            << " render_ms=" << milliseconds(rendered - exposure_ready)
            << " total_ms=" << milliseconds(rendered - started) << '\n';
  return 0;
}
