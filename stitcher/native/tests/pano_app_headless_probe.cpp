#include "pano_app.h"
#include "pano_gpu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#define NOMINMAX
#include <dxgi1_4.h>
#include <psapi.h>
#include <windows.h>
#include <wrl/client.h>

namespace {
using Clock = std::chrono::steady_clock;

class PreviewResources {
public:
  bool start(const std::uint64_t adapter_luid) {
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    LUID luid{};
    static_assert(sizeof(luid) == sizeof(adapter_luid));
    std::memcpy(&luid, &adapter_luid, sizeof(luid));
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter_))))
      return false;
    thread_ = std::thread([this] {
      do {
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
        if (!GetProcessMemoryInfo(
                GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),
                sizeof(memory)) ||
            FAILED(adapter_->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) ||
            FAILED(adapter_->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared))) {
          valid_ = false;
          return;
        }
        working_ = std::max(working_, memory.WorkingSetSize);
        private_ = std::max(private_, memory.PrivateUsage);
        local_ = std::max(local_, local.CurrentUsage);
        shared_ = std::max(shared_, shared.CurrentUsage);
        ++samples_;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      } while (!stop_.load(std::memory_order_relaxed));
    });
    return true;
  }
  void stop() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable())
      thread_.join();
  }
  bool report(const char *label = "preview_resources") {
    stop();
    std::cout << label << " samples=" << samples_
              << " working_bytes=" << working_ << " private_bytes=" << private_
              << " local_bytes=" << local_ << " nonlocal_bytes=" << shared_
              << '\n';
    return valid_ && samples_ != 0U;
  }
  ~PreviewResources() { stop(); }

private:
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  bool valid_ = true;
  unsigned samples_ = 0;
  SIZE_T working_ = 0, private_ = 0;
  UINT64 local_ = 0, shared_ = 0;
};

void preview_progress(void *user, unsigned completed, unsigned,
                      const char *phase) {
  const auto elapsed =
      std::chrono::duration<double, std::milli>(
          Clock::now() - *static_cast<const Clock::time_point *>(user))
          .count();
  std::cout << "preview_progress ms=" << elapsed << " completed=" << completed
            << " phase=" << phase << '\n';
}

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
         "       [--probe-preview-width PIXELS] [--probe-hold-ms "
         "MILLISECONDS]\n"
         "       [--probe-exposure-target POSE] [--probe-preview-only]\n"
         "       [--probe-preview-dump PATH]\n";
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
  const auto preview_only_option =
      std::find(arguments.begin(), arguments.end(), "--probe-preview-only");
  const bool preview_only = preview_only_option != arguments.end();
  if (preview_only)
    arguments.erase(preview_only_option);
  std::string preview_dump;
  const auto dump_option =
      std::find(arguments.begin(), arguments.end(), "--probe-preview-dump");
  if (dump_option != arguments.end()) {
    if (dump_option + 1 == arguments.end())
      return 1;
    preview_dump = *(dump_option + 1);
    arguments.erase(dump_option, dump_option + 2);
    if (std::filesystem::exists(std::filesystem::u8path(preview_dump))) {
      std::cerr << "error: preview dump already exists\n";
      return 1;
    }
  }
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

  PreviewResources resources;
  if (!resources.start(device_diagnostics.adapter.luid)) {
    std::cerr << "error: cannot start preview resource sampling\n";
    return 2;
  }
  auto started = Clock::now();
  pano::app::NativePreviewOptions preview_options;
  pano::app::SourceLoadDiagnostics load;
  preview_options.source_load_diagnostics = &load;
  preview_options.viewport_width = preview_width;
  preview_options.progress = preview_progress;
  preview_options.progress_user_data = &started;
  if (!pano::app::create_native_preview(device, plan, preview_options, &preview,
                                        error)) {
    std::cerr << "error: " << error << '\n';
    return 3;
  }
  const auto preview_ready = Clock::now();
  std::cout << "source_load allocation_ms=" << load.allocation_ms
            << " inspect_ms=" << load.inspect_ms
            << " decode_ms=" << load.decode_ms
            << " upload_ms=" << load.upload_ms
            << " finish_ms=" << load.finish_ms << " total_ms=" << load.total_ms
            << " decoders=" << load.decoder_count
            << " host_buffer_bytes=" << load.host_buffer_bytes << '\n';
  if (!resources.report()) {
    std::cerr << "error: preview resource sampling failed\n";
    return 3;
  }
  std::cout << "preview_time ms="
            << std::chrono::duration<double, std::milli>(preview_ready -
                                                         started)
                   .count()
            << '\n';
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
  if (!preview_dump.empty()) {
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(preview_diagnostics.preview_width) *
        preview_diagnostics.preview_height * 3U);
    std::vector<std::uint8_t> overview(
        static_cast<std::size_t>(preview_diagnostics.overview_width) *
        preview_diagnostics.overview_height * 3U);
    std::vector<std::uint8_t> masks(
        static_cast<std::size_t>(preview_diagnostics.mask_width) *
        preview_diagnostics.mask_height * preview_diagnostics.frame_count);
    if (pano_gpu_test_read_preview_retained(
            pano::app::native_preview_handle(preview), pixels.data(),
            pixels.size(), overview.data(), overview.size(), masks.data(),
            masks.size(), gpu_error.data(),
            static_cast<std::uint32_t>(gpu_error.size())) != PANO_GPU_SUCCESS) {
      std::cerr << "error: " << gpu_error.data() << '\n';
      return 3;
    }
    std::ofstream dump(std::filesystem::u8path(preview_dump), std::ios::binary);
    for (const auto *bytes : {&pixels, &overview, &masks})
      dump.write(reinterpret_cast<const char *>(bytes->data()),
                 static_cast<std::streamsize>(bytes->size()));
    dump.close();
    if (!dump)
      return 3;
  }
  if (preview_only) {
    pano::app::destroy_native_preview(&preview);
    pano_gpu_device_destroy(&device);
    pano_gpu_diagnostics after{};
    after.size = sizeof(after);
    after.abi_version = PANO_GPU_ABI_VERSION;
    return pano_gpu_query_diagnostics(
               &after, gpu_error.data(),
               static_cast<std::uint32_t>(gpu_error.size())) ==
                       PANO_GPU_SUCCESS &&
                   same_live_counts(before, after)
               ? 0
               : 4;
  }
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
  pano::app::NativeRenderTimings render_timings;
  native_options.timings = &render_timings;
  PreviewResources render_resources;
  if (!render_resources.start(device_diagnostics.adapter.luid))
    return 3;
  native_options.progress = progress;
  pano::app::NativeRenderResult result;
  if (!pano::app::render_native_session(preview, native_options, result,
                                        error)) {
    std::cerr << "error: " << error << '\n';
    return 3;
  }
  const auto rendered = Clock::now();
  if (!render_resources.report("render_resources"))
    return 3;
  std::cout << "render_timings bands=" << render_timings.encoded_bands
            << " overlapped_bands=" << render_timings.overlapped_bands
            << " setup_ms=" << render_timings.setup_ms
            << " composite_ms=" << render_timings.composite_ms
            << " contrast_ms=" << render_timings.contrast_ms
            << " convert_ms=" << render_timings.convert_ms
            << " readback_ms=" << render_timings.readback_ms
            << " encode_ms=" << render_timings.encode_ms
            << " finish_ms=" << render_timings.finish_ms
            << " publish_ms=" << render_timings.publish_ms
            << " total_ms=" << render_timings.total_ms << '\n';
  std::cout << "png_encoder=" << result.png_encoder << '\n';
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
