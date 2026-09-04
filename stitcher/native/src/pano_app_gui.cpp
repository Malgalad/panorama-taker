#define NOMINMAX
#include <windows.h>

#include <initguid.h>
#undef INITGUID
#include <shobjidl.h>

#if defined(_MSC_VER)
#pragma comment(linker,                                                        \
                "\"/manifestdependency:type='win32' "                          \
                "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "  \
                "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
                "language='*'\"")
#endif

#include "pano_app.h"
#include "pano_app_resource.h"
#include "pano_app_webview.h"
#include "pano_gpu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t window_class[] = L"PanoramaStitchNativeWindow";
constexpr wchar_t preview_window_class[] = L"PanoramaStitchPreviewWindow";
constexpr wchar_t exposure_window_class[] = L"PanoramaStitchExposureWindow";
constexpr DWORD exposure_window_extended_style = WS_EX_CONTROLPARENT;
constexpr DWORD exposure_window_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
static_assert((exposure_window_extended_style & WS_EX_TOOLWINDOW) == 0U);
static_assert((exposure_window_style & WS_SYSMENU) != 0U);
static_assert((exposure_window_style &
               (WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME)) == 0U);
constexpr UINT refresh_complete_message = WM_APP + 1U;
constexpr UINT validation_complete_message = WM_APP + 2U;
constexpr UINT preview_complete_message = WM_APP + 3U;
constexpr UINT exposure_complete_message = WM_APP + 4U;
constexpr UINT render_complete_message = WM_APP + 5U;
constexpr UINT render_progress_message = WM_APP + 6U;
constexpr UINT preview_progress_message = WM_APP + 7U;
constexpr UINT validation_timer_id = 1U;
constexpr UINT webview_resize_timer_id = 2U;
constexpr UINT webview_resize_interval_ms = 50U;
struct Controls {
  HWND preview_surface = nullptr;
};

struct GuiRuntimeState;

enum class WebViewModalKind {
  none,
  edit_tag,
  input_options,
  preview_options,
  app_settings,
  delete_session,
  overwrite_output,
  notice
};

enum class GuiValidationPurpose { preview, output };

struct WebViewModalPayload {
  std::wstring title;
  std::wstring description;
  std::wstring value;
  std::wstring error;
  unsigned characters_remaining = 64U;
  bool valid = true;
  bool read_only = false;
  bool checked = false;
  std::optional<std::size_t> session_index;
  std::string session_id;
  std::string game_directory;
  std::string record_path;
  std::vector<std::string> target_paths;
};

struct WebViewModalState {
  WebViewModalKind kind = WebViewModalKind::none;
  std::uint64_t generation = 0U;
  bool dismissible = true;
  WebViewModalPayload payload;
};

struct GuiShellState {
  Controls controls;
  ITaskbarList3 *taskbar = nullptr;
  pano::app::ApplicationSettings application_settings;
  std::string application_settings_path;
  bool persistence_enabled = false;
  bool settings_loaded = false;
  bool headless = false;
  bool self_test_allows_warp = false;
  bool d3d12_debug = false;
  std::filesystem::path d3d12_debug_log_path;
  std::unique_ptr<GuiRuntimeState> runtime;
  std::unique_ptr<pano::app::WebViewHost> webview;
  std::unique_ptr<pano::app::WebViewHost> exposure_webview;
  bool webview_failed = false;
  bool webview_preview_visible = false;
  bool webview_resize_timer_active = false;
  bool webview_width_resize_dirty = false;
  bool webview_sizing_active = false;
  bool webview_sizing_changed_width = false;
  bool webview_maximized = false;
  std::uint64_t webview_layout_generation = 1U;
  std::optional<pano::app::GuiStage> webview_snapshot_stage;
  std::optional<double> webview_content_height;
  std::optional<double> exposure_content_height;
  WebViewModalState webview_modal;
  pano::app::GuiWorkflowState workflow;
  std::wstring game_directory;
  std::wstring screenshots_directory;
  std::wstring output_directory;
  std::wstring output_name = L"panorama.jpg";
  std::wstring jpeg_quality = L"95";
  std::wstring resolution_percent = L"100";
  std::wstring explicit_width;
  std::wstring status_text = L"Ready";
  std::string blend = "feather";
  std::string output_format = "jpeg";
  unsigned cpu_memory_mib = 1024U;
  unsigned cpu_workers = 0U;
  unsigned gpu_memory_mib = 1024U;
  bool allow_incomplete = false;
  bool auto_contrast = true;
  bool debug_coverage = false;
  bool use_gpu = true;
  bool require_gpu = false;
  pano::app::GuiBackendDecision backend =
      pano::app::GuiBackendDecision::unavailable;
  std::wstring backend_reason;
  bool thumbnail = false;
  bool exposure_open = false;
  std::optional<std::size_t> selected_record;
  std::string manual_session_path;
  bool rebuild_preview_after_validation = false;
  bool resolution_pixels = false;
  std::optional<bool> pending_render_with_thumbnail;
  std::optional<POINT> preview_pointer;
  HWND exposure_window = nullptr;
  bool exposure_overlay_boundaries = false;
  bool exposure_edits_applied = false;
  double final_exposure_ev = 0.0;
  unsigned operation_progress_percent = 0U;
  unsigned output_width = 0U;
  unsigned output_height = 0U;
  unsigned output_maximum_width = 0U;
  bool final_render_complete = false;
  int window_width = -1;
  int window_height = -1;
};

GuiShellState *active_application_state = nullptr;

GuiShellState &application_state() noexcept {
  return *active_application_state;
}

GuiShellState *shell_state(const HWND window) noexcept {
  return reinterpret_cast<GuiShellState *>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
}

struct RefreshResult {
  std::uint64_t generation = 0;
  std::vector<pano::app::GuiSessionRecord> records;
  std::string error;
};

struct ValidationResult {
  std::uint64_t generation = 0;
  GuiValidationPurpose purpose = GuiValidationPurpose::preview;
  std::optional<pano::app::RenderPlan> plan;
  std::string error;
};

struct PreviewResult {
  std::uint64_t generation = 0;
  std::uint64_t operation_generation = 0;
  pano::app::NativePreview *preview = nullptr;
  pano::app::CpuNativePreview *cpu_preview = nullptr;
  pano::app::GuiBackendDecision backend =
      pano::app::GuiBackendDecision::unavailable;
  bool reused = false;
  bool retained_owner_in_worker = false;
  bool succeeded = false;
  std::string fallback_error;
  std::string error;

  ~PreviewResult() {
    pano::app::destroy_native_preview(&preview);
    pano::app::destroy_cpu_native_preview(&cpu_preview);
  }
};

enum class ExposureCommand { automatic, manual_match, discard };

struct ExposureResult {
  std::uint64_t generation = 0;
  std::uint64_t operation_generation = 0;
  ExposureCommand command = ExposureCommand::automatic;
  pano::app::NativeExposureResult exposure;
  bool succeeded = false;
  std::string error;
};

struct RenderResult {
  std::uint64_t generation = 0;
  std::uint64_t operation_generation = 0;
  std::string session_id;
  std::string panorama_path;
  pano::app::NativeRenderResult render;
  bool cpu = false;
  bool succeeded = false;
  std::string error;
};

struct GuiRuntimeState {
  pano::app::GuiRefreshState refresh_state;
  std::mutex refresh_mutex;
  std::vector<std::unique_ptr<RefreshResult>> refresh_results;
  std::vector<std::thread> refresh_threads;
  std::atomic<HWND> refresh_window{nullptr};
  pano::app::GuiValidationState validation_state;
  GuiValidationPurpose scheduled_validation = GuiValidationPurpose::preview;
  std::optional<GuiValidationPurpose> completed_validation;
  std::mutex validation_mutex;
  std::vector<std::unique_ptr<ValidationResult>> validation_results;
  std::vector<std::thread> validation_threads;
  std::mutex preview_mutex;
  std::vector<std::unique_ptr<PreviewResult>> preview_results;
  std::vector<std::thread> preview_threads;
  std::vector<std::unique_ptr<ExposureResult>> exposure_results;
  std::vector<std::unique_ptr<RenderResult>> render_results;
  std::atomic<unsigned> render_progress_completed{0};
  std::atomic<unsigned> render_progress_total{0};
  std::mutex render_progress_mutex;
  std::string render_progress_phase;
  std::atomic<unsigned> preview_progress_percent{0};
  std::mutex preview_progress_mutex;
  std::string preview_progress_phase;
  std::uint64_t preview_generation = 0;
  bool preview_building = false;
  pano_gpu_cancellation_token *preview_cancellation = nullptr;
  pano_gpu_device *preview_device = nullptr;
  pano_gpu_preview_surface *preview_surface = nullptr;
  std::string d3d12_error;
  pano_gpu_preview *active_preview = nullptr;
  pano::app::NativePreview *active_preview_owner = nullptr;
  pano::app::CpuNativePreview *active_cpu_preview_owner = nullptr;
  pano::app::GuiPreviewViewState preview_view;
  unsigned preview_source_width = 0;
  unsigned preview_source_height = 0;
  unsigned preview_viewport_width = 0;
  unsigned preview_viewport_height = 0;
  unsigned preview_mask_width = 0;
  unsigned preview_mask_height = 0;
  std::optional<unsigned> exposure_target;
  std::vector<unsigned> exposure_selected;
  std::vector<std::uint8_t> preview_hovered;
  std::vector<std::uint8_t> preview_overlay_frames;
};

GuiRuntimeState &runtime_state() noexcept {
  return *application_state().runtime;
}

void reap_completed_threads(std::vector<std::thread> &threads) {
  for (auto thread = threads.begin(); thread != threads.end();) {
    if (thread->joinable() &&
        WaitForSingleObject(thread->native_handle(), 0U) == WAIT_OBJECT_0) {
      thread->join();
      thread = threads.erase(thread);
    } else {
      ++thread;
    }
  }
}

void reap_completed_workers() {
  reap_completed_threads(runtime_state().refresh_threads);
  reap_completed_threads(runtime_state().validation_threads);
  reap_completed_threads(runtime_state().preview_threads);
}

LRESULT CALLBACK preview_window_procedure(HWND window, UINT message,
                                          WPARAM wparam, LPARAM lparam);
void schedule_validation(HWND window, bool discard_preview,
                         GuiValidationPurpose purpose);

LRESULT CALLBACK exposure_window_procedure(HWND window, UINT message,
                                           WPARAM wparam, LPARAM lparam);
void layout_controls(HWND window);
bool retained_preview_ready();
void sync_webview_snapshot(HWND window);
void sync_exposure_webview_snapshot(HWND window);
void update_exposure_enablement();
void handle_exposure_webview_command(HWND window,
                                     const pano::app::WebViewCommand &command);
std::optional<int> webview_outer_height(HWND window, double css_height);
void report_application_error(const std::wstring &title,
                              const std::wstring &message);

std::mutex gui_debug_log_mutex;

void append_gui_debug_log(const std::filesystem::path &path,
                          const std::string_view message) {
  if (path.empty())
    return;
  SYSTEMTIME time{};
  GetLocalTime(&time);
  std::array<char, 2304> line{};
  const int prefix_size = std::snprintf(
      line.data(), line.size(),
      "%04u-%02u-%02u %02u:%02u:%02u.%03u [pid=%lu tid=%lu] gui: ",
      time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
      time.wSecond, time.wMilliseconds,
      static_cast<unsigned long>(GetCurrentProcessId()),
      static_cast<unsigned long>(GetCurrentThreadId()));
  if (prefix_size < 0 || static_cast<std::size_t>(prefix_size) >= line.size())
    return;
  const auto available = line.size() - static_cast<std::size_t>(prefix_size);
  const int message_size = std::snprintf(
      line.data() + prefix_size, available, "%.*s\r\n",
      static_cast<int>(std::min(message.size(), available - 3U)),
      message.data());
  if (message_size < 0)
    return;
  const auto line_size = std::min(
      line.size() - 1U, static_cast<std::size_t>(prefix_size) +
                            static_cast<std::size_t>(message_size));
  std::lock_guard<std::mutex> lock(gui_debug_log_mutex);
  const HANDLE file = CreateFileW(
      path.c_str(), FILE_APPEND_DATA,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;
  DWORD written = 0;
  WriteFile(file, line.data(), static_cast<DWORD>(line_size), &written,
            nullptr);
  FlushFileBuffers(file);
  CloseHandle(file);
}

bool configure_d3d12_debug(std::filesystem::path &path,
                           std::wstring &error) {
  std::array<wchar_t, 32768> local_app_data{};
  const DWORD length = GetEnvironmentVariableW(
      L"LOCALAPPDATA", local_app_data.data(),
      static_cast<DWORD>(local_app_data.size()));
  if (length == 0U || length >= local_app_data.size()) {
    error = L"LOCALAPPDATA is unavailable";
    return false;
  }
  const auto directory = std::filesystem::path(local_app_data.data()) /
                         L"PanoramaCapture" / L"logs";
  std::error_code directory_error;
  std::filesystem::create_directories(directory, directory_error);
  if (directory_error) {
    error = L"Cannot create the D3D12 log directory";
    return false;
  }
  SYSTEMTIME time{};
  GetLocalTime(&time);
  wchar_t filename[128]{};
  std::swprintf(filename, std::size(filename),
                L"d3d12-%04u%02u%02u-%02u%02u%02u-%lu.log", time.wYear,
                time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
                static_cast<unsigned long>(GetCurrentProcessId()));
  path = directory / filename;
  if (SetEnvironmentVariableW(L"PANO_D3D12_DEBUG", L"1") == FALSE ||
      SetEnvironmentVariableW(L"PANO_D3D12_DEBUG_LOG_PATH",
                              path.c_str()) == FALSE) {
    error = L"Cannot configure the D3D12 diagnostic environment";
    path.clear();
    return false;
  }
  append_gui_debug_log(path, "diagnostic mode configured");
  error.clear();
  return true;
}

bool write_runtime_probe_result(const std::filesystem::path &path,
                                const std::string &result) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << result << '\n';
  return output.good();
}

int run_runtime_probe(const std::filesystem::path &result_path) {
  DWORD previous_error_mode = 0;
  SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX,
                     &previous_error_mode);
  const HMODULE library = LoadLibraryExW(L"pano_gpu.dll", nullptr,
                                         LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                                             LOAD_LIBRARY_SEARCH_SYSTEM32);
  SetThreadErrorMode(previous_error_mode, nullptr);
  if (library == nullptr) {
    write_runtime_probe_result(result_path,
                               "GPU runtime failure: cannot load pano_gpu.dll");
    return 3;
  }
  pano_gpu_probe_options options{};
  options.size = sizeof(options);
  options.abi_version = PANO_GPU_ABI_VERSION;
  pano_gpu_adapter_info adapter{};
  adapter.size = sizeof(adapter);
  adapter.abi_version = PANO_GPU_ABI_VERSION;
  std::array<char, 512> error{};
  const pano_gpu_result probe = pano_gpu_probe_adapter(
      &options, &adapter, error.data(), static_cast<uint32_t>(error.size()));
  if (probe != PANO_GPU_SUCCESS) {
    const std::string detail =
        error.front() == '\0' ? "no compatible adapter" : error.data();
    write_runtime_probe_result(result_path,
                               "GPU runtime unavailable: " + detail);
    FreeLibrary(library);
    return probe == PANO_GPU_UNAVAILABLE ? 2 : 3;
  }
  if (pano_gpu_dispatch_self_test(0, error.data(),
                                  static_cast<uint32_t>(error.size())) !=
      PANO_GPU_SUCCESS) {
    const std::string detail =
        error.front() == '\0' ? "native D3D12 self-test failed" : error.data();
    write_runtime_probe_result(result_path, "GPU runtime failure: " + detail);
    FreeLibrary(library);
    return 3;
  }
  std::ostringstream result;
  result << "D3D12 runtime verified; ABI=" << PANO_GPU_ABI_VERSION
         << "; adapter=" << adapter.name << "; vendor=0x" << std::hex
         << std::setfill('0') << std::setw(4) << adapter.vendor_id
         << "; device=0x" << std::setw(4) << adapter.device_id << "; luid=0x"
         << std::setw(16) << adapter.luid;
  const bool written = write_runtime_probe_result(result_path, result.str());
  FreeLibrary(library);
  return written ? 0 : 3;
}

std::string wide_to_utf8(const std::wstring &value) {
  if (value.empty())
    return {};
  const int bytes = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (bytes <= 0)
    return {};
  std::string result(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), bytes,
                      nullptr, nullptr);
  return result;
}

std::wstring utf8_to_wide(const std::string &value) {
  if (value.empty())
    return {};
  const int characters =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), nullptr, 0);
  if (characters <= 0)
    return L"Invalid UTF-8";
  std::wstring result(static_cast<std::size_t>(characters), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(),
                      characters);
  return result;
}

std::size_t utf8_character_count(const std::string &value) noexcept {
  return static_cast<std::size_t>(
      std::count_if(value.begin(), value.end(), [](const unsigned char byte) {
        return (byte & 0xC0U) != 0x80U;
      }));
}

void set_status_text(const std::wstring &text) {
  application_state().status_text = text;
}

HBRUSH dark_background_brush() {
  static HBRUSH brush = CreateSolidBrush(RGB(3, 7, 18));
  return brush;
}

void apply_dark_caption(const HWND window) {
  using DwmSetWindowAttributeFunction =
      HRESULT(WINAPI *)(HWND, DWORD, const void *, DWORD);
  HMODULE module = LoadLibraryW(L"dwmapi.dll");
  const auto set_attribute =
      module == nullptr ? nullptr
                        : reinterpret_cast<DwmSetWindowAttributeFunction>(
                              GetProcAddress(module, "DwmSetWindowAttribute"));
  if (set_attribute != nullptr) {
    constexpr DWORD immersive_dark_mode = 20U;
    constexpr DWORD caption_color = 35U;
    constexpr DWORD caption_text_color = 36U;
    const BOOL enabled = TRUE;
    set_attribute(window, immersive_dark_mode, &enabled, sizeof(enabled));
    constexpr COLORREF caption = RGB(31, 31, 36);
    set_attribute(window, caption_color, &caption, sizeof(caption));
    constexpr COLORREF text = RGB(245, 245, 245);
    set_attribute(window, caption_text_color, &text, sizeof(text));
  }
  if (module != nullptr)
    FreeLibrary(module);
  RedrawWindow(window, nullptr, nullptr,
               RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
}

std::string native_settings_path() {
  std::wstring value(32768U, L'\0');
  const DWORD length = GetEnvironmentVariableW(
      L"APPDATA", value.data(), static_cast<DWORD>(value.size()));
  if (length == 0U || length >= value.size())
    return {};
  value.resize(length);
  return (std::filesystem::path(value) / L"PanoramaCapture" /
          L"gui-settings.json")
      .u8string();
}

unsigned default_gpu_memory_mib() {
  pano_gpu_probe_options probe{};
  probe.size = sizeof(probe);
  probe.abi_version = PANO_GPU_ABI_VERSION;
  pano_gpu_adapter_info adapter{};
  adapter.size = sizeof(adapter);
  adapter.abi_version = PANO_GPU_ABI_VERSION;
  std::array<char, 256> error{};
  if (pano_gpu_probe_adapter(&probe, &adapter, error.data(),
                             static_cast<std::uint32_t>(error.size())) !=
      PANO_GPU_SUCCESS)
    return 1024U;
  constexpr std::uint64_t mib = 1024ULL * 1024ULL;
  const auto half_mib = static_cast<unsigned>(
      std::min<std::uint64_t>(adapter.dedicated_bytes / (2U * mib), 4096U));
  return std::max(1024U, half_mib);
}

void save_gui_settings() {
  if (!application_state().persistence_enabled ||
      !application_state().settings_loaded ||
      application_state().application_settings_path.empty())
    return;
  application_state().application_settings.game_directory =
      wide_to_utf8(application_state().game_directory);
  application_state().application_settings.image_directory =
      wide_to_utf8(application_state().screenshots_directory);
  application_state().application_settings.output_directory =
      wide_to_utf8(application_state().output_directory);
  application_state().application_settings.auto_contrast =
      application_state().auto_contrast;
  std::string ignored;
  pano::app::save_application_settings(
      application_state().application_settings_path,
      application_state().application_settings, ignored);
}

void load_gui_settings() {
  if (!application_state().persistence_enabled)
    return;
  application_state().application_settings_path = native_settings_path();
  std::string error;
  application_state().settings_loaded = pano::app::load_application_settings(
      application_state().application_settings_path,
      application_state().application_settings, error);
  if (!application_state().settings_loaded) {
    application_state().status_text =
        L"Settings could not be loaded; changes will not be saved";
    return;
  }
  application_state().game_directory =
      utf8_to_wide(application_state().application_settings.game_directory);
  application_state().screenshots_directory =
      utf8_to_wide(application_state().application_settings.image_directory);
  application_state().output_directory =
      utf8_to_wide(application_state().application_settings.output_directory);
  application_state().auto_contrast =
      application_state().application_settings.auto_contrast;
  application_state().gpu_memory_mib =
      application_state().application_settings.gpu_memory_mib == 0U
          ? default_gpu_memory_mib()
          : application_state().application_settings.gpu_memory_mib;
  application_state().debug_coverage =
      application_state().application_settings.debug_coverage;
}

bool capture_gui_request(
    pano::app::GuiRenderRequestState &request, std::string &error,
    const GuiValidationPurpose purpose = GuiValidationPurpose::output) {
  pano::app::GuiRenderRequestState captured;
  const HWND window =
      runtime_state().refresh_window.load(std::memory_order_acquire);
  const auto *const shell = window == nullptr ? nullptr : shell_state(window);
  if (shell != nullptr && shell->selected_record.has_value() &&
      *shell->selected_record < runtime_state().refresh_state.records.size()) {
    const auto &record =
        runtime_state().refresh_state.records[*shell->selected_record];
    captured.session = record.path;
    captured.session_id = record.session.session_id;
  } else if (shell != nullptr)
    captured.session = shell->manual_session_path;
  if (shell == nullptr) {
    error = "GUI state is unavailable";
    return false;
  }
  const auto parse_value = [&error](const std::wstring &text,
                                    const bool allow_empty,
                                    std::optional<unsigned> &value) {
    const std::string bytes = wide_to_utf8(text);
    if (bytes.empty() && allow_empty) {
      value.reset();
      return true;
    }
    unsigned parsed = 0U;
    const auto result =
        std::from_chars(bytes.data(), bytes.data() + bytes.size(), parsed);
    if (bytes.empty() || result.ec != std::errc{} ||
        result.ptr != bytes.data() + bytes.size()) {
      error = "option values must be whole numbers";
      return false;
    }
    value = parsed;
    return true;
  };
  captured.image_dir = wide_to_utf8(shell->screenshots_directory);
  captured.output_directory = wide_to_utf8(shell->output_directory);
  captured.output_name = wide_to_utf8(shell->output_name);
  captured.format = shell->output_format;
  captured.blend = shell->blend;
  if (purpose == GuiValidationPurpose::output) {
    std::optional<unsigned> parsed;
    if (captured.format == "jpeg") {
      if (!parse_value(shell->jpeg_quality, false, parsed))
        return false;
      captured.jpeg_quality = *parsed;
    }
    if (shell->resolution_pixels) {
      captured.resolution_percent = 100U;
      if (!parse_value(shell->explicit_width, false, captured.width))
        return false;
      if (shell->output_maximum_width != 0U &&
          *captured.width > shell->output_maximum_width) {
        error = "output width exceeds the retained preview 100% scale";
        return false;
      }
    } else {
      if (!parse_value(shell->resolution_percent, false, parsed))
        return false;
      captured.resolution_percent = *parsed;
      captured.width.reset();
    }
  } else {
    captured.output_name = "preview.png";
    captured.resolution_percent = 100U;
    captured.width.reset();
    captured.jpeg_quality = 95U;
    if (captured.format != "jpeg" && captured.format != "png" &&
        captured.format != "exr")
      captured.format = "png";
  }
  captured.memory_mib = shell->cpu_memory_mib;
  captured.workers = shell->cpu_workers;
  captured.thumbnail = shell->thumbnail;
  captured.coverage = shell->debug_coverage;
  captured.allow_incomplete = shell->allow_incomplete;
  captured.auto_contrast = shell->auto_contrast;
  captured.final_exposure_ev = shell->final_exposure_ev;
  captured.gpu = shell->use_gpu;
  if (shell->use_gpu)
    captured.gpu_memory_mib = shell->gpu_memory_mib;
  else
    captured.gpu_memory_mib.reset();
  captured.gpu_strict = shell->use_gpu && shell->require_gpu;
  request = std::move(captured);
  return true;
}

void update_option_enablement() {
  if (!application_state().use_gpu)
    application_state().require_gpu = false;
}

void select_output_format(const HWND window, const int selection) {
  auto *const shell = shell_state(window);
  if (shell == nullptr || selection < 0 || selection > 2)
    return;
  constexpr std::array<const char *, 3> formats{"jpeg", "png", "exr"};
  shell->output_format = formats[static_cast<std::size_t>(selection)];
  auto name = std::filesystem::path(shell->output_name);
  name.replace_extension(shell->output_format == "jpeg"
                             ? L".jpg"
                             : utf8_to_wide("." + shell->output_format));
  shell->output_name = name.wstring();
  update_option_enablement();
  schedule_validation(window, false, GuiValidationPurpose::output);
}

bool copy_text_to_clipboard(const HWND owner, const std::wstring_view text,
                            std::wstring &error) {
  if (text.empty() ||
      text.size() > std::numeric_limits<SIZE_T>::max() / sizeof(wchar_t) - 1U) {
    error = L"Coordinates are too large for the clipboard";
    return false;
  }
  const SIZE_T bytes = (text.size() + 1U) * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (memory == nullptr) {
    error = L"Cannot allocate clipboard text";
    return false;
  }
  auto *const destination = static_cast<wchar_t *>(GlobalLock(memory));
  if (destination == nullptr) {
    GlobalFree(memory);
    error = L"Cannot prepare clipboard text";
    return false;
  }
  std::copy(text.begin(), text.end(), destination);
  destination[text.size()] = L'\0';
  GlobalUnlock(memory);
  if (!OpenClipboard(owner)) {
    GlobalFree(memory);
    error = L"Cannot open the clipboard";
    return false;
  }
  if (!EmptyClipboard()) {
    CloseClipboard();
    GlobalFree(memory);
    error = L"Cannot clear the clipboard";
    return false;
  }
  if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
    CloseClipboard();
    GlobalFree(memory);
    error = L"Cannot copy coordinates to the clipboard";
    return false;
  }
  CloseClipboard();
  error.clear();
  return true;
}

std::optional<std::wstring> choose_path(const HWND owner, const bool folder) {
  IFileOpenDialog *dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    return std::nullopt;
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_FORCEFILESYSTEM |
                     (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
  if (!folder) {
    const COMDLG_FILTERSPEC filters[] = {
        {L"Panorama sessions", L"PanoramaCaptureBridge.pano-*.json"},
        {L"JSON files", L"*.json"}};
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
  }
  std::optional<std::wstring> selected;
  if (SUCCEEDED(dialog->Show(owner))) {
    IShellItem *item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        selected = path;
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dialog->Release();
  return selected;
}

bool create_controls(const HWND window) {
  application_state().controls.preview_surface =
      CreateWindowExW(0, preview_window_class, L"", WS_CHILD, 0, 0, 0, 0,
                      window, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (application_state().controls.preview_surface == nullptr)
    return false;
  ShowWindow(application_state().controls.preview_surface, SW_HIDE);
  if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr,
                                 CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&application_state().taskbar))))
    application_state().taskbar->HrInit();
  return true;
}

void layout_controls(const HWND window) {
  RECT client{};
  auto *const shell = shell_state(window);
  if (shell == nullptr || !GetClientRect(window, &client))
    return;
  if (shell->webview != nullptr)
    shell->webview->resize(client);
  if (!shell->webview_preview_visible) {
    const int width = std::max(64L, client.right - client.left);
    const int height = std::max(32, width / 2);
    MoveWindow(application_state().controls.preview_surface, 0, 0, width,
               height, FALSE);
    ShowWindow(application_state().controls.preview_surface, SW_HIDE);
  }
}

bool update_preview_surface() {
  RECT bounds{};
  if (!GetClientRect(application_state().controls.preview_surface, &bounds))
    return false;
  if (runtime_state().active_cpu_preview_owner != nullptr ||
      (!application_state().use_gpu &&
       runtime_state().active_preview_owner == nullptr)) {
    pano_gpu_preview_surface_destroy(&runtime_state().preview_surface);
    pano_gpu_device_destroy(&runtime_state().preview_device);
    InvalidateRect(application_state().controls.preview_surface, nullptr,
                   FALSE);
    return true;
  }
  const auto width = static_cast<uint32_t>(std::max(0L, bounds.right));
  const auto height = static_cast<uint32_t>(std::max(0L, bounds.bottom));
  std::array<char, 512> error{};
  if (runtime_state().preview_device == nullptr) {
    pano_gpu_probe_options probe{};
    probe.size = sizeof(probe);
    probe.abi_version = PANO_GPU_ABI_VERSION;
    probe.allow_warp = application_state().self_test_allows_warp ? 1U : 0U;
    if (pano_gpu_device_create(
            &probe, &runtime_state().preview_device, error.data(),
            static_cast<uint32_t>(error.size())) != PANO_GPU_SUCCESS) {
      runtime_state().d3d12_error = error.data();
      return false;
    }
  }
  if (runtime_state().preview_surface == nullptr) {
    if (width == 0 || height == 0)
      return true;
    pano_gpu_preview_surface_create_options options{};
    options.size = sizeof(options);
    options.abi_version = PANO_GPU_ABI_VERSION;
    options.native_window = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
        application_state().controls.preview_surface));
    options.width = width;
    options.height = height;
    if (pano_gpu_preview_surface_create(
            runtime_state().preview_device, &options,
            &runtime_state().preview_surface, error.data(),
            static_cast<uint32_t>(error.size())) != PANO_GPU_SUCCESS) {
      runtime_state().d3d12_error = error.data();
      return false;
    }
  } else if (pano_gpu_preview_surface_resize(
                 runtime_state().preview_surface, width, height, error.data(),
                 static_cast<uint32_t>(error.size())) != PANO_GPU_SUCCESS) {
    runtime_state().d3d12_error = error.data();
    return false;
  }
  if (runtime_state().active_preview_owner != nullptr) {
    runtime_state().d3d12_error.clear();
    return true;
  }
  constexpr float background[4]{0.035F, 0.035F, 0.045F, 1.0F};
  const bool presented =
      pano_gpu_preview_surface_clear_present(
          runtime_state().preview_surface, background, error.data(),
          static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS;
  runtime_state().d3d12_error = presented ? std::string{} : error.data();
  return presented;
}

bool recover_preview_surface() {
  pano_gpu_preview_surface_destroy(&runtime_state().preview_surface);
  pano_gpu_device_destroy(&runtime_state().preview_device);
  return update_preview_surface();
}

bool prepare_preview_overlay_frames(const bool show_boundaries) {
  if (runtime_state().preview_overlay_frames.size() !=
      runtime_state().preview_hovered.size())
    return false;
  std::copy(runtime_state().preview_hovered.begin(),
            runtime_state().preview_hovered.end(),
            runtime_state().preview_overlay_frames.begin());
  if (show_boundaries) {
    for (const unsigned pose : runtime_state().exposure_selected)
      if (pose < runtime_state().preview_overlay_frames.size())
        runtime_state().preview_overlay_frames[pose] = 1U;
    if (runtime_state().exposure_target.has_value() &&
        *runtime_state().exposure_target <
            runtime_state().preview_overlay_frames.size())
      runtime_state().preview_overlay_frames[*runtime_state().exposure_target] =
          1U;
  }
  return true;
}

bool present_cpu_preview_view(HDC context = nullptr) {
  const auto *const preview = runtime_state().active_cpu_preview_owner;
  if (preview == nullptr)
    return true;
  pano::app::NativePreviewDiagnostics diagnostics;
  std::string error;
  if (!pano::app::query_cpu_native_preview(preview, diagnostics, error))
    return false;
  const auto &pixels = pano::app::cpu_native_preview_pixels(preview);
  const auto expected = static_cast<std::size_t>(diagnostics.preview_width) *
                        diagnostics.preview_height * 4U;
  if (pixels.size() != expected)
    return false;
  RECT client{};
  if (!GetClientRect(application_state().controls.preview_surface, &client))
    return false;
  const bool borrowed = context != nullptr;
  if (!borrowed)
    context = GetDC(application_state().controls.preview_surface);
  if (context == nullptr)
    return false;
  const auto crop =
      runtime_state().preview_view.overview
          ? pano::app::GuiPreviewCrop{0U, 0U, diagnostics.preview_width,
                                      diagnostics.preview_height}
          : runtime_state().preview_view.crop;
  BITMAPINFO bitmap{};
  bitmap.bmiHeader.biSize = sizeof(bitmap.bmiHeader);
  bitmap.bmiHeader.biWidth = static_cast<LONG>(diagnostics.preview_width);
  bitmap.bmiHeader.biHeight = -static_cast<LONG>(diagnostics.preview_height);
  bitmap.bmiHeader.biPlanes = 1U;
  bitmap.bmiHeader.biBitCount = 32U;
  bitmap.bmiHeader.biCompression = BI_RGB;
  SetStretchBltMode(context, HALFTONE);
  SetBrushOrgEx(context, 0, 0, nullptr);
  const int copied = StretchDIBits(
      context, 0, 0, std::max(0L, client.right), std::max(0L, client.bottom),
      static_cast<int>(crop.left), static_cast<int>(crop.top),
      static_cast<int>(crop.width), static_cast<int>(crop.height),
      pixels.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
  if (!borrowed)
    ReleaseDC(application_state().controls.preview_surface, context);
  return copied != GDI_ERROR;
}

bool present_preview_view() {
  if (runtime_state().active_cpu_preview_owner != nullptr)
    return present_cpu_preview_view();
  if (runtime_state().preview_surface == nullptr ||
      runtime_state().active_preview == nullptr)
    return true;
  const auto *const shell = shell_state(
      runtime_state().refresh_window.load(std::memory_order_acquire));
  const bool exposure_open = shell != nullptr &&
                             shell->exposure_window != nullptr &&
                             IsWindowVisible(shell->exposure_window);
  if (!exposure_open) {
    std::fill(runtime_state().preview_hovered.begin(),
              runtime_state().preview_hovered.end(), std::uint8_t{0});
    pano_gpu_preview_surface_present_request request{};
    request.size = sizeof(request);
    request.abi_version = PANO_GPU_ABI_VERSION;
    request.use_overview = runtime_state().preview_view.overview ? 1U : 0U;
    if (!runtime_state().preview_view.overview) {
      request.crop_left = runtime_state().preview_view.crop.left;
      request.crop_top = runtime_state().preview_view.crop.top;
      request.crop_width = runtime_state().preview_view.crop.width;
      request.crop_height = runtime_state().preview_view.crop.height;
    }
    std::array<char, 512> error{};
    return pano_gpu_preview_surface_present_base(
               runtime_state().preview_surface, runtime_state().active_preview,
               &request, error.data(),
               static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS;
  }
  pano_gpu_preview_surface_overlay_request request{};
  request.size = sizeof(request);
  request.abi_version = PANO_GPU_ABI_VERSION;
  request.use_overview = runtime_state().preview_view.overview ? 1U : 0U;
  if (!runtime_state().preview_view.overview) {
    request.crop_left = runtime_state().preview_view.crop.left;
    request.crop_top = runtime_state().preview_view.crop.top;
    request.crop_width = runtime_state().preview_view.crop.width;
    request.crop_height = runtime_state().preview_view.crop.height;
  }
  request.target_pose =
      runtime_state().exposure_target.has_value()
          ? static_cast<std::int32_t>(*runtime_state().exposure_target)
          : -1;
  request.target_mode = 0U;
  request.show_boundaries =
      exposure_open && shell->exposure_overlay_boundaries ? 1U : 0U;
  if (!prepare_preview_overlay_frames(request.show_boundaries != 0U))
    return false;
  request.hovered_frames = runtime_state().preview_overlay_frames.data();
  request.hovered_frame_bytes = runtime_state().preview_overlay_frames.size();
  std::array<char, 512> error{};
  return pano_gpu_preview_surface_present_overlay(
             runtime_state().preview_surface, runtime_state().active_preview,
             &request, error.data(),
             static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS;
}

void discard_active_preview();
void rebuild_exposure_pose_grid();
void report_application_error(const std::wstring &title,
                              const std::wstring &message);

void navigate_stage(const HWND window, const pano::app::GuiStage stage) {
  auto *const shell = shell_state(window);
  if (shell == nullptr)
    return;
  pano::app::navigate_gui_stage(shell->workflow, stage);
  shell->preview_pointer.reset();
  pano::app::reset_gui_preview_view(runtime_state().preview_view);
  if (stage != pano::app::GuiStage::preview &&
      shell->exposure_window != nullptr &&
      IsWindowVisible(shell->exposure_window)) {
    ShowWindow(shell->exposure_window, SW_HIDE);
    shell->exposure_open = false;
    std::fill(runtime_state().preview_hovered.begin(),
              runtime_state().preview_hovered.end(), std::uint8_t{0});
  }
  layout_controls(window);
  sync_webview_snapshot(window);
  if (stage != pano::app::GuiStage::preview || runtime_state().preview_building)
    return;
  if (!update_preview_surface() || !present_preview_view()) {
    discard_active_preview();
    if (!recover_preview_surface())
      report_application_error(L"Preview",
                               L"D3D12 preview surface is unavailable");
  }
}

pano::app::GuiPresentationState
gui_presentation(const GuiShellState &shell) noexcept {
  return pano::app::derive_gui_presentation(
      shell.workflow,
      runtime_state().active_preview_owner != nullptr ||
          runtime_state().active_cpu_preview_owner != nullptr,
      runtime_state().exposure_target.has_value(),
      !runtime_state().exposure_selected.empty(),
      shell.exposure_edits_applied || shell.final_exposure_ev != 0.0,
      shell.operation_progress_percent, shell.final_render_complete);
}

void update_exposure_enablement() {}

bool retained_preview_ready() {
  return runtime_state().active_preview_owner != nullptr ||
         runtime_state().active_cpu_preview_owner != nullptr;
}

void set_cancel_enabled(const bool) {}

void cancel_active_operation(const HWND window) {
  if (!runtime_state().preview_building ||
      runtime_state().preview_cancellation == nullptr)
    return;
  pano_gpu_cancellation_token_cancel(runtime_state().preview_cancellation);
  const auto *const shell = shell_state(window);
  const wchar_t *phase =
      shell != nullptr &&
              shell->workflow.operation == pano::app::GuiOperation::render
          ? L"render"
      : shell != nullptr &&
              shell->workflow.operation == pano::app::GuiOperation::exposure
          ? L"exposure"
          : L"preview";
  set_status_text(std::wstring(L"Requesting ") + phase + L" cancellation...");
}

void set_mutating_controls_enabled(const bool enabled) {
  if (enabled)
    update_option_enablement();
}

void set_operation_title(const HWND window, const std::wstring &detail,
                         const unsigned completed = 0U,
                         const unsigned total = 0U) {
  std::wstring title = L"Cyberpunk Panorama Stitcher";
  if (!detail.empty()) {
    title += L" \x2014 " + detail;
    if (total != 0U)
      title +=
          L" " + std::to_wstring(completed) + L"/" + std::to_wstring(total);
  }
  SetWindowTextW(window, title.c_str());
}

void begin_operation_progress(const HWND window) {
  if (auto *const shell = shell_state(window); shell != nullptr)
    shell->operation_progress_percent = 0U;
  if (application_state().taskbar != nullptr) {
    application_state().taskbar->SetProgressState(window, TBPF_NORMAL);
    application_state().taskbar->SetProgressValue(window, 0U, 100U);
  }
}

void update_operation_progress(const HWND window, const unsigned completed,
                               const unsigned total) {
  if (total == 0U)
    return;
  if (auto *const shell = shell_state(window); shell != nullptr)
    shell->operation_progress_percent =
        static_cast<unsigned>(std::min<std::uint64_t>(
            100U, static_cast<std::uint64_t>(completed) * 100U / total));
  if (application_state().taskbar != nullptr) {
    application_state().taskbar->SetProgressState(window, TBPF_NORMAL);
    application_state().taskbar->SetProgressValue(window, completed, total);
  }
}

void end_operation_progress(const HWND window) {
  if (auto *const shell = shell_state(window); shell != nullptr)
    shell->operation_progress_percent = 0U;
  if (application_state().taskbar != nullptr)
    application_state().taskbar->SetProgressState(window, TBPF_NOPROGRESS);
}

void notify_operation_complete() {
  if (!application_state().headless)
    MessageBeep(MB_OK);
}

void report_preview_progress(void *, const unsigned completed,
                             const unsigned total, const char *const phase) {
  if (total == 0U)
    return;
  const unsigned percent = static_cast<unsigned>(std::min<std::uint64_t>(
      100U, static_cast<std::uint64_t>(completed) * 100U / total));
  runtime_state().preview_progress_percent.store(percent,
                                                 std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(runtime_state().preview_progress_mutex);
    runtime_state().preview_progress_phase =
        phase == nullptr ? "Preparing preview" : phase;
  }
  if (application_state().d3d12_debug) {
    std::ostringstream message;
    message << "preview progress phase="
            << (phase == nullptr ? "Preparing preview" : phase)
            << " completed=" << completed << " total=" << total;
    append_gui_debug_log(application_state().d3d12_debug_log_path,
                         message.str());
  }
  const HWND target =
      runtime_state().refresh_window.load(std::memory_order_acquire);
  if (target != nullptr)
    PostMessageW(target, preview_progress_message, 0, 0);
}

void apply_preview_progress() {
  const unsigned percent =
      runtime_state().preview_progress_percent.load(std::memory_order_acquire);
  std::string phase;
  {
    std::lock_guard<std::mutex> lock(runtime_state().preview_progress_mutex);
    phase = runtime_state().preview_progress_phase;
  }
  const HWND window =
      runtime_state().refresh_window.load(std::memory_order_acquire);
  if (window == nullptr)
    return;
  set_status_text(utf8_to_wide(phase));
  set_operation_title(window, utf8_to_wide(phase), percent, 100U);
  update_operation_progress(window, percent, 100U);
}

bool begin_owned_operation(const pano::app::GuiOperation operation,
                           std::uint64_t &generation) {
  const HWND window =
      runtime_state().refresh_window.load(std::memory_order_acquire);
  auto *const shell = window == nullptr ? nullptr : shell_state(window);
  std::string error;
  if (shell == nullptr || !pano::app::begin_gui_operation(
                              shell->workflow, operation, generation, error)) {
    if (window != nullptr)
      set_status_text(utf8_to_wide(error));
    return false;
  }
  set_mutating_controls_enabled(false);
  set_cancel_enabled(true);
  const wchar_t *detail =
      operation == pano::app::GuiOperation::preview    ? L"Preparing preview"
      : operation == pano::app::GuiOperation::exposure ? L"Updating exposure"
                                                       : L"Rendering";
  set_operation_title(window, detail);
  begin_operation_progress(window);
  return true;
}

void complete_owned_operation(const std::uint64_t generation) {
  const HWND window =
      runtime_state().refresh_window.load(std::memory_order_acquire);
  auto *const shell = window == nullptr ? nullptr : shell_state(window);
  if (shell != nullptr)
    pano::app::complete_gui_operation(shell->workflow, generation);
  set_mutating_controls_enabled(true);
  set_cancel_enabled(false);
  if (window != nullptr) {
    end_operation_progress(window);
    set_operation_title(window, L"");
    apply_dark_caption(window);
  }
}

void discard_active_preview() {
  auto *const shell = shell_state(
      runtime_state().refresh_window.load(std::memory_order_acquire));
  runtime_state().active_preview = nullptr;
  pano::app::destroy_native_preview(&runtime_state().active_preview_owner);
  pano::app::destroy_cpu_native_preview(
      &runtime_state().active_cpu_preview_owner);
  runtime_state().preview_source_width = 0;
  runtime_state().preview_source_height = 0;
  runtime_state().preview_viewport_width = 0;
  runtime_state().preview_viewport_height = 0;
  runtime_state().preview_mask_width = 0;
  runtime_state().preview_mask_height = 0;
  runtime_state().exposure_target.reset();
  runtime_state().exposure_selected.clear();
  runtime_state().preview_hovered.clear();
  runtime_state().preview_overlay_frames.clear();
  if (shell != nullptr) {
    shell->exposure_edits_applied = false;
    shell->output_width = 0U;
    shell->output_height = 0U;
    shell->output_maximum_width = 0U;
    shell->final_render_complete = false;
    shell->backend = pano::app::GuiBackendDecision::unavailable;
    shell->backend_reason.clear();
    if (shell->exposure_window != nullptr) {
      ShowWindow(shell->exposure_window, SW_HIDE);
      shell->exposure_open = false;
    }
  }
  pano::app::reset_gui_preview_view(runtime_state().preview_view);
  if (const HWND window =
          runtime_state().refresh_window.load(std::memory_order_acquire);
      window != nullptr) {
    if (auto *const workflow_shell = shell_state(window);
        workflow_shell != nullptr)
      workflow_shell->workflow.preview_ready = false;
  }
  update_exposure_enablement();
  if (!runtime_state().preview_building &&
      runtime_state().preview_surface != nullptr)
    update_preview_surface();
}

void invalidate_preview() {
  ++runtime_state().preview_generation;
  if (runtime_state().preview_building) {
    if (runtime_state().preview_cancellation != nullptr)
      pano_gpu_cancellation_token_cancel(runtime_state().preview_cancellation);
    runtime_state().active_preview = nullptr;
    pano::app::reset_gui_preview_view(runtime_state().preview_view);
    return;
  }
  discard_active_preview();
}

void start_preview() {
  if (!runtime_state().validation_state.plan.has_value() ||
      runtime_state().preview_building)
    return;
  const auto &plan = *runtime_state().validation_state.plan;
  RECT bounds{};
  if (!GetClientRect(application_state().controls.preview_surface, &bounds) ||
      bounds.right <= 0) {
    report_application_error(L"Preview", L"Preview surface has no usable size");
    return;
  }
  if (application_state().d3d12_debug) {
    std::ostringstream message;
    message << "preview requested viewport-width=" << bounds.right
            << " frames=" << plan.session.frames.size()
            << " blend=" << plan.blend
            << " auto-contrast=" << (plan.auto_contrast ? 1 : 0)
            << " gpu-memory-mib="
            << (plan.gpu_memory_mib.has_value()
                    ? std::to_string(*plan.gpu_memory_mib)
                    : std::string("automatic"));
    append_gui_debug_log(application_state().d3d12_debug_log_path,
                         message.str());
  }
  const bool retaining_cpu =
      runtime_state().active_cpu_preview_owner != nullptr;
  const bool d3d12_available =
      !retaining_cpu && plan.use_gpu && update_preview_surface();
  const auto backend =
      retaining_cpu
          ? application_state().backend
          : pano::app::select_gui_backend(plan.use_gpu, plan.gpu_strict,
                                          d3d12_available, true);
  if (backend == pano::app::GuiBackendDecision::strict_d3d12_rejection) {
    report_application_error(
        L"Preview", utf8_to_wide(runtime_state().d3d12_error.empty()
                                     ? "D3D12 preview device is unavailable"
                                     : runtime_state().d3d12_error));
    return;
  }
  if (backend == pano::app::GuiBackendDecision::unavailable) {
    report_application_error(L"Preview",
                             L"No native render backend is available");
    return;
  }
  const bool cpu = backend != pano::app::GuiBackendDecision::d3d12;
  if (cpu) {
    pano_gpu_preview_surface_destroy(&runtime_state().preview_surface);
    pano_gpu_device_destroy(&runtime_state().preview_device);
  }
  std::uint64_t operation_generation = 0;
  if (!begin_owned_operation(pano::app::GuiOperation::preview,
                             operation_generation))
    return;
  const bool reuse =
      cpu ? runtime_state().active_cpu_preview_owner != nullptr
          : runtime_state().active_preview_owner != nullptr;
  auto *const retained_owner = runtime_state().active_preview_owner;
  auto *const retained_cpu_owner = runtime_state().active_cpu_preview_owner;
  if (!reuse) {
    discard_active_preview();
  } else {
    runtime_state().active_preview = nullptr;
    if (cpu)
      runtime_state().active_cpu_preview_owner = nullptr;
    if (auto *const shell = shell_state(
            runtime_state().refresh_window.load(std::memory_order_acquire));
        shell != nullptr)
      shell->workflow.preview_ready = false;
  }
  pano_gpu_cancellation_token_destroy(&runtime_state().preview_cancellation);
  std::array<char, 512> gpu_error{};
  if (pano_gpu_cancellation_token_create(
          &runtime_state().preview_cancellation, gpu_error.data(),
          static_cast<uint32_t>(gpu_error.size())) != PANO_GPU_SUCCESS) {
    report_application_error(L"Preview",
                             L"Cannot create preview cancellation state");
    complete_owned_operation(operation_generation);
    return;
  }
  const std::uint64_t generation = ++runtime_state().preview_generation;
  const unsigned viewport_width = static_cast<unsigned>(bounds.right);
  auto result = std::make_unique<PreviewResult>();
  result->generation = generation;
  result->operation_generation = operation_generation;
  result->backend = backend;
  result->reused = reuse;
  result->retained_owner_in_worker = reuse;
  if (cpu && reuse)
    result->cpu_preview = retained_cpu_owner;
  runtime_state().preview_building = true;
  set_cancel_enabled(true);
  set_status_text(cpu ? L"Building CPU preview..."
                      : L"Building D3D12 preview...");
  if (const HWND window =
          runtime_state().refresh_window.load(std::memory_order_acquire);
      window != nullptr)
    layout_controls(window);
  try {
    reap_completed_workers();
    runtime_state().preview_threads.emplace_back(
        [viewport_width, retained_owner, cpu,
         plan = *runtime_state().validation_state.plan,
         result = std::move(result)]() mutable {
          pano::app::NativePreviewOptions options;
          options.viewport_width = viewport_width;
          options.gpu_cancellation = runtime_state().preview_cancellation;
          options.progress = report_preview_progress;
          if (cpu && result->reused) {
            result->succeeded = pano::app::rebuild_cpu_native_preview(
                result->cpu_preview, plan, options, result->error);
          } else if (cpu) {
            result->succeeded = pano::app::create_cpu_native_preview(
                plan, options, &result->cpu_preview, result->error);
          } else if (result->reused) {
            result->succeeded = pano::app::rebuild_native_preview(
                retained_owner, plan, options, result->error);
          } else {
            result->succeeded = pano::app::create_native_preview(
                runtime_state().preview_device, plan, options, &result->preview,
                result->error);
          }
          if (!cpu && !result->succeeded && !plan.gpu_strict &&
              pano_gpu_cancellation_token_is_cancelled(
                  runtime_state().preview_cancellation) == 0) {
            result->fallback_error = result->error;
            if (application_state().d3d12_debug) {
              append_gui_debug_log(
                  application_state().d3d12_debug_log_path,
                  "D3D12 preview failed; starting CPU fallback: " +
                      result->fallback_error);
            }
            result->error.clear();
            pano::app::destroy_native_preview(&result->preview);
            result->backend = pano::app::GuiBackendDecision::cpu_fallback;
            result->reused = false;
            result->succeeded = pano::app::create_cpu_native_preview(
                plan, options, &result->cpu_preview, result->error);
          }
          {
            std::lock_guard<std::mutex> lock(runtime_state().preview_mutex);
            runtime_state().preview_results.push_back(std::move(result));
          }
          const HWND target =
              runtime_state().refresh_window.load(std::memory_order_acquire);
          if (target != nullptr)
            PostMessageW(target, preview_complete_message, 0, 0);
        });
  } catch (...) {
    runtime_state().preview_building = false;
    if (reuse && !cpu) {
      runtime_state().active_preview = pano::app::native_preview_handle(
          runtime_state().active_preview_owner);
      if (auto *const shell = shell_state(
              runtime_state().refresh_window.load(std::memory_order_acquire));
          shell != nullptr)
        shell->workflow.preview_ready = true;
    }
    pano_gpu_cancellation_token_destroy(&runtime_state().preview_cancellation);
    set_cancel_enabled(false);
    complete_owned_operation(operation_generation);
    report_application_error(L"Preview", L"Cannot start preview worker");
  }
}

void apply_preview_results() {
  std::vector<std::unique_ptr<PreviewResult>> results;
  {
    std::lock_guard<std::mutex> lock(runtime_state().preview_mutex);
    results.swap(runtime_state().preview_results);
  }
  for (auto &result : results) {
    runtime_state().preview_building = false;
    const bool cpu = result->backend != pano::app::GuiBackendDecision::d3d12;
    if (result->generation != runtime_state().preview_generation) {
      if (result->retained_owner_in_worker)
        discard_active_preview();
      complete_owned_operation(result->operation_generation);
      continue;
    }
    const bool missing_preview =
        !result->reused &&
        (cpu ? result->cpu_preview == nullptr : result->preview == nullptr);
    if (!result->succeeded || missing_preview) {
      if (result->reused) {
        if (cpu) {
          runtime_state().active_cpu_preview_owner = result->cpu_preview;
          result->cpu_preview = nullptr;
        } else {
          runtime_state().active_preview = pano::app::native_preview_handle(
              runtime_state().active_preview_owner);
        }
        const HWND window =
            runtime_state().refresh_window.load(std::memory_order_acquire);
        if (auto *const shell =
                window == nullptr ? nullptr : shell_state(window);
            shell != nullptr)
          shell->workflow.preview_ready = true;
        if (window != nullptr)
          layout_controls(window);
        if (!update_preview_surface() || !present_preview_view())
          discard_active_preview();
      } else if (result->retained_owner_in_worker) {
        discard_active_preview();
      }
      complete_owned_operation(result->operation_generation);
      report_application_error(L"Preview", utf8_to_wide(result->error));
      continue;
    }
    pano::app::NativePreviewDiagnostics diagnostics;
    pano::app::NativePreview *const owner =
        result->reused ? runtime_state().active_preview_owner : result->preview;
    pano::app::CpuNativePreview *const cpu_owner = result->cpu_preview;
    const bool queried = cpu ? pano::app::query_cpu_native_preview(
                                   cpu_owner, diagnostics, result->error)
                             : pano::app::query_native_preview(
                                   owner, diagnostics, result->error);
    if (!queried) {
      if (result->retained_owner_in_worker)
        discard_active_preview();
      complete_owned_operation(result->operation_generation);
      report_application_error(L"Preview", utf8_to_wide(result->error));
      continue;
    }
    if (cpu) {
      if (!result->reused) {
        pano_gpu_preview_surface_destroy(&runtime_state().preview_surface);
        pano_gpu_device_destroy(&runtime_state().preview_device);
        pano::app::destroy_native_preview(
            &runtime_state().active_preview_owner);
      }
      runtime_state().active_cpu_preview_owner = result->cpu_preview;
      result->cpu_preview = nullptr;
    } else if (!result->reused) {
      runtime_state().active_preview_owner = result->preview;
      result->preview = nullptr;
    }
    runtime_state().active_preview =
        cpu ? nullptr
            : pano::app::native_preview_handle(
                  runtime_state().active_preview_owner);
    runtime_state().preview_source_width = diagnostics.preview_width;
    runtime_state().preview_source_height = diagnostics.preview_height;
    runtime_state().preview_viewport_width = diagnostics.overview_width;
    runtime_state().preview_viewport_height = diagnostics.overview_height;
    runtime_state().preview_mask_width = cpu ? 0U : diagnostics.mask_width;
    runtime_state().preview_mask_height = cpu ? 0U : diagnostics.mask_height;
    runtime_state().preview_hovered.assign(diagnostics.frame_count, 0U);
    runtime_state().preview_overlay_frames.assign(
        diagnostics.frame_count, 0U);
    if (!cpu)
      rebuild_exposure_pose_grid();
    pano::app::reset_gui_preview_view(runtime_state().preview_view);
    std::wstring ready_status =
        cpu ? L"CPU preview ready" : L"D3D12 preview ready";
    const HWND window =
        runtime_state().refresh_window.load(std::memory_order_acquire);
    if (window != nullptr) {
      if (auto *const shell = shell_state(window); shell != nullptr) {
        shell->workflow.preview_ready = true;
        shell->backend = result->backend;
        shell->backend_reason =
            result->backend == pano::app::GuiBackendDecision::cpu_forced
                ? L"CPU backend forced by --no-gpu"
            : result->backend == pano::app::GuiBackendDecision::cpu_fallback
                ? L"D3D12 unavailable (" +
                      utf8_to_wide(result->fallback_error.empty()
                                       ? runtime_state().d3d12_error
                                       : result->fallback_error) +
                      L"); using memory-bounded CPU backend"
                : L"D3D12 backend selected";
        ready_status = shell->backend_reason;
      }
      layout_controls(window);
    }
    if (!update_preview_surface() || !present_preview_view()) {
      discard_active_preview();
      complete_owned_operation(result->operation_generation);
      set_status_text(L"Cannot present the retained native preview");
      continue;
    }
    if (window != nullptr)
      update_operation_progress(window, 100U, 100U);
    complete_owned_operation(result->operation_generation);
    set_status_text(ready_status);
    if (application_state().d3d12_debug) {
      append_gui_debug_log(
          application_state().d3d12_debug_log_path,
          cpu ? "CPU fallback preview ready" : "D3D12 preview ready");
    }
    notify_operation_complete();
  }
  pano_gpu_cancellation_token_destroy(&runtime_state().preview_cancellation);
  set_cancel_enabled(false);
  update_exposure_enablement();
}

void start_exposure(const ExposureCommand command) {
  if (!retained_preview_ready() ||
      runtime_state().preview_building)
    return;
  if (command != ExposureCommand::discard &&
      !runtime_state().exposure_target.has_value())
    return;
  if (command == ExposureCommand::manual_match &&
      runtime_state().exposure_selected.empty())
    return;
  std::uint64_t operation_generation = 0;
  if (!begin_owned_operation(pano::app::GuiOperation::exposure,
                             operation_generation))
    return;
  pano_gpu_cancellation_token_destroy(&runtime_state().preview_cancellation);
  std::array<char, 512> gpu_error{};
  if (pano_gpu_cancellation_token_create(
          &runtime_state().preview_cancellation, gpu_error.data(),
          static_cast<uint32_t>(gpu_error.size())) != PANO_GPU_SUCCESS) {
    set_status_text(L"Cannot create exposure cancellation state");
    complete_owned_operation(operation_generation);
    return;
  }
  const std::uint64_t generation = ++runtime_state().preview_generation;
  auto result = std::make_unique<ExposureResult>();
  result->generation = generation;
  result->operation_generation = operation_generation;
  result->command = command;
  const auto target = runtime_state().exposure_target;
  const auto selected = runtime_state().exposure_selected;
  auto *const owner = runtime_state().active_preview_owner;
  auto *const cpu_owner = runtime_state().active_cpu_preview_owner;
  const bool cpu = cpu_owner != nullptr;
  runtime_state().active_preview = nullptr;
  runtime_state().preview_building = true;
  if (auto *const shell = shell_state(
          runtime_state().refresh_window.load(std::memory_order_acquire));
      shell != nullptr) {
    shell->workflow.preview_ready = false;
  }
  set_cancel_enabled(true);
  update_exposure_enablement();
  set_status_text(command == ExposureCommand::discard
                      ? L"Resetting exposure preview..."
                      : L"Measuring exposure overlaps...");
  if (const HWND window =
          runtime_state().refresh_window.load(std::memory_order_acquire);
      window != nullptr)
    layout_controls(window);
  try {
    reap_completed_workers();
    runtime_state().preview_threads.emplace_back(
        [command, target, selected, owner, cpu_owner, cpu,
         result = std::move(result)]() mutable {
          pano::app::NativePreviewOptions options;
          options.gpu_cancellation = runtime_state().preview_cancellation;
          options.progress = report_preview_progress;
          if (command == ExposureCommand::automatic) {
            result->succeeded =
                cpu ? pano::app::apply_cpu_native_automatic_exposure(
                          cpu_owner, *target, options, result->exposure,
                          result->error)
                    : pano::app::apply_native_automatic_exposure(
                          owner, *target, options, result->exposure,
                          result->error);
          } else if (command == ExposureCommand::manual_match) {
            result->succeeded =
                cpu ? pano::app::apply_cpu_native_manual_exposure_match(
                          cpu_owner, *target, selected, options,
                          result->exposure, result->error)
                    : pano::app::apply_native_manual_exposure_match(
                          owner, *target, selected, options, result->exposure,
                          result->error);
          } else {
            result->succeeded =
                cpu ? pano::app::discard_cpu_native_exposure_edits(
                          cpu_owner, options, result->exposure, result->error)
                    : pano::app::discard_native_exposure_edits(
                          owner, options, result->exposure, result->error);
          }
          {
            std::lock_guard<std::mutex> lock(runtime_state().preview_mutex);
            runtime_state().exposure_results.push_back(std::move(result));
          }
          const HWND target_window =
              runtime_state().refresh_window.load(std::memory_order_acquire);
          if (target_window != nullptr)
            PostMessageW(target_window, exposure_complete_message, 0, 0);
        });
  } catch (...) {
    runtime_state().preview_building = false;
    runtime_state().active_preview =
        runtime_state().active_preview_owner == nullptr
            ? nullptr
            : pano::app::native_preview_handle(
                  runtime_state().active_preview_owner);
    if (auto *const shell = shell_state(
            runtime_state().refresh_window.load(std::memory_order_acquire));
        shell != nullptr)
      shell->workflow.preview_ready = true;
    pano_gpu_cancellation_token_destroy(&runtime_state().preview_cancellation);
    set_cancel_enabled(false);
    update_exposure_enablement();
    complete_owned_operation(operation_generation);
    report_application_error(L"Exposure", L"Cannot start exposure worker");
  }
}

void apply_exposure_results() {
  std::vector<std::unique_ptr<ExposureResult>> results;
  {
    std::lock_guard<std::mutex> lock(runtime_state().preview_mutex);
    results.swap(runtime_state().exposure_results);
  }
  for (auto &result : results) {
    runtime_state().preview_building = false;
    if (result->generation != runtime_state().preview_generation) {
      discard_active_preview();
      complete_owned_operation(result->operation_generation);
      continue;
    }
    runtime_state().active_preview =
        runtime_state().active_preview_owner == nullptr
            ? nullptr
            : pano::app::native_preview_handle(
                  runtime_state().active_preview_owner);
    const HWND window =
        runtime_state().refresh_window.load(std::memory_order_acquire);
    if (!result->succeeded) {
      if (auto *const shell = window == nullptr ? nullptr : shell_state(window);
          shell != nullptr)
        shell->workflow.preview_ready = true;
      if (window != nullptr)
        layout_controls(window);
      complete_owned_operation(result->operation_generation);
      report_application_error(
          result->command == ExposureCommand::automatic
              ? L"Automatic exposure"
              : L"Exposure",
          utf8_to_wide(result->error));
      if (!update_preview_surface() || !present_preview_view())
        discard_active_preview();
      continue;
    }
    if (auto *const shell = window == nullptr ? nullptr : shell_state(window);
        shell != nullptr) {
      shell->workflow.preview_ready = true;
      shell->exposure_edits_applied = std::any_of(
          result->exposure.gains.begin(), result->exposure.gains.end(),
          [](const float gain) { return std::fabs(gain - 1.0F) > 1.0e-6F; });
    }
    if (window != nullptr)
      layout_controls(window);
    if (!update_preview_surface() || !present_preview_view()) {
      discard_active_preview();
      complete_owned_operation(result->operation_generation);
      set_status_text(L"Cannot present recomputed exposure preview");
      continue;
    }
    if (window != nullptr)
      update_operation_progress(window, 100U, 100U);
    complete_owned_operation(result->operation_generation);
    const std::wstring status =
        result->exposure.warning
            ? L"Exposure applied with disconnected-pose warning"
            : L"Exposure preview updated";
    set_status_text(status);
    notify_operation_complete();
  }
  pano_gpu_cancellation_token_destroy(&runtime_state().preview_cancellation);
  set_cancel_enabled(false);
  update_exposure_enablement();
}

bool set_exposure_reference_pose(const unsigned pose) {
  if (runtime_state().preview_building ||
      pose >= runtime_state().preview_hovered.size())
    return false;
  const bool changed =
      runtime_state().exposure_target != pose ||
      std::find(runtime_state().exposure_selected.begin(),
                runtime_state().exposure_selected.end(),
                pose) != runtime_state().exposure_selected.end();
  runtime_state().exposure_target = pose;
  runtime_state().exposure_selected.erase(
      std::remove(runtime_state().exposure_selected.begin(),
                  runtime_state().exposure_selected.end(), pose),
      runtime_state().exposure_selected.end());
  return changed;
}

bool toggle_exposure_manual_pose(const unsigned pose) {
  if (runtime_state().preview_building ||
      pose >= runtime_state().preview_hovered.size() ||
      runtime_state().exposure_target == pose)
    return false;
  const auto found = std::find(runtime_state().exposure_selected.begin(),
                               runtime_state().exposure_selected.end(), pose);
  if (found == runtime_state().exposure_selected.end())
    runtime_state().exposure_selected.push_back(pose);
  else
    runtime_state().exposure_selected.erase(found);
  std::sort(runtime_state().exposure_selected.begin(),
            runtime_state().exposure_selected.end());
  return true;
}

bool apply_exposure_preview_click(const bool reference) {
  const auto &hovered = runtime_state().preview_hovered;
  if (reference) {
    const auto count = std::count(hovered.begin(), hovered.end(), 1U);
    if (count != 1)
      return false;
    return set_exposure_reference_pose(static_cast<unsigned>(
        std::distance(hovered.begin(), std::find(hovered.begin(), hovered.end(),
                                                 std::uint8_t{1}))));
  }
  bool changed = false;
  for (std::size_t pose = 0; pose < hovered.size(); ++pose)
    if (hovered[pose] != 0U)
      changed =
          toggle_exposure_manual_pose(static_cast<unsigned>(pose)) || changed;
  return changed;
}

void publish_exposure_selection(const HWND window) {
  update_exposure_enablement();
  sync_exposure_webview_snapshot(window);
  sync_webview_snapshot(window);
  present_preview_view();
}

LRESULT CALLBACK preview_window_procedure(const HWND window, const UINT message,
                                          const WPARAM wparam,
                                          const LPARAM lparam) {
  switch (message) {
  case WM_PAINT:
    if (runtime_state().active_cpu_preview_owner != nullptr) {
      PAINTSTRUCT paint{};
      HDC context = BeginPaint(window, &paint);
      present_cpu_preview_view(context);
      EndPaint(window, &paint);
      return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
  case WM_MOUSEMOVE:
    if (retained_preview_ready() &&
        runtime_state().preview_viewport_width != 0U &&
        runtime_state().preview_viewport_height != 0U) {
      auto *const shell = shell_state(GetParent(window));
      const POINT pointer{static_cast<short>(LOWORD(lparam)),
                          static_cast<short>(HIWORD(lparam))};
      if (shell != nullptr && shell->preview_pointer.has_value() &&
          shell->preview_pointer->x == pointer.x &&
          shell->preview_pointer->y == pointer.y)
        return 0;
      if (shell != nullptr)
        shell->preview_pointer = pointer;
      RECT client{};
      GetClientRect(window, &client);
      const double width = std::max(1L, client.right - client.left);
      const double height = std::max(1L, client.bottom - client.top);
      std::string error;
      if (pano::app::calculate_gui_preview_crop(
              runtime_state().preview_source_width,
              runtime_state().preview_source_height,
              runtime_state().preview_viewport_width,
              runtime_state().preview_viewport_height,
              static_cast<double>(static_cast<short>(LOWORD(lparam))) / width,
              static_cast<double>(static_cast<short>(HIWORD(lparam))) / height,
              runtime_state().preview_view, error)) {
        std::fill(runtime_state().preview_hovered.begin(),
                  runtime_state().preview_hovered.end(), std::uint8_t{0});
        const bool exposure_open = shell != nullptr &&
                                   shell->exposure_window != nullptr &&
                                   IsWindowVisible(shell->exposure_window);
        if (exposure_open && runtime_state().active_preview_owner != nullptr) {
          pano::app::GuiPreviewHitRequest hit;
          hit.source_width = runtime_state().preview_source_width;
          hit.source_height = runtime_state().preview_source_height;
          hit.mask_width = runtime_state().preview_mask_width;
          hit.mask_height = runtime_state().preview_mask_height;
          hit.frame_count =
              static_cast<unsigned>(runtime_state().preview_hovered.size());
          hit.pointer_x = static_cast<double>(pointer.x) / width;
          hit.pointer_y = static_cast<double>(pointer.y) / height;
          hit.view = runtime_state().preview_view;
          std::vector<unsigned> candidates;
          if (pano::app::gui_preview_hit_test(
                  hit,
                  pano::app::native_preview_masks(
                      runtime_state().active_preview_owner),
                  candidates, error))
            for (const unsigned candidate : candidates)
              runtime_state().preview_hovered[candidate] = 1U;
        }
        present_preview_view();
      }
      TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
      TrackMouseEvent(&tracking);
    }
    return 0;
  case WM_MOUSELEAVE:
    if (auto *const shell = shell_state(GetParent(window)); shell != nullptr)
      shell->preview_pointer.reset();
    pano::app::reset_gui_preview_view(runtime_state().preview_view);
    std::fill(runtime_state().preview_hovered.begin(),
              runtime_state().preview_hovered.end(), std::uint8_t{0});
    present_preview_view();
    return 0;
  case WM_LBUTTONUP:
    if (const auto *const shell = shell_state(GetParent(window));
        shell != nullptr && shell->exposure_window != nullptr &&
        IsWindowVisible(shell->exposure_window) &&
        apply_exposure_preview_click(true))
      publish_exposure_selection(GetParent(window));
    return 0;
  case WM_RBUTTONUP:
    if (const auto *const shell = shell_state(GetParent(window));
        shell != nullptr && shell->exposure_window != nullptr &&
        IsWindowVisible(shell->exposure_window) &&
        apply_exposure_preview_click(false))
      publish_exposure_selection(GetParent(window));
    return 0;
  default:
    return DefWindowProcW(window, message, wparam, lparam);
  }
}

void position_exposure_panel(const HWND owner) {
  auto *const shell = shell_state(owner);
  if (shell == nullptr || shell->exposure_window == nullptr)
    return;
  RECT owner_bounds{};
  GetWindowRect(owner, &owner_bounds);
  const unsigned dpi = GetDpiForWindow(owner);
  const int width = MulDiv(340, static_cast<int>(dpi), 96);
  const int height = shell->exposure_content_height.has_value()
                         ? webview_outer_height(shell->exposure_window,
                                                *shell->exposure_content_height)
                               .value_or(MulDiv(520, static_cast<int>(dpi), 96))
                         : MulDiv(520, static_cast<int>(dpi), 96);
  HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{sizeof(info)};
  GetMonitorInfoW(monitor, &info);
  int x = owner_bounds.right;
  if (x + width > info.rcWork.right)
    x = std::max(info.rcWork.left, owner_bounds.left - width);
  const int work_height = info.rcWork.bottom - info.rcWork.top;
  const int fitted_height = std::min(height, work_height);
  const int y =
      std::clamp(owner_bounds.top, info.rcWork.top,
                 std::max(info.rcWork.top, info.rcWork.bottom - fitted_height));
  SetWindowPos(shell->exposure_window, HWND_TOP, x, y, width, fitted_height,
               SWP_NOACTIVATE);
}

void rebuild_exposure_pose_grid() {
  const HWND owner =
      runtime_state().refresh_window.load(std::memory_order_acquire);
  if (owner != nullptr)
    sync_exposure_webview_snapshot(owner);
}

void show_exposure_panel(const HWND owner) {
  auto *const shell = shell_state(owner);
  if (shell == nullptr)
    return;
  if (shell->exposure_window != nullptr &&
      IsWindowVisible(shell->exposure_window)) {
    ShowWindow(shell->exposure_window, SW_HIDE);
    shell->exposure_open = false;
    std::fill(runtime_state().preview_hovered.begin(),
              runtime_state().preview_hovered.end(), std::uint8_t{0});
    present_preview_view();
    sync_webview_snapshot(owner);
    return;
  }
  if (shell->exposure_window == nullptr) {
    shell->exposure_window = CreateWindowExW(
        exposure_window_extended_style, exposure_window_class, L"Exposure",
        exposure_window_style, CW_USEDEFAULT, CW_USEDEFAULT, 340, 520, owner,
        nullptr, GetModuleHandleW(nullptr), owner);
    if (shell->exposure_window == nullptr)
      return;
    apply_dark_caption(shell->exposure_window);
  }
  position_exposure_panel(owner);
  ShowWindow(shell->exposure_window, SW_SHOWNOACTIVATE);
  shell->exposure_open = true;
  SetWindowTextW(shell->exposure_window, L"Exposure");
  apply_dark_caption(shell->exposure_window);
  SetActiveWindow(shell->exposure_window);
  sync_webview_snapshot(owner);
  sync_exposure_webview_snapshot(owner);
}

LRESULT CALLBACK exposure_window_procedure(const HWND window,
                                           const UINT message,
                                           const WPARAM wparam,
                                           const LPARAM lparam) {
  const HWND owner = GetWindow(window, GW_OWNER);
  auto *const shell = owner == nullptr ? nullptr : shell_state(owner);
  switch (message) {
  case WM_CREATE: {
    if (shell == nullptr)
      return -1;
    apply_dark_caption(window);
    shell->exposure_webview = std::make_unique<pano::app::WebViewHost>(
        window,
        [owner](const pano::app::WebViewCommand &command) {
          handle_exposure_webview_command(owner, command);
        },
        pano::app::WebViewPage::exposure);
    std::wstring error;
    if (!shell->exposure_webview->start(error)) {
      shell->exposure_webview.reset();
      report_application_error(L"Exposure", error);
      return -1;
    }
    return 0;
  }
  case WM_SIZE:
    if (shell != nullptr && shell->exposure_webview != nullptr) {
      const RECT bounds{0, 0, LOWORD(lparam), HIWORD(lparam)};
      shell->exposure_webview->resize(bounds);
    }
    return 0;
  case WM_CLOSE:
    ShowWindow(window, SW_HIDE);
    if (shell != nullptr)
      shell->exposure_open = false;
    std::fill(runtime_state().preview_hovered.begin(),
              runtime_state().preview_hovered.end(), std::uint8_t{0});
    present_preview_view();
    if (owner != nullptr)
      sync_webview_snapshot(owner);
    return 0;
  case WM_DESTROY:
    if (shell != nullptr) {
      if (shell->exposure_webview != nullptr) {
        shell->exposure_webview->close();
        shell->exposure_webview.reset();
      }
      shell->exposure_window = nullptr;
      shell->exposure_open = false;
    }
    return 0;
  default:
    return DefWindowProcW(window, message, wparam, lparam);
  }
}

void schedule_validation(
    HWND window, bool discard_preview = true,
    GuiValidationPurpose purpose = GuiValidationPurpose::preview);
void confirm_render(HWND window);
void open_webview_overwrite_output(GuiShellState &shell,
                                   std::vector<std::string> paths);
bool open_webview_delete_session(GuiShellState &shell, std::size_t index);

void reset_session_for_game_change(const HWND window) {
  auto *const shell = shell_state(window);
  if (shell == nullptr)
    return;
  pano::app::begin_gui_session_refresh(runtime_state().refresh_state);
  shell->selected_record.reset();
  shell->manual_session_path.clear();
  shell->workflow.session_selected = false;
  shell->workflow.validation_ready = false;
  invalidate_preview();
  pano::app::begin_gui_validation(runtime_state().validation_state);
  runtime_state().completed_validation.reset();
}

void apply_session_selection(const HWND window, const std::size_t index) {
  auto *const shell = shell_state(window);
  if (shell == nullptr || index >= runtime_state().refresh_state.records.size())
    return;
  if (shell->workflow.operation != pano::app::GuiOperation::idle ||
      runtime_state().preview_building) {
    set_status_text(L"Finish or cancel the active operation before changing sessions");
    return;
  }
  shell->selected_record = index;
  shell->manual_session_path.clear();
  shell->workflow.session_selected = true;
  const auto &record = runtime_state().refresh_state.records[index];
  if (!record.image_paths.empty()) {
    const auto parent =
        std::filesystem::u8path(record.image_paths.front()).parent_path();
    shell->screenshots_directory = utf8_to_wide(parent.u8string());
  }
  if (!record.session.session_id.empty()) {
    const auto stitched_name = pano::app::application_stitched_name(
        application_state().application_settings,
        wide_to_utf8(application_state().game_directory),
        record.session.session_id);
    if (stitched_name.has_value()) {
      shell->output_name = utf8_to_wide(*stitched_name);
      if (const auto format =
              pano::app::gui_output_format_from_filename(*stitched_name);
          format.has_value())
        shell->output_format = *format;
    } else {
      const std::string extension = shell->output_format == "jpeg"
                                        ? ".jpg"
                                        : "." + shell->output_format;
      shell->output_name =
          utf8_to_wide("panorama-" + record.session.session_id + extension);
    }
  }
  schedule_validation(window);
}

void apply_refresh_results() {
  std::vector<std::unique_ptr<RefreshResult>> results;
  {
    std::lock_guard<std::mutex> lock(runtime_state().refresh_mutex);
    results.swap(runtime_state().refresh_results);
  }
  for (auto &result : results) {
    if (!result->error.empty()) {
      if (result->generation == runtime_state().refresh_state.generation)
        report_application_error(L"Session discovery",
                                 utf8_to_wide(result->error));
      continue;
    }
    if (!pano::app::complete_gui_session_refresh(runtime_state().refresh_state,
                                                 result->generation,
                                                 std::move(result->records)))
      continue;
    const HWND window =
        runtime_state().refresh_window.load(std::memory_order_acquire);
    auto *const shell = window == nullptr ? nullptr : shell_state(window);
    if (shell == nullptr)
      continue;
    shell->selected_record.reset();
    shell->manual_session_path.clear();
    shell->workflow.session_selected = false;
    if (!runtime_state().refresh_state.records.empty())
      apply_session_selection(window, 0U);
    const std::wstring status =
        std::to_wstring(runtime_state().refresh_state.records.size()) +
        L" session(s) found";
    set_status_text(status);
  }
}

void start_refresh() {
  const std::string game_directory =
      wide_to_utf8(application_state().game_directory);
  if (game_directory.empty()) {
    set_status_text(L"Choose a game folder first");
    return;
  }
  const std::uint64_t generation =
      pano::app::begin_gui_session_refresh(runtime_state().refresh_state);
  set_status_text(L"Refreshing sessions...");
  try {
    reap_completed_workers();
    runtime_state().refresh_threads.emplace_back([generation, game_directory] {
      auto result = std::make_unique<RefreshResult>();
      result->generation = generation;
      pano::app::discover_gui_sessions(game_directory, result->records,
                                       result->error);
      {
        std::lock_guard<std::mutex> lock(runtime_state().refresh_mutex);
        runtime_state().refresh_results.push_back(std::move(result));
      }
      const HWND target =
          runtime_state().refresh_window.load(std::memory_order_acquire);
      if (target != nullptr)
        PostMessageW(target, refresh_complete_message, 0, 0);
    });
  } catch (const std::system_error &) {
    report_application_error(L"Session discovery",
                             L"Cannot start session refresh");
  }
}

void schedule_validation(const HWND window, const bool discard_preview,
                         const GuiValidationPurpose purpose) {
  if (discard_preview)
    invalidate_preview();
  pano::app::begin_gui_validation(runtime_state().validation_state);
  runtime_state().scheduled_validation = purpose;
  runtime_state().completed_validation.reset();
  if (auto *const shell = shell_state(window); shell != nullptr)
    shell->workflow.validation_ready = false;
  KillTimer(window, validation_timer_id);
  SetTimer(window, validation_timer_id, 300U, nullptr);
}

void schedule_preview_option_validation(const HWND window) {
  const bool preview_was_ready = retained_preview_ready();
  if (preview_was_ready)
    if (auto *const shell = shell_state(window); shell != nullptr)
      shell->rebuild_preview_after_validation = true;
  schedule_validation(window, !preview_was_ready);
}

void start_validation() {
  const GuiValidationPurpose purpose = runtime_state().scheduled_validation;
  pano::app::GuiRenderRequestState request;
  std::string error;
  if (!capture_gui_request(request, error, purpose)) {
    runtime_state().validation_state.error = error;
    if (auto *const shell = shell_state(
            runtime_state().refresh_window.load(std::memory_order_acquire));
        shell != nullptr)
      shell->pending_render_with_thumbnail.reset();
    set_status_text(utf8_to_wide(error));
    return;
  }
  pano::app::RenderOptions options;
  bool captured = false;
  if (purpose == GuiValidationPurpose::preview) {
    try {
      const std::string temporary_directory =
          (pano::app::webview_user_data_folder().parent_path() / L"Temp")
              .u8string();
      captured = pano::app::snapshot_gui_preview_request(
          request, temporary_directory, options, error);
    } catch (const std::exception &exception) {
      error = "cannot locate preview temporary directory: " +
              std::string(exception.what());
    }
  } else {
    captured = pano::app::snapshot_gui_render_request(request, options, error);
  }
  if (!captured) {
    runtime_state().validation_state.error = error;
    if (auto *const shell = shell_state(
            runtime_state().refresh_window.load(std::memory_order_acquire));
        shell != nullptr)
      shell->pending_render_with_thumbnail.reset();
    set_status_text(utf8_to_wide(error));
    return;
  }
  const std::uint64_t generation = runtime_state().validation_state.generation;
  set_status_text(L"Validating session...");
  try {
    reap_completed_workers();
    runtime_state().validation_threads.emplace_back(
        [generation, purpose, options = std::move(options)] {
          auto result = std::make_unique<ValidationResult>();
          result->generation = generation;
          result->purpose = purpose;
          pano::app::RenderPlan plan;
          if (pano::app::make_render_plan(options, plan, result->error))
            result->plan = std::move(plan);
          {
            std::lock_guard<std::mutex> lock(runtime_state().validation_mutex);
            runtime_state().validation_results.push_back(std::move(result));
          }
          const HWND target =
              runtime_state().refresh_window.load(std::memory_order_acquire);
          if (target != nullptr)
            PostMessageW(target, validation_complete_message, 0, 0);
        });
  } catch (const std::system_error &) {
    report_application_error(L"Validation", L"Cannot start validation");
  }
}

void apply_validation_results() {
  std::vector<std::unique_ptr<ValidationResult>> results;
  {
    std::lock_guard<std::mutex> lock(runtime_state().validation_mutex);
    results.swap(runtime_state().validation_results);
  }
  for (auto &result : results) {
    if (!pano::app::complete_gui_validation(
            runtime_state().validation_state, result->generation,
            std::move(result->plan), std::move(result->error)))
      continue;
    const HWND window =
        runtime_state().refresh_window.load(std::memory_order_acquire);
    auto *const shell = window == nullptr ? nullptr : shell_state(window);
    bool valid = runtime_state().validation_state.plan.has_value();
    runtime_state().completed_validation =
        valid ? std::optional<GuiValidationPurpose>(result->purpose)
              : std::nullopt;
    bool retained_update_failed = false;
    if (valid && retained_preview_ready() &&
        (shell == nullptr || !shell->rebuild_preview_after_validation)) {
      std::string update_error;
      const bool updated =
          runtime_state().active_preview_owner != nullptr
              ? pano::app::update_native_preview_render_plan(
                    runtime_state().active_preview_owner,
                    *runtime_state().validation_state.plan, update_error)
              : pano::app::update_cpu_native_preview_render_plan(
                    runtime_state().active_cpu_preview_owner,
                    *runtime_state().validation_state.plan, update_error);
      if (!updated) {
        discard_active_preview();
        valid = false;
        runtime_state().completed_validation.reset();
        retained_update_failed = true;
        report_application_error(L"Validation", utf8_to_wide(update_error));
      }
    }
    if (shell != nullptr)
      shell->workflow.validation_ready = valid;
    if (!retained_update_failed)
      set_status_text(
          valid ? L"Session and options are valid"
                : utf8_to_wide(runtime_state().validation_state.error));
    if (shell != nullptr && shell->rebuild_preview_after_validation) {
      shell->rebuild_preview_after_validation = false;
      if (valid) {
        navigate_stage(window, pano::app::GuiStage::preview);
        start_preview();
      }
    }
    if (shell != nullptr && shell->pending_render_with_thumbnail.has_value()) {
      shell->pending_render_with_thumbnail.reset();
      if (valid)
        confirm_render(window);
    }
  }
}

void report_render_progress(void *, const unsigned completed,
                            const unsigned total, const char *const phase) {
  {
    std::lock_guard<std::mutex> lock(runtime_state().render_progress_mutex);
    runtime_state().render_progress_phase = phase == nullptr ? "render" : phase;
  }
  runtime_state().render_progress_completed.store(completed,
                                                  std::memory_order_release);
  runtime_state().render_progress_total.store(total, std::memory_order_release);
  const HWND target =
      runtime_state().refresh_window.load(std::memory_order_acquire);
  if (target != nullptr)
    PostMessageW(target, render_progress_message, 0, 0);
}

void start_render() {
  if (!runtime_state().validation_state.plan.has_value() ||
      runtime_state().completed_validation != GuiValidationPurpose::output ||
      !retained_preview_ready() || runtime_state().preview_building)
    return;
  std::uint64_t operation_generation = 0;
  if (!begin_owned_operation(pano::app::GuiOperation::render,
                             operation_generation))
    return;
  pano_gpu_cancellation_token_destroy(&runtime_state().preview_cancellation);
  std::array<char, 512> gpu_error{};
  if (pano_gpu_cancellation_token_create(
          &runtime_state().preview_cancellation, gpu_error.data(),
          static_cast<uint32_t>(gpu_error.size())) != PANO_GPU_SUCCESS) {
    report_application_error(L"Render",
                             L"Cannot create render cancellation state");
    complete_owned_operation(operation_generation);
    return;
  }
  const std::uint64_t generation = ++runtime_state().preview_generation;
  auto result = std::make_unique<RenderResult>();
  result->generation = generation;
  result->operation_generation = operation_generation;
  result->session_id =
      runtime_state().validation_state.plan->session.session_id;
  result->panorama_path =
      runtime_state().validation_state.plan->outputs.panorama.final_path;
  result->cpu = runtime_state().active_cpu_preview_owner != nullptr;
  auto *const owner = runtime_state().active_preview_owner;
  auto *const cpu_owner = runtime_state().active_cpu_preview_owner;
  runtime_state().preview_building = true;
  set_cancel_enabled(true);
  update_exposure_enablement();
  runtime_state().render_progress_completed.store(0, std::memory_order_release);
  runtime_state().render_progress_total.store(0, std::memory_order_release);
  set_status_text(L"Rendering panorama...");
  try {
    reap_completed_workers();
    runtime_state().preview_threads.emplace_back(
        [owner, cpu_owner, result = std::move(result)]() mutable {
          pano::app::NativeRenderOptions options;
          options.gpu_cancellation = runtime_state().preview_cancellation;
          options.progress = report_render_progress;
          result->succeeded =
              result->cpu
                  ? pano::app::render_cpu_native_session(
                        cpu_owner, options, result->render, result->error)
                  : pano::app::render_native_session(
                        owner, options, result->render, result->error);
          {
            std::lock_guard<std::mutex> lock(runtime_state().preview_mutex);
            runtime_state().render_results.push_back(std::move(result));
          }
          const HWND target =
              runtime_state().refresh_window.load(std::memory_order_acquire);
          if (target != nullptr)
            PostMessageW(target, render_complete_message, 0, 0);
        });
  } catch (...) {
    runtime_state().preview_building = false;
    pano_gpu_cancellation_token_destroy(&runtime_state().preview_cancellation);
    set_cancel_enabled(false);
    update_exposure_enablement();
    complete_owned_operation(operation_generation);
    report_application_error(L"Render", L"Cannot start render worker");
  }
}

void apply_render_progress() {
  const unsigned completed =
      runtime_state().render_progress_completed.load(std::memory_order_acquire);
  const unsigned total =
      runtime_state().render_progress_total.load(std::memory_order_acquire);
  std::string phase;
  {
    std::lock_guard<std::mutex> lock(runtime_state().render_progress_mutex);
    phase = runtime_state().render_progress_phase;
  }
  const std::wstring label =
      phase == "contrast" || phase == "CPU contrast"
          ? L"Analyzing output contrast"
      : phase == "render" || phase == "CPU render"
          ? L"Rendering panorama"
      : phase == "thumbnail" ? L"Rendering thumbnail"
      : phase == "Preparing output" || phase == "Preparing CPU output"
          ? L"Preparing output"
      : phase == "Publishing output" || phase == "Publishing CPU output"
          ? L"Publishing output"
      : phase == "Output ready" || phase == "CPU output ready"
          ? L"Output ready"
          : utf8_to_wide(phase);
  const std::wstring status =
      label == L"Output ready" ? label : label + L"...";
  set_status_text(status);
  const HWND window =
      runtime_state().refresh_window.load(std::memory_order_acquire);
  if (window != nullptr)
    set_operation_title(window, label, completed, total);
  if (window != nullptr)
    update_operation_progress(window, completed, total);
}

bool present_retained_preview_if_visible() {
  const HWND window =
      runtime_state().refresh_window.load(std::memory_order_acquire);
  const auto *const shell = window == nullptr ? nullptr : shell_state(window);
  if (shell == nullptr || shell->workflow.stage != pano::app::GuiStage::preview)
    return true;
  return update_preview_surface() && present_preview_view();
}

void apply_render_results() {
  std::vector<std::unique_ptr<RenderResult>> results;
  {
    std::lock_guard<std::mutex> lock(runtime_state().preview_mutex);
    results.swap(runtime_state().render_results);
  }
  for (auto &result : results) {
    runtime_state().preview_building = false;
    complete_owned_operation(result->operation_generation);
    if (result->generation != runtime_state().preview_generation) {
      discard_active_preview();
      continue;
    }
    runtime_state().active_preview =
        result->cpu ? nullptr
                    : pano::app::native_preview_handle(
                          runtime_state().active_preview_owner);
    if (!result->succeeded) {
      const bool was_cancelled =
          result->error.find("cancel") != std::string::npos;
      if (was_cancelled)
        set_status_text(L"Render cancelled");
      else
        report_application_error(L"Render", utf8_to_wide(result->error));
      if (!present_retained_preview_if_visible())
        discard_active_preview();
      continue;
    }
    const std::wstring status =
        L"Published " + std::to_wstring(result->render.published_paths.size()) +
        L" output file(s)";
    set_status_text(status);
    if (!result->render.published_paths.empty()) {
      const auto published = std::find(result->render.published_paths.begin(),
                                       result->render.published_paths.end(),
                                       result->panorama_path);
      if (published == result->render.published_paths.end()) {
        report_application_error(L"Render",
                                 L"Published output omitted the panorama");
        continue;
      }
      const std::string output_name =
          std::filesystem::u8path(*published).filename().u8string();
      pano::app::mark_application_session_stitched(
          application_state().application_settings,
          wide_to_utf8(application_state().game_directory), result->session_id,
          output_name);
      save_gui_settings();
    }
    if (const HWND window =
            runtime_state().refresh_window.load(std::memory_order_acquire);
        window != nullptr)
      if (auto *const shell = shell_state(window); shell != nullptr)
        shell->final_render_complete = true;
    notify_operation_complete();
    if (!present_retained_preview_if_visible())
      discard_active_preview();
  }
  pano_gpu_cancellation_token_destroy(&runtime_state().preview_cancellation);
  set_cancel_enabled(false);
  update_exposure_enablement();
}

void confirm_render(const HWND window) {
  if (!runtime_state().validation_state.plan.has_value() ||
      runtime_state().completed_validation != GuiValidationPurpose::output)
    return;
  const auto paths = pano::app::gui_existing_output_paths(
      *runtime_state().validation_state.plan);
  if (!paths.empty()) {
    if (auto *const shell = shell_state(window); shell != nullptr)
      open_webview_overwrite_output(*shell, paths);
    return;
  }
  start_render();
}

void request_render(const HWND window, const bool with_thumbnail) {
  auto *const shell = shell_state(window);
  if (shell == nullptr || runtime_state().preview_building ||
      !retained_preview_ready())
    return;
  if (shell->output_directory.empty()) {
    shell->pending_render_with_thumbnail.reset();
    set_status_text(L"Choose an output directory");
    return;
  }
  shell->thumbnail = with_thumbnail;
  const bool plan_matches =
      runtime_state().validation_state.plan.has_value() &&
      runtime_state().completed_validation == GuiValidationPurpose::output &&
      runtime_state().validation_state.plan->outputs.thumbnail.has_value() ==
          with_thumbnail;
  if (plan_matches) {
    confirm_render(window);
    return;
  }
  shell->pending_render_with_thumbnail = with_thumbnail;
  schedule_validation(window, false, GuiValidationPurpose::output);
}

void confirm_delete_session(const HWND window) {
  if (runtime_state().preview_building)
    return;
  auto *const shell = shell_state(window);
  if (shell == nullptr || !shell->selected_record.has_value() ||
      *shell->selected_record >= runtime_state().refresh_state.records.size())
    return;
  open_webview_delete_session(*shell, *shell->selected_record);
}

std::wstring json_string(const std::wstring_view value) {
  constexpr wchar_t hexadecimal[] = L"0123456789abcdef";
  std::wstring result = L"\"";
  for (const wchar_t character : value) {
    switch (character) {
    case L'\"':
      result += L"\\\"";
      break;
    case L'\\':
      result += L"\\\\";
      break;
    case L'\b':
      result += L"\\b";
      break;
    case L'\f':
      result += L"\\f";
      break;
    case L'\n':
      result += L"\\n";
      break;
    case L'\r':
      result += L"\\r";
      break;
    case L'\t':
      result += L"\\t";
      break;
    default:
      if (character < 0x20) {
        result += L"\\u00";
        result += hexadecimal[(character >> 4) & 0x0f];
        result += hexadecimal[character & 0x0f];
      } else {
        result += character;
      }
    }
  }
  result += L'\"';
  return result;
}

const wchar_t *webview_modal_kind_name(const WebViewModalKind kind) noexcept {
  switch (kind) {
  case WebViewModalKind::none:
    return L"none";
  case WebViewModalKind::edit_tag:
    return L"edit-tag";
  case WebViewModalKind::input_options:
    return L"input-options";
  case WebViewModalKind::preview_options:
    return L"preview-options";
  case WebViewModalKind::app_settings:
    return L"app-settings";
  case WebViewModalKind::delete_session:
    return L"delete-session";
  case WebViewModalKind::overwrite_output:
    return L"overwrite-output";
  case WebViewModalKind::notice:
    return L"notice";
  }
  return L"none";
}

const wchar_t *
backend_name(const pano::app::GuiBackendDecision backend) noexcept {
  switch (backend) {
  case pano::app::GuiBackendDecision::d3d12:
    return L"d3d12";
  case pano::app::GuiBackendDecision::cpu_forced:
  case pano::app::GuiBackendDecision::cpu_fallback:
    return L"cpu";
  case pano::app::GuiBackendDecision::strict_d3d12_rejection:
    return L"strict-d3d12-rejection";
  case pano::app::GuiBackendDecision::unavailable:
    return L"none";
  }
  return L"none";
}

void hide_webview_preview(GuiShellState &shell) {
  ShowWindow(application_state().controls.preview_surface, SW_HIDE);
  if (!shell.webview_preview_visible)
    return;
  shell.webview_preview_visible = false;
  shell.preview_pointer.reset();
  pano::app::reset_gui_preview_view(runtime_state().preview_view);
}

void invalidate_webview_layout(GuiShellState &shell) {
  ++shell.webview_layout_generation;
  hide_webview_preview(shell);
}

void report_application_error(const std::wstring &title,
                              const std::wstring &message) {
  set_status_text(message);
  const HWND window =
      runtime_state().refresh_window.load(std::memory_order_acquire);
  auto *const shell = window == nullptr ? nullptr : shell_state(window);
  if (shell == nullptr || shell->webview == nullptr || shell->webview_failed ||
      (shell->webview_modal.kind != WebViewModalKind::none &&
       shell->webview_modal.kind != WebViewModalKind::notice))
    return;
  WebViewModalPayload payload;
  payload.title = title;
  payload.description = message;
  shell->webview_modal.kind = WebViewModalKind::notice;
  ++shell->webview_modal.generation;
  shell->webview_modal.dismissible = true;
  shell->webview_modal.payload = std::move(payload);
  invalidate_webview_layout(*shell);
  sync_webview_snapshot(window);
}

void validate_webview_edit_tag(WebViewModalPayload &payload) {
  const std::string value = wide_to_utf8(payload.value);
  const std::size_t characters = utf8_character_count(value);
  payload.characters_remaining =
      static_cast<unsigned>(characters >= 64U ? 0U : 64U - characters);
  payload.valid = !payload.read_only &&
                  (payload.value.empty() || !value.empty()) &&
                  characters <= 64U;
  if (payload.read_only)
    payload.error = L"Read-only while an operation is active";
  else if (!payload.valid)
    payload.error = L"Enter at most 64 characters";
  else
    payload.error.clear();
}

std::optional<unsigned>
parse_webview_gpu_memory_mib(const std::wstring &value) {
  const std::string text = wide_to_utf8(value);
  unsigned parsed = 0;
  std::string error;
  if (!pano::app::parse_application_gpu_memory_mib(text, parsed, error))
    return std::nullopt;
  return parsed;
}

void validate_webview_app_settings(WebViewModalPayload &payload) {
  payload.valid = !payload.read_only &&
                  parse_webview_gpu_memory_mib(payload.value).has_value();
  if (payload.read_only)
    payload.error = L"Read-only while an operation is active";
  else if (!payload.valid)
    payload.error = L"Enter a value from 1024 to 8192 MiB";
  else
    payload.error.clear();
}

bool open_webview_edit_tag(GuiShellState &shell, const std::size_t index) {
  if (index >= runtime_state().refresh_state.records.size())
    return false;
  const auto &record = runtime_state().refresh_state.records[index];
  WebViewModalPayload payload;
  payload.title = L"Session Tag";
  payload.session_index = index;
  payload.session_id = record.session.session_id;
  payload.game_directory = wide_to_utf8(shell.game_directory);
  payload.value = utf8_to_wide(pano::app::application_session_tag(
                                   application_state().application_settings,
                                   payload.game_directory, payload.session_id)
                                   .value_or(""));
  payload.read_only = shell.workflow.operation != pano::app::GuiOperation::idle;
  validate_webview_edit_tag(payload);
  shell.webview_modal.kind = WebViewModalKind::edit_tag;
  ++shell.webview_modal.generation;
  shell.webview_modal.dismissible = true;
  shell.webview_modal.payload = std::move(payload);
  invalidate_webview_layout(shell);
  return true;
}

void open_webview_input_options(GuiShellState &shell) {
  WebViewModalPayload payload;
  payload.title = L"Input Options";
  payload.checked = shell.allow_incomplete;
  payload.read_only = shell.workflow.operation != pano::app::GuiOperation::idle;
  payload.valid = !payload.read_only;
  if (payload.read_only)
    payload.error = L"Read-only while an operation is active";
  shell.webview_modal.kind = WebViewModalKind::input_options;
  ++shell.webview_modal.generation;
  shell.webview_modal.dismissible = true;
  shell.webview_modal.payload = std::move(payload);
  invalidate_webview_layout(shell);
}

void open_webview_preview_options(GuiShellState &shell) {
  WebViewModalPayload payload;
  payload.title = L"Preview Options";
  payload.value = shell.blend == "feather" ? L"feather" : L"hard";
  payload.checked = shell.auto_contrast;
  payload.read_only = shell.workflow.operation != pano::app::GuiOperation::idle;
  payload.valid = !payload.read_only;
  if (payload.read_only)
    payload.error = L"Read-only while an operation is active";
  shell.webview_modal.kind = WebViewModalKind::preview_options;
  ++shell.webview_modal.generation;
  shell.webview_modal.dismissible = true;
  shell.webview_modal.payload = std::move(payload);
  invalidate_webview_layout(shell);
}

void open_webview_app_settings(GuiShellState &shell) {
  WebViewModalPayload payload;
  payload.title = L"App Settings";
  payload.value = std::to_wstring(shell.gpu_memory_mib);
  payload.checked = shell.debug_coverage;
  payload.read_only = shell.workflow.operation != pano::app::GuiOperation::idle;
  validate_webview_app_settings(payload);
  shell.webview_modal.kind = WebViewModalKind::app_settings;
  ++shell.webview_modal.generation;
  shell.webview_modal.dismissible = true;
  shell.webview_modal.payload = std::move(payload);
  invalidate_webview_layout(shell);
}

std::wstring webview_target_summary(const std::wstring_view introduction,
                                    const std::vector<std::string> &paths) {
  std::wstring summary{introduction};
  for (const auto &path : paths)
    summary += L"\n" + utf8_to_wide(path);
  return summary;
}

void update_webview_delete_targets(WebViewModalPayload &payload) {
  if (!payload.session_index.has_value() ||
      *payload.session_index >= runtime_state().refresh_state.records.size()) {
    payload.valid = false;
    payload.error = L"The capture session is no longer available";
    return;
  }
  const auto &record =
      runtime_state().refresh_state.records[*payload.session_index];
  if (record.session.session_id != payload.session_id ||
      record.path != payload.record_path) {
    payload.valid = false;
    payload.error = L"The capture session changed; close and try again";
    return;
  }
  payload.target_paths =
      pano::app::application_deletion_targets(record, payload.checked);
  payload.description = webview_target_summary(
      L"The following files will be permanently deleted:",
      payload.target_paths);
  payload.valid = !payload.read_only;
  payload.error =
      payload.read_only ? L"Read-only while an operation is active" : L"";
}

bool open_webview_delete_session(GuiShellState &shell,
                                 const std::size_t index) {
  if (runtime_state().preview_building ||
      index >= runtime_state().refresh_state.records.size())
    return false;
  const auto &record = runtime_state().refresh_state.records[index];
  WebViewModalPayload payload;
  payload.title = L"Delete capture session";
  payload.session_index = index;
  payload.session_id = record.session.session_id;
  payload.record_path = record.path;
  payload.read_only = shell.workflow.operation != pano::app::GuiOperation::idle;
  update_webview_delete_targets(payload);
  shell.webview_modal.kind = WebViewModalKind::delete_session;
  ++shell.webview_modal.generation;
  shell.webview_modal.dismissible = true;
  shell.webview_modal.payload = std::move(payload);
  invalidate_webview_layout(shell);
  return true;
}

void open_webview_overwrite_output(GuiShellState &shell,
                                   std::vector<std::string> paths) {
  WebViewModalPayload payload;
  payload.title = L"Overwrite existing files?";
  payload.target_paths = std::move(paths);
  payload.description = webview_target_summary(
      L"The following output files already exist and will be replaced:",
      payload.target_paths);
  payload.read_only = shell.workflow.operation != pano::app::GuiOperation::idle;
  payload.valid = !payload.read_only;
  if (payload.read_only)
    payload.error = L"Read-only while an operation is active";
  shell.webview_modal.kind = WebViewModalKind::overwrite_output;
  ++shell.webview_modal.generation;
  shell.webview_modal.dismissible = true;
  shell.webview_modal.payload = std::move(payload);
  invalidate_webview_layout(shell);
}

bool submit_webview_delete_session(GuiShellState &shell, std::string &error) {
  WebViewModalPayload &payload = shell.webview_modal.payload;
  update_webview_delete_targets(payload);
  if (!payload.valid) {
    error = wide_to_utf8(payload.error);
    return false;
  }
  invalidate_preview();
  pano::app::DeletionResult result;
  if (!pano::app::delete_application_files(payload.target_paths, result, error))
    return false;
  const std::wstring status = L"Deleted " + std::to_wstring(result.deleted) +
                              L" file(s); " + std::to_wstring(result.missing) +
                              L" missing";
  set_status_text(status);
  start_refresh();
  return true;
}

bool submit_webview_overwrite_output(GuiShellState &shell, std::string &error) {
  if (!runtime_state().validation_state.plan.has_value()) {
    error = "render plan is no longer available";
    return false;
  }
  const auto paths = pano::app::gui_existing_output_paths(
      *runtime_state().validation_state.plan);
  if (paths != shell.webview_modal.payload.target_paths) {
    shell.webview_modal.payload.target_paths = paths;
    shell.webview_modal.payload.description = webview_target_summary(
        L"Output targets changed. Confirm the files that will be replaced:",
        paths);
    error = "output targets changed; review and confirm again";
    return false;
  }
  start_render();
  error.clear();
  return true;
}

bool save_webview_app_settings(const HWND window, GuiShellState &shell,
                               std::string &error) {
  const WebViewModalPayload &payload = shell.webview_modal.payload;
  const std::optional<unsigned> parsed =
      parse_webview_gpu_memory_mib(payload.value);
  if (shell.webview_modal.kind != WebViewModalKind::app_settings ||
      !payload.valid || !parsed.has_value()) {
    error = "Enter a value from 1024 to 8192 MiB";
    return false;
  }
  const unsigned old_value = shell.gpu_memory_mib;
  const bool preview_was_ready =
      runtime_state().active_preview_owner != nullptr;
  const bool budget_changed = old_value != *parsed;
  const bool decreased = *parsed < old_value;

  pano::app::ApplicationSettings updated =
      application_state().application_settings;
  updated.game_directory = wide_to_utf8(shell.game_directory);
  updated.image_directory = wide_to_utf8(shell.screenshots_directory);
  updated.output_directory = wide_to_utf8(shell.output_directory);
  updated.auto_contrast = shell.auto_contrast;
  updated.gpu_memory_mib = *parsed;
  updated.debug_coverage = payload.checked;
  if (application_state().persistence_enabled &&
      application_state().settings_loaded &&
      !application_state().application_settings_path.empty() &&
      !pano::app::save_application_settings(
          application_state().application_settings_path, updated, error))
    return false;

  application_state().application_settings = std::move(updated);
  shell.gpu_memory_mib = *parsed;
  shell.debug_coverage = payload.checked;
  if (budget_changed || runtime_state().validation_state.plan.has_value()) {
    if (decreased && preview_was_ready)
      shell.rebuild_preview_after_validation = true;
    schedule_validation(window, decreased);
  }
  error.clear();
  return true;
}

bool save_webview_edit_tag(GuiShellState &shell, std::string &error) {
  WebViewModalPayload &payload = shell.webview_modal.payload;
  if (shell.webview_modal.kind != WebViewModalKind::edit_tag ||
      !payload.valid || !payload.session_index.has_value() ||
      *payload.session_index >= runtime_state().refresh_state.records.size() ||
      runtime_state()
              .refresh_state.records[*payload.session_index]
              .session.session_id != payload.session_id) {
    error = "session tag target is no longer available";
    return false;
  }
  pano::app::ApplicationSettings updated =
      application_state().application_settings;
  updated.game_directory = wide_to_utf8(shell.game_directory);
  updated.image_directory = wide_to_utf8(shell.screenshots_directory);
  updated.output_directory = wide_to_utf8(shell.output_directory);
  updated.auto_contrast = shell.auto_contrast;
  std::optional<std::string> settings_path;
  if (application_state().persistence_enabled &&
      application_state().settings_loaded &&
      !application_state().application_settings_path.empty())
    settings_path = application_state().application_settings_path;
  const std::string tag = wide_to_utf8(payload.value);
  if (!pano::app::set_and_save_application_session_tag(
          updated, payload.game_directory, payload.session_id, tag,
          settings_path, error))
    return false;
  application_state().application_settings = std::move(updated);
  error.clear();
  return true;
}

void publish_webview_resize(const HWND window, GuiShellState &shell) {
  shell.webview_width_resize_dirty = false;
  ++shell.webview_layout_generation;
  sync_webview_snapshot(window);
}

void stop_webview_resize_timer(const HWND window, GuiShellState &shell) {
  if (shell.webview_resize_timer_active)
    KillTimer(window, webview_resize_timer_id);
  shell.webview_resize_timer_active = false;
}

void finish_webview_sizing(const HWND window, GuiShellState &shell) {
  stop_webview_resize_timer(window, shell);
  if (shell.webview_sizing_changed_width || shell.webview_width_resize_dirty)
    publish_webview_resize(window, shell);
  shell.webview_sizing_active = false;
  shell.webview_sizing_changed_width = false;
}

void apply_webview_preview_geometry(
    const HWND window, const pano::app::WebViewPreviewGeometry &geometry) {
  auto *const shell = shell_state(window);
  if (shell != nullptr && shell->webview_width_resize_dirty)
    return;
  if (shell == nullptr || shell->webview == nullptr ||
      geometry.layout_generation != shell->webview_layout_generation ||
      !geometry.visible ||
      shell->webview_modal.kind != WebViewModalKind::none ||
      shell->workflow.stage != pano::app::GuiStage::preview ||
      !retained_preview_ready()) {
    if (shell != nullptr)
      hide_webview_preview(*shell);
    return;
  }
  const double scale = shell->webview->rasterization_scale();
  RECT client{};
  GetClientRect(window, &client);
  RECT preview_bounds{};
  std::string error;
  if (!pano::app::calculate_webview_preview_bounds(geometry, scale,
                                                   client.right, client.bottom,
                                                   preview_bounds, error)) {
    hide_webview_preview(*shell);
    return;
  }
  if (!SetWindowPos(application_state().controls.preview_surface, HWND_TOP,
                    preview_bounds.left, preview_bounds.top,
                    preview_bounds.right - preview_bounds.left,
                    preview_bounds.bottom - preview_bounds.top,
                    SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
    hide_webview_preview(*shell);
    report_application_error(L"Preview",
                             L"Cannot position native preview surface");
    return;
  }
  shell->webview_preview_visible = true;
  if (!update_preview_surface() || !present_preview_view()) {
    hide_webview_preview(*shell);
    report_application_error(L"Preview",
                             L"D3D12 preview surface is unavailable");
  }
}

std::optional<int> webview_outer_height(const HWND window,
                                        const double css_height) {
  const unsigned dpi = GetDpiForWindow(window);
  const double scaled_height = css_height * static_cast<double>(dpi) / 96.0;
  if (!std::isfinite(scaled_height) || scaled_height <= 0.0 ||
      scaled_height > static_cast<double>(std::numeric_limits<LONG>::max()))
    return std::nullopt;

  RECT client{};
  if (!GetClientRect(window, &client))
    return std::nullopt;
  RECT desired_bounds{0, 0, std::max(1L, client.right),
                      static_cast<LONG>(std::lround(scaled_height))};
  const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
  const DWORD extended_style =
      static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
  if (!AdjustWindowRectExForDpi(&desired_bounds, style, FALSE, extended_style,
                                dpi))
    return std::nullopt;
  return static_cast<int>(desired_bounds.bottom - desired_bounds.top);
}

void apply_webview_content_height(const HWND window, const double css_height) {
  auto *const shell = shell_state(window);
  if (shell == nullptr || shell->webview == nullptr)
    return;
  if (shell->webview_maximized)
    return;
  const std::optional<int> outer_height =
      webview_outer_height(window, css_height);
  if (!outer_height.has_value())
    return;
  shell->webview_content_height = css_height;

  const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitor_info{sizeof(monitor_info)};
  if (!GetMonitorInfoW(monitor, &monitor_info))
    return;
  const int work_height = monitor_info.rcWork.bottom - monitor_info.rcWork.top;
  const int desired_height = std::min(*outer_height, work_height);
  RECT window_bounds{};
  if (!GetWindowRect(window, &window_bounds))
    return;
  const int y =
      std::clamp(window_bounds.top, monitor_info.rcWork.top,
                 std::max(monitor_info.rcWork.top,
                          monitor_info.rcWork.bottom - desired_height));
  if (window_bounds.bottom - window_bounds.top == desired_height &&
      window_bounds.top == y)
    return;
  SetWindowPos(window, nullptr, window_bounds.left, y,
               window_bounds.right - window_bounds.left, desired_height,
               SWP_NOACTIVATE | SWP_NOZORDER);
}

bool preserve_webview_height_during_width_sizing(const HWND window,
                                                 const WPARAM sizing_edge,
                                                 RECT &proposed_bounds) {
  if (sizing_edge != WMSZ_LEFT && sizing_edge != WMSZ_RIGHT)
    return false;
  RECT current_bounds{};
  if (!GetWindowRect(window, &current_bounds))
    return false;
  proposed_bounds.bottom =
      proposed_bounds.top + current_bounds.bottom - current_bounds.top;
  return true;
}

bool current_output_dimensions(GuiShellState &shell, unsigned &width,
                               unsigned &height) {
  std::string error;
  unsigned updated_width = 0U;
  unsigned updated_height = 0U;
  const bool updated = runtime_state().active_preview_owner != nullptr
                           ? pano::app::query_native_render_dimensions(
                                 runtime_state().active_preview_owner,
                                 updated_width, updated_height, error)
                       : runtime_state().active_cpu_preview_owner != nullptr
                           ? pano::app::query_cpu_native_render_dimensions(
                                 runtime_state().active_cpu_preview_owner,
                                 updated_width, updated_height, error)
                           : false;
  if (updated) {
    shell.output_width = updated_width;
    shell.output_height = updated_height;
  }
  unsigned maximum_width = 0U;
  const bool maximum_updated =
      runtime_state().active_preview_owner != nullptr
          ? pano::app::query_native_maximum_render_width(
                runtime_state().active_preview_owner, maximum_width, error)
      : runtime_state().active_cpu_preview_owner != nullptr
          ? pano::app::query_cpu_native_maximum_render_width(
                runtime_state().active_cpu_preview_owner, maximum_width, error)
          : false;
  if (maximum_updated)
    shell.output_maximum_width = maximum_width;
  width = shell.output_width;
  height = shell.output_height;
  return width != 0U && height != 0U;
}

const wchar_t *session_status_name(const pano::app::GuiSessionStatus status) {
  switch (status) {
  case pano::app::GuiSessionStatus::complete:
    return L"complete";
  case pano::app::GuiSessionStatus::incomplete:
    return L"incomplete";
  case pano::app::GuiSessionStatus::invalid:
    return L"invalid";
  case pano::app::GuiSessionStatus::stitched:
    return L"stitched";
  }
  return L"unknown";
}

void sync_webview_snapshot(const HWND window) {
  auto *const shell = shell_state(window);
  if (shell == nullptr || shell->webview == nullptr || !shell->webview->ready())
    return;
  if (!shell->webview_snapshot_stage.has_value() ||
      *shell->webview_snapshot_stage != shell->workflow.stage) {
    shell->webview_snapshot_stage = shell->workflow.stage;
    invalidate_webview_layout(*shell);
  }
  const Controls &controls = application_state().controls;
  if (shell->webview_modal.kind != WebViewModalKind::none)
    hide_webview_preview(*shell);
  if (!shell->webview_preview_visible)
    ShowWindow(controls.preview_surface, SW_HIDE);
  const std::string game_directory = wide_to_utf8(shell->game_directory);
  std::wstring output_summary = L"0x0 (0MP)";
  unsigned output_width = 0U;
  unsigned output_height = 0U;
  if (current_output_dimensions(*shell, output_width, output_height)) {
    const std::uint64_t width = output_width;
    const std::uint64_t height = output_height;
    output_summary =
        std::to_wstring(width) + L"x" + std::to_wstring(height) + L" (" +
        std::to_wstring((width * height + 500000U) / 1000000U) + L"MP)";
  }
  const auto presentation = gui_presentation(*shell);
  std::wostringstream json;
  json << L"{\"version\":1,\"kind\":\"snapshot\",\"pageGeneration\":"
       << shell->webview->page_generation() << L",\"layoutGeneration\":"
       << shell->webview_layout_generation << L",\"maximized\":"
       << (shell->webview_maximized ? L"true" : L"false") << L",\"stage\":"
       << json_string(
              shell->workflow.stage == pano::app::GuiStage::preview ? L"preview"
              : shell->workflow.stage == pano::app::GuiStage::output ? L"output"
                                                                     : L"input")
       << L",\"gameDirectory\":" << json_string(shell->game_directory)
       << L",\"screenshotsDirectory\":"
       << json_string(shell->screenshots_directory) << L",\"selectedIndex\":";
  if (shell->selected_record.has_value())
    json << *shell->selected_record;
  else
    json << L"null";
  json << L",\"status\":" << json_string(shell->status_text) << L",\"backend\":"
       << json_string(backend_name(shell->backend)) << L",\"backendReason\":"
       << json_string(shell->backend_reason) << L",\"busy\":"
       << (presentation.busy ? L"true" : L"false") << L",\"previewEnabled\":"
       << (presentation.preview_enabled ? L"true" : L"false")
       << L",\"previewReady\":"
       << (presentation.preview_ready ? L"true" : L"false")
       << L",\"exposureOpen\":" << (shell->exposure_open ? L"true" : L"false")
       << L",\"exposureAdjusted\":"
       << (shell->exposure_edits_applied || shell->final_exposure_ev != 0.0
               ? L"true"
               : L"false")
       << L",\"previewProgress\":" << presentation.preview_progress
       << L",\"previewMessage\":"
       << json_string(presentation.busy && !presentation.rendering
                          ? shell->status_text
                          : L"No preview loaded")
       << L",\"outputDirectory\":" << json_string(shell->output_directory)
       << L",\"outputName\":" << json_string(shell->output_name)
       << L",\"resolutionPixels\":"
       << (shell->resolution_pixels ? L"true" : L"false")
       << L",\"resolutionPercent\":" << json_string(shell->resolution_percent)
       << L",\"outputWidth\":" << json_string(shell->explicit_width)
       << L",\"outputMaximumWidth\":" << shell->output_maximum_width
       << L",\"outputSummary\":" << json_string(output_summary)
       << L",\"outputFormat\":"
       << json_string(utf8_to_wide(shell->output_format))
       << L",\"jpegQuality\":" << json_string(shell->jpeg_quality)
       << L",\"renderEnabled\":"
       << (presentation.render_enabled ? L"true" : L"false")
       << L",\"rendering\":" << (presentation.rendering ? L"true" : L"false")
       << L",\"outputProgress\":" << presentation.output_progress
       << L",\"outputComplete\":"
       << (presentation.output_complete ? L"true" : L"false") << L",\"modal\":";
  if (shell->webview_modal.kind == WebViewModalKind::none) {
    json << L"null";
  } else {
    json << L"{\"generation\":" << shell->webview_modal.generation
         << L",\"kind\":"
         << json_string(webview_modal_kind_name(shell->webview_modal.kind))
         << L",\"dismissible\":"
         << (shell->webview_modal.dismissible ? L"true" : L"false")
         << L",\"payload\":{\"title\":"
         << json_string(shell->webview_modal.payload.title)
         << L",\"description\":"
         << json_string(shell->webview_modal.payload.description)
         << L",\"value\":" << json_string(shell->webview_modal.payload.value)
         << L",\"error\":" << json_string(shell->webview_modal.payload.error)
         << L",\"charactersRemaining\":"
         << shell->webview_modal.payload.characters_remaining
         << L",\"canSubmit\":"
         << (shell->webview_modal.payload.valid ? L"true" : L"false")
         << L",\"readOnly\":"
         << (shell->webview_modal.payload.read_only ? L"true" : L"false")
         << L",\"checked\":"
         << (shell->webview_modal.payload.checked ? L"true" : L"false")
         << L"}}";
  }
  json << L",\"sessions\":[";
  for (std::size_t index = 0;
       index < runtime_state().refresh_state.records.size(); ++index) {
    if (index != 0U)
      json << L',';
    const auto &record = runtime_state().refresh_state.records[index];
    const bool stitched = pano::app::application_stitched_name(
                              application_state().application_settings,
                              game_directory, record.session.session_id)
                              .has_value();
    const pano::app::GuiSessionStatus status =
        pano::app::gui_session_status(record, stitched);
    const auto tag = pano::app::application_session_tag(
        application_state().application_settings, game_directory,
        record.session.session_id);
    json << L"{\"name\":"
         << json_string(utf8_to_wide(
                pano::app::gui_session_local_label(record.session.session_id)))
         << L",\"poses\":" << record.session.frames.size() << L",\"tag\":"
         << json_string(utf8_to_wide(tag.value_or(""))) << L",\"status\":"
         << json_string(session_status_name(status)) << L",\"detail\":"
         << json_string(utf8_to_wide(record.error))
         << L",\"hasCoordinates\":"
         << (record.session.location.has_value() ? L"true" : L"false") << L'}';
  }
  json << L"]}";
  std::wstring error;
  if (!shell->webview->post_snapshot(json.str(), error) && !error.empty())
    set_status_text(error);
  sync_exposure_webview_snapshot(window);
}

void sync_exposure_webview_snapshot(const HWND window) {
  auto *const shell = shell_state(window);
  if (shell == nullptr || shell->exposure_webview == nullptr ||
      !shell->exposure_webview->ready())
    return;
  std::wostringstream json;
  json << L"{\"version\":1,\"kind\":\"exposure-snapshot\",\"pageGeneration\":"
       << shell->exposure_webview->page_generation() << L",\"busy\":"
       << (runtime_state().preview_building ? L"true" : L"false")
       << L",\"overlay\":"
       << (shell->exposure_overlay_boundaries ? L"true" : L"false")
       << L",\"reference\":";
  if (runtime_state().exposure_target.has_value())
    json << *runtime_state().exposure_target;
  else
    json << L"null";
  json << L",\"selected\":[";
  for (std::size_t index = 0; index < runtime_state().exposure_selected.size();
       ++index) {
    if (index != 0U)
      json << L',';
    json << runtime_state().exposure_selected[index];
  }
  json << L"],\"poseCount\":" << runtime_state().preview_hovered.size()
       << L",\"finalExposure\":" << shell->final_exposure_ev << L'}';
  std::wstring error;
  if (!shell->exposure_webview->post_snapshot(json.str(), error) &&
      !error.empty())
    set_status_text(error);
}

void handle_exposure_webview_command(const HWND window,
                                     const pano::app::WebViewCommand &command) {
  auto *const shell = shell_state(window);
  if (shell == nullptr)
    return;
  const auto valid_pose = [&](const std::optional<unsigned> pose) {
    return pose.has_value() && *pose < runtime_state().preview_hovered.size() &&
           !runtime_state().preview_building;
  };
  switch (command.kind) {
  case pano::app::WebViewCommandKind::ready:
    sync_exposure_webview_snapshot(window);
    return;
  case pano::app::WebViewCommandKind::content_size:
    if (command.content_height.has_value()) {
      shell->exposure_content_height = *command.content_height;
      position_exposure_panel(window);
    }
    return;
  case pano::app::WebViewCommandKind::set_exposure_overlay:
    if (command.enabled.has_value() && !runtime_state().preview_building)
      shell->exposure_overlay_boundaries = *command.enabled;
    break;
  case pano::app::WebViewCommandKind::hover_exposure_pose:
    if (valid_pose(command.pose_index)) {
      std::fill(runtime_state().preview_hovered.begin(),
                runtime_state().preview_hovered.end(), std::uint8_t{0});
      runtime_state().preview_hovered[*command.pose_index] = 1U;
    }
    present_preview_view();
    return;
  case pano::app::WebViewCommandKind::clear_exposure_hover:
    std::fill(runtime_state().preview_hovered.begin(),
              runtime_state().preview_hovered.end(), std::uint8_t{0});
    present_preview_view();
    return;
  case pano::app::WebViewCommandKind::set_exposure_reference:
    if (valid_pose(command.pose_index))
      set_exposure_reference_pose(*command.pose_index);
    break;
  case pano::app::WebViewCommandKind::toggle_exposure_selection:
    if (valid_pose(command.pose_index))
      toggle_exposure_manual_pose(*command.pose_index);
    break;
  case pano::app::WebViewCommandKind::set_final_exposure:
    if (command.exposure_ev.has_value() && !runtime_state().preview_building &&
        shell->final_exposure_ev != *command.exposure_ev) {
      shell->final_exposure_ev = *command.exposure_ev;
      if (runtime_state().validation_state.plan.has_value()) {
        runtime_state().validation_state.plan->final_exposure_ev =
            *command.exposure_ev;
        start_preview();
      }
    }
    break;
  case pano::app::WebViewCommandKind::reset_exposure:
    if (!runtime_state().preview_building) {
      const bool discard_gains = shell->exposure_edits_applied;
      runtime_state().exposure_target.reset();
      runtime_state().exposure_selected.clear();
      std::fill(runtime_state().preview_hovered.begin(),
                runtime_state().preview_hovered.end(), std::uint8_t{0});
      if (discard_gains)
        start_exposure(ExposureCommand::discard);
    }
    break;
  case pano::app::WebViewCommandKind::equalize_exposure:
    if (!runtime_state().preview_building &&
        runtime_state().exposure_target.has_value()) {
      start_exposure(runtime_state().exposure_selected.empty()
                         ? ExposureCommand::automatic
                         : ExposureCommand::manual_match);
    }
    break;
  case pano::app::WebViewCommandKind::host_failed:
    report_application_error(L"Exposure", command.value);
    return;
  default:
    return;
  }
  update_exposure_enablement();
  sync_exposure_webview_snapshot(window);
  sync_webview_snapshot(window);
  present_preview_view();
}

void handle_webview_command(const HWND window,
                            const pano::app::WebViewCommand &command) {
  auto *const shell = shell_state(window);
  if (shell == nullptr)
    return;
  switch (command.kind) {
  case pano::app::WebViewCommandKind::ready:
    break;
  case pano::app::WebViewCommandKind::set_game_directory:
    shell->game_directory = command.value;
    reset_session_for_game_change(window);
    break;
  case pano::app::WebViewCommandKind::set_screenshots_directory:
    shell->screenshots_directory = command.value;
    schedule_validation(window);
    break;
  case pano::app::WebViewCommandKind::set_output_directory:
    shell->output_directory = command.value;
    schedule_validation(window, false, GuiValidationPurpose::output);
    break;
  case pano::app::WebViewCommandKind::browse_game_directory:
    if (const auto path = choose_path(window, true)) {
      shell->game_directory = *path;
      reset_session_for_game_change(window);
      start_refresh();
    }
    break;
  case pano::app::WebViewCommandKind::browse_screenshots_directory:
    if (const auto path = choose_path(window, true)) {
      shell->screenshots_directory = *path;
      schedule_validation(window);
    }
    break;
  case pano::app::WebViewCommandKind::browse_output_directory:
    if (const auto path = choose_path(window, true)) {
      shell->output_directory = *path;
      schedule_validation(window, false, GuiValidationPurpose::output);
    }
    break;
  case pano::app::WebViewCommandKind::refresh:
    start_refresh();
    break;
  case pano::app::WebViewCommandKind::select_session:
    if (command.session_index.has_value())
      apply_session_selection(window, *command.session_index);
    break;
  case pano::app::WebViewCommandKind::copy_session_coordinates:
    if (command.session_index.has_value() &&
        *command.session_index < runtime_state().refresh_state.records.size()) {
      const auto coordinates = pano::app::gui_session_coordinates(
          runtime_state().refresh_state.records[*command.session_index].session);
      if (!coordinates.has_value()) {
        report_application_error(L"Copy coordinates",
                                 L"Session coordinates are unavailable");
      } else {
        std::wstring clipboard_error;
        if (copy_text_to_clipboard(window, utf8_to_wide(*coordinates),
                                   clipboard_error))
          set_status_text(L"Coordinates copied to clipboard");
        else
          report_application_error(L"Copy coordinates", clipboard_error);
      }
    }
    break;
  case pano::app::WebViewCommandKind::edit_tag:
    if (command.session_index.has_value())
      open_webview_edit_tag(*shell, *command.session_index);
    break;
  case pano::app::WebViewCommandKind::delete_session:
    if (command.session_index.has_value())
      open_webview_delete_session(*shell, *command.session_index);
    break;
  case pano::app::WebViewCommandKind::navigate_input:
    navigate_stage(window, pano::app::GuiStage::input);
    if (shell->workflow.operation == pano::app::GuiOperation::idle &&
        !runtime_state().completed_validation.has_value())
      schedule_validation(window, false, GuiValidationPurpose::preview);
    break;
  case pano::app::WebViewCommandKind::navigate_preview:
    navigate_stage(window, pano::app::GuiStage::preview);
    if (shell->workflow.operation == pano::app::GuiOperation::idle &&
        !runtime_state().completed_validation.has_value())
      schedule_validation(window, false, GuiValidationPurpose::preview);
    break;
  case pano::app::WebViewCommandKind::navigate_output:
    if (retained_preview_ready()) {
      navigate_stage(window, pano::app::GuiStage::output);
      if (shell->workflow.operation == pano::app::GuiOperation::idle &&
          runtime_state().completed_validation != GuiValidationPurpose::output)
        schedule_validation(window, false, GuiValidationPurpose::output);
    }
    break;
  case pano::app::WebViewCommandKind::set_output_name:
    shell->output_name = command.value;
    schedule_validation(window, false, GuiValidationPurpose::output);
    break;
  case pano::app::WebViewCommandKind::toggle_resolution_mode:
    if (!shell->resolution_pixels && shell->explicit_width.empty()) {
      unsigned width = 0U;
      unsigned height = 0U;
      if (current_output_dimensions(*shell, width, height))
        shell->explicit_width = std::to_wstring(width);
    }
    shell->resolution_pixels = !shell->resolution_pixels;
    schedule_validation(window, false, GuiValidationPurpose::output);
    break;
  case pano::app::WebViewCommandKind::set_resolution_percent:
    shell->resolution_percent = command.value;
    schedule_validation(window, false, GuiValidationPurpose::output);
    break;
  case pano::app::WebViewCommandKind::set_output_width:
    try {
      std::size_t consumed = 0U;
      const auto value = std::stoul(command.value, &consumed);
      if (consumed == command.value.size()) {
        shell->explicit_width = std::to_wstring(
            shell->output_maximum_width != 0U
                ? std::min<unsigned long>(value, shell->output_maximum_width)
                : value);
        schedule_validation(window, false, GuiValidationPurpose::output);
        break;
      }
    } catch (const std::exception &) {
    }
    shell->explicit_width = command.value;
    schedule_validation(window, false, GuiValidationPurpose::output);
    break;
  case pano::app::WebViewCommandKind::set_output_format:
    if (command.value == L"jpeg")
      select_output_format(window, 0);
    else if (command.value == L"png")
      select_output_format(window, 1);
    else if (command.value == L"exr")
      select_output_format(window, 2);
    break;
  case pano::app::WebViewCommandKind::set_jpeg_quality:
    shell->jpeg_quality = command.value;
    schedule_validation(window, false, GuiValidationPurpose::output);
    break;
  case pano::app::WebViewCommandKind::open_settings:
    open_webview_app_settings(*shell);
    break;
  case pano::app::WebViewCommandKind::open_options:
    if (shell->workflow.stage == pano::app::GuiStage::preview)
      open_webview_preview_options(*shell);
    else
      open_webview_input_options(*shell);
    break;
  case pano::app::WebViewCommandKind::open_exposure:
    show_exposure_panel(window);
    break;
  case pano::app::WebViewCommandKind::set_exposure_overlay:
  case pano::app::WebViewCommandKind::hover_exposure_pose:
  case pano::app::WebViewCommandKind::clear_exposure_hover:
  case pano::app::WebViewCommandKind::set_exposure_reference:
  case pano::app::WebViewCommandKind::toggle_exposure_selection:
  case pano::app::WebViewCommandKind::set_final_exposure:
  case pano::app::WebViewCommandKind::reset_exposure:
  case pano::app::WebViewCommandKind::equalize_exposure:
    return;
  case pano::app::WebViewCommandKind::abort_operation:
    cancel_active_operation(window);
    break;
  case pano::app::WebViewCommandKind::start_preview:
    if (runtime_state().validation_state.plan.has_value()) {
      navigate_stage(window, pano::app::GuiStage::preview);
      start_preview();
    }
    break;
  case pano::app::WebViewCommandKind::finalize_preview:
    if (retained_preview_ready()) {
      navigate_stage(window, pano::app::GuiStage::output);
      if (runtime_state().completed_validation != GuiValidationPurpose::output)
        schedule_validation(window, false, GuiValidationPurpose::output);
    }
    break;
  case pano::app::WebViewCommandKind::render:
    request_render(window, false);
    break;
  case pano::app::WebViewCommandKind::render_with_thumbnail:
    request_render(window, true);
    break;
  case pano::app::WebViewCommandKind::set_modal_value:
    if (pano::app::webview_modal_command_is_current(
            shell->webview_modal.kind != WebViewModalKind::none,
            shell->webview_modal.generation, command) &&
        !shell->webview_modal.payload.read_only) {
      if (shell->webview_modal.kind == WebViewModalKind::edit_tag) {
        shell->webview_modal.payload.value = command.value;
        validate_webview_edit_tag(shell->webview_modal.payload);
      } else if (shell->webview_modal.kind == WebViewModalKind::app_settings) {
        shell->webview_modal.payload.value = command.value;
        validate_webview_app_settings(shell->webview_modal.payload);
      } else if (shell->webview_modal.kind ==
                     WebViewModalKind::preview_options &&
                 (command.value == L"hard" || command.value == L"feather")) {
        shell->webview_modal.payload.value = command.value;
      }
    }
    break;
  case pano::app::WebViewCommandKind::set_modal_toggle:
    if (pano::app::webview_modal_command_is_current(
            shell->webview_modal.kind != WebViewModalKind::none,
            shell->webview_modal.generation, command) &&
        command.enabled.has_value() &&
        (shell->webview_modal.kind == WebViewModalKind::input_options ||
         shell->webview_modal.kind == WebViewModalKind::preview_options ||
         shell->webview_modal.kind == WebViewModalKind::app_settings ||
         shell->webview_modal.kind == WebViewModalKind::delete_session) &&
        !shell->webview_modal.payload.read_only)
      if (shell->webview_modal.kind == WebViewModalKind::delete_session) {
        shell->webview_modal.payload.checked = *command.enabled;
        update_webview_delete_targets(shell->webview_modal.payload);
      } else {
        shell->webview_modal.payload.checked = *command.enabled;
      }
    break;
  case pano::app::WebViewCommandKind::submit_modal:
    if (pano::app::webview_modal_command_is_current(
            shell->webview_modal.kind != WebViewModalKind::none,
            shell->webview_modal.generation, command)) {
      bool close = false;
      std::string error;
      if (shell->webview_modal.kind == WebViewModalKind::edit_tag) {
        close = save_webview_edit_tag(*shell, error);
      } else if (shell->webview_modal.kind == WebViewModalKind::app_settings) {
        close = save_webview_app_settings(window, *shell, error);
      } else if (shell->webview_modal.kind ==
                 WebViewModalKind::delete_session) {
        close = submit_webview_delete_session(*shell, error);
      } else if (shell->webview_modal.kind ==
                 WebViewModalKind::overwrite_output) {
        close = submit_webview_overwrite_output(*shell, error);
      } else if (shell->webview_modal.kind == WebViewModalKind::input_options &&
                 shell->webview_modal.payload.valid) {
        shell->allow_incomplete = shell->webview_modal.payload.checked;
        schedule_validation(window, false);
        close = true;
      } else if (shell->webview_modal.kind ==
                     WebViewModalKind::preview_options &&
                 shell->webview_modal.payload.valid) {
        const LRESULT blend =
            shell->webview_modal.payload.value == L"feather" ? 1 : 0;
        const bool contrast = shell->webview_modal.payload.checked;
        const std::string blend_value = blend == 1 ? "feather" : "hard";
        const bool changed =
            blend_value != shell->blend || contrast != shell->auto_contrast;
        shell->blend = blend_value;
        shell->auto_contrast = contrast;
        application_state().application_settings.auto_contrast = contrast;
        save_gui_settings();
        if (changed)
          schedule_preview_option_validation(window);
        close = true;
      }
      if (close) {
        shell->webview_modal.kind = WebViewModalKind::none;
        shell->webview_modal.payload = {};
        invalidate_webview_layout(*shell);
      } else if (!error.empty()) {
        shell->webview_modal.payload.error = utf8_to_wide(error);
      }
    }
    break;
  case pano::app::WebViewCommandKind::dismiss_modal:
    if (pano::app::webview_modal_command_is_current(
            shell->webview_modal.kind != WebViewModalKind::none,
            shell->webview_modal.generation, command)) {
      shell->webview_modal.kind = WebViewModalKind::none;
      shell->webview_modal.payload = {};
      invalidate_webview_layout(*shell);
    }
    break;
  case pano::app::WebViewCommandKind::content_size:
    if (command.content_height.has_value() &&
        command.layout_generation == shell->webview_layout_generation &&
        !shell->webview_width_resize_dirty)
      apply_webview_content_height(window, *command.content_height);
    return;
  case pano::app::WebViewCommandKind::preview_geometry:
    if (command.preview.has_value())
      apply_webview_preview_geometry(window, *command.preview);
    return;
  case pano::app::WebViewCommandKind::host_failed:
    if (!shell->webview_failed) {
      shell->webview_failed = true;
      MessageBoxW(window, command.value.c_str(), L"Panorama Stitcher",
                  MB_ICONERROR | MB_OK);
    }
    return;
  }
  sync_webview_snapshot(window);
}

LRESULT CALLBACK window_procedure(const HWND window, const UINT message,
                                  const WPARAM wparam, const LPARAM lparam) {
  switch (message) {
  case WM_NCCREATE: {
    const auto *const create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return TRUE;
  }
  case WM_CREATE: {
    if (!create_controls(window))
      return -1;
    apply_dark_caption(window);
    runtime_state().refresh_window.store(window, std::memory_order_release);
    load_gui_settings();
    if (!application_state().game_directory.empty())
      start_refresh();
    auto *const shell = shell_state(window);
    if (shell == nullptr || !pano::app::ensure_webview_runtime(window))
      return -1;
    shell->webview = std::make_unique<pano::app::WebViewHost>(
        window, [window](const pano::app::WebViewCommand &command) {
          handle_webview_command(window, command);
        });
    std::wstring error;
    if (!shell->webview->start(error)) {
      MessageBoxW(window, error.c_str(), L"Panorama Stitcher",
                  MB_ICONERROR | MB_OK);
      shell->webview.reset();
      return -1;
    }
    hide_webview_preview(*shell);
    return 0;
  }
  case WM_GETMINMAXINFO: {
    auto *const limits = reinterpret_cast<MINMAXINFO *>(lparam);
    const unsigned dpi = GetDpiForWindow(window);
    limits->ptMinTrackSize.x = MulDiv(920, static_cast<int>(dpi), 96);
    const auto *const shell = shell_state(window);
    int minimum_height = MulDiv(480, static_cast<int>(dpi), 96);
    if (shell != nullptr && !shell->webview_resize_timer_active &&
        shell->webview_content_height.has_value()) {
      const std::optional<int> content_height =
          webview_outer_height(window, *shell->webview_content_height);
      MONITORINFO monitor_info{sizeof(monitor_info)};
      const HMONITOR monitor =
          MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
      if (content_height.has_value() &&
          GetMonitorInfoW(monitor, &monitor_info)) {
        minimum_height = std::min(*content_height,
                                  static_cast<int>(monitor_info.rcWork.bottom -
                                                   monitor_info.rcWork.top));
      }
    }
    limits->ptMinTrackSize.y = minimum_height;
    return 0;
  }
  case WM_SIZING: {
    const auto *const shell = shell_state(window);
    auto *const proposed_bounds = reinterpret_cast<RECT *>(lparam);
    if (shell != nullptr && proposed_bounds != nullptr &&
        preserve_webview_height_during_width_sizing(window, wparam,
                                                    *proposed_bounds))
      return TRUE;
    break;
  }
  case WM_ENTERSIZEMOVE:
    if (auto *const shell = shell_state(window); shell != nullptr) {
      shell->webview_sizing_active = true;
      shell->webview_sizing_changed_width = false;
    }
    return 0;
  case WM_EXITSIZEMOVE:
    if (auto *const shell = shell_state(window); shell != nullptr)
      finish_webview_sizing(window, *shell);
    return 0;
  case WM_SIZE: {
    auto *const size_shell = shell_state(window);
    const int width = LOWORD(lparam);
    const int height = HIWORD(lparam);
    const bool width_changed =
        size_shell == nullptr || size_shell->window_width != width;
    if (size_shell != nullptr) {
      const bool maximized =
          wparam == SIZE_MAXIMIZED ||
          (wparam == SIZE_MINIMIZED && size_shell->webview_maximized);
      const bool maximized_changed = size_shell->webview_maximized != maximized;
      size_shell->webview_maximized = maximized;
      size_shell->window_width = width;
      size_shell->window_height = height;
      if (size_shell->webview != nullptr) {
        const RECT bounds{0, 0, width, height};
        size_shell->webview->resize(bounds);
        if (width_changed || maximized_changed) {
          if (size_shell->webview_sizing_active)
            size_shell->webview_sizing_changed_width = true;
          size_shell->webview_width_resize_dirty = true;
          if (!size_shell->webview_resize_timer_active) {
            size_shell->webview_resize_timer_active =
                SetTimer(window, webview_resize_timer_id,
                         webview_resize_interval_ms, nullptr) != 0U;
            publish_webview_resize(window, *size_shell);
          }
        }
      }
    }
    position_exposure_panel(window);
    return 0;
  }
  case WM_MOVE:
    position_exposure_panel(window);
    return 0;
  case WM_DPICHANGED: {
    const auto *suggested = reinterpret_cast<const RECT *>(lparam);
    SetWindowPos(window, nullptr, suggested->left, suggested->top,
                 suggested->right - suggested->left,
                 suggested->bottom - suggested->top,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    layout_controls(window);
    return 0;
  }
  case WM_CLOSE:
    DestroyWindow(window);
    return 0;
  case refresh_complete_message:
    apply_refresh_results();
    sync_webview_snapshot(window);
    return 0;
  case validation_complete_message:
    apply_validation_results();
    sync_webview_snapshot(window);
    return 0;
  case preview_complete_message:
    apply_preview_results();
    sync_webview_snapshot(window);
    return 0;
  case exposure_complete_message:
    apply_exposure_results();
    sync_webview_snapshot(window);
    return 0;
  case render_complete_message:
    apply_render_results();
    sync_webview_snapshot(window);
    return 0;
  case render_progress_message:
    apply_render_progress();
    sync_webview_snapshot(window);
    return 0;
  case preview_progress_message:
    apply_preview_progress();
    sync_webview_snapshot(window);
    return 0;
  case WM_TIMER:
    if (wparam == validation_timer_id) {
      KillTimer(window, validation_timer_id);
      start_validation();
      sync_webview_snapshot(window);
      return 0;
    }
    if (wparam == webview_resize_timer_id) {
      auto *const shell = shell_state(window);
      if (shell != nullptr && shell->webview_width_resize_dirty) {
        publish_webview_resize(window, *shell);
      } else {
        if (shell != nullptr)
          stop_webview_resize_timer(window, *shell);
        else
          KillTimer(window, webview_resize_timer_id);
      }
      return 0;
    }
    break;
  case WM_DESTROY:
    if (auto *const shell = shell_state(window); shell != nullptr) {
      stop_webview_resize_timer(window, *shell);
      shell->webview_width_resize_dirty = false;
      shell->webview_sizing_active = false;
      shell->webview_sizing_changed_width = false;
      if (shell->webview != nullptr) {
        shell->webview->close();
        shell->webview.reset();
      }
    }
    save_gui_settings();
    runtime_state().refresh_window.store(nullptr, std::memory_order_release);
    if (runtime_state().preview_cancellation != nullptr)
      pano_gpu_cancellation_token_cancel(runtime_state().preview_cancellation);
    KillTimer(window, validation_timer_id);
    for (auto &thread : runtime_state().refresh_threads)
      if (thread.joinable())
        thread.join();
    runtime_state().refresh_threads.clear();
    {
      std::lock_guard<std::mutex> lock(runtime_state().refresh_mutex);
      runtime_state().refresh_results.clear();
    }
    for (auto &thread : runtime_state().validation_threads)
      if (thread.joinable())
        thread.join();
    runtime_state().validation_threads.clear();
    {
      std::lock_guard<std::mutex> lock(runtime_state().validation_mutex);
      runtime_state().validation_results.clear();
    }
    for (auto &thread : runtime_state().preview_threads)
      if (thread.joinable())
        thread.join();
    runtime_state().preview_threads.clear();
    {
      std::lock_guard<std::mutex> lock(runtime_state().preview_mutex);
      runtime_state().preview_results.clear();
      runtime_state().exposure_results.clear();
      runtime_state().render_results.clear();
    }
    pano_gpu_preview_surface_destroy(&runtime_state().preview_surface);
    discard_active_preview();
    pano_gpu_cancellation_token_destroy(&runtime_state().preview_cancellation);
    pano_gpu_device_destroy(&runtime_state().preview_device);
    if (application_state().taskbar != nullptr) {
      application_state().taskbar->Release();
      application_state().taskbar = nullptr;
    }
    PostQuitMessage(0);
    return 0;
  default:
    return DefWindowProcW(window, message, wparam, lparam);
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

bool register_window_class(const HINSTANCE instance) {
  WNDCLASSEXW preview_descriptor{};
  preview_descriptor.cbSize = sizeof(preview_descriptor);
  preview_descriptor.lpfnWndProc = preview_window_procedure;
  preview_descriptor.hInstance = instance;
  preview_descriptor.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  preview_descriptor.hbrBackground = dark_background_brush();
  preview_descriptor.lpszClassName = preview_window_class;
  if (RegisterClassExW(&preview_descriptor) == 0)
    return false;
  WNDCLASSEXW exposure_descriptor{};
  exposure_descriptor.cbSize = sizeof(exposure_descriptor);
  exposure_descriptor.lpfnWndProc = exposure_window_procedure;
  exposure_descriptor.hInstance = instance;
  exposure_descriptor.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  exposure_descriptor.hbrBackground = dark_background_brush();
  exposure_descriptor.lpszClassName = exposure_window_class;
  if (RegisterClassExW(&exposure_descriptor) == 0) {
    UnregisterClassW(preview_window_class, instance);
    return false;
  }
  WNDCLASSEXW descriptor{};
  descriptor.cbSize = sizeof(descriptor);
  descriptor.style = CS_HREDRAW | CS_VREDRAW;
  descriptor.lpfnWndProc = window_procedure;
  descriptor.hInstance = instance;
  descriptor.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(PANO_APP_ICON));
  descriptor.hIconSm = static_cast<HICON>(LoadImageW(
      instance, MAKEINTRESOURCEW(PANO_APP_ICON), IMAGE_ICON,
      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
  descriptor.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  descriptor.hbrBackground = dark_background_brush();
  descriptor.lpszClassName = window_class;
  if (RegisterClassExW(&descriptor) != 0)
    return true;
  UnregisterClassW(exposure_window_class, instance);
  UnregisterClassW(preview_window_class, instance);
  return false;
}

int webview_self_test(const HINSTANCE instance, GuiShellState &shell,
                      const bool forced_cpu,
                      const std::filesystem::path &codec_fixture_directory) {
  application_state().self_test_allows_warp = true;
  HWND window = CreateWindowExW(
      WS_EX_CONTROLPARENT, window_class, L"Cyberpunk Panorama Stitcher",
      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 780, nullptr,
      nullptr, instance, &shell);
  if (window == nullptr)
    return 50;
  const auto fail = [window](const int code) {
    DestroyWindow(window);
    return code;
  };
  if (&application_state() != &shell || shell_state(window) != &shell ||
      shell.runtime == nullptr || shell.webview == nullptr ||
      application_state().controls.preview_surface == nullptr)
    return fail(51);
  const auto pump_until = [](const auto &condition) {
    const ULONGLONG deadline = GetTickCount64() + 10'000U;
    while (!condition()) {
      MSG message{};
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      if (GetTickCount64() >= deadline)
        return false;
      MsgWaitForMultipleObjects(0, nullptr, FALSE, 20U, QS_ALLINPUT);
    }
    return true;
  };
  if (!pump_until([&shell] { return shell.webview->ready(); }))
    return fail(52);
  pano::app::GuiRenderRequestState captured_request;
  std::string capture_error;
  if (!capture_gui_request(captured_request, capture_error) ||
      !captured_request.output_directory.empty() ||
      captured_request.gpu == forced_cpu ||
      (forced_cpu && (captured_request.gpu_memory_mib.has_value() ||
                      captured_request.gpu_strict)) ||
      (!forced_cpu && (!captured_request.gpu_memory_mib.has_value() ||
                       *captured_request.gpu_memory_mib == 0U)))
    return fail(62);
  shell.jpeg_quality = L"invalid";
  shell.resolution_percent = L"invalid";
  shell.explicit_width = L"invalid";
  pano::app::GuiRenderRequestState preview_request;
  if (!capture_gui_request(preview_request, capture_error,
                           GuiValidationPurpose::preview) ||
      preview_request.output_name != "preview.png" ||
      preview_request.resolution_percent != 100U ||
      preview_request.width.has_value() || preview_request.jpeg_quality != 95U)
    return fail(78);
  shell.output_format = "png";
  shell.resolution_percent = L"100";
  pano::app::GuiRenderRequestState png_request;
  if (!capture_gui_request(png_request, capture_error) ||
      png_request.jpeg_quality != 95U)
    return fail(79);
  shell.output_format = "jpeg";
  shell.jpeg_quality = L"95";
  shell.explicit_width.clear();

  const std::filesystem::path output_directory =
      std::filesystem::temp_directory_path() /
      (L"pano-app-webview-self-test-" + std::to_wstring(GetCurrentProcessId()) +
       L"-" + std::to_wstring(GetTickCount64()));
  std::error_code filesystem_error;
  if (codec_fixture_directory.empty() ||
      !std::filesystem::is_regular_file(codec_fixture_directory /
                                        L"rgb8-srgb.png") ||
      !std::filesystem::create_directory(output_directory, filesystem_error))
    return fail(53);
  struct TestOutputCleanup {
    std::filesystem::path directory;
    ~TestOutputCleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(directory, ignored);
    }
  } output_cleanup{output_directory};
  shell.output_directory = output_directory.wstring();

  pano::app::RenderPlan plan;
  plan.session.schema_version = 5U;
  plan.session.session_id = "webview-self-test";
  plan.session.capture_mode = "full_sphere";
  plan.session.horizontal_fov_deg = 90.0;
  plan.session.vertical_fov_deg = 90.0;
  plan.session.overlap_fraction = 0.25;
  plan.session.completed = true;
  plan.session.image_encoding = {"uint8", "srgb", "srgb", 100.0};
  pano::app::FrameSummary frame;
  frame.filename = (codec_fixture_directory / L"rgb8-srgb.png").u8string();
  frame.status = "captured";
  frame.camera_basis_row_major =
      std::array<double, 9>{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0};
  plan.session.frames = {frame, frame};
  plan.output_width = 8U;
  plan.output_height = 4U;
  plan.blend = "hard";
  plan.memory_mib = 2048U;
  plan.use_gpu = !forced_cpu;
  plan.allow_incomplete = true;
  plan.auto_contrast = false;
  plan.outputs.panorama.final_path =
      (output_directory / L"panorama.png").u8string();
  plan.outputs.coverage = pano::app::OutputTarget{
      (output_directory / L"panorama-coverage.png").u8string(), {}, false};
  plan.outputs.thumbnail = pano::app::OutputTarget{
      (output_directory / L"panorama-thumbnail.png").u8string(), {}, false};
  runtime_state().validation_state.plan = plan;
  shell.workflow.validation_ready = true;
  pano::app::GuiSessionRecord selection_record;
  selection_record.session = plan.session;
  selection_record.image_paths = {frame.filename};
  runtime_state().refresh_state.records = {selection_record, selection_record};
  pano::app::mark_application_session_stitched(
      application_state().application_settings,
      wide_to_utf8(application_state().game_directory), plan.session.session_id,
      "custom-history.png");
  apply_session_selection(window, 0U);
  if (shell.output_name != L"custom-history.png" ||
      shell.output_format != "png")
    return fail(65);
  if (!open_webview_edit_tag(shell, 0U) ||
      !shell.webview_modal.payload.description.empty())
    return fail(71);
  shell.webview_modal = {};
  open_webview_input_options(shell);
  if (!shell.webview_modal.payload.description.empty())
    return fail(72);
  shell.webview_modal = {};
  open_webview_preview_options(shell);
  if (!shell.webview_modal.payload.description.empty())
    return fail(73);
  shell.webview_modal = {};
  open_webview_app_settings(shell);
  if (!shell.webview_modal.payload.description.empty())
    return fail(74);
  shell.webview_modal = {};
  KillTimer(window, validation_timer_id);
  runtime_state().validation_state.plan = plan;
  runtime_state().completed_validation = GuiValidationPurpose::output;
  shell.workflow.validation_ready = true;
  sync_webview_snapshot(window);

  pano::app::WebViewCommand command;
  command.page_generation = shell.webview->page_generation();
  if (!forced_cpu) {
    runtime_state().validation_state.plan->gpu_memory_mib = 1U;
    command.kind = pano::app::WebViewCommandKind::start_preview;
    handle_webview_command(window, command);
    if (!pump_until([&shell] { return shell.workflow.preview_ready; }) ||
        runtime_state().active_cpu_preview_owner == nullptr ||
        shell.backend != pano::app::GuiBackendDecision::cpu_fallback)
      return fail(66);
    discard_active_preview();
    runtime_state().validation_state.plan = plan;
    shell.workflow.validation_ready = true;
  }
  command.kind = pano::app::WebViewCommandKind::start_preview;
  handle_webview_command(window, command);
  shell.selected_record = 0U;
  command.kind = pano::app::WebViewCommandKind::select_session;
  command.session_index = 1U;
  handle_webview_command(window, command);
  if (shell.selected_record != 0U)
    return fail(67);
  command.kind = pano::app::WebViewCommandKind::abort_operation;
  handle_webview_command(window, command);
  if (!pump_until([] { return !runtime_state().preview_building; }) ||
      shell.workflow.operation != pano::app::GuiOperation::idle)
    return fail(54);
  discard_active_preview();

  MoveWindow(application_state().controls.preview_surface, 7, 11, 4, 2, FALSE);
  command.kind = pano::app::WebViewCommandKind::start_preview;
  handle_webview_command(window, command);
  if (!pump_until([&shell] { return shell.workflow.preview_ready; }))
    return fail(55);
  const bool correct_backend =
      forced_cpu
          ? runtime_state().active_cpu_preview_owner != nullptr &&
                runtime_state().active_preview_owner == nullptr &&
                runtime_state().preview_device == nullptr &&
                runtime_state().preview_surface == nullptr &&
                shell.backend == pano::app::GuiBackendDecision::cpu_forced
          : runtime_state().active_preview_owner != nullptr &&
                runtime_state().active_cpu_preview_owner == nullptr &&
                runtime_state().preview_device != nullptr &&
                runtime_state().preview_surface != nullptr &&
                shell.backend == pano::app::GuiBackendDecision::d3d12;
  if (!correct_backend || !present_preview_view())
    return fail(56);
  pano::app::WebViewPreviewGeometry stale_geometry;
  stale_geometry.layout_generation = shell.webview_layout_generation;
  stale_geometry.x = -1.0;
  stale_geometry.y = 0.0;
  stale_geometry.width = 64.0;
  stale_geometry.height = 32.0;
  stale_geometry.device_scale = shell.webview->rasterization_scale();
  stale_geometry.visible = true;
  const WebViewModalKind modal_kind = shell.webview_modal.kind;
  const std::uint64_t modal_generation = shell.webview_modal.generation;
  const std::wstring status_text = shell.status_text;
  shell.webview_width_resize_dirty = false;
  apply_webview_preview_geometry(window, stale_geometry);
  if (shell.webview_modal.kind != modal_kind ||
      shell.webview_modal.generation != modal_generation ||
      shell.status_text != status_text)
    return fail(78);
  if (!forced_cpu) {
    pano_gpu_preview_surface_diagnostics before_update{};
    before_update.size = sizeof(before_update);
    before_update.abi_version = PANO_GPU_ABI_VERSION;
    pano_gpu_preview_surface_diagnostics after_update = before_update;
    std::array<char, 512> surface_error{};
    auto *const active_preview = runtime_state().active_preview;
    runtime_state().active_preview = nullptr;
    const bool retained_update_preserved_surface =
        pano_gpu_preview_surface_query_diagnostics(
            runtime_state().preview_surface, &before_update,
            surface_error.data(),
            static_cast<std::uint32_t>(surface_error.size())) ==
            PANO_GPU_SUCCESS &&
        update_preview_surface() &&
        pano_gpu_preview_surface_query_diagnostics(
            runtime_state().preview_surface, &after_update,
            surface_error.data(),
            static_cast<std::uint32_t>(surface_error.size())) ==
            PANO_GPU_SUCCESS &&
        after_update.present_count == before_update.present_count;
    runtime_state().active_preview = active_preview;
    if (!retained_update_preserved_surface)
      return fail(77);
  }
  if (runtime_state().preview_hovered.size() != 2U ||
      !set_exposure_reference_pose(0U))
    return fail(68);
  start_exposure(ExposureCommand::automatic);
  if (!pump_until([] { return !runtime_state().preview_building; }) ||
      !retained_preview_ready() || !shell.workflow.preview_ready)
    return fail(69);
  shell.exposure_edits_applied = true;
  pano::app::WebViewCommand exposure_command;
  exposure_command.kind = pano::app::WebViewCommandKind::reset_exposure;
  handle_exposure_webview_command(window, exposure_command);
  if (!pump_until([] { return !runtime_state().preview_building; }) ||
      shell.exposure_edits_applied)
    return fail(70);
  exposure_command.kind = pano::app::WebViewCommandKind::set_final_exposure;
  exposure_command.exposure_ev = -1.6;
  handle_exposure_webview_command(window, exposure_command);
  if (shell.final_exposure_ev != -1.6 ||
      !runtime_state().validation_state.plan.has_value() ||
      runtime_state().validation_state.plan->final_exposure_ev != -1.6 ||
      !pump_until([] { return !runtime_state().preview_building; }) ||
      shell.final_exposure_ev != -1.6 || !retained_preview_ready() ||
      !shell.workflow.preview_ready)
    return fail(76);
  auto overlay_frames = std::move(runtime_state().preview_overlay_frames);
  SendMessageW(application_state().controls.preview_surface, WM_MOUSEMOVE, 0,
               MAKELPARAM(2, 1));
  const bool magnified_without_exposure_overlay =
      !runtime_state().preview_view.overview && present_preview_view();
  runtime_state().preview_overlay_frames = std::move(overlay_frames);
  if (!magnified_without_exposure_overlay)
    return fail(57);

  command.kind = pano::app::WebViewCommandKind::finalize_preview;
  handle_webview_command(window, command);
  command.kind = pano::app::WebViewCommandKind::render_with_thumbnail;
  handle_webview_command(window, command);
  if (!pump_until([&shell] { return shell.final_render_complete; }))
    return fail(58);
  pano::app::ImageInfo rendered_image;
  pano::app::CodecErrorCategory codec_category{};
  std::string error;
  if (shell.workflow.stage != pano::app::GuiStage::output ||
      runtime_state().preview_building ||
      shell.workflow.operation != pano::app::GuiOperation::idle ||
      !pano::app::inspect_image(plan.outputs.panorama.final_path,
                                rendered_image, codec_category, error) ||
      rendered_image.width != 8U || rendered_image.height != 4U ||
      !std::filesystem::is_regular_file(
          std::filesystem::u8path(plan.outputs.coverage->final_path)) ||
      !std::filesystem::is_regular_file(
          std::filesystem::u8path(plan.outputs.thumbnail->final_path)))
    return fail(59);
  runtime_state().render_progress_completed.store(2U,
                                                   std::memory_order_release);
  runtime_state().render_progress_total.store(4U, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(runtime_state().render_progress_mutex);
    runtime_state().render_progress_phase = "CPU render";
  }
  apply_render_progress();
  std::array<wchar_t, 256> rendered_title{};
  GetWindowTextW(window, rendered_title.data(),
                 static_cast<int>(rendered_title.size()));
  if (shell.status_text != L"Rendering panorama..." ||
      shell.status_text.find(L"2/4") != std::wstring::npos ||
      std::wstring_view(rendered_title.data()).find(L"2/4") ==
          std::wstring_view::npos)
    return fail(75);
  set_operation_title(window, L"");
  const auto stitched_name = pano::app::application_stitched_name(
      application_state().application_settings,
      wide_to_utf8(application_state().game_directory),
      plan.session.session_id);
  if (!stitched_name.has_value() || *stitched_name != "panorama.png")
    return fail(63);
  if (!pump_until([] {
        reap_completed_workers();
        return runtime_state().refresh_threads.empty() &&
               runtime_state().validation_threads.empty() &&
               runtime_state().preview_threads.empty();
      }))
    return fail(64);

  const BOOL first_close = DestroyWindow(window);
  if (first_close == FALSE)
    return 60;
  pano_gpu_diagnostics diagnostics{};
  diagnostics.size = sizeof(diagnostics);
  diagnostics.abi_version = PANO_GPU_ABI_VERSION;
  std::array<char, 512> gpu_error{};
  if (!runtime_state().refresh_threads.empty() ||
      !runtime_state().validation_threads.empty() ||
      !runtime_state().preview_threads.empty() ||
      !runtime_state().refresh_results.empty() ||
      !runtime_state().validation_results.empty() ||
      !runtime_state().preview_results.empty() ||
      !runtime_state().exposure_results.empty() ||
      !runtime_state().render_results.empty() ||
      runtime_state().refresh_window.load(std::memory_order_acquire) !=
          nullptr ||
      runtime_state().preview_cancellation != nullptr ||
      runtime_state().preview_device != nullptr ||
      runtime_state().preview_surface != nullptr ||
      runtime_state().active_preview != nullptr ||
      runtime_state().active_preview_owner != nullptr ||
      runtime_state().active_cpu_preview_owner != nullptr ||
      application_state().taskbar != nullptr ||
      pano_gpu_query_diagnostics(&diagnostics, gpu_error.data(),
                                 static_cast<uint32_t>(gpu_error.size())) !=
          PANO_GPU_SUCCESS ||
      diagnostics.live_device_count != 0U ||
      diagnostics.live_queue_count != 0U ||
      diagnostics.live_fence_count != 0U ||
      diagnostics.live_session_count != 0U ||
      diagnostics.live_output_count != 0U)
    return 61;
  return 0;
}

} // namespace

int WINAPI wWinMain(const HINSTANCE instance, HINSTANCE, PWSTR,
                    int show_command) {
  int argument_count = 0;
  wchar_t **const arguments =
      CommandLineToArgvW(GetCommandLineW(), &argument_count);
  if (arguments != nullptr && argument_count == 3 &&
      std::wstring(arguments[1]) == L"--verify-gpu-runtime") {
    const std::filesystem::path result_path(arguments[2]);
    LocalFree(arguments);
    return run_runtime_probe(result_path);
  }
  bool self_test_requested = false;
  bool no_gpu_requested = false;
  bool d3d12_debug_requested = false;
  std::filesystem::path self_test_codec_directory;
  for (int index = 1; arguments != nullptr && index < argument_count; ++index) {
    self_test_requested =
        self_test_requested || std::wstring(arguments[index]) == L"--self-test";
    no_gpu_requested =
        no_gpu_requested || std::wstring(arguments[index]) == L"--no-gpu";
    d3d12_debug_requested =
        d3d12_debug_requested ||
        std::wstring(arguments[index]) == L"--d3d12-debug";
    if (std::wstring(arguments[index]) == L"--self-test-codec-dir" &&
        index + 1 < argument_count)
      self_test_codec_directory = arguments[++index];
  }
  if (arguments != nullptr)
    LocalFree(arguments);
  std::filesystem::path d3d12_debug_log_path;
  if (d3d12_debug_requested) {
    std::wstring debug_error;
    if (!configure_d3d12_debug(d3d12_debug_log_path, debug_error)) {
      MessageBoxW(nullptr, debug_error.c_str(), L"D3D12 diagnostics",
                  MB_OK | MB_ICONERROR);
      return 25;
    }
  }
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const HRESULT com_result = CoInitializeEx(
      nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  if (FAILED(com_result))
    return 24;
  struct ComCleanup {
    ~ComCleanup() { CoUninitialize(); }
  } com_cleanup;
  if (!register_window_class(instance))
    return 22;
  GuiShellState shell;
  shell.runtime = std::make_unique<GuiRuntimeState>();
  shell.headless = self_test_requested;
  shell.use_gpu = !no_gpu_requested;
  shell.d3d12_debug = d3d12_debug_requested;
  shell.d3d12_debug_log_path = std::move(d3d12_debug_log_path);
  active_application_state = &shell;
  if (self_test_requested) {
    const int result = webview_self_test(instance, shell, no_gpu_requested,
                                         self_test_codec_directory);
    UnregisterClassW(window_class, instance);
    UnregisterClassW(preview_window_class, instance);
    UnregisterClassW(exposure_window_class, instance);
    active_application_state = nullptr;
    return result;
  }
  application_state().persistence_enabled = true;
  HWND window = CreateWindowExW(
      WS_EX_CONTROLPARENT, window_class, L"Cyberpunk Panorama Stitcher",
      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 780, nullptr,
      nullptr, instance, &shell);
  if (window == nullptr)
    return 23;
  layout_controls(window);
  ShowWindow(window, show_command);
  SetWindowTextW(window, L"Cyberpunk Panorama Stitcher");
  apply_dark_caption(window);
  UpdateWindow(window);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(window, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  UnregisterClassW(window_class, instance);
  UnregisterClassW(preview_window_class, instance);
  UnregisterClassW(exposure_window_class, instance);
  active_application_state = nullptr;
  return static_cast<int>(message.wParam);
}
