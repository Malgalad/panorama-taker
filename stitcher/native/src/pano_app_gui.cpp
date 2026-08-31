#define NOMINMAX
#include <windows.h>

#include <commctrl.h>
#include <initguid.h>
#include <oleacc.h>
#include <uiautomation.h>
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
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t window_class[] = L"PanoramaStitchNativeWindow";
constexpr wchar_t preview_window_class[] = L"PanoramaStitchPreviewWindow";
constexpr wchar_t modal_window_class[] = L"PanoramaStitchModalWindow";
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
constexpr int game_edit_id = 101;
constexpr int game_browse_id = 102;
constexpr int refresh_id = 103;
constexpr int session_combo_id = 105;
constexpr int image_edit_id = 107;
constexpr int image_browse_id = 108;
constexpr int output_edit_id = 109;
constexpr int output_browse_id = 110;
constexpr int output_name_id = 111;
constexpr int format_combo_id = 112;
constexpr int quality_edit_id = 113;
constexpr int resolution_edit_id = 114;
constexpr int width_edit_id = 115;
constexpr int blend_combo_id = 116;
constexpr int memory_edit_id = 117;
constexpr int workers_edit_id = 118;
constexpr int thumbnail_id = 119;
constexpr int coverage_id = 120;
constexpr int incomplete_id = 121;
constexpr int auto_contrast_id = 122;
constexpr int gpu_id = 123;
constexpr int gpu_strict_id = 124;
constexpr int preview_id = 125;
constexpr int render_id = 126;
constexpr int cancel_id = 127;
constexpr int automatic_exposure_id = 136;
constexpr int match_exposure_id = 137;
constexpr int discard_exposure_id = 138;
constexpr int delete_session_id = 139;
constexpr int delete_images_id = 140;
constexpr int input_stage_id = 141;
constexpr int preview_stage_id = 142;
constexpr int output_stage_id = 143;
constexpr int settings_id = 144;
constexpr int preview_next_id = 145;
constexpr int input_options_id = 146;
constexpr int render_thumbnail_id = 147;
constexpr int preview_options_id = 148;
constexpr int resolution_mode_id = 149;
constexpr int resolution_slider_id = 150;
constexpr int quality_slider_id = 151;
constexpr int format_jpeg_id = 152;
constexpr int format_png_id = 153;
constexpr int format_exr_id = 154;
constexpr int exposure_panel_id = 155;
constexpr int exposure_overlay_id = 156;
constexpr int exposure_select_target_id = 157;
constexpr int exposure_pose_id_base = 5000;
constexpr DWORD accessibility_object_id = static_cast<DWORD>(OBJID_CLIENT);
constexpr DWORD accessibility_self_id = static_cast<DWORD>(CHILDID_SELF);
constexpr int modal_value_id = 301;
constexpr int modal_check_id = 302;
constexpr int modal_ok_id = IDOK;
constexpr int modal_cancel_id = IDCANCEL;
constexpr wchar_t button_hot_property[] = L"PanoButtonHot";
constexpr wchar_t action_hot_property[] = L"PanoActionHot";
constexpr wchar_t action_pressed_property[] = L"PanoActionPressed";

struct Controls {
  HWND input_stage_button = nullptr;
  HWND preview_stage_button = nullptr;
  HWND output_stage_button = nullptr;
  HWND settings_button = nullptr;
  HWND input_options_button = nullptr;
  HWND preview_options_button = nullptr;
  HWND exposure_panel_button = nullptr;
  HWND resolution_mode_button = nullptr;
  HWND game_label = nullptr;
  HWND game_edit = nullptr;
  HWND game_browse_button = nullptr;
  HWND refresh_button = nullptr;
  HWND session_label = nullptr;
  HWND session_combo = nullptr;
  HWND session_header_session = nullptr;
  HWND session_header_poses = nullptr;
  HWND session_header_tag = nullptr;
  HWND session_header_actions = nullptr;
  HWND image_label = nullptr;
  HWND image_edit = nullptr;
  HWND image_browse_button = nullptr;
  HWND output_label = nullptr;
  HWND output_edit = nullptr;
  HWND output_browse_button = nullptr;
  HWND output_name_label = nullptr;
  HWND output_name_edit = nullptr;
  HWND format_label = nullptr;
  HWND format_combo = nullptr;
  HWND format_jpeg = nullptr;
  HWND format_png = nullptr;
  HWND format_exr = nullptr;
  HWND quality_label = nullptr;
  HWND quality_edit = nullptr;
  HWND quality_slider = nullptr;
  HWND resolution_label = nullptr;
  HWND resolution_edit = nullptr;
  HWND resolution_slider = nullptr;
  HWND width_label = nullptr;
  HWND width_edit = nullptr;
  HWND blend_label = nullptr;
  HWND blend_combo = nullptr;
  HWND memory_label = nullptr;
  HWND memory_edit = nullptr;
  HWND workers_label = nullptr;
  HWND workers_edit = nullptr;
  HWND thumbnail_check = nullptr;
  HWND coverage_check = nullptr;
  HWND incomplete_check = nullptr;
  HWND auto_contrast_check = nullptr;
  HWND gpu_check = nullptr;
  HWND gpu_strict_check = nullptr;
  HWND automatic_exposure_button = nullptr;
  HWND match_exposure_button = nullptr;
  HWND discard_exposure_button = nullptr;
  HWND delete_session_button = nullptr;
  HWND delete_images_check = nullptr;
  HWND preview_button = nullptr;
  HWND preview_next_button = nullptr;
  HWND render_button = nullptr;
  HWND render_thumbnail_button = nullptr;
  HWND cancel_button = nullptr;
  HWND status_label = nullptr;
  HWND operation_progress = nullptr;
  HWND preview_surface = nullptr;
};

struct GuiRuntimeState;

struct GuiShellState {
  Controls controls;
  IAccPropServices *accessibility_properties = nullptr;
  ITaskbarList3 *taskbar = nullptr;
  HFONT body_font = nullptr;
  HFONT heading_font = nullptr;
  pano::app::ApplicationSettings application_settings;
  std::string application_settings_path;
  bool persistence_enabled = false;
  bool settings_loaded = false;
  bool self_test_allows_warp = false;
  std::unique_ptr<GuiRuntimeState> runtime;
  std::unique_ptr<pano::app::WebViewHost> webview;
  std::unique_ptr<pano::app::WebViewHost> exposure_webview;
  bool webview_enabled = false;
  bool webview_failed = false;
  bool webview_preview_visible = false;
  bool webview_resize_timer_active = false;
  bool webview_width_resize_dirty = false;
  bool webview_sizing_active = false;
  bool webview_sizing_changed_width = false;
  std::uint64_t webview_layout_generation = 1U;
  std::optional<pano::app::GuiStage> webview_snapshot_stage;
  std::optional<double> webview_content_height;
  std::optional<double> exposure_content_height;
  pano::app::GuiWorkflowState workflow;
  std::optional<std::size_t> selected_record;
  std::string manual_session_path;
  bool rebuild_preview_after_validation = false;
  bool suppress_control_changes = false;
  bool resolution_pixels = false;
  std::optional<bool> pending_render_with_thumbnail;
  std::optional<POINT> preview_pointer;
  HWND exposure_window = nullptr;
  HWND exposure_overlay_check = nullptr;
  HWND exposure_select_target_button = nullptr;
  HWND exposure_automatic_button = nullptr;
  HWND exposure_match_button = nullptr;
  HWND exposure_discard_button = nullptr;
  std::vector<HWND> exposure_pose_buttons;
  bool exposure_selecting_target = false;
  bool exposure_overlay_boundaries = false;
  bool exposure_edits_applied = false;
  unsigned operation_progress_percent = 0U;
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
  std::optional<pano::app::RenderPlan> plan;
  std::string error;
};

struct PreviewResult {
  std::uint64_t generation = 0;
  std::uint64_t operation_generation = 0;
  pano::app::NativePreview *preview = nullptr;
  pano::app::CpuNativePreview *cpu_preview = nullptr;
  bool reused = false;
  bool succeeded = false;
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
  pano::app::NativeRenderResult render;
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

LRESULT CALLBACK preview_window_procedure(HWND window, UINT message,
                                          WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK exposure_window_procedure(HWND window, UINT message,
                                           WPARAM wparam, LPARAM lparam);
void layout_controls(HWND window);
bool retained_preview_ready();
void sync_webview_snapshot(HWND window);
void sync_exposure_webview_snapshot(HWND window);
void refresh_exposure_pose_labels();
void update_exposure_enablement();
void handle_exposure_webview_command(HWND window,
                                     const pano::app::WebViewCommand &command);
std::optional<int> webview_outer_height(HWND window, double css_height);

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

std::wstring window_text(const HWND window) {
  const int length = GetWindowTextLengthW(window);
  std::wstring result(static_cast<std::size_t>(std::max(length, 0)) + 1U,
                      L'\0');
  const int copied = GetWindowTextW(window, result.data(), length + 1);
  result.resize(static_cast<std::size_t>(std::max(copied, 0)));
  return result;
}

HBRUSH dark_background_brush() {
  static HBRUSH brush = CreateSolidBrush(RGB(3, 7, 18));
  return brush;
}

HBRUSH dark_field_brush() {
  static HBRUSH brush = CreateSolidBrush(RGB(17, 24, 39));
  return brush;
}

HBRUSH table_header_brush() {
  static HBRUSH brush = CreateSolidBrush(RGB(75, 85, 99));
  return brush;
}

HFONT create_ui_font(const unsigned dpi, const int points,
                     const int weight = FW_NORMAL) {
  return CreateFontW(-MulDiv(points, static_cast<int>(dpi), 72), 0, 0, 0,
                     weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void replace_ui_fonts(const HWND window) {
  const unsigned dpi = GetDpiForWindow(window);
  HFONT next_body = create_ui_font(dpi, 12);
  HFONT next_heading = create_ui_font(dpi, 12, FW_SEMIBOLD);
  if (next_body == nullptr || next_heading == nullptr) {
    if (next_body != nullptr)
      DeleteObject(next_body);
    if (next_heading != nullptr)
      DeleteObject(next_heading);
    return;
  }
  const std::array<HWND, 64> all{
      application_state().controls.input_stage_button,
      application_state().controls.preview_stage_button,
      application_state().controls.output_stage_button,
      application_state().controls.settings_button,
      application_state().controls.input_options_button,
      application_state().controls.preview_options_button,
      application_state().controls.exposure_panel_button,
      application_state().controls.resolution_mode_button,
      application_state().controls.game_label,
      application_state().controls.game_edit,
      application_state().controls.game_browse_button,
      application_state().controls.refresh_button,
      application_state().controls.session_label,
      application_state().controls.session_combo,
      application_state().controls.session_header_session,
      application_state().controls.session_header_poses,
      application_state().controls.session_header_tag,
      application_state().controls.session_header_actions,
      application_state().controls.image_label,
      application_state().controls.image_edit,
      application_state().controls.image_browse_button,
      application_state().controls.output_label,
      application_state().controls.output_edit,
      application_state().controls.output_browse_button,
      application_state().controls.output_name_label,
      application_state().controls.output_name_edit,
      application_state().controls.format_label,
      application_state().controls.format_combo,
      application_state().controls.format_jpeg,
      application_state().controls.format_png,
      application_state().controls.format_exr,
      application_state().controls.quality_label,
      application_state().controls.quality_edit,
      application_state().controls.quality_slider,
      application_state().controls.resolution_label,
      application_state().controls.resolution_edit,
      application_state().controls.resolution_slider,
      application_state().controls.width_label,
      application_state().controls.width_edit,
      application_state().controls.blend_label,
      application_state().controls.blend_combo,
      application_state().controls.memory_label,
      application_state().controls.memory_edit,
      application_state().controls.workers_label,
      application_state().controls.workers_edit,
      application_state().controls.thumbnail_check,
      application_state().controls.coverage_check,
      application_state().controls.incomplete_check,
      application_state().controls.auto_contrast_check,
      application_state().controls.gpu_check,
      application_state().controls.gpu_strict_check,
      application_state().controls.automatic_exposure_button,
      application_state().controls.match_exposure_button,
      application_state().controls.discard_exposure_button,
      application_state().controls.delete_session_button,
      application_state().controls.delete_images_check,
      application_state().controls.preview_button,
      application_state().controls.preview_next_button,
      application_state().controls.render_button,
      application_state().controls.render_thumbnail_button,
      application_state().controls.cancel_button,
      application_state().controls.status_label,
      application_state().controls.operation_progress,
      application_state().controls.preview_surface};
  for (const HWND control : all)
    if (control != nullptr)
      SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(next_body),
                   TRUE);
  for (const HWND heading :
       {application_state().controls.session_header_session,
        application_state().controls.session_header_poses,
        application_state().controls.session_header_tag,
        application_state().controls.session_header_actions})
    SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(next_heading),
                 TRUE);
  if (application_state().body_font != nullptr)
    DeleteObject(application_state().body_font);
  if (application_state().heading_font != nullptr)
    DeleteObject(application_state().heading_font);
  application_state().body_font = next_body;
  application_state().heading_font = next_heading;
}

void apply_system_dark_theme(const HWND window) {
  using SetWindowThemeFunction = HRESULT(WINAPI *)(HWND, LPCWSTR, LPCWSTR);
  HMODULE theme_module = LoadLibraryW(L"uxtheme.dll");
  const auto set_theme =
      theme_module == nullptr
          ? nullptr
          : reinterpret_cast<SetWindowThemeFunction>(
                GetProcAddress(theme_module, "SetWindowTheme"));
  if (set_theme != nullptr)
    set_theme(window, L"DarkMode_Explorer", nullptr);
  if (theme_module != nullptr)
    FreeLibrary(theme_module);
}

void disable_control_theme(const HWND window) {
  using SetWindowThemeFunction = HRESULT(WINAPI *)(HWND, LPCWSTR, LPCWSTR);
  HMODULE theme_module = LoadLibraryW(L"uxtheme.dll");
  const auto set_theme =
      theme_module == nullptr
          ? nullptr
          : reinterpret_cast<SetWindowThemeFunction>(
                GetProcAddress(theme_module, "SetWindowTheme"));
  if (set_theme != nullptr)
    set_theme(window, L"", L"");
  if (theme_module != nullptr)
    FreeLibrary(theme_module);
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

LRESULT dark_control_color(const UINT message, const WPARAM wparam) {
  HDC context = reinterpret_cast<HDC>(wparam);
  SetTextColor(context, RGB(232, 234, 236));
  if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
    SetBkColor(context, RGB(17, 24, 39));
    return reinterpret_cast<LRESULT>(dark_field_brush());
  }
  SetBkColor(context, RGB(3, 7, 18));
  return reinterpret_cast<LRESULT>(dark_background_brush());
}

bool output_format_selected(const int identifier) {
  const int selected_format = static_cast<int>(SendMessageW(
      application_state().controls.format_combo, CB_GETCURSEL, 0, 0));
  return (identifier == format_jpeg_id && selected_format == 0) ||
         (identifier == format_png_id && selected_format == 1) ||
         (identifier == format_exr_id && selected_format == 2);
}

bool button_is_hot(const HWND window) {
  return GetPropW(window, button_hot_property) != nullptr;
}

LRESULT CALLBACK button_window_procedure(const HWND window, const UINT message,
                                         const WPARAM wparam,
                                         const LPARAM lparam, UINT_PTR,
                                         DWORD_PTR) {
  if (message == WM_MOUSEMOVE && !button_is_hot(window)) {
    SetPropW(window, button_hot_property, reinterpret_cast<HANDLE>(1));
    TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
    TrackMouseEvent(&tracking);
    InvalidateRect(window, nullptr, FALSE);
  } else if (message == WM_MOUSELEAVE) {
    RemovePropW(window, button_hot_property);
    InvalidateRect(window, nullptr, FALSE);
  } else if (message == WM_NCDESTROY) {
    RemovePropW(window, button_hot_property);
    RemoveWindowSubclass(window, button_window_procedure, 2U);
  }
  const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
  if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
      message == WM_ENABLE || message == WM_SETFOCUS || message == WM_KILLFOCUS)
    InvalidateRect(window, nullptr, FALSE);
  return result;
}

void center_edit_text(const HWND window) {
  RECT bounds{};
  if (!GetClientRect(window, &bounds))
    return;
  HDC context = GetDC(window);
  if (context == nullptr)
    return;
  const HFONT font =
      reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
  const HGDIOBJ old_font =
      font == nullptr ? nullptr : SelectObject(context, font);
  TEXTMETRICW metrics{};
  GetTextMetricsW(context, &metrics);
  if (old_font != nullptr)
    SelectObject(context, old_font);
  ReleaseDC(window, context);
  const int horizontal = std::max(1, MulDiv(8, GetDpiForWindow(window), 96));
  const int text_height = metrics.tmHeight + metrics.tmExternalLeading;
  const int client_height = static_cast<int>(bounds.bottom);
  const int client_width = static_cast<int>(bounds.right);
  const int top = std::max(0, (client_height - text_height) / 2);
  RECT formatting{horizontal, top,
                  std::max(horizontal, client_width - horizontal),
                  std::min(client_height, top + text_height + 1)};
  SendMessageW(window, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&formatting));
  InvalidateRect(window, nullptr, FALSE);
}

int button_content_width(const HWND window, const int leading_width = 0) {
  const int dpi = static_cast<int>(GetDpiForWindow(window));
  const int padding = MulDiv(16, dpi, 96);
  const int gap = leading_width == 0 ? 0 : MulDiv(8, dpi, 96);
  std::array<wchar_t, 128> text{};
  GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
  HDC context = GetDC(window);
  if (context == nullptr)
    return 2 * padding + leading_width + gap;
  const HFONT font =
      reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
  const HGDIOBJ old_font =
      font == nullptr ? nullptr : SelectObject(context, font);
  SIZE text_size{};
  GetTextExtentPoint32W(context, text.data(), lstrlenW(text.data()),
                        &text_size);
  if (old_font != nullptr)
    SelectObject(context, old_font);
  ReleaseDC(window, context);
  return 2 * padding + leading_width + gap + text_size.cx;
}

int compact_button_content_width(const HWND window) {
  const int dpi = static_cast<int>(GetDpiForWindow(window));
  const int ordinary_width = button_content_width(window);
  return ordinary_width - 2 * MulDiv(8, dpi, 96);
}

int control_text_width(const HWND window) {
  std::array<wchar_t, 256> text{};
  GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
  HDC context = GetDC(window);
  if (context == nullptr)
    return 0;
  const HFONT font =
      reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
  const HGDIOBJ old_font =
      font == nullptr ? nullptr : SelectObject(context, font);
  SIZE size{};
  GetTextExtentPoint32W(context, text.data(), lstrlenW(text.data()), &size);
  if (old_font != nullptr)
    SelectObject(context, old_font);
  ReleaseDC(window, context);
  return size.cx;
}

HFONT create_options_icon_font(const HWND window) {
  return CreateFontW(-MulDiv(12, static_cast<int>(GetDpiForWindow(window)), 72),
                     0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, L"Segoe Fluent Icons");
}

int fluent_icon_width(const HWND window, const wchar_t glyph) {
  const int fallback =
      MulDiv(16, static_cast<int>(GetDpiForWindow(window)), 96);
  HDC context = GetDC(window);
  if (context == nullptr)
    return fallback;
  HFONT font = create_options_icon_font(window);
  const HGDIOBJ old_font =
      font == nullptr ? nullptr : SelectObject(context, font);
  SIZE size{};
  const bool measured =
      GetTextExtentPoint32W(context, &glyph, 1, &size) != FALSE;
  if (old_font != nullptr)
    SelectObject(context, old_font);
  if (font != nullptr)
    DeleteObject(font);
  ReleaseDC(window, context);
  return measured ? size.cx : fallback;
}

int options_icon_width(const HWND window) {
  return fluent_icon_width(window, L'\xE70F');
}

int action_button_width(const HWND window) {
  const int dpi = static_cast<int>(GetDpiForWindow(window));
  const int padding = MulDiv(8, dpi, 96);
  const int gap = MulDiv(8, dpi, 96);
  const int separator = std::max(1, MulDiv(1, dpi, 96));
  const int arrow = MulDiv(16, dpi, 96);
  HDC context = GetDC(window);
  if (context == nullptr)
    return 2 * padding + 2 * gap + separator + arrow;
  const HFONT font =
      reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
  const HGDIOBJ old_font =
      font == nullptr ? nullptr : SelectObject(context, font);
  SIZE text_size{};
  GetTextExtentPoint32W(context, L"Actions", 7, &text_size);
  if (old_font != nullptr)
    SelectObject(context, old_font);
  ReleaseDC(window, context);
  return 2 * padding + text_size.cx + 2 * gap + separator + arrow;
}

int table_cell_padding(const HWND window) {
  return MulDiv(8, static_cast<int>(GetDpiForWindow(window)), 96);
}

LRESULT CALLBACK edit_window_procedure(const HWND window, const UINT message,
                                       const WPARAM wparam, const LPARAM lparam,
                                       UINT_PTR, DWORD_PTR) {
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(window, edit_window_procedure, 3U);
    return DefSubclassProc(window, message, wparam, lparam);
  }
  if (message == WM_CHAR && (wparam == L'\r' || wparam == L'\n'))
    return 0;
  const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
  if (message == WM_SETTEXT) {
    const HWND parent = GetParent(window);
    if (parent != nullptr)
      SendMessageW(parent, WM_COMMAND,
                   MAKEWPARAM(GetDlgCtrlID(window), EN_CHANGE),
                   reinterpret_cast<LPARAM>(window));
  }
  if (message == WM_SIZE || message == WM_SETFONT)
    center_edit_text(window);
  return result;
}

std::size_t action_property_index(const HWND window,
                                  const wchar_t *const property) {
  return static_cast<std::size_t>(
      reinterpret_cast<UINT_PTR>(GetPropW(window, property)));
}

void set_action_property_index(const HWND window, const wchar_t *const property,
                               const std::size_t index) {
  if (index == 0U)
    RemovePropW(window, property);
  else
    SetPropW(window, property,
             reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(index)));
}

LRESULT CALLBACK session_list_window_procedure(const HWND window,
                                               const UINT message,
                                               const WPARAM wparam,
                                               const LPARAM lparam, UINT_PTR,
                                               DWORD_PTR) {
  if (message == WM_MOUSEMOVE) {
    LVHITTESTINFO hit{};
    hit.pt = {static_cast<short>(LOWORD(lparam)),
              static_cast<short>(HIWORD(lparam))};
    ListView_SubItemHitTest(window, &hit);
    std::size_t next = 0U;
    if (hit.iItem >= 0 && hit.iSubItem == 3) {
      RECT cell{};
      ListView_GetSubItemRect(window, hit.iItem, 3, LVIR_BOUNDS, &cell);
      const int button_width = action_button_width(window);
      const int button_height =
          MulDiv(31, static_cast<int>(GetDpiForWindow(window)), 96);
      const int cell_padding = table_cell_padding(window);
      RECT button{cell.left + cell_padding,
                  cell.top + (cell.bottom - cell.top - button_height) / 2,
                  std::min(cell.right - cell_padding,
                           cell.left + cell_padding + button_width),
                  0};
      button.bottom = button.top + button_height;
      if (PtInRect(&button, hit.pt) != FALSE)
        next = static_cast<std::size_t>(hit.iItem) + 1U;
    }
    if (action_property_index(window, action_hot_property) != next) {
      set_action_property_index(window, action_hot_property, next);
      InvalidateRect(window, nullptr, FALSE);
    }
    TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
    TrackMouseEvent(&tracking);
  } else if (message == WM_MOUSELEAVE) {
    RemovePropW(window, action_hot_property);
    RemovePropW(window, action_pressed_property);
    InvalidateRect(window, nullptr, FALSE);
  } else if (message == WM_LBUTTONDOWN) {
    set_action_property_index(
        window, action_pressed_property,
        action_property_index(window, action_hot_property));
    InvalidateRect(window, nullptr, FALSE);
  } else if (message == WM_LBUTTONUP || message == WM_CAPTURECHANGED) {
    RemovePropW(window, action_pressed_property);
    InvalidateRect(window, nullptr, FALSE);
  } else if (message == WM_NCDESTROY) {
    RemovePropW(window, action_hot_property);
    RemovePropW(window, action_pressed_property);
    RemoveWindowSubclass(window, session_list_window_procedure, 4U);
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

void fill_tab_progress(const HDC context, const RECT &bounds,
                       const unsigned percent, const int corner) {
  if (percent == 0U)
    return;
  const int width = bounds.right - bounds.left;
  const int filled = static_cast<int>(static_cast<std::int64_t>(width) *
                                      std::min(100U, percent) / 100);
  if (filled <= 0)
    return;
  const int saved = SaveDC(context);
  HRGN clip = CreateRoundRectRgn(bounds.left, bounds.top, bounds.right + 1,
                                 bounds.bottom + 1, corner, corner);
  SelectClipRgn(context, clip);
  constexpr int stripes = 32;
  for (int stripe = 0; stripe < stripes; ++stripe) {
    const int left = bounds.left + filled * stripe / stripes;
    const int right = bounds.left + filled * (stripe + 1) / stripes;
    if (right <= left)
      continue;
    const int red = 22 + (34 - 22) * stripe / (stripes - 1);
    const int green = 101 + (197 - 101) * stripe / (stripes - 1);
    const int blue = 52 + (94 - 52) * stripe / (stripes - 1);
    HBRUSH brush = CreateSolidBrush(RGB(red, green, blue));
    RECT stripe_bounds{left, bounds.top, right, bounds.bottom};
    FillRect(context, &stripe_bounds, brush);
    DeleteObject(brush);
  }
  SelectClipRgn(context, nullptr);
  DeleteObject(clip);
  RestoreDC(context, saved);
}

bool draw_workflow_button(const DRAWITEMSTRUCT &draw, const HWND window) {
  const int identifier = static_cast<int>(draw.CtlID);
  const bool stage = identifier == input_stage_id ||
                     identifier == preview_stage_id ||
                     identifier == output_stage_id;
  const bool primary =
      identifier == preview_id || identifier == preview_next_id ||
      identifier == render_id || identifier == render_thumbnail_id;
  const bool secondary =
      identifier == settings_id || identifier == input_options_id ||
      identifier == preview_options_id || identifier == resolution_mode_id ||
      identifier == cancel_id || identifier == game_browse_id ||
      identifier == image_browse_id || identifier == output_browse_id ||
      identifier == refresh_id || identifier == automatic_exposure_id ||
      identifier == match_exposure_id || identifier == discard_exposure_id ||
      identifier == exposure_panel_id ||
      identifier == exposure_select_target_id;
  const bool radio = identifier == format_jpeg_id ||
                     identifier == format_png_id || identifier == format_exr_id;
  if (!stage && !primary && !secondary && !radio)
    return false;
  const auto *const shell = shell_state(window);
  const bool active =
      shell != nullptr &&
      ((identifier == input_stage_id &&
        shell->workflow.stage == pano::app::GuiStage::input) ||
       (identifier == preview_stage_id &&
        shell->workflow.stage == pano::app::GuiStage::preview) ||
       (identifier == output_stage_id &&
        shell->workflow.stage == pano::app::GuiStage::output));
  const bool completed = shell != nullptr && identifier == preview_stage_id &&
                         shell->workflow.preview_ready;
  const bool operation_tab =
      shell != nullptr &&
      shell->workflow.operation != pano::app::GuiOperation::idle &&
      (((shell->workflow.operation == pano::app::GuiOperation::preview ||
         shell->workflow.operation == pano::app::GuiOperation::exposure) &&
        identifier == preview_stage_id) ||
       (shell->workflow.operation == pano::app::GuiOperation::render &&
        identifier == output_stage_id));
  const bool enabled = (draw.itemState & ODS_DISABLED) == 0U;
  const bool hot = enabled && button_is_hot(draw.hwndItem);
  const bool pressed = enabled && (draw.itemState & ODS_SELECTED) != 0U;
  const int dpi = static_cast<int>(GetDpiForWindow(draw.hwndItem));
  const auto scaled = [dpi](const int value) { return MulDiv(value, dpi, 96); };
  if (radio) {
    FillRect(draw.hDC, &draw.rcItem, dark_background_brush());
    const int diameter = scaled(16);
    const int top =
        draw.rcItem.top + (draw.rcItem.bottom - draw.rcItem.top - diameter) / 2;
    RECT circle{draw.rcItem.left + scaled(2), top,
                draw.rcItem.left + scaled(2) + diameter, top + diameter};
    HPEN radio_pen =
        CreatePen(PS_SOLID, 1, enabled ? RGB(185, 188, 191) : RGB(90, 93, 96));
    const HGDIOBJ old_radio_pen = SelectObject(draw.hDC, radio_pen);
    const HGDIOBJ old_radio_brush =
        SelectObject(draw.hDC, GetStockObject(NULL_BRUSH));
    Ellipse(draw.hDC, circle.left, circle.top, circle.right, circle.bottom);
    if (output_format_selected(identifier)) {
      HBRUSH selected = CreateSolidBrush(RGB(215, 235, 45));
      SelectObject(draw.hDC, selected);
      InflateRect(&circle, -4, -4);
      Ellipse(draw.hDC, circle.left, circle.top, circle.right, circle.bottom);
      SelectObject(draw.hDC, GetStockObject(NULL_BRUSH));
      DeleteObject(selected);
    }
    SelectObject(draw.hDC, old_radio_brush);
    SelectObject(draw.hDC, old_radio_pen);
    DeleteObject(radio_pen);
    std::array<wchar_t, 64> text{};
    GetWindowTextW(draw.hwndItem, text.data(), static_cast<int>(text.size()));
    RECT text_bounds = draw.rcItem;
    text_bounds.left += diameter + scaled(10);
    SetBkMode(draw.hDC, TRANSPARENT);
    SetTextColor(draw.hDC, enabled ? RGB(209, 213, 219) : RGB(120, 123, 126));
    DrawTextW(draw.hDC, text.data(), -1, &text_bounds,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    return true;
  }
  const COLORREF background = primary && enabled ? pressed ? RGB(251, 191, 36)
                                                   : hot   ? RGB(253, 230, 138)
                                                           : RGB(252, 211, 77)
                              : pressed ? RGB(17, 24, 39)
                              : hot              ? RGB(31, 41, 55)
                              : active           ? RGB(17, 24, 39)
                                                 : RGB(3, 7, 18);
  const COLORREF border = pressed                          ? background
                          : active || (primary && enabled) ? RGB(252, 211, 77)
                                                           : RGB(156, 163, 175);
  HBRUSH brush = CreateSolidBrush(background);
  HPEN pen = CreatePen(PS_SOLID, active ? 2 : 1, border);
  const HGDIOBJ old_brush = SelectObject(draw.hDC, brush);
  const HGDIOBJ old_pen = SelectObject(draw.hDC, pen);
  const int corner = scaled(12);
  RoundRect(draw.hDC, draw.rcItem.left, draw.rcItem.top, draw.rcItem.right,
            draw.rcItem.bottom, corner, corner);
  SelectObject(draw.hDC, old_pen);
  SelectObject(draw.hDC, old_brush);
  DeleteObject(pen);
  DeleteObject(brush);
  if (operation_tab) {
    fill_tab_progress(draw.hDC, draw.rcItem, shell->operation_progress_percent,
                      corner);
    HPEN progress_border = CreatePen(PS_SOLID, active ? 2 : 1, border);
    const HGDIOBJ old_progress_pen = SelectObject(draw.hDC, progress_border);
    const HGDIOBJ old_progress_brush =
        SelectObject(draw.hDC, GetStockObject(NULL_BRUSH));
    RoundRect(draw.hDC, draw.rcItem.left, draw.rcItem.top, draw.rcItem.right,
              draw.rcItem.bottom, corner, corner);
    SelectObject(draw.hDC, old_progress_brush);
    SelectObject(draw.hDC, old_progress_pen);
    DeleteObject(progress_border);
  }
  SetBkMode(draw.hDC, TRANSPARENT);
  SetTextColor(draw.hDC, primary && enabled ? RGB(15, 15, 15)
                         : enabled          ? RGB(209, 213, 219)
                                            : RGB(120, 123, 126));
  if (stage) {
    const wchar_t *label = identifier == input_stage_id     ? L"Input"
                           : identifier == preview_stage_id ? L"Preview"
                                                            : L"Output";
    const wchar_t number = identifier == input_stage_id     ? L'1'
                           : identifier == preview_stage_id ? L'2'
                                                            : L'3';
    const int badge = scaled(24);
    RECT circle{
        draw.rcItem.left + scaled(16),
        draw.rcItem.top + (draw.rcItem.bottom - draw.rcItem.top - badge) / 2,
        draw.rcItem.left + scaled(16) + badge,
        draw.rcItem.top + (draw.rcItem.bottom - draw.rcItem.top + badge) / 2};
    HBRUSH circle_brush = CreateSolidBrush(completed ? RGB(34, 197, 94)
                                           : active  ? RGB(252, 211, 77)
                                                     : RGB(156, 163, 175));
    const HGDIOBJ previous_circle_brush = SelectObject(draw.hDC, circle_brush);
    const HGDIOBJ previous_circle_pen =
        SelectObject(draw.hDC, GetStockObject(NULL_PEN));
    Ellipse(draw.hDC, circle.left, circle.top, circle.right, circle.bottom);
    SelectObject(draw.hDC, previous_circle_pen);
    SelectObject(draw.hDC, previous_circle_brush);
    DeleteObject(circle_brush);
    SetTextColor(draw.hDC, RGB(15, 15, 15));
    wchar_t number_text[2]{completed ? L'\xE73E' : number, L'\0'};
    HFONT badge_font =
        completed
            ? CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE,
                          FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, L"Segoe Fluent Icons")
            : create_ui_font(GetDpiForWindow(draw.hwndItem), 10, FW_NORMAL);
    const HGDIOBJ old_badge_font =
        badge_font == nullptr ? nullptr : SelectObject(draw.hDC, badge_font);
    DrawTextW(draw.hDC, number_text, 1, &circle,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (old_badge_font != nullptr)
      SelectObject(draw.hDC, old_badge_font);
    if (badge_font != nullptr)
      DeleteObject(badge_font);
    RECT label_bounds = draw.rcItem;
    label_bounds.left += scaled(48);
    label_bounds.right -= scaled(16);
    const HGDIOBJ old_label_font =
        active && application_state().heading_font != nullptr
            ? SelectObject(draw.hDC, application_state().heading_font)
            : nullptr;
    SetTextColor(draw.hDC, active    ? RGB(252, 211, 77)
                           : enabled ? RGB(209, 213, 219)
                                     : RGB(120, 123, 126));
    DrawTextW(draw.hDC, label, -1, &label_bounds,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (old_label_font != nullptr)
      SelectObject(draw.hDC, old_label_font);
  } else {
    if (identifier == settings_id) {
      HFONT icon_font = CreateFontW(
          -MulDiv(12, static_cast<int>(GetDpiForWindow(draw.hwndItem)), 72), 0,
          0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
          DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
      const HGDIOBJ old_font =
          icon_font == nullptr ? nullptr : SelectObject(draw.hDC, icon_font);
      RECT icon_bounds = draw.rcItem;
      DrawTextW(draw.hDC, L"\xE713", 1, &icon_bounds,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      if (old_font != nullptr)
        SelectObject(draw.hDC, old_font);
      if (icon_font != nullptr)
        DeleteObject(icon_font);
    } else if (identifier == game_browse_id || identifier == image_browse_id ||
               identifier == refresh_id) {
      HFONT icon_font = CreateFontW(
          -MulDiv(12, static_cast<int>(GetDpiForWindow(draw.hwndItem)), 72), 0,
          0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
          DEFAULT_PITCH | FF_DONTCARE, L"Segoe Fluent Icons");
      const HGDIOBJ old_font =
          icon_font == nullptr ? nullptr : SelectObject(draw.hDC, icon_font);
      RECT icon_bounds = draw.rcItem;
      const wchar_t *const glyph =
          identifier == refresh_id ? L"\xEDAB" : L"\xED25";
      DrawTextW(draw.hDC, glyph, 1, &icon_bounds,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      if (old_font != nullptr)
        SelectObject(draw.hDC, old_font);
      if (icon_font != nullptr)
        DeleteObject(icon_font);
    } else {
      std::array<wchar_t, 128> text{};
      GetWindowTextW(draw.hwndItem, text.data(), static_cast<int>(text.size()));
      RECT text_bounds = draw.rcItem;
      if (identifier == input_options_id) {
        HFONT icon_font = create_options_icon_font(draw.hwndItem);
        const HGDIOBJ old_font =
            icon_font == nullptr ? nullptr : SelectObject(draw.hDC, icon_font);
        RECT icon_bounds = draw.rcItem;
        icon_bounds.left += scaled(16);
        icon_bounds.right =
            icon_bounds.left + options_icon_width(draw.hwndItem);
        DrawTextW(draw.hDC, L"\xE70F", 1, &icon_bounds,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        if (old_font != nullptr)
          SelectObject(draw.hDC, old_font);
        if (icon_font != nullptr)
          DeleteObject(icon_font);
        text_bounds.left = icon_bounds.right + scaled(8);
        text_bounds.right -= scaled(16);
        DrawTextW(draw.hDC, text.data(), -1, &text_bounds,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      } else if (identifier == exposure_panel_id) {
        const int icon_width = fluent_icon_width(draw.hwndItem, L'\xE76C');
        text_bounds.left += scaled(16);
        text_bounds.right -= scaled(16) + icon_width + scaled(8);
        DrawTextW(draw.hDC, text.data(), -1, &text_bounds,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        HFONT icon_font = create_options_icon_font(draw.hwndItem);
        const HGDIOBJ old_font =
            icon_font == nullptr ? nullptr : SelectObject(draw.hDC, icon_font);
        RECT icon_bounds = draw.rcItem;
        icon_bounds.left = text_bounds.right + scaled(8);
        icon_bounds.right = icon_bounds.left + icon_width;
        DrawTextW(draw.hDC, L"\xE76C", 1, &icon_bounds,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        if (old_font != nullptr)
          SelectObject(draw.hDC, old_font);
        if (icon_font != nullptr)
          DeleteObject(icon_font);
      } else {
        DrawTextW(draw.hDC, text.data(), -1, &text_bounds,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      }
    }
  }
  if ((draw.itemState & ODS_FOCUS) != 0U) {
    RECT focus = draw.rcItem;
    InflateRect(&focus, -scaled(4), -scaled(4));
    DrawFocusRect(draw.hDC, &focus);
  }
  return true;
}

bool draw_modal_button(const DRAWITEMSTRUCT &draw) {
  if (draw.CtlType != ODT_BUTTON)
    return false;
  const bool enabled = (draw.itemState & ODS_DISABLED) == 0U;
  const bool pressed = enabled && (draw.itemState & ODS_SELECTED) != 0U;
  const bool hot = enabled && button_is_hot(draw.hwndItem);
  const bool primary = draw.CtlID == IDOK;
  const int dpi = static_cast<int>(GetDpiForWindow(draw.hwndItem));
  const auto scaled = [dpi](const int value) { return MulDiv(value, dpi, 96); };
  const COLORREF background = primary && enabled ? pressed ? RGB(251, 191, 36)
                                                   : hot   ? RGB(253, 230, 138)
                                                           : RGB(252, 211, 77)
                              : pressed ? RGB(17, 24, 39)
                              : hot              ? RGB(31, 41, 55)
                                                 : RGB(3, 7, 18);
  const COLORREF border = pressed              ? background
                          : primary && enabled ? RGB(252, 211, 77)
                                               : RGB(156, 163, 175);
  HBRUSH brush = CreateSolidBrush(background);
  HPEN pen = CreatePen(PS_SOLID, 1, border);
  const HGDIOBJ old_brush = SelectObject(draw.hDC, brush);
  const HGDIOBJ old_pen = SelectObject(draw.hDC, pen);
  const int corner = scaled(12);
  RoundRect(draw.hDC, draw.rcItem.left, draw.rcItem.top, draw.rcItem.right,
            draw.rcItem.bottom, corner, corner);
  SelectObject(draw.hDC, old_pen);
  SelectObject(draw.hDC, old_brush);
  DeleteObject(pen);
  DeleteObject(brush);

  std::array<wchar_t, 32> text{};
  GetWindowTextW(draw.hwndItem, text.data(), static_cast<int>(text.size()));
  SetBkMode(draw.hDC, TRANSPARENT);
  SetTextColor(draw.hDC, primary && enabled ? RGB(15, 15, 15)
                         : enabled          ? RGB(209, 213, 219)
                                            : RGB(120, 123, 126));
  RECT text_bounds = draw.rcItem;
  if (pressed)
    OffsetRect(&text_bounds, 1, 1);
  DrawTextW(draw.hDC, text.data(), -1, &text_bounds,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  if ((draw.itemState & ODS_FOCUS) != 0U) {
    RECT focus = draw.rcItem;
    InflateRect(&focus, -scaled(4), -scaled(4));
    DrawFocusRect(draw.hDC, &focus);
  }
  return true;
}

bool draw_session_row(const DRAWITEMSTRUCT &draw) {
  if (draw.CtlType != ODT_LISTVIEW || draw.CtlID != session_combo_id ||
      draw.itemID == static_cast<UINT>(-1))
    return false;
  const auto index = static_cast<std::size_t>(draw.itemID);
  const bool selected = (draw.itemState & ODS_SELECTED) != 0U;
  HBRUSH background = CreateSolidBrush(selected           ? RGB(35, 49, 63)
                                       : index % 2U == 1U ? RGB(31, 41, 55)
                                                          : RGB(27, 30, 34));
  FillRect(draw.hDC, &draw.rcItem, background);
  DeleteObject(background);
  COLORREF text_color = RGB(209, 213, 219);
  if (index < runtime_state().refresh_state.records.size()) {
    const auto &record = runtime_state().refresh_state.records[index];
    const bool stitched =
        pano::app::application_stitched_name(
            application_state().application_settings,
            wide_to_utf8(window_text(application_state().controls.game_edit)),
            record.session.session_id)
            .has_value();
    switch (pano::app::gui_session_status(record, stitched)) {
    case pano::app::GuiSessionStatus::invalid:
      text_color = RGB(239, 68, 68);
      break;
    case pano::app::GuiSessionStatus::incomplete:
      text_color = RGB(220, 145, 45);
      break;
    case pano::app::GuiSessionStatus::stitched:
      text_color = RGB(34, 197, 94);
      break;
    case pano::app::GuiSessionStatus::complete:
      break;
    }
  }
  SetBkMode(draw.hDC, TRANSPARENT);
  const int cell_padding = table_cell_padding(draw.hwndItem);
  int left = draw.rcItem.left;
  for (int column = 0; column < 4; ++column) {
    const int width = ListView_GetColumnWidth(
        application_state().controls.session_combo, column);
    RECT text{left + cell_padding, draw.rcItem.top, left + width - cell_padding,
              draw.rcItem.bottom};
    std::array<wchar_t, 512> value{};
    LVITEMW item{};
    item.iSubItem = column;
    item.pszText = value.data();
    item.cchTextMax = static_cast<int>(value.size());
    SendMessageW(application_state().controls.session_combo, LVM_GETITEMTEXTW,
                 draw.itemID, reinterpret_cast<LPARAM>(&item));
    SetTextColor(draw.hDC, column == 0 ? text_color : RGB(209, 213, 219));
    if (column == 3) {
      const int dpi = static_cast<int>(GetDpiForWindow(draw.hwndItem));
      const auto scaled = [dpi](const int value) {
        return MulDiv(value, dpi, 96);
      };
      const int button_width = action_button_width(draw.hwndItem);
      const int button_height = MulDiv(31, dpi, 96);
      const int button_top =
          text.top + (text.bottom - text.top - button_height) / 2;
      RECT button{text.left, button_top,
                  std::min(text.right, text.left + button_width),
                  button_top + button_height};
      const bool action_hot =
          action_property_index(draw.hwndItem, action_hot_property) ==
          index + 1U;
      const bool action_pressed =
          action_property_index(draw.hwndItem, action_pressed_property) ==
          index + 1U;
      const COLORREF button_background = action_hot || action_pressed
                                             ? RGB(31, 41, 55)
                                         : selected         ? RGB(35, 49, 63)
                                         : index % 2U == 1U ? RGB(31, 41, 55)
                                                            : RGB(17, 24, 39);
      const COLORREF button_border =
          action_pressed ? button_background : RGB(156, 163, 175);
      HBRUSH button_brush = CreateSolidBrush(button_background);
      HPEN button_pen = CreatePen(PS_SOLID, 1, button_border);
      const HGDIOBJ old_button_brush = SelectObject(draw.hDC, button_brush);
      const HGDIOBJ old_button_pen = SelectObject(draw.hDC, button_pen);
      const int corner = scaled(12);
      RoundRect(draw.hDC, button.left, button.top, button.right, button.bottom,
                corner, corner);
      SelectObject(draw.hDC, old_button_pen);
      SelectObject(draw.hDC, old_button_brush);
      DeleteObject(button_pen);
      DeleteObject(button_brush);
      const int padding = scaled(8);
      const int gap = scaled(8);
      const int arrow_width = scaled(16);
      SetTextColor(draw.hDC, RGB(209, 213, 219));
      SIZE label_size{};
      GetTextExtentPoint32W(draw.hDC, L"Actions", 7, &label_size);
      RECT label{button.left + padding, button.top,
                 button.left + padding + label_size.cx, button.bottom};
      DrawTextW(draw.hDC, L"Actions", -1, &label,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      const int separator_x = label.right + gap;
      HPEN separator = CreatePen(PS_SOLID, 1, RGB(156, 163, 175));
      const HGDIOBJ old_separator = SelectObject(draw.hDC, separator);
      MoveToEx(draw.hDC, separator_x, button.top + scaled(4), nullptr);
      LineTo(draw.hDC, separator_x, button.bottom - scaled(4));
      SelectObject(draw.hDC, old_separator);
      DeleteObject(separator);
      const int arrow_left = separator_x + scaled(1) + gap;
      const int arrow_x = arrow_left + arrow_width / 2;
      const int arrow_y = (button.top + button.bottom) / 2;
      HPEN arrow = CreatePen(PS_SOLID, 1, RGB(209, 213, 219));
      const HGDIOBJ old_arrow = SelectObject(draw.hDC, arrow);
      MoveToEx(draw.hDC, arrow_x - scaled(3), arrow_y - scaled(2), nullptr);
      LineTo(draw.hDC, arrow_x, arrow_y + scaled(1));
      LineTo(draw.hDC, arrow_x + scaled(3), arrow_y - scaled(2));
      SelectObject(draw.hDC, old_arrow);
      DeleteObject(arrow);
    } else {
      DrawTextW(draw.hDC, value.data(), -1, &text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    left += width;
  }
  if ((draw.itemState & ODS_FOCUS) != 0U) {
    RECT focus = draw.rcItem;
    InflateRect(&focus, -1, -1);
    DrawFocusRect(draw.hDC, &focus);
  }
  return true;
}

LRESULT CALLBACK trackbar_window_procedure(const HWND window,
                                           const UINT message,
                                           const WPARAM wparam,
                                           const LPARAM lparam, UINT_PTR,
                                           DWORD_PTR) {
  if (message == WM_ERASEBKGND)
    return 1;
  if (message == WM_PAINT) {
    PAINTSTRUCT paint{};
    HDC context = BeginPaint(window, &paint);
    RECT bounds{};
    GetClientRect(window, &bounds);
    FillRect(context, &bounds, dark_background_brush());
    const int minimum =
        static_cast<int>(SendMessageW(window, TBM_GETRANGEMIN, 0, 0));
    const int maximum =
        static_cast<int>(SendMessageW(window, TBM_GETRANGEMAX, 0, 0));
    const int position =
        static_cast<int>(SendMessageW(window, TBM_GETPOS, 0, 0));
    const int radius = std::max(16, MulDiv(18, GetDpiForWindow(window), 96));
    const int left = radius;
    const int right =
        std::max(left + 1, static_cast<int>(bounds.right) - radius);
    const int center = bounds.bottom / 2;
    const int thumb =
        minimum < maximum
            ? left + MulDiv(position - minimum, right - left, maximum - minimum)
            : left;
    RECT track{left, center - 2, right, center + 2};
    HBRUSH unfilled = CreateSolidBrush(RGB(62, 66, 70));
    FillRect(context, &track, unfilled);
    DeleteObject(unfilled);
    RECT filled = track;
    filled.right = thumb;
    HBRUSH accent = CreateSolidBrush(
        IsWindowEnabled(window) ? RGB(215, 235, 45) : RGB(95, 100, 55));
    FillRect(context, &filled, accent);
    const HGDIOBJ old_brush = SelectObject(context, accent);
    const HGDIOBJ old_pen = SelectObject(context, GetStockObject(NULL_PEN));
    Ellipse(context, thumb - radius, center - radius, thumb + radius,
            center + radius);
    SelectObject(context, old_pen);
    SelectObject(context, old_brush);
    DeleteObject(accent);
    EndPaint(window, &paint);
    return 0;
  }
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(window, trackbar_window_procedure, 1U);
    return DefSubclassProc(window, message, wparam, lparam);
  }
  const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
  if (message == WM_ENABLE || message == WM_SETFOCUS ||
      message == WM_KILLFOCUS || message == WM_LBUTTONDOWN ||
      message == WM_LBUTTONUP || message == WM_MOUSEMOVE ||
      message == WM_KEYDOWN || message == WM_KEYUP)
    InvalidateRect(window, nullptr, FALSE);
  return result;
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
      wide_to_utf8(window_text(application_state().controls.game_edit));
  application_state().application_settings.image_directory =
      wide_to_utf8(window_text(application_state().controls.image_edit));
  application_state().application_settings.output_directory =
      wide_to_utf8(window_text(application_state().controls.output_edit));
  application_state().application_settings.auto_contrast =
      SendMessageW(application_state().controls.auto_contrast_check,
                   BM_GETCHECK, 0, 0) == BST_CHECKED;
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
    SetWindowTextW(application_state().controls.status_label,
                   L"Settings could not be loaded; changes will not be saved");
    return;
  }
  SetWindowTextW(
      application_state().controls.game_edit,
      utf8_to_wide(application_state().application_settings.game_directory)
          .c_str());
  SetWindowTextW(
      application_state().controls.image_edit,
      utf8_to_wide(application_state().application_settings.image_directory)
          .c_str());
  SetWindowTextW(
      application_state().controls.output_edit,
      utf8_to_wide(application_state().application_settings.output_directory)
          .c_str());
  SendMessageW(application_state().controls.auto_contrast_check, BM_SETCHECK,
               application_state().application_settings.auto_contrast
                   ? BST_CHECKED
                   : BST_UNCHECKED,
               0);
  const unsigned memory_mib =
      application_state().application_settings.gpu_memory_mib == 0U
          ? default_gpu_memory_mib()
          : application_state().application_settings.gpu_memory_mib;
  SetWindowTextW(application_state().controls.memory_edit,
                 std::to_wstring(memory_mib).c_str());
  SendMessageW(application_state().controls.coverage_check, BM_SETCHECK,
               application_state().application_settings.debug_coverage
                   ? BST_CHECKED
                   : BST_UNCHECKED,
               0);
}

bool checked(const HWND control) {
  return SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

bool parse_unsigned_text(const HWND control, const bool allow_empty,
                         std::optional<unsigned> &value, std::string &error) {
  const std::string text = wide_to_utf8(window_text(control));
  if (text.empty() && allow_empty) {
    value.reset();
    return true;
  }
  unsigned parsed = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (text.empty() || result.ec != std::errc{} ||
      result.ptr != text.data() + text.size()) {
    error = "option values must be whole numbers";
    return false;
  }
  value = parsed;
  return true;
}

std::string combo_value(const HWND combo,
                        const std::array<const char *, 3> &values) {
  const LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
  if (selected < 0 || static_cast<std::size_t>(selected) >= values.size())
    return {};
  return values[static_cast<std::size_t>(selected)];
}

bool capture_gui_request(pano::app::GuiRenderRequestState &request,
                         std::string &error) {
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
  captured.image_dir =
      wide_to_utf8(window_text(application_state().controls.image_edit));
  captured.output_directory =
      wide_to_utf8(window_text(application_state().controls.output_edit));
  captured.output_name =
      wide_to_utf8(window_text(application_state().controls.output_name_edit));
  captured.format = combo_value(application_state().controls.format_combo,
                                {"jpeg", "png", "exr"});
  captured.blend = combo_value(application_state().controls.blend_combo,
                               {"hard", "feather", ""});
  std::optional<unsigned> parsed;
  if (!parse_unsigned_text(application_state().controls.quality_edit, false,
                           parsed, error))
    return false;
  captured.jpeg_quality = *parsed;
  if (shell != nullptr && shell->resolution_pixels) {
    captured.resolution_percent = 100U;
    if (!parse_unsigned_text(application_state().controls.width_edit, false,
                             captured.width, error))
      return false;
  } else {
    if (!parse_unsigned_text(application_state().controls.resolution_edit,
                             false, parsed, error))
      return false;
    captured.resolution_percent = *parsed;
    captured.width.reset();
  }
  if (!parse_unsigned_text(application_state().controls.memory_edit, false,
                           parsed, error))
    return false;
  captured.memory_mib = *parsed;
  if (!parse_unsigned_text(application_state().controls.workers_edit, false,
                           parsed, error))
    return false;
  captured.workers = *parsed;
  captured.thumbnail = checked(application_state().controls.thumbnail_check);
  captured.coverage = checked(application_state().controls.coverage_check);
  captured.allow_incomplete =
      checked(application_state().controls.incomplete_check);
  captured.auto_contrast =
      checked(application_state().controls.auto_contrast_check);
  captured.gpu = checked(application_state().controls.gpu_check);
  captured.gpu_strict = checked(application_state().controls.gpu_strict_check);
  request = std::move(captured);
  return true;
}

void schedule_validation(HWND window, bool discard_preview);

void update_option_enablement() {
  pano::app::GuiRenderRequestState request;
  request.format = combo_value(application_state().controls.format_combo,
                               {"jpeg", "png", "exr"});
  request.gpu = checked(application_state().controls.gpu_check);
  const auto enabled = pano::app::gui_option_enablement(request);
  EnableWindow(application_state().controls.quality_edit, enabled.jpeg_quality);
  EnableWindow(application_state().controls.quality_slider,
               enabled.jpeg_quality);
  EnableWindow(application_state().controls.quality_label,
               enabled.jpeg_quality);
  EnableWindow(application_state().controls.memory_edit, enabled.cpu_memory);
  EnableWindow(application_state().controls.workers_edit, enabled.workers);
  EnableWindow(application_state().controls.gpu_strict_check,
               enabled.gpu_strict);
  if (!enabled.gpu_strict)
    SendMessageW(application_state().controls.gpu_strict_check, BM_SETCHECK,
                 BST_UNCHECKED, 0);
}

void select_output_format(const HWND window, const int selection) {
  if (selection < 0 || selection > 2)
    return;
  SendMessageW(application_state().controls.format_combo, CB_SETCURSEL,
               selection, 0);
  SendMessageW(application_state().controls.format_jpeg, BM_SETCHECK,
               selection == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageW(application_state().controls.format_png, BM_SETCHECK,
               selection == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageW(application_state().controls.format_exr, BM_SETCHECK,
               selection == 2 ? BST_CHECKED : BST_UNCHECKED, 0);
  InvalidateRect(application_state().controls.format_jpeg, nullptr, FALSE);
  InvalidateRect(application_state().controls.format_png, nullptr, FALSE);
  InvalidateRect(application_state().controls.format_exr, nullptr, FALSE);
  auto name = std::filesystem::path(
      window_text(application_state().controls.output_name_edit));
  const auto format = combo_value(application_state().controls.format_combo,
                                  {"jpeg", "png", "exr"});
  name.replace_extension(format == "jpeg" ? L".jpg"
                                          : utf8_to_wide("." + format));
  if (auto *const shell = shell_state(window); shell != nullptr)
    shell->suppress_control_changes = true;
  SetWindowTextW(application_state().controls.output_name_edit, name.c_str());
  if (auto *const shell = shell_state(window); shell != nullptr)
    shell->suppress_control_changes = false;
  update_option_enablement();
  schedule_validation(window, false);
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

HWND child(const HWND parent, const wchar_t *class_name, const wchar_t *text,
           const DWORD style, const int identifier) {
  const bool centered_edit =
      lstrcmpW(class_name, WC_EDITW) == 0 && (style & ES_MULTILINE) == 0U;
  const DWORD effective_style =
      centered_edit ? style | ES_MULTILINE | ES_AUTOHSCROLL : style;
  HWND result = CreateWindowExW(
      0, class_name, text, WS_CHILD | WS_VISIBLE | effective_style, 0, 0, 0, 0,
      parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
      GetModuleHandleW(nullptr), nullptr);
  if (result != nullptr)
    SendMessageW(
        result, WM_SETFONT,
        reinterpret_cast<WPARAM>(application_state().body_font != nullptr
                                     ? application_state().body_font
                                     : GetStockObject(DEFAULT_GUI_FONT)),
        TRUE);
  if (result != nullptr)
    apply_system_dark_theme(result);
  if (result != nullptr && lstrcmpW(class_name, WC_BUTTONW) == 0 &&
      (style & BS_TYPEMASK) == BS_OWNERDRAW)
    SetWindowSubclass(result, button_window_procedure, 2U, 0U);
  if (result != nullptr && centered_edit) {
    SetWindowSubclass(result, edit_window_procedure, 3U, 0U);
    center_edit_text(result);
  }
  if (result != nullptr && identifier == session_combo_id)
    SetWindowSubclass(result, session_list_window_procedure, 4U, 0U);
  return result;
}

void set_list_text(const HWND list, const int row, const int column,
                   const std::wstring &text) {
  LVITEMW item{};
  item.iSubItem = column;
  item.pszText = const_cast<wchar_t *>(text.c_str());
  SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(row),
               reinterpret_cast<LPARAM>(&item));
}

bool set_accessible_name(const HWND control, const wchar_t *name,
                         const DWORD child_id = accessibility_self_id) {
  if (control == nullptr ||
      application_state().accessibility_properties == nullptr)
    return false;
  const HRESULT msaa_result =
      application_state().accessibility_properties->SetHwndPropStr(
          control, accessibility_object_id, child_id, PROPID_ACC_NAME, name);
  const HRESULT uia_result =
      application_state().accessibility_properties->SetHwndPropStr(
          control, accessibility_object_id, child_id, Name_Property_GUID, name);
  return SUCCEEDED(msaa_result) && SUCCEEDED(uia_result);
}

void clear_session_accessible_names() {
  if (application_state().accessibility_properties == nullptr)
    return;
  const int count =
      ListView_GetItemCount(application_state().controls.session_combo);
  const std::array<MSAAPROPID, 2> properties{PROPID_ACC_NAME,
                                             Name_Property_GUID};
  for (int index = 0; index < count; ++index) {
    application_state().accessibility_properties->ClearHwndProps(
        application_state().controls.session_combo, accessibility_object_id,
        static_cast<DWORD>(index + 1), properties.data(),
        static_cast<int>(properties.size()));
  }
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

bool update_session_accessible_name(const std::size_t index) {
  if (index >= runtime_state().refresh_state.records.size())
    return false;
  const auto &record = runtime_state().refresh_state.records[index];
  const std::string game_directory =
      wide_to_utf8(window_text(application_state().controls.game_edit));
  const bool stitched = pano::app::application_stitched_name(
                            application_state().application_settings,
                            game_directory, record.session.session_id)
                            .has_value();
  std::wstring name = utf8_to_wide(
      pano::app::gui_session_local_label(record.session.session_id));
  name += L", " + std::to_wstring(record.session.frames.size()) + L" poses, ";
  name += session_status_name(pano::app::gui_session_status(record, stitched));
  const auto tag = pano::app::application_session_tag(
      application_state().application_settings, game_directory,
      record.session.session_id);
  if (tag.has_value() && !tag->empty())
    name += L", tag " + utf8_to_wide(*tag);
  return set_accessible_name(application_state().controls.session_combo,
                             name.c_str(), static_cast<DWORD>(index + 1U));
}

bool create_controls(const HWND window) {
  if (FAILED(CoCreateInstance(
          CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER,
          IID_PPV_ARGS(&application_state().accessibility_properties))))
    return false;
  application_state().controls.input_stage_button =
      child(window, WC_BUTTONW, L"1  Input", WS_TABSTOP | BS_OWNERDRAW,
            input_stage_id);
  application_state().controls.preview_stage_button =
      child(window, WC_BUTTONW, L"2  Preview", WS_TABSTOP | BS_OWNERDRAW,
            preview_stage_id);
  application_state().controls.output_stage_button =
      child(window, WC_BUTTONW, L"3  Output", WS_TABSTOP | BS_OWNERDRAW,
            output_stage_id);
  application_state().controls.settings_button = child(
      window, WC_BUTTONW, L"Settings", WS_TABSTOP | BS_OWNERDRAW, settings_id);
  application_state().controls.input_options_button =
      child(window, WC_BUTTONW, L"Options", WS_TABSTOP | BS_OWNERDRAW,
            input_options_id);
  application_state().controls.preview_options_button =
      child(window, WC_BUTTONW, L"Options", WS_TABSTOP | BS_OWNERDRAW,
            preview_options_id);
  application_state().controls.exposure_panel_button =
      child(window, WC_BUTTONW, L"Adjust exposure", WS_TABSTOP | BS_OWNERDRAW,
            exposure_panel_id);
  application_state().controls.resolution_mode_button =
      child(window, WC_BUTTONW, L"Switch", WS_TABSTOP | BS_OWNERDRAW,
            resolution_mode_id);
  application_state().controls.game_label = child(
      window, WC_STATICW, L"Game directory:", SS_LEFT | SS_CENTERIMAGE, 100);
  application_state().controls.game_edit =
      child(window, WC_EDITW, L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
            game_edit_id);
  application_state().controls.game_browse_button = child(
      window, WC_BUTTONW, L"...", WS_TABSTOP | BS_OWNERDRAW, game_browse_id);
  application_state().controls.refresh_button = child(
      window, WC_BUTTONW, L"Refresh", WS_TABSTOP | BS_OWNERDRAW, refresh_id);
  application_state().controls.session_label =
      child(window, WC_STATICW, L"Sessions", SS_LEFT | SS_CENTERIMAGE, 104);
  application_state().controls.session_combo =
      child(window, WC_LISTVIEWW, L"",
            WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL |
                LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER | LVS_OWNERDRAWFIXED,
            session_combo_id);
  application_state().controls.session_header_session =
      child(window, WC_STATICW, L"  Session", SS_LEFT | SS_CENTERIMAGE, 0);
  application_state().controls.session_header_poses =
      child(window, WC_STATICW, L"  #", SS_LEFT | SS_CENTERIMAGE, 0);
  application_state().controls.session_header_tag =
      child(window, WC_STATICW, L"  Tag", SS_LEFT | SS_CENTERIMAGE, 0);
  application_state().controls.session_header_actions =
      child(window, WC_STATICW, L"", SS_LEFT | SS_CENTERIMAGE, 0);
  application_state().controls.image_label =
      child(window, WC_STATICW, L"Screenshots directory:",
            SS_LEFT | SS_CENTERIMAGE, 128);
  application_state().controls.image_edit =
      child(window, WC_EDITW, L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
            image_edit_id);
  application_state().controls.image_browse_button = child(
      window, WC_BUTTONW, L"...", WS_TABSTOP | BS_OWNERDRAW, image_browse_id);
  application_state().controls.output_label = child(
      window, WC_STATICW, L"Output directory", SS_LEFT | SS_CENTERIMAGE, 107);
  application_state().controls.output_edit =
      child(window, WC_EDITW, L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
            output_edit_id);
  application_state().controls.output_browse_button = child(
      window, WC_BUTTONW, L"...", WS_TABSTOP | BS_OWNERDRAW, output_browse_id);
  application_state().controls.output_name_label =
      child(window, WC_STATICW, L"Filename", SS_LEFT | SS_CENTERIMAGE, 129);
  application_state().controls.output_name_edit =
      child(window, WC_EDITW, L"panorama.jpg",
            WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, output_name_id);
  application_state().controls.format_label =
      child(window, WC_STATICW, L"Format:", SS_LEFT | SS_CENTERIMAGE, 109);
  application_state().controls.format_combo =
      child(window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, format_combo_id);
  application_state().controls.format_jpeg =
      child(window, WC_BUTTONW, L"JPEG (SDR)",
            WS_TABSTOP | BS_OWNERDRAW | WS_GROUP, format_jpeg_id);
  application_state().controls.format_png =
      child(window, WC_BUTTONW, L"PNG (SDR)", WS_TABSTOP | BS_OWNERDRAW,
            format_png_id);
  application_state().controls.format_exr =
      child(window, WC_BUTTONW, L"EXR (HDR)", WS_TABSTOP | BS_OWNERDRAW,
            format_exr_id);
  application_state().controls.quality_label =
      child(window, WC_STATICW, L"Quality", SS_LEFT | SS_CENTERIMAGE, 130);
  application_state().controls.quality_edit =
      child(window, WC_EDITW, L"95", WS_TABSTOP | WS_BORDER | ES_NUMBER,
            quality_edit_id);
  application_state().controls.quality_slider = child(
      window, TRACKBAR_CLASSW, L"",
      WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_FIXEDLENGTH, quality_slider_id);
  application_state().controls.resolution_label =
      child(window, WC_STATICW, L"Scale (%)", SS_LEFT | SS_CENTERIMAGE, 131);
  application_state().controls.resolution_edit =
      child(window, WC_EDITW, L"100", WS_TABSTOP | WS_BORDER | ES_NUMBER,
            resolution_edit_id);
  application_state().controls.resolution_slider =
      child(window, TRACKBAR_CLASSW, L"",
            WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_FIXEDLENGTH,
            resolution_slider_id);
  application_state().controls.width_label = child(
      window, WC_STATICW, L"Width (optional):", SS_LEFT | SS_CENTERIMAGE, 132);
  application_state().controls.width_edit = child(
      window, WC_EDITW, L"", WS_TABSTOP | WS_BORDER | ES_NUMBER, width_edit_id);
  application_state().controls.blend_label =
      child(window, WC_STATICW, L"Blend:", SS_LEFT | SS_CENTERIMAGE, 111);
  application_state().controls.blend_combo = child(
      window, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST, blend_combo_id);
  application_state().controls.memory_label = child(
      window, WC_STATICW, L"CPU memory MiB:", SS_LEFT | SS_CENTERIMAGE, 133);
  application_state().controls.memory_edit =
      child(window, WC_EDITW, L"1024", WS_TABSTOP | WS_BORDER | ES_NUMBER,
            memory_edit_id);
  application_state().controls.workers_label = child(
      window, WC_STATICW, L"Workers (0=auto):", SS_LEFT | SS_CENTERIMAGE, 134);
  application_state().controls.workers_edit =
      child(window, WC_EDITW, L"0", WS_TABSTOP | WS_BORDER | ES_NUMBER,
            workers_edit_id);
  application_state().controls.thumbnail_check =
      child(window, WC_BUTTONW, L"Generate thumbnail",
            WS_TABSTOP | BS_AUTOCHECKBOX, thumbnail_id);
  application_state().controls.coverage_check =
      child(window, WC_BUTTONW, L"Write coverage PNG",
            WS_TABSTOP | BS_AUTOCHECKBOX, coverage_id);
  application_state().controls.incomplete_check =
      child(window, WC_BUTTONW, L"Allow incomplete session",
            WS_TABSTOP | BS_AUTOCHECKBOX, incomplete_id);
  application_state().controls.auto_contrast_check =
      child(window, WC_BUTTONW, L"Auto contrast", WS_TABSTOP | BS_AUTOCHECKBOX,
            auto_contrast_id);
  application_state().controls.gpu_check =
      child(window, WC_BUTTONW, L"Use GPU when available",
            WS_TABSTOP | BS_AUTOCHECKBOX, gpu_id);
  application_state().controls.gpu_strict_check =
      child(window, WC_BUTTONW, L"Require GPU", WS_TABSTOP | BS_AUTOCHECKBOX,
            gpu_strict_id);
  application_state().controls.automatic_exposure_button =
      child(window, WC_BUTTONW, L"Auto to target", WS_TABSTOP | BS_OWNERDRAW,
            automatic_exposure_id);
  application_state().controls.match_exposure_button =
      child(window, WC_BUTTONW, L"Match selected", WS_TABSTOP | BS_OWNERDRAW,
            match_exposure_id);
  application_state().controls.discard_exposure_button =
      child(window, WC_BUTTONW, L"Discard exposure", WS_TABSTOP | BS_OWNERDRAW,
            discard_exposure_id);
  application_state().controls.delete_session_button =
      child(window, WC_BUTTONW, L"Delete session...",
            WS_TABSTOP | BS_PUSHBUTTON, delete_session_id);
  application_state().controls.delete_images_check =
      child(window, WC_BUTTONW, L"Include captured images",
            WS_TABSTOP | BS_AUTOCHECKBOX, delete_images_id);
  application_state().controls.preview_button = child(
      window, WC_BUTTONW, L"Preview", WS_TABSTOP | BS_OWNERDRAW, preview_id);
  application_state().controls.preview_next_button =
      child(window, WC_BUTTONW, L"Render >>", WS_TABSTOP | BS_OWNERDRAW,
            preview_next_id);
  application_state().controls.render_button = child(
      window, WC_BUTTONW, L"Render", WS_TABSTOP | BS_OWNERDRAW, render_id);
  application_state().controls.render_thumbnail_button =
      child(window, WC_BUTTONW, L"Render with thumbnail",
            WS_TABSTOP | BS_OWNERDRAW, render_thumbnail_id);
  application_state().controls.cancel_button =
      child(window, WC_BUTTONW, L"Abort", WS_TABSTOP | BS_OWNERDRAW, cancel_id);
  application_state().controls.status_label =
      child(window, WC_STATICW, L"Ready", SS_LEFT | SS_CENTERIMAGE, 116);
  application_state().controls.operation_progress =
      child(window, PROGRESS_CLASSW, L"", PBS_SMOOTH | PBS_MARQUEE, 0);
  application_state().controls.preview_surface =
      child(window, preview_window_class, L"", WS_BORDER, 135);
  SendMessageW(application_state().controls.format_combo, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(L"JPEG (SDR)"));
  SendMessageW(application_state().controls.format_combo, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(L"PNG (SDR)"));
  SendMessageW(application_state().controls.format_combo, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(L"EXR (HDR)"));
  SendMessageW(application_state().controls.format_combo, CB_SETCURSEL, 0, 0);
  SendMessageW(application_state().controls.format_jpeg, BM_SETCHECK,
               BST_CHECKED, 0);
  SendMessageW(application_state().controls.resolution_slider, TBM_SETRANGE,
               TRUE, MAKELPARAM(1, 100));
  SendMessageW(application_state().controls.resolution_slider, TBM_SETPOS, TRUE,
               100);
  SendMessageW(application_state().controls.quality_slider, TBM_SETRANGE, TRUE,
               MAKELPARAM(1, 100));
  SendMessageW(application_state().controls.quality_slider, TBM_SETPOS, TRUE,
               95);
  const LPARAM slider_thumb = static_cast<LPARAM>(
      std::max(32, MulDiv(36, GetDpiForWindow(window), 96)));
  SendMessageW(application_state().controls.resolution_slider,
               TBM_SETTHUMBLENGTH, slider_thumb, 0);
  SendMessageW(application_state().controls.quality_slider, TBM_SETTHUMBLENGTH,
               slider_thumb, 0);
  SendMessageW(application_state().controls.resolution_edit, EM_SETLIMITTEXT, 3,
               0);
  SendMessageW(application_state().controls.quality_edit, EM_SETLIMITTEXT, 3,
               0);
  SendMessageW(application_state().controls.width_edit, EM_SETLIMITTEXT, 5, 0);
  if (SetWindowSubclass(application_state().controls.resolution_slider,
                        trackbar_window_procedure, 1U, 0U) == FALSE ||
      SetWindowSubclass(application_state().controls.quality_slider,
                        trackbar_window_procedure, 1U, 0U) == FALSE)
    return false;
  SendMessageW(application_state().controls.blend_combo, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(L"Hard"));
  SendMessageW(application_state().controls.blend_combo, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(L"Feather"));
  SendMessageW(application_state().controls.blend_combo, CB_SETCURSEL, 1, 0);
  ListView_SetExtendedListViewStyle(application_state().controls.session_combo,
                                    LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                        LVS_EX_LABELTIP | LVS_EX_INFOTIP);
  const std::array<std::pair<const wchar_t *, int>, 4> session_columns{
      {{L"Session", 300}, {L"Poses", 70}, {L"Tag", 180}, {L"Actions", 80}}};
  for (std::size_t index = 0; index < session_columns.size(); ++index) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t *>(session_columns[index].first);
    column.cx = session_columns[index].second;
    column.iSubItem = static_cast<int>(index);
    SendMessageW(application_state().controls.session_combo, LVM_INSERTCOLUMNW,
                 static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&column));
  }
  SendMessageW(application_state().controls.auto_contrast_check, BM_SETCHECK,
               BST_CHECKED, 0);
  SendMessageW(application_state().controls.gpu_check, BM_SETCHECK, BST_CHECKED,
               0);
  const std::array<HWND, 64> all{
      application_state().controls.input_stage_button,
      application_state().controls.preview_stage_button,
      application_state().controls.output_stage_button,
      application_state().controls.settings_button,
      application_state().controls.input_options_button,
      application_state().controls.preview_options_button,
      application_state().controls.exposure_panel_button,
      application_state().controls.resolution_mode_button,
      application_state().controls.game_label,
      application_state().controls.game_edit,
      application_state().controls.game_browse_button,
      application_state().controls.refresh_button,
      application_state().controls.session_label,
      application_state().controls.session_combo,
      application_state().controls.session_header_session,
      application_state().controls.session_header_poses,
      application_state().controls.session_header_tag,
      application_state().controls.session_header_actions,
      application_state().controls.image_label,
      application_state().controls.image_edit,
      application_state().controls.image_browse_button,
      application_state().controls.output_label,
      application_state().controls.output_edit,
      application_state().controls.output_browse_button,
      application_state().controls.output_name_label,
      application_state().controls.output_name_edit,
      application_state().controls.format_label,
      application_state().controls.format_combo,
      application_state().controls.format_jpeg,
      application_state().controls.format_png,
      application_state().controls.format_exr,
      application_state().controls.quality_label,
      application_state().controls.quality_edit,
      application_state().controls.quality_slider,
      application_state().controls.resolution_label,
      application_state().controls.resolution_edit,
      application_state().controls.resolution_slider,
      application_state().controls.width_label,
      application_state().controls.width_edit,
      application_state().controls.blend_label,
      application_state().controls.blend_combo,
      application_state().controls.memory_label,
      application_state().controls.memory_edit,
      application_state().controls.workers_label,
      application_state().controls.workers_edit,
      application_state().controls.thumbnail_check,
      application_state().controls.coverage_check,
      application_state().controls.incomplete_check,
      application_state().controls.auto_contrast_check,
      application_state().controls.gpu_check,
      application_state().controls.gpu_strict_check,
      application_state().controls.automatic_exposure_button,
      application_state().controls.match_exposure_button,
      application_state().controls.discard_exposure_button,
      application_state().controls.delete_session_button,
      application_state().controls.delete_images_check,
      application_state().controls.preview_button,
      application_state().controls.preview_next_button,
      application_state().controls.render_button,
      application_state().controls.render_thumbnail_button,
      application_state().controls.cancel_button,
      application_state().controls.status_label,
      application_state().controls.operation_progress,
      application_state().controls.preview_surface};
  for (const HWND control : all)
    if (control == nullptr)
      return false;
    else
      apply_system_dark_theme(control);
  const std::array<std::pair<HWND, const wchar_t *>, 21> accessible_names{{
      {application_state().controls.game_edit, L"Game directory"},
      {application_state().controls.game_browse_button,
       L"Browse game directory"},
      {application_state().controls.session_combo, L"Sessions"},
      {application_state().controls.image_edit, L"Screenshots directory"},
      {application_state().controls.image_browse_button,
       L"Browse screenshots directory"},
      {application_state().controls.output_edit, L"Output directory"},
      {application_state().controls.output_browse_button,
       L"Browse output directory"},
      {application_state().controls.output_name_edit, L"Output filename"},
      {application_state().controls.format_combo, L"Output format"},
      {application_state().controls.quality_edit, L"JPEG quality value"},
      {application_state().controls.quality_slider, L"JPEG quality"},
      {application_state().controls.resolution_edit,
       L"Output scale percentage value"},
      {application_state().controls.resolution_slider,
       L"Output scale percentage"},
      {application_state().controls.exposure_panel_button,
       L"Open exposure controls"},
      {application_state().controls.resolution_mode_button,
       L"Switch resolution mode"},
      {application_state().controls.width_edit, L"Output width"},
      {application_state().controls.blend_combo, L"Preview blending"},
      {application_state().controls.memory_edit, L"Memory limit in MiB"},
      {application_state().controls.workers_edit, L"CPU workers"},
      {application_state().controls.preview_surface, L"Panorama preview"},
      {application_state().controls.cancel_button, L"Cancel current operation"},
  }};
  for (const auto &[control, name] : accessible_names)
    if (!set_accessible_name(control, name))
      return false;
  ShowWindow(application_state().controls.format_combo, SW_HIDE);
  replace_ui_fonts(window);
  ListView_SetBkColor(application_state().controls.session_combo,
                      RGB(17, 24, 39));
  ListView_SetTextBkColor(application_state().controls.session_combo,
                          RGB(17, 24, 39));
  ListView_SetTextColor(application_state().controls.session_combo,
                        RGB(232, 234, 236));
  disable_control_theme(application_state().controls.session_combo);
  EnableWindow(application_state().controls.preview_button, FALSE);
  EnableWindow(application_state().controls.render_button, FALSE);
  EnableWindow(application_state().controls.render_thumbnail_button, FALSE);
  EnableWindow(application_state().controls.preview_next_button, FALSE);
  EnableWindow(application_state().controls.cancel_button, FALSE);
  ShowWindow(application_state().controls.cancel_button, SW_HIDE);
  ShowWindow(application_state().controls.operation_progress, SW_HIDE);
  if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr,
                                 CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&application_state().taskbar))))
    application_state().taskbar->HrInit();
  EnableWindow(application_state().controls.automatic_exposure_button, FALSE);
  EnableWindow(application_state().controls.match_exposure_button, FALSE);
  EnableWindow(application_state().controls.discard_exposure_button, FALSE);
  EnableWindow(application_state().controls.exposure_panel_button, FALSE);
  ShowWindow(application_state().controls.automatic_exposure_button, SW_HIDE);
  ShowWindow(application_state().controls.match_exposure_button, SW_HIDE);
  ShowWindow(application_state().controls.discard_exposure_button, SW_HIDE);
  EnableWindow(application_state().controls.delete_session_button, FALSE);
  update_option_enablement();
  return true;
}

void show_controls(const std::initializer_list<HWND> list, const bool visible) {
  for (const HWND control : list)
    ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
}

void update_stage_visibility(const HWND window) {
  const auto *const shell = shell_state(window);
  if (shell == nullptr)
    return;
  const bool input = shell->workflow.stage == pano::app::GuiStage::input;
  const bool preview = shell->workflow.stage == pano::app::GuiStage::preview;
  const bool output = shell->workflow.stage == pano::app::GuiStage::output;
  show_controls({application_state().controls.game_label,
                 application_state().controls.game_edit,
                 application_state().controls.game_browse_button,
                 application_state().controls.refresh_button,
                 application_state().controls.session_combo,
                 application_state().controls.session_header_session,
                 application_state().controls.session_header_poses,
                 application_state().controls.session_header_tag,
                 application_state().controls.session_header_actions,
                 application_state().controls.image_label,
                 application_state().controls.image_edit,
                 application_state().controls.image_browse_button,
                 application_state().controls.input_options_button,
                 application_state().controls.preview_button},
                input);
  ShowWindow(application_state().controls.session_label, SW_HIDE);
  show_controls({application_state().controls.preview_options_button,
                 application_state().controls.preview_next_button,
                 application_state().controls.preview_surface},
                preview);
  ShowWindow(application_state().controls.exposure_panel_button,
             preview && retained_preview_ready() &&
                     !runtime_state().preview_building
                 ? SW_SHOW
                 : SW_HIDE);
  show_controls({application_state().controls.output_label,
                 application_state().controls.output_edit,
                 application_state().controls.output_browse_button,
                 application_state().controls.output_name_label,
                 application_state().controls.output_name_edit,
                 application_state().controls.format_label,
                 application_state().controls.format_jpeg,
                 application_state().controls.format_png,
                 application_state().controls.format_exr,
                 application_state().controls.quality_label,
                 application_state().controls.quality_edit,
                 application_state().controls.quality_slider,
                 application_state().controls.resolution_label,
                 application_state().controls.resolution_edit,
                 application_state().controls.resolution_slider,
                 application_state().controls.width_label,
                 application_state().controls.width_edit,
                 application_state().controls.resolution_mode_button,
                 application_state().controls.render_button,
                 application_state().controls.render_thumbnail_button},
                output);
  show_controls({application_state().controls.memory_label,
                 application_state().controls.memory_edit,
                 application_state().controls.workers_label,
                 application_state().controls.workers_edit,
                 application_state().controls.gpu_check,
                 application_state().controls.gpu_strict_check,
                 application_state().controls.incomplete_check,
                 application_state().controls.delete_session_button,
                 application_state().controls.delete_images_check,
                 application_state().controls.thumbnail_check,
                 application_state().controls.coverage_check,
                 application_state().controls.blend_label,
                 application_state().controls.blend_combo,
                 application_state().controls.auto_contrast_check},
                false);
  ShowWindow(application_state().controls.resolution_label,
             output && !shell->resolution_pixels ? SW_SHOW : SW_HIDE);
  ShowWindow(application_state().controls.resolution_edit,
             output && !shell->resolution_pixels ? SW_SHOW : SW_HIDE);
  ShowWindow(application_state().controls.resolution_slider,
             output && !shell->resolution_pixels ? SW_SHOW : SW_HIDE);
  ShowWindow(application_state().controls.width_label,
             output && shell->resolution_pixels ? SW_SHOW : SW_HIDE);
  ShowWindow(application_state().controls.width_edit,
             output && shell->resolution_pixels ? SW_SHOW : SW_HIDE);
  SetWindowTextW(application_state().controls.input_stage_button, L"Input");
  SetWindowTextW(application_state().controls.preview_stage_button, L"Preview");
  SetWindowTextW(application_state().controls.output_stage_button, L"Output");
}

void layout_controls(const HWND window) {
  RECT client{};
  if (!GetClientRect(window, &client))
    return;
  const auto *const shell = shell_state(window);
  if (shell == nullptr)
    return;
  const unsigned dpi = GetDpiForWindow(window);
  pano::app::GuiLayoutMetrics metrics;
  std::string error;
  if (!pano::app::calculate_gui_layout_metrics(dpi, client.right - client.left,
                                               metrics, error))
    return;
  const auto scaled = [dpi](const int value) {
    return MulDiv(value, static_cast<int>(dpi), 96);
  };
  const int field_height = metrics.row_height;
  const int label_height = scaled(24);
  const int compact_button = scaled(44);
  const int action_width = scaled(164);
  const int page_margin = scaled(16);
  const int page_gap = scaled(16);
  const int stage_height = scaled(42);
  int y = page_margin;
  const int settings_width = scaled(50);
  const int page_width = std::max(1L, client.right - 2L * page_margin);
  const int stages_width = page_width - settings_width - page_gap;
  const int stage_width = (stages_width - 2 * page_gap) / 3;
  MoveWindow(application_state().controls.input_stage_button, page_margin, y,
             stage_width, stage_height, TRUE);
  MoveWindow(application_state().controls.preview_stage_button,
             page_margin + stage_width + page_gap, y, stage_width, stage_height,
             TRUE);
  MoveWindow(application_state().controls.output_stage_button,
             page_margin + 2 * (stage_width + page_gap), y, stage_width,
             stage_height, TRUE);
  MoveWindow(application_state().controls.settings_button,
             page_margin + stages_width + page_gap, y, settings_width,
             stage_height, TRUE);
  y += stage_height + page_gap;

  const int status_y = std::max(y, static_cast<int>(client.bottom) -
                                       metrics.margin - field_height);
  MoveWindow(application_state().controls.status_label, metrics.margin,
             status_y,
             metrics.content_width - metrics.button_width - metrics.gap,
             field_height, TRUE);
  MoveWindow(application_state().controls.cancel_button,
             metrics.margin + metrics.content_width - metrics.button_width,
             status_y, metrics.button_width, field_height, TRUE);
  MoveWindow(application_state().controls.operation_progress, metrics.margin,
             status_y, 1, 1, TRUE);
  const int action_y = status_y;

  if (shell->workflow.stage == pano::app::GuiStage::input) {
    const int card_padding = scaled(16);
    const int small_gap = scaled(8);
    const int input_control_height = scaled(42);
    const int input_compact_button = scaled(50);
    const int footer_height = scaled(42);
    const int card_height = scaled(500);
    const int card_bottom = y + card_height;
    const int footer_y = card_bottom + page_gap;
    const int card_left = page_margin + card_padding;
    const int card_width = page_width - 2 * card_padding;
    y += card_padding;
    MoveWindow(application_state().controls.game_label, card_left, y,
               card_width, label_height, TRUE);
    y += label_height + small_gap;
    const int refresh_width = input_compact_button;
    const int game_edit_width =
        card_width - 2 * input_compact_button - 2 * page_gap;
    MoveWindow(application_state().controls.game_edit, card_left, y,
               game_edit_width, input_control_height, TRUE);
    MoveWindow(application_state().controls.game_browse_button,
               card_left + game_edit_width + page_gap, y, input_compact_button,
               input_control_height, TRUE);
    MoveWindow(application_state().controls.refresh_button,
               card_left + game_edit_width + input_compact_button +
                   2 * page_gap,
               y, refresh_width, input_control_height, TRUE);
    y += input_control_height + page_gap;
    const int session_header_height = scaled(42);
    const std::array<int, 4> session_widths{
        card_width * 34 / 100, card_width * 6 / 100, card_width * 38 / 100,
        card_width - card_width * 78 / 100};
    const std::array<HWND, 4> session_headers{
        application_state().controls.session_header_session,
        application_state().controls.session_header_poses,
        application_state().controls.session_header_tag,
        application_state().controls.session_header_actions};
    int session_x = card_left;
    for (std::size_t index = 0; index < session_headers.size(); ++index) {
      MoveWindow(session_headers[index], session_x, y, session_widths[index],
                 session_header_height, TRUE);
      ListView_SetColumnWidth(application_state().controls.session_combo,
                              static_cast<int>(index), session_widths[index]);
      session_x += session_widths[index];
    }
    y += session_header_height;
    const int session_body_height = scaled(288) - session_header_height;
    MoveWindow(application_state().controls.session_combo, card_left, y,
               card_width, session_body_height, TRUE);
    y += session_body_height + page_gap;
    const int image_label_y = y;
    MoveWindow(application_state().controls.image_label, card_left,
               image_label_y, card_width, label_height, TRUE);
    const int image_y = image_label_y + label_height + small_gap;
    const int image_edit_width = card_width - input_compact_button - page_gap;
    MoveWindow(application_state().controls.image_edit, card_left, image_y,
               image_edit_width, input_control_height, TRUE);
    MoveWindow(application_state().controls.image_browse_button,
               card_left + image_edit_width + page_gap, image_y,
               input_compact_button, input_control_height, TRUE);
    const int options_width = button_content_width(
        application_state().controls.input_options_button,
        options_icon_width(application_state().controls.input_options_button));
    const int preview_width =
        button_content_width(application_state().controls.preview_button);
    const int preview_x = page_margin + page_width - preview_width;
    MoveWindow(application_state().controls.input_options_button,
               preview_x - page_gap - options_width, footer_y, options_width,
               footer_height, TRUE);
    MoveWindow(application_state().controls.preview_button, preview_x, footer_y,
               preview_width, footer_height, TRUE);
    MoveWindow(
        application_state().controls.status_label, page_margin, footer_y,
        std::max(1, preview_x - 2 * page_gap - options_width - page_margin),
        footer_height, TRUE);
    MoveWindow(application_state().controls.cancel_button, preview_x, footer_y,
               preview_width, footer_height, TRUE);
    MoveWindow(application_state().controls.operation_progress, page_margin,
               card_bottom + scaled(5), page_width, scaled(4), TRUE);
  } else if (shell->workflow.stage == pano::app::GuiStage::preview) {
    const bool preview_ready =
        retained_preview_ready() && !runtime_state().preview_building;
    const int exposure_width = button_content_width(
        application_state().controls.exposure_panel_button,
        fluent_icon_width(application_state().controls.exposure_panel_button,
                          L'\xE76C'));
    const int exposure_y = action_y - 2 * metrics.gap - field_height;
    MoveWindow(application_state().controls.exposure_panel_button,
               metrics.margin + metrics.content_width - exposure_width,
               exposure_y, exposure_width, field_height, TRUE);
    MoveWindow(application_state().controls.preview_options_button,
               metrics.margin + metrics.content_width - action_width * 2 -
                   metrics.gap,
               action_y, action_width, scaled(44), TRUE);
    MoveWindow(application_state().controls.preview_next_button,
               metrics.margin + metrics.content_width - action_width, action_y,
               action_width, scaled(44), TRUE);
    if (!shell->webview_enabled || !shell->webview_preview_visible)
      MoveWindow(
          application_state().controls.preview_surface, metrics.margin, y,
          metrics.content_width,
          std::max(1, (preview_ready ? exposure_y : action_y - metrics.gap) -
                          metrics.gap - y),
          TRUE);
  } else {
    const int field_width =
        metrics.content_width - compact_button - metrics.gap;
    MoveWindow(application_state().controls.output_label, metrics.margin, y,
               metrics.content_width, label_height, TRUE);
    y += label_height;
    MoveWindow(application_state().controls.output_edit, metrics.margin, y,
               field_width, field_height, TRUE);
    MoveWindow(application_state().controls.output_browse_button,
               metrics.margin + field_width + metrics.gap, y, compact_button,
               field_height, TRUE);
    y += field_height + metrics.gap;
    MoveWindow(application_state().controls.output_name_label, metrics.margin,
               y, metrics.content_width, label_height, TRUE);
    y += label_height;
    MoveWindow(application_state().controls.output_name_edit, metrics.margin, y,
               metrics.content_width, field_height, TRUE);
    y += field_height + 2 * metrics.gap;
    const int slider_height = scaled(40);
    const int resolution_mode_width = scaled(76);
    MoveWindow(application_state().controls.resolution_mode_button,
               metrics.margin, y + (slider_height - field_height) / 2,
               resolution_mode_width, field_height, TRUE);
    const int resolution_label_x =
        metrics.margin + resolution_mode_width + metrics.gap;
    MoveWindow(application_state().controls.resolution_label,
               resolution_label_x, y, metrics.label_width, slider_height, TRUE);
    MoveWindow(application_state().controls.width_label, resolution_label_x, y,
               metrics.label_width, slider_height, TRUE);
    const int value_width = scaled(68);
    const int value_x = metrics.margin + metrics.content_width - value_width;
    const int slider_x = resolution_label_x + metrics.label_width;
    MoveWindow(application_state().controls.resolution_slider, slider_x, y,
               std::max(1, value_x - metrics.gap - slider_x), slider_height,
               TRUE);
    MoveWindow(application_state().controls.resolution_edit, value_x,
               y + (slider_height - field_height) / 2, value_width,
               field_height, TRUE);
    MoveWindow(application_state().controls.width_edit, slider_x,
               y + (slider_height - field_height) / 2, scaled(96), field_height,
               TRUE);
    y += slider_height + 2 * metrics.gap;
    MoveWindow(application_state().controls.format_label, metrics.margin, y,
               metrics.content_width, label_height, TRUE);
    y += label_height;
    const int format_width = scaled(154);
    MoveWindow(application_state().controls.format_jpeg, metrics.margin, y,
               format_width, field_height, TRUE);
    MoveWindow(application_state().controls.format_png,
               metrics.margin + format_width + metrics.gap, y, format_width,
               field_height, TRUE);
    MoveWindow(application_state().controls.format_exr,
               metrics.margin + 2 * (format_width + metrics.gap), y,
               format_width, field_height, TRUE);
    y += field_height + 2 * metrics.gap;
    MoveWindow(application_state().controls.quality_label, metrics.margin, y,
               metrics.label_width, slider_height, TRUE);
    const int quality_slider_x = metrics.margin + metrics.label_width;
    MoveWindow(application_state().controls.quality_slider, quality_slider_x, y,
               std::max(1, value_x - metrics.gap - quality_slider_x),
               slider_height, TRUE);
    MoveWindow(application_state().controls.quality_edit, value_x,
               y + (slider_height - field_height) / 2, value_width,
               field_height, TRUE);
    const int thumbnail_width = scaled(224);
    MoveWindow(application_state().controls.render_thumbnail_button,
               metrics.margin + metrics.content_width - action_width -
                   metrics.gap - thumbnail_width,
               action_y, thumbnail_width, scaled(44), TRUE);
    MoveWindow(application_state().controls.render_button,
               metrics.margin + metrics.content_width - action_width, action_y,
               action_width, scaled(44), TRUE);
  }
  if (shell->workflow.stage != pano::app::GuiStage::input) {
    const int abort_width = compact_button_content_width(
        application_state().controls.cancel_button);
    const int status_width = std::min(
        control_text_width(application_state().controls.status_label),
        std::max(1, metrics.content_width - abort_width - metrics.gap));
    MoveWindow(application_state().controls.status_label, metrics.margin,
               status_y, status_width, field_height, TRUE);
    MoveWindow(application_state().controls.cancel_button,
               metrics.margin + status_width + metrics.gap,
               status_y + scaled(5), abort_width,
               std::max(1, field_height - scaled(10)), TRUE);
  }
  update_stage_visibility(window);
}

bool update_preview_surface() {
  RECT bounds{};
  if (!GetClientRect(application_state().controls.preview_surface, &bounds))
    return false;
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
            static_cast<uint32_t>(error.size())) != PANO_GPU_SUCCESS)
      return false;
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
            static_cast<uint32_t>(error.size())) != PANO_GPU_SUCCESS)
      return false;
  } else if (pano_gpu_preview_surface_resize(
                 runtime_state().preview_surface, width, height, error.data(),
                 static_cast<uint32_t>(error.size())) != PANO_GPU_SUCCESS) {
    return false;
  }
  constexpr float background[4]{0.035F, 0.035F, 0.045F, 1.0F};
  return pano_gpu_preview_surface_clear_present(
             runtime_state().preview_surface, background, error.data(),
             static_cast<uint32_t>(error.size())) == PANO_GPU_SUCCESS;
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

bool present_preview_view() {
  if (runtime_state().preview_surface == nullptr ||
      runtime_state().active_preview == nullptr)
    return true;
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
  const auto *const shell = shell_state(
      runtime_state().refresh_window.load(std::memory_order_acquire));
  const bool exposure_open = shell != nullptr &&
                             shell->exposure_window != nullptr &&
                             IsWindowVisible(shell->exposure_window);
  if (!exposure_open)
    std::fill(runtime_state().preview_hovered.begin(),
              runtime_state().preview_hovered.end(), std::uint8_t{0});
  request.target_pose =
      runtime_state().exposure_target.has_value()
          ? static_cast<std::int32_t>(*runtime_state().exposure_target)
          : -1;
  request.target_mode =
      shell != nullptr && shell->exposure_selecting_target ? 1U : 0U;
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
    std::fill(runtime_state().preview_hovered.begin(),
              runtime_state().preview_hovered.end(), std::uint8_t{0});
  }
  layout_controls(window);
  if (shell->webview_enabled)
    sync_webview_snapshot(window);
  if (stage != pano::app::GuiStage::preview || runtime_state().preview_building)
    return;
  if (!update_preview_surface() || !present_preview_view()) {
    discard_active_preview();
    if (!recover_preview_surface())
      SetWindowTextW(application_state().controls.status_label,
                     L"D3D12 preview surface is unavailable");
  }
}

void update_exposure_enablement() {
  const bool ready = runtime_state().active_preview_owner != nullptr &&
                     !runtime_state().preview_building;
  auto *const shell = shell_state(
      runtime_state().refresh_window.load(std::memory_order_acquire));
  EnableWindow(application_state().controls.exposure_panel_button, ready);
  EnableWindow(application_state().controls.automatic_exposure_button,
               ready && runtime_state().exposure_target.has_value());
  EnableWindow(application_state().controls.match_exposure_button,
               ready && runtime_state().exposure_target.has_value() &&
                   !runtime_state().exposure_selected.empty());
  EnableWindow(application_state().controls.discard_exposure_button,
               ready && shell != nullptr && shell->exposure_edits_applied);
  if (shell != nullptr) {
    EnableWindow(shell->exposure_select_target_button, ready);
    EnableWindow(shell->exposure_automatic_button,
                 ready && runtime_state().exposure_target.has_value());
    EnableWindow(shell->exposure_match_button,
                 ready && runtime_state().exposure_target.has_value() &&
                     !runtime_state().exposure_selected.empty());
    EnableWindow(shell->exposure_discard_button,
                 ready && shell->exposure_edits_applied);
    for (const HWND button : shell->exposure_pose_buttons)
      EnableWindow(button, ready);
  }
}

bool retained_preview_ready() {
  return runtime_state().active_preview_owner != nullptr ||
         runtime_state().active_cpu_preview_owner != nullptr;
}

void set_cancel_enabled(const bool enabled) {
  EnableWindow(application_state().controls.cancel_button,
               enabled ? TRUE : FALSE);
  ShowWindow(application_state().controls.cancel_button,
             enabled ? SW_SHOW : SW_HIDE);
  const HWND window =
      runtime_state().refresh_window.load(std::memory_order_acquire);
  if (window != nullptr)
    layout_controls(window);
}

void set_mutating_controls_enabled(const bool enabled) {
  const BOOL value = enabled ? TRUE : FALSE;
  const std::array<HWND, 30> mutating{
      application_state().controls.game_edit,
      application_state().controls.game_browse_button,
      application_state().controls.refresh_button,
      application_state().controls.session_combo,
      application_state().controls.image_edit,
      application_state().controls.image_browse_button,
      application_state().controls.input_options_button,
      application_state().controls.preview_options_button,
      application_state().controls.resolution_mode_button,
      application_state().controls.blend_combo,
      application_state().controls.auto_contrast_check,
      application_state().controls.automatic_exposure_button,
      application_state().controls.match_exposure_button,
      application_state().controls.discard_exposure_button,
      application_state().controls.output_edit,
      application_state().controls.output_browse_button,
      application_state().controls.output_name_edit,
      application_state().controls.format_combo,
      application_state().controls.format_jpeg,
      application_state().controls.format_png,
      application_state().controls.format_exr,
      application_state().controls.quality_edit,
      application_state().controls.quality_slider,
      application_state().controls.resolution_edit,
      application_state().controls.resolution_slider,
      application_state().controls.width_edit,
      application_state().controls.preview_button,
      application_state().controls.render_button,
      application_state().controls.render_thumbnail_button};
  for (const HWND control : mutating)
    EnableWindow(control, value);
  if (enabled) {
    const bool valid = runtime_state().validation_state.plan.has_value();
    EnableWindow(application_state().controls.preview_button, valid);
    EnableWindow(application_state().controls.preview_next_button,
                 retained_preview_ready());
    EnableWindow(application_state().controls.render_button,
                 valid && retained_preview_ready());
    EnableWindow(application_state().controls.render_thumbnail_button,
                 valid && retained_preview_ready());
    update_option_enablement();
    update_exposure_enablement();
  } else {
    EnableWindow(application_state().controls.preview_next_button, FALSE);
  }
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
  ShowWindow(application_state().controls.operation_progress, SW_HIDE);
  if (auto *const shell = shell_state(window); shell != nullptr)
    shell->operation_progress_percent = 0U;
  InvalidateRect(application_state().controls.preview_stage_button, nullptr,
                 FALSE);
  InvalidateRect(application_state().controls.output_stage_button, nullptr,
                 FALSE);
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
  InvalidateRect(application_state().controls.preview_stage_button, nullptr,
                 FALSE);
  InvalidateRect(application_state().controls.output_stage_button, nullptr,
                 FALSE);
  if (application_state().taskbar != nullptr) {
    application_state().taskbar->SetProgressState(window, TBPF_NORMAL);
    application_state().taskbar->SetProgressValue(window, completed, total);
  }
}

void end_operation_progress(const HWND window) {
  ShowWindow(application_state().controls.operation_progress, SW_HIDE);
  if (auto *const shell = shell_state(window); shell != nullptr)
    shell->operation_progress_percent = 0U;
  InvalidateRect(application_state().controls.preview_stage_button, nullptr,
                 FALSE);
  InvalidateRect(application_state().controls.output_stage_button, nullptr,
                 FALSE);
  if (application_state().taskbar != nullptr)
    application_state().taskbar->SetProgressState(window, TBPF_NOPROGRESS);
}

void notify_operation_complete() { MessageBeep(MB_OK); }

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
  SetWindowTextW(application_state().controls.status_label,
                 utf8_to_wide(phase).c_str());
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
      SetWindowTextW(application_state().controls.status_label,
                     utf8_to_wide(error).c_str());
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
    shell->exposure_selecting_target = false;
    shell->exposure_edits_applied = false;
    if (shell->exposure_window != nullptr)
      ShowWindow(shell->exposure_window, SW_HIDE);
  }
  pano::app::reset_gui_preview_view(runtime_state().preview_view);
  if (const HWND window =
          runtime_state().refresh_window.load(std::memory_order_acquire);
      window != nullptr) {
    if (auto *const workflow_shell = shell_state(window);
        workflow_shell != nullptr)
      workflow_shell->workflow.preview_ready = false;
  }
  EnableWindow(application_state().controls.preview_next_button, FALSE);
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
  if (!runtime_state().validation_state.plan->use_gpu) {
    SetWindowTextW(application_state().controls.status_label,
                   L"Native preview currently requires the GPU option");
    return;
  }
  RECT bounds{};
  if (!GetClientRect(application_state().controls.preview_surface, &bounds) ||
      bounds.right <= 0) {
    SetWindowTextW(application_state().controls.status_label,
                   L"Preview surface has no usable size");
    return;
  }
  if (runtime_state().preview_device == nullptr && !update_preview_surface()) {
    SetWindowTextW(application_state().controls.status_label,
                   L"D3D12 preview device is unavailable");
    return;
  }
  std::uint64_t operation_generation = 0;
  if (!begin_owned_operation(pano::app::GuiOperation::preview,
                             operation_generation))
    return;
  const bool reuse = runtime_state().active_preview_owner != nullptr;
  auto *const retained_owner = runtime_state().active_preview_owner;
  if (!reuse) {
    discard_active_preview();
  } else {
    runtime_state().active_preview = nullptr;
    if (auto *const shell = shell_state(
            runtime_state().refresh_window.load(std::memory_order_acquire));
        shell != nullptr)
      shell->workflow.preview_ready = false;
    InvalidateRect(application_state().controls.preview_stage_button, nullptr,
                   FALSE);
  }
  pano_gpu_cancellation_token_destroy(&runtime_state().preview_cancellation);
  std::array<char, 512> gpu_error{};
  if (pano_gpu_cancellation_token_create(
          &runtime_state().preview_cancellation, gpu_error.data(),
          static_cast<uint32_t>(gpu_error.size())) != PANO_GPU_SUCCESS) {
    SetWindowTextW(application_state().controls.status_label,
                   L"Cannot create preview cancellation state");
    complete_owned_operation(operation_generation);
    return;
  }
  const std::uint64_t generation = ++runtime_state().preview_generation;
  const unsigned viewport_width = static_cast<unsigned>(bounds.right);
  auto result = std::make_unique<PreviewResult>();
  result->generation = generation;
  result->operation_generation = operation_generation;
  result->reused = reuse;
  runtime_state().preview_building = true;
  EnableWindow(application_state().controls.preview_button, FALSE);
  EnableWindow(application_state().controls.render_button, FALSE);
  EnableWindow(application_state().controls.render_thumbnail_button, FALSE);
  set_cancel_enabled(true);
  SetWindowTextW(application_state().controls.status_label,
                 L"Building D3D12 preview...");
  if (const HWND window =
          runtime_state().refresh_window.load(std::memory_order_acquire);
      window != nullptr)
    layout_controls(window);
  try {
    runtime_state().preview_threads.emplace_back(
        [viewport_width, retained_owner,
         plan = *runtime_state().validation_state.plan,
         result = std::move(result)]() mutable {
          pano::app::NativePreviewOptions options;
          options.viewport_width = viewport_width;
          options.gpu_cancellation = runtime_state().preview_cancellation;
          options.progress = report_preview_progress;
          if (result->reused) {
            result->succeeded = pano::app::rebuild_native_preview(
                retained_owner, plan, options, result->error);
          } else {
            result->succeeded = pano::app::create_native_preview(
                runtime_state().preview_device, plan, options, &result->preview,
                result->error);
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
    if (reuse) {
      runtime_state().active_preview = pano::app::native_preview_handle(
          runtime_state().active_preview_owner);
      if (auto *const shell = shell_state(
              runtime_state().refresh_window.load(std::memory_order_acquire));
          shell != nullptr)
        shell->workflow.preview_ready = true;
    }
    pano_gpu_cancellation_token_destroy(&runtime_state().preview_cancellation);
    EnableWindow(application_state().controls.preview_button, TRUE);
    EnableWindow(application_state().controls.render_button, TRUE);
    set_cancel_enabled(false);
    complete_owned_operation(operation_generation);
    SetWindowTextW(application_state().controls.status_label,
                   L"Cannot start preview worker");
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
    if (result->generation != runtime_state().preview_generation) {
      if (result->reused)
        discard_active_preview();
      complete_owned_operation(result->operation_generation);
      continue;
    }
    if (!result->succeeded || (!result->reused && result->preview == nullptr)) {
      if (result->reused) {
        runtime_state().active_preview = pano::app::native_preview_handle(
            runtime_state().active_preview_owner);
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
      }
      complete_owned_operation(result->operation_generation);
      SetWindowTextW(application_state().controls.status_label,
                     utf8_to_wide(result->error).c_str());
      continue;
    }
    pano::app::NativePreview *const owner =
        result->reused ? runtime_state().active_preview_owner : result->preview;
    pano::app::NativePreviewDiagnostics diagnostics;
    if (!pano::app::query_native_preview(owner, diagnostics, result->error)) {
      if (result->reused)
        discard_active_preview();
      complete_owned_operation(result->operation_generation);
      SetWindowTextW(application_state().controls.status_label,
                     utf8_to_wide(result->error).c_str());
      continue;
    }
    if (!result->reused) {
      runtime_state().active_preview_owner = result->preview;
      result->preview = nullptr;
    }
    runtime_state().active_preview =
        pano::app::native_preview_handle(runtime_state().active_preview_owner);
    runtime_state().preview_source_width = diagnostics.preview_width;
    runtime_state().preview_source_height = diagnostics.preview_height;
    runtime_state().preview_viewport_width = diagnostics.overview_width;
    runtime_state().preview_viewport_height = diagnostics.overview_height;
    runtime_state().preview_mask_width = diagnostics.mask_width;
    runtime_state().preview_mask_height = diagnostics.mask_height;
    runtime_state().preview_hovered.assign(diagnostics.frame_count, 0U);
    runtime_state().preview_overlay_frames.assign(diagnostics.frame_count, 0U);
    rebuild_exposure_pose_grid();
    pano::app::reset_gui_preview_view(runtime_state().preview_view);
    const HWND window =
        runtime_state().refresh_window.load(std::memory_order_acquire);
    if (window != nullptr) {
      if (auto *const shell = shell_state(window); shell != nullptr)
        shell->workflow.preview_ready = true;
      layout_controls(window);
    }
    if (!update_preview_surface() || !present_preview_view()) {
      discard_active_preview();
      complete_owned_operation(result->operation_generation);
      SetWindowTextW(application_state().controls.status_label,
                     L"Cannot present the retained D3D12 preview");
      continue;
    }
    if (window != nullptr)
      update_operation_progress(window, 100U, 100U);
    complete_owned_operation(result->operation_generation);
    EnableWindow(application_state().controls.preview_next_button, TRUE);
    SetWindowTextW(application_state().controls.status_label,
                   L"D3D12 preview ready");
    notify_operation_complete();
  }
  pano_gpu_cancellation_token_destroy(&runtime_state().preview_cancellation);
  const bool valid = runtime_state().validation_state.plan.has_value();
  EnableWindow(application_state().controls.preview_button, valid);
  EnableWindow(application_state().controls.render_button,
               valid && runtime_state().active_preview_owner != nullptr);
  EnableWindow(application_state().controls.render_thumbnail_button,
               valid && runtime_state().active_preview_owner != nullptr);
  set_cancel_enabled(false);
  update_exposure_enablement();
}

void start_exposure(const ExposureCommand command) {
  if (runtime_state().active_preview_owner == nullptr ||
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
    SetWindowTextW(application_state().controls.status_label,
                   L"Cannot create exposure cancellation state");
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
  runtime_state().active_preview = nullptr;
  runtime_state().preview_building = true;
  if (auto *const shell = shell_state(
          runtime_state().refresh_window.load(std::memory_order_acquire));
      shell != nullptr) {
    shell->workflow.preview_ready = false;
    InvalidateRect(application_state().controls.preview_stage_button, nullptr,
                   FALSE);
  }
  EnableWindow(application_state().controls.preview_button, FALSE);
  EnableWindow(application_state().controls.render_button, FALSE);
  EnableWindow(application_state().controls.render_thumbnail_button, FALSE);
  set_cancel_enabled(true);
  update_exposure_enablement();
  SetWindowTextW(application_state().controls.status_label,
                 command == ExposureCommand::discard
                     ? L"Resetting exposure preview..."
                     : L"Measuring exposure overlaps...");
  if (const HWND window =
          runtime_state().refresh_window.load(std::memory_order_acquire);
      window != nullptr)
    layout_controls(window);
  try {
    runtime_state().preview_threads.emplace_back(
        [command, target, selected, owner,
         result = std::move(result)]() mutable {
          pano::app::NativePreviewOptions options;
          options.gpu_cancellation = runtime_state().preview_cancellation;
          options.progress = report_preview_progress;
          if (command == ExposureCommand::automatic)
            result->succeeded = pano::app::apply_native_automatic_exposure(
                owner, *target, options, result->exposure, result->error);
          else if (command == ExposureCommand::manual_match)
            result->succeeded = pano::app::apply_native_manual_exposure_match(
                owner, *target, selected, options, result->exposure,
                result->error);
          else
            result->succeeded = pano::app::discard_native_exposure_edits(
                owner, options, result->exposure, result->error);
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
        pano::app::native_preview_handle(runtime_state().active_preview_owner);
    if (auto *const shell = shell_state(
            runtime_state().refresh_window.load(std::memory_order_acquire));
        shell != nullptr)
      shell->workflow.preview_ready = true;
    pano_gpu_cancellation_token_destroy(&runtime_state().preview_cancellation);
    EnableWindow(application_state().controls.preview_button, TRUE);
    EnableWindow(application_state().controls.render_button, TRUE);
    set_cancel_enabled(false);
    update_exposure_enablement();
    complete_owned_operation(operation_generation);
    SetWindowTextW(application_state().controls.status_label,
                   L"Cannot start exposure worker");
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
        pano::app::native_preview_handle(runtime_state().active_preview_owner);
    const HWND window =
        runtime_state().refresh_window.load(std::memory_order_acquire);
    if (!result->succeeded) {
      if (auto *const shell = window == nullptr ? nullptr : shell_state(window);
          shell != nullptr)
        shell->workflow.preview_ready = true;
      if (window != nullptr)
        layout_controls(window);
      complete_owned_operation(result->operation_generation);
      SetWindowTextW(application_state().controls.status_label,
                     utf8_to_wide(result->error).c_str());
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
      SetWindowTextW(application_state().controls.status_label,
                     L"Cannot present recomputed exposure preview");
      continue;
    }
    if (window != nullptr)
      update_operation_progress(window, 100U, 100U);
    complete_owned_operation(result->operation_generation);
    const std::wstring status =
        result->exposure.warning
            ? L"Exposure applied with disconnected-pose warning"
            : L"Exposure preview updated";
    SetWindowTextW(application_state().controls.status_label, status.c_str());
    notify_operation_complete();
  }
  pano_gpu_cancellation_token_destroy(&runtime_state().preview_cancellation);
  const bool valid = runtime_state().validation_state.plan.has_value();
  EnableWindow(application_state().controls.preview_button, valid);
  EnableWindow(application_state().controls.render_button,
               valid && runtime_state().active_preview_owner != nullptr);
  EnableWindow(application_state().controls.render_thumbnail_button,
               valid && runtime_state().active_preview_owner != nullptr);
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
  refresh_exposure_pose_labels();
  update_exposure_enablement();
  sync_exposure_webview_snapshot(window);
  sync_webview_snapshot(window);
  present_preview_view();
}

LRESULT CALLBACK preview_window_procedure(const HWND window, const UINT message,
                                          const WPARAM wparam,
                                          const LPARAM lparam) {
  switch (message) {
  case WM_MOUSEMOVE:
    if (runtime_state().active_preview != nullptr &&
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

void refresh_exposure_pose_labels() {
  auto *const shell = shell_state(
      runtime_state().refresh_window.load(std::memory_order_acquire));
  if (shell == nullptr)
    return;
  for (std::size_t index = 0; index < shell->exposure_pose_buttons.size();
       ++index) {
    const bool target = runtime_state().exposure_target == index;
    const bool selected =
        std::find(runtime_state().exposure_selected.begin(),
                  runtime_state().exposure_selected.end(),
                  index) != runtime_state().exposure_selected.end();
    const std::wstring label = (target     ? L"Target  "
                                : selected ? L"Selected  "
                                           : L"") +
                               std::to_wstring(index + 1U);
    SetWindowTextW(shell->exposure_pose_buttons[index], label.c_str());
    InvalidateRect(shell->exposure_pose_buttons[index], nullptr, FALSE);
  }
}

LRESULT CALLBACK exposure_pose_procedure(const HWND window, const UINT message,
                                         const WPARAM wparam,
                                         const LPARAM lparam, UINT_PTR,
                                         DWORD_PTR) {
  const int identifier = GetDlgCtrlID(window) - exposure_pose_id_base;
  if (identifier >= 0 && static_cast<std::size_t>(identifier) <
                             runtime_state().preview_hovered.size()) {
    if (message == WM_MOUSEMOVE) {
      std::fill(runtime_state().preview_hovered.begin(),
                runtime_state().preview_hovered.end(), std::uint8_t{0});
      runtime_state().preview_hovered[static_cast<std::size_t>(identifier)] =
          1U;
      TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
      TrackMouseEvent(&tracking);
      present_preview_view();
    } else if (message == WM_MOUSELEAVE) {
      std::fill(runtime_state().preview_hovered.begin(),
                runtime_state().preview_hovered.end(), std::uint8_t{0});
      present_preview_view();
    }
  }
  if (message == WM_NCDESTROY)
    RemoveWindowSubclass(window, exposure_pose_procedure, 1U);
  return DefSubclassProc(window, message, wparam, lparam);
}

void layout_exposure_panel(const HWND window) {
  auto *const shell = shell_state(GetWindow(window, GW_OWNER));
  if (shell == nullptr)
    return;
  RECT client{};
  GetClientRect(window, &client);
  const unsigned dpi = GetDpiForWindow(window);
  const auto scaled = [dpi](const int value) {
    return MulDiv(value, static_cast<int>(dpi), 96);
  };
  const int margin = scaled(16);
  const int gap = scaled(10);
  const int field_height = scaled(36);
  const int content_width = std::max(1L, client.right - 2L * margin);
  int y = margin;
  MoveWindow(shell->exposure_overlay_check, margin, y, content_width,
             field_height, TRUE);
  y += field_height + gap;
  MoveWindow(shell->exposure_select_target_button, margin, y, content_width,
             field_height, TRUE);
  y += field_height + gap;
  const int columns = 3;
  const int pose_width = (content_width - (columns - 1) * gap) / columns;
  for (std::size_t index = 0; index < shell->exposure_pose_buttons.size();
       ++index) {
    const int row = static_cast<int>(index) / columns;
    const int column = static_cast<int>(index) % columns;
    MoveWindow(shell->exposure_pose_buttons[index],
               margin + column * (pose_width + gap),
               y + row * (field_height + gap), pose_width, field_height, TRUE);
  }
  const int rows = static_cast<int>(
      (shell->exposure_pose_buttons.size() + columns - 1U) / columns);
  y += rows * (field_height + gap) + gap;
  MoveWindow(shell->exposure_match_button, margin, y, content_width,
             field_height, TRUE);
  y += field_height + gap;
  MoveWindow(shell->exposure_automatic_button, margin, y, content_width,
             field_height, TRUE);
  y += field_height + gap;
  MoveWindow(shell->exposure_discard_button, margin, y, content_width,
             field_height, TRUE);
}

void position_exposure_panel(const HWND owner) {
  auto *const shell = shell_state(owner);
  if (shell == nullptr || shell->exposure_window == nullptr)
    return;
  RECT owner_bounds{};
  GetWindowRect(owner, &owner_bounds);
  const unsigned dpi = GetDpiForWindow(owner);
  const int width = MulDiv(340, static_cast<int>(dpi), 96);
  const int pose_rows = std::max(
      1, static_cast<int>((shell->exposure_pose_buttons.size() + 2U) / 3U));
  const int legacy_height =
      MulDiv(250 + pose_rows * 46, static_cast<int>(dpi), 96);
  const int height =
      shell->webview_enabled && shell->exposure_content_height.has_value()
          ? webview_outer_height(shell->exposure_window,
                                 *shell->exposure_content_height)
                .value_or(MulDiv(520, static_cast<int>(dpi), 96))
      : shell->webview_enabled ? MulDiv(520, static_cast<int>(dpi), 96)
                               : legacy_height;
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
  auto *const shell = owner == nullptr ? nullptr : shell_state(owner);
  if (shell == nullptr || shell->exposure_window == nullptr)
    return;
  if (shell->webview_enabled) {
    sync_exposure_webview_snapshot(owner);
    return;
  }
  for (const HWND button : shell->exposure_pose_buttons)
    DestroyWindow(button);
  shell->exposure_pose_buttons.clear();
  shell->exposure_pose_buttons.reserve(runtime_state().preview_hovered.size());
  for (std::size_t index = 0; index < runtime_state().preview_hovered.size();
       ++index) {
    HWND button =
        child(shell->exposure_window, WC_BUTTONW,
              std::to_wstring(index + 1U).c_str(), WS_TABSTOP | BS_OWNERDRAW,
              exposure_pose_id_base + static_cast<int>(index));
    if (button == nullptr)
      continue;
    SendMessageW(button, WM_SETFONT,
                 reinterpret_cast<WPARAM>(application_state().body_font), TRUE);
    SetWindowSubclass(button, exposure_pose_procedure, 1U, 0U);
    shell->exposure_pose_buttons.push_back(button);
  }
  refresh_exposure_pose_labels();
  position_exposure_panel(owner);
  layout_exposure_panel(shell->exposure_window);
}

void show_exposure_panel(const HWND owner) {
  auto *const shell = shell_state(owner);
  if (shell == nullptr)
    return;
  if (shell->exposure_window != nullptr &&
      IsWindowVisible(shell->exposure_window)) {
    ShowWindow(shell->exposure_window, SW_HIDE);
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
    if (!shell->webview_enabled)
      rebuild_exposure_pose_grid();
  }
  position_exposure_panel(owner);
  ShowWindow(shell->exposure_window, SW_SHOWNOACTIVATE);
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
  case WM_CREATE:
    if (shell == nullptr)
      return -1;
    apply_dark_caption(window);
    if (shell->webview_enabled) {
      shell->exposure_webview = std::make_unique<pano::app::WebViewHost>(
          window,
          [owner](const pano::app::WebViewCommand &command) {
            handle_exposure_webview_command(owner, command);
          },
          pano::app::WebViewPage::exposure);
      std::wstring error;
      if (!shell->exposure_webview->start(error)) {
        shell->exposure_webview.reset();
        MessageBoxW(owner, error.c_str(), L"Exposure", MB_ICONERROR | MB_OK);
        return -1;
      }
      return 0;
    }
    shell->exposure_overlay_check =
        child(window, WC_BUTTONW, L"Show boundaries overlay",
              WS_TABSTOP | BS_AUTOCHECKBOX, exposure_overlay_id);
    shell->exposure_select_target_button =
        child(window, WC_BUTTONW, L"Select target pose",
              WS_TABSTOP | BS_OWNERDRAW, exposure_select_target_id);
    shell->exposure_match_button =
        child(window, WC_BUTTONW, L"Match exposure", WS_TABSTOP | BS_OWNERDRAW,
              match_exposure_id);
    shell->exposure_automatic_button =
        child(window, WC_BUTTONW, L"Automatic correction",
              WS_TABSTOP | BS_OWNERDRAW, automatic_exposure_id);
    shell->exposure_discard_button =
        child(window, WC_BUTTONW, L"Discard exposure changes",
              WS_TABSTOP | BS_OWNERDRAW, discard_exposure_id);
    for (const HWND control :
         {shell->exposure_overlay_check, shell->exposure_select_target_button,
          shell->exposure_match_button, shell->exposure_automatic_button,
          shell->exposure_discard_button})
      SendMessageW(control, WM_SETFONT,
                   reinterpret_cast<WPARAM>(application_state().body_font),
                   TRUE);
    update_exposure_enablement();
    return 0;
  case WM_SIZE:
    if (shell != nullptr && shell->exposure_webview != nullptr) {
      const RECT bounds{0, 0, LOWORD(lparam), HIWORD(lparam)};
      shell->exposure_webview->resize(bounds);
      return 0;
    }
    layout_exposure_panel(window);
    return 0;
  case WM_CTLCOLORSTATIC:
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORBTN:
    return dark_control_color(message, wparam);
  case WM_DRAWITEM: {
    const auto *const draw = reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
    if (draw != nullptr && draw_modal_button(*draw))
      return TRUE;
    break;
  }
  case WM_COMMAND:
    if (shell == nullptr)
      break;
    if (LOWORD(wparam) == exposure_overlay_id && HIWORD(wparam) == BN_CLICKED) {
      shell->exposure_overlay_boundaries =
          checked(shell->exposure_overlay_check);
      present_preview_view();
      return 0;
    }
    if (LOWORD(wparam) == exposure_select_target_id &&
        HIWORD(wparam) == BN_CLICKED) {
      shell->exposure_selecting_target = true;
      SetWindowTextW(shell->exposure_select_target_button,
                     L"Choose a pose below...");
      present_preview_view();
      return 0;
    }
    if (LOWORD(wparam) >= exposure_pose_id_base &&
        HIWORD(wparam) == BN_CLICKED) {
      const unsigned pose =
          static_cast<unsigned>(LOWORD(wparam) - exposure_pose_id_base);
      if (pose >= runtime_state().preview_hovered.size())
        return 0;
      if (shell->exposure_selecting_target) {
        runtime_state().exposure_target = pose;
        runtime_state().exposure_selected.erase(
            std::remove(runtime_state().exposure_selected.begin(),
                        runtime_state().exposure_selected.end(), pose),
            runtime_state().exposure_selected.end());
        shell->exposure_selecting_target = false;
        SetWindowTextW(shell->exposure_select_target_button,
                       L"Select target pose");
      } else if (runtime_state().exposure_target != pose) {
        const auto found =
            std::find(runtime_state().exposure_selected.begin(),
                      runtime_state().exposure_selected.end(), pose);
        if (found == runtime_state().exposure_selected.end())
          runtime_state().exposure_selected.push_back(pose);
        else
          runtime_state().exposure_selected.erase(found);
        std::sort(runtime_state().exposure_selected.begin(),
                  runtime_state().exposure_selected.end());
      }
      refresh_exposure_pose_labels();
      update_exposure_enablement();
      present_preview_view();
      return 0;
    }
    if ((LOWORD(wparam) == automatic_exposure_id ||
         LOWORD(wparam) == match_exposure_id ||
         LOWORD(wparam) == discard_exposure_id) &&
        HIWORD(wparam) == BN_CLICKED) {
      SendMessageW(owner, WM_COMMAND, wparam, lparam);
      return 0;
    }
    break;
  case WM_CLOSE:
    ShowWindow(window, SW_HIDE);
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
      shell->exposure_overlay_check = nullptr;
      shell->exposure_select_target_button = nullptr;
      shell->exposure_automatic_button = nullptr;
      shell->exposure_match_button = nullptr;
      shell->exposure_discard_button = nullptr;
      shell->exposure_pose_buttons.clear();
    }
    return 0;
  default:
    break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void schedule_validation(HWND window, bool discard_preview = true);
void confirm_render(HWND window);

void reset_session_for_game_change(const HWND window) {
  auto *const shell = shell_state(window);
  if (shell == nullptr)
    return;
  pano::app::begin_gui_session_refresh(runtime_state().refresh_state);
  shell->selected_record.reset();
  shell->manual_session_path.clear();
  shell->workflow.session_selected = false;
  clear_session_accessible_names();
  ListView_DeleteAllItems(application_state().controls.session_combo);
  EnableWindow(application_state().controls.delete_session_button, FALSE);
  invalidate_preview();
  pano::app::begin_gui_validation(runtime_state().validation_state);
  EnableWindow(application_state().controls.preview_button, FALSE);
  EnableWindow(application_state().controls.render_button, FALSE);
  EnableWindow(application_state().controls.render_thumbnail_button, FALSE);
  EnableWindow(application_state().controls.render_thumbnail_button, FALSE);
}

void apply_session_selection(const HWND window, const std::size_t index) {
  auto *const shell = shell_state(window);
  if (shell == nullptr || index >= runtime_state().refresh_state.records.size())
    return;
  shell->selected_record = index;
  shell->manual_session_path.clear();
  shell->workflow.session_selected = true;
  const auto &record = runtime_state().refresh_state.records[index];
  shell->suppress_control_changes = true;
  if (!record.image_paths.empty()) {
    const auto parent =
        std::filesystem::u8path(record.image_paths.front()).parent_path();
    SetWindowTextW(application_state().controls.image_edit,
                   utf8_to_wide(parent.u8string()).c_str());
  }
  if (!record.session.session_id.empty()) {
    const auto format = combo_value(application_state().controls.format_combo,
                                    {"jpeg", "png", "exr"});
    const std::string extension = format == "jpeg" ? ".jpg" : "." + format;
    SetWindowTextW(
        application_state().controls.output_name_edit,
        utf8_to_wide("panorama-" + record.session.session_id + extension)
            .c_str());
  }
  shell->suppress_control_changes = false;
  EnableWindow(application_state().controls.delete_session_button, TRUE);
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
        SetWindowTextW(application_state().controls.status_label,
                       utf8_to_wide(result->error).c_str());
      continue;
    }
    if (!pano::app::complete_gui_session_refresh(runtime_state().refresh_state,
                                                 result->generation,
                                                 std::move(result->records)))
      continue;
    clear_session_accessible_names();
    ListView_DeleteAllItems(application_state().controls.session_combo);
    const std::string game_directory =
        wide_to_utf8(window_text(application_state().controls.game_edit));
    for (std::size_t index = 0;
         index < runtime_state().refresh_state.records.size(); ++index) {
      const auto &record = runtime_state().refresh_state.records[index];
      const std::wstring label = utf8_to_wide(
          pano::app::gui_session_local_label(record.session.session_id));
      LVITEMW item{};
      item.mask = LVIF_TEXT | LVIF_PARAM;
      item.iItem = static_cast<int>(index);
      item.pszText = const_cast<wchar_t *>(label.c_str());
      item.lParam = static_cast<LPARAM>(index);
      const int row = static_cast<int>(
          SendMessageW(application_state().controls.session_combo,
                       LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)));
      const std::wstring poses = std::to_wstring(record.session.frames.size());
      set_list_text(application_state().controls.session_combo, row, 1, poses);
      const auto tag = pano::app::application_session_tag(
          application_state().application_settings, game_directory,
          record.session.session_id);
      const std::wstring tag_text = utf8_to_wide(tag.value_or(""));
      set_list_text(application_state().controls.session_combo, row, 2,
                    tag_text);
      set_list_text(application_state().controls.session_combo, row, 3,
                    L"Actions");
      update_session_accessible_name(index);
    }
    if (auto *const shell =
            shell_state(GetParent(application_state().controls.session_combo));
        shell != nullptr) {
      shell->selected_record.reset();
      shell->manual_session_path.clear();
      shell->workflow.session_selected = false;
    }
    EnableWindow(application_state().controls.delete_session_button, FALSE);
    if (!runtime_state().refresh_state.records.empty()) {
      apply_session_selection(
          GetParent(application_state().controls.session_combo), 0U);
      ListView_SetItemState(application_state().controls.session_combo, 0,
                            LVIS_SELECTED | LVIS_FOCUSED,
                            LVIS_SELECTED | LVIS_FOCUSED);
    }
    const std::wstring status =
        std::to_wstring(runtime_state().refresh_state.records.size()) +
        L" session(s) found";
    SetWindowTextW(application_state().controls.status_label, status.c_str());
  }
}

void start_refresh() {
  const std::string game_directory =
      wide_to_utf8(window_text(application_state().controls.game_edit));
  if (game_directory.empty()) {
    SetWindowTextW(application_state().controls.status_label,
                   L"Choose a game folder first");
    return;
  }
  const std::uint64_t generation =
      pano::app::begin_gui_session_refresh(runtime_state().refresh_state);
  SetWindowTextW(application_state().controls.status_label,
                 L"Refreshing sessions...");
  try {
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
    SetWindowTextW(application_state().controls.status_label,
                   L"Cannot start session refresh");
  }
}

void schedule_validation(const HWND window, const bool discard_preview) {
  if (discard_preview)
    invalidate_preview();
  pano::app::begin_gui_validation(runtime_state().validation_state);
  EnableWindow(application_state().controls.preview_button, FALSE);
  EnableWindow(application_state().controls.render_button, FALSE);
  KillTimer(window, validation_timer_id);
  SetTimer(window, validation_timer_id, 300U, nullptr);
}

void schedule_preview_option_validation(const HWND window) {
  const bool preview_was_ready =
      runtime_state().active_preview_owner != nullptr;
  if (preview_was_ready)
    if (auto *const shell = shell_state(window); shell != nullptr)
      shell->rebuild_preview_after_validation = true;
  schedule_validation(window, !preview_was_ready);
}

void start_validation() {
  pano::app::GuiRenderRequestState request;
  std::string error;
  if (!capture_gui_request(request, error)) {
    runtime_state().validation_state.error = error;
    SetWindowTextW(application_state().controls.status_label,
                   utf8_to_wide(error).c_str());
    return;
  }
  pano::app::RenderOptions options;
  if (!pano::app::snapshot_gui_render_request(request, options, error)) {
    runtime_state().validation_state.error = error;
    SetWindowTextW(application_state().controls.status_label,
                   utf8_to_wide(error).c_str());
    return;
  }
  const std::uint64_t generation = runtime_state().validation_state.generation;
  SetWindowTextW(application_state().controls.status_label,
                 L"Validating session...");
  try {
    runtime_state().validation_threads.emplace_back(
        [generation, options = std::move(options)] {
          auto result = std::make_unique<ValidationResult>();
          result->generation = generation;
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
    SetWindowTextW(application_state().controls.status_label,
                   L"Cannot start validation");
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
    bool retained_update_failed = false;
    if (valid && runtime_state().active_preview_owner != nullptr &&
        (shell == nullptr || !shell->rebuild_preview_after_validation)) {
      std::string update_error;
      if (!pano::app::update_native_preview_render_plan(
              runtime_state().active_preview_owner,
              *runtime_state().validation_state.plan, update_error)) {
        discard_active_preview();
        valid = false;
        retained_update_failed = true;
        SetWindowTextW(application_state().controls.status_label,
                       utf8_to_wide(update_error).c_str());
      }
    }
    EnableWindow(application_state().controls.preview_button,
                 valid && !runtime_state().preview_building);
    EnableWindow(application_state().controls.render_button,
                 valid && !runtime_state().preview_building &&
                     runtime_state().active_preview_owner != nullptr);
    EnableWindow(application_state().controls.render_thumbnail_button,
                 valid && !runtime_state().preview_building &&
                     runtime_state().active_preview_owner != nullptr);
    if (!retained_update_failed)
      SetWindowTextW(
          application_state().controls.status_label,
          valid ? L"Session and options are valid"
                : utf8_to_wide(runtime_state().validation_state.error).c_str());
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
      runtime_state().active_preview_owner == nullptr ||
      runtime_state().preview_building)
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
    SetWindowTextW(application_state().controls.status_label,
                   L"Cannot create render cancellation state");
    complete_owned_operation(operation_generation);
    return;
  }
  const std::uint64_t generation = ++runtime_state().preview_generation;
  auto result = std::make_unique<RenderResult>();
  result->generation = generation;
  result->operation_generation = operation_generation;
  auto *const owner = runtime_state().active_preview_owner;
  runtime_state().preview_building = true;
  EnableWindow(application_state().controls.preview_button, FALSE);
  EnableWindow(application_state().controls.render_button, FALSE);
  set_cancel_enabled(true);
  update_exposure_enablement();
  runtime_state().render_progress_completed.store(0, std::memory_order_release);
  runtime_state().render_progress_total.store(0, std::memory_order_release);
  SetWindowTextW(application_state().controls.status_label,
                 L"Rendering panorama...");
  try {
    runtime_state().preview_threads.emplace_back(
        [owner, result = std::move(result)]() mutable {
          pano::app::NativeRenderOptions options;
          options.gpu_cancellation = runtime_state().preview_cancellation;
          options.progress = report_render_progress;
          result->succeeded = pano::app::render_native_session(
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
    EnableWindow(application_state().controls.preview_button, TRUE);
    EnableWindow(application_state().controls.render_button, TRUE);
    set_cancel_enabled(false);
    update_exposure_enablement();
    complete_owned_operation(operation_generation);
    SetWindowTextW(application_state().controls.status_label,
                   L"Cannot start render worker");
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
  const std::wstring status = utf8_to_wide(phase) + L": " +
                              std::to_wstring(completed) + L"/" +
                              std::to_wstring(total);
  SetWindowTextW(application_state().controls.status_label, status.c_str());
  const HWND window =
      runtime_state().refresh_window.load(std::memory_order_acquire);
  if (window != nullptr)
    set_operation_title(window, utf8_to_wide(phase), completed, total);
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
        pano::app::native_preview_handle(runtime_state().active_preview_owner);
    if (!result->succeeded) {
      const bool was_cancelled =
          result->error.find("cancel") != std::string::npos;
      SetWindowTextW(application_state().controls.status_label,
                     was_cancelled ? L"Render cancelled"
                                   : utf8_to_wide(result->error).c_str());
      if (!present_retained_preview_if_visible())
        discard_active_preview();
      continue;
    }
    const std::wstring status =
        L"Published " + std::to_wstring(result->render.published_paths.size()) +
        L" output file(s)";
    SetWindowTextW(application_state().controls.status_label, status.c_str());
    if (runtime_state().validation_state.plan.has_value() &&
        !result->render.published_paths.empty()) {
      const std::string output_name =
          std::filesystem::u8path(result->render.published_paths.front())
              .filename()
              .u8string();
      pano::app::mark_application_session_stitched(
          application_state().application_settings,
          wide_to_utf8(window_text(application_state().controls.game_edit)),
          runtime_state().validation_state.plan->session.session_id,
          output_name);
      save_gui_settings();
      if (application_state().selected_record.has_value())
        update_session_accessible_name(*application_state().selected_record);
    }
    notify_operation_complete();
    if (!present_retained_preview_if_visible())
      discard_active_preview();
  }
  pano_gpu_cancellation_token_destroy(&runtime_state().preview_cancellation);
  const bool valid = runtime_state().validation_state.plan.has_value();
  EnableWindow(application_state().controls.preview_button, valid);
  EnableWindow(application_state().controls.render_button,
               valid && runtime_state().active_preview_owner != nullptr);
  EnableWindow(application_state().controls.render_thumbnail_button,
               valid && runtime_state().active_preview_owner != nullptr);
  set_cancel_enabled(false);
  update_exposure_enablement();
}

void confirm_render(const HWND window) {
  if (!runtime_state().validation_state.plan.has_value())
    return;
  const auto paths = pano::app::gui_existing_output_paths(
      *runtime_state().validation_state.plan);
  if (!paths.empty()) {
    std::wstring message = L"The following output files already exist:\n";
    for (const auto &path : paths)
      message += utf8_to_wide(path) + L"\n";
    message += L"Replace them?";
    if (MessageBoxW(window, message.c_str(), L"Overwrite existing files?",
                    MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
      return;
  }
  start_render();
}

void request_render(const HWND window, const bool with_thumbnail) {
  auto *const shell = shell_state(window);
  if (shell == nullptr || runtime_state().preview_building ||
      runtime_state().active_preview_owner == nullptr)
    return;
  SendMessageW(application_state().controls.thumbnail_check, BM_SETCHECK,
               with_thumbnail ? BST_CHECKED : BST_UNCHECKED, 0);
  const bool plan_matches =
      runtime_state().validation_state.plan.has_value() &&
      runtime_state().validation_state.plan->outputs.thumbnail.has_value() ==
          with_thumbnail;
  if (plan_matches) {
    confirm_render(window);
    return;
  }
  shell->pending_render_with_thumbnail = with_thumbnail;
  schedule_validation(window, false);
}

void confirm_delete_session(const HWND window) {
  if (runtime_state().preview_building)
    return;
  const auto *const shell = shell_state(window);
  if (shell == nullptr || !shell->selected_record.has_value() ||
      *shell->selected_record >= runtime_state().refresh_state.records.size())
    return;
  const auto &record =
      runtime_state().refresh_state.records[*shell->selected_record];
  constexpr int delete_session_only = 201;
  constexpr int delete_with_images = 202;
  const TASKDIALOG_BUTTON buttons[] = {
      {delete_session_only, L"Delete session metadata only"},
      {delete_with_images, L"Delete session and captured screenshots"}};
  TASKDIALOGCONFIG dialog{};
  dialog.cbSize = sizeof(dialog);
  dialog.hwndParent = window;
  dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
  dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
  dialog.pszWindowTitle = L"Delete capture session";
  dialog.pszMainIcon = TD_WARNING_ICON;
  dialog.pszMainInstruction = L"Permanently delete this capture session?";
  const std::wstring detail = utf8_to_wide(record.path);
  dialog.pszContent = detail.c_str();
  dialog.cButtons = static_cast<UINT>(std::size(buttons));
  dialog.pButtons = buttons;
  dialog.nDefaultButton = IDCANCEL;
  int choice = IDCANCEL;
  using TaskDialogFunction =
      HRESULT(WINAPI *)(const TASKDIALOGCONFIG *, int *, int *, BOOL *);
  HMODULE controls_module = LoadLibraryW(L"comctl32.dll");
  const auto task_dialog =
      controls_module == nullptr
          ? nullptr
          : reinterpret_cast<TaskDialogFunction>(
                GetProcAddress(controls_module, "TaskDialogIndirect"));
  HRESULT dialog_result = E_NOTIMPL;
  if (task_dialog != nullptr)
    dialog_result = task_dialog(&dialog, &choice, nullptr, nullptr);
  if (controls_module != nullptr)
    FreeLibrary(controls_module);
  if (task_dialog == nullptr) {
    const int fallback = MessageBoxW(
        window,
        L"Delete captured screenshots too?\n\nYes: session and screenshots\n"
        L"No: session metadata only\nCancel: keep everything",
        L"Delete capture session",
        MB_ICONWARNING | MB_YESNOCANCEL | MB_DEFBUTTON3);
    choice = fallback == IDYES  ? delete_with_images
             : fallback == IDNO ? delete_session_only
                                : IDCANCEL;
    dialog_result = S_OK;
  }
  if (FAILED(dialog_result) ||
      (choice != delete_session_only && choice != delete_with_images))
    return;
  const auto targets = pano::app::application_deletion_targets(
      record, choice == delete_with_images);
  invalidate_preview();
  pano::app::DeletionResult result;
  std::string error;
  if (!pano::app::delete_application_files(targets, result, error)) {
    SetWindowTextW(application_state().controls.status_label,
                   utf8_to_wide(error).c_str());
    return;
  }
  const std::wstring status = L"Deleted " + std::to_wstring(result.deleted) +
                              L" file(s); " + std::to_wstring(result.missing) +
                              L" missing";
  SetWindowTextW(application_state().controls.status_label, status.c_str());
  start_refresh();
}

enum class ModalKind {
  app_settings,
  input_options,
  preview_options,
  session_tag
};

struct ModalState {
  ModalKind kind = ModalKind::app_settings;
  HWND owner = nullptr;
  HWND value = nullptr;
  HWND check = nullptr;
  HWND error = nullptr;
  HWND character_count = nullptr;
  std::optional<std::size_t> session_index;
  bool done = false;
};

LRESULT CALLBACK modal_window_procedure(const HWND window, const UINT message,
                                        const WPARAM wparam,
                                        const LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto *const create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return TRUE;
  }
  auto *const state =
      reinterpret_cast<ModalState *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (state == nullptr)
    return DefWindowProcW(window, message, wparam, lparam);
  switch (message) {
  case WM_CREATE: {
    apply_dark_caption(window);
    const unsigned dpi = GetDpiForWindow(window);
    const auto scaled = [dpi](const int value) {
      return MulDiv(value, static_cast<int>(dpi), 96);
    };
    RECT client{};
    GetClientRect(window, &client);
    const int margin = scaled(20);
    const int gap = scaled(10);
    const int row = scaled(32);
    const int label_height = scaled(22);
    const int content_width = client.right - margin * 2;
    if (state->kind == ModalKind::app_settings) {
      const HWND label =
          child(window, WC_STATICW, L"D3D12 allocation (MiB):", SS_LEFT, 0);
      state->value =
          child(window, WC_EDITW,
                window_text(application_state().controls.memory_edit).c_str(),
                WS_TABSTOP | WS_BORDER | ES_NUMBER, modal_value_id);
      state->check = child(window, WC_BUTTONW, L"Write debug coverage image",
                           WS_TABSTOP | BS_AUTOCHECKBOX, modal_check_id);
      SendMessageW(state->check, BM_SETCHECK,
                   application_state().application_settings.debug_coverage
                       ? BST_CHECKED
                       : BST_UNCHECKED,
                   0);
      MoveWindow(label, margin, margin, content_width, label_height, TRUE);
      MoveWindow(state->value, margin, margin + label_height, content_width,
                 row, TRUE);
      MoveWindow(state->check, margin, margin + label_height + row + gap,
                 content_width, row, TRUE);
    } else if (state->kind == ModalKind::input_options) {
      state->check = child(window, WC_BUTTONW, L"Allow incomplete session",
                           WS_TABSTOP | BS_AUTOCHECKBOX, modal_check_id);
      SendMessageW(state->check, BM_SETCHECK,
                   checked(application_state().controls.incomplete_check)
                       ? BST_CHECKED
                       : BST_UNCHECKED,
                   0);
      MoveWindow(state->check, margin, margin, content_width, row, TRUE);
    } else if (state->kind == ModalKind::preview_options) {
      const HWND label = child(window, WC_STATICW, L"Blending:", SS_LEFT, 0);
      state->value = child(window, WC_COMBOBOXW, L"",
                           WS_TABSTOP | CBS_DROPDOWNLIST, modal_value_id);
      SendMessageW(state->value, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(L"Hard"));
      SendMessageW(state->value, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(L"Feather"));
      SendMessageW(state->value, CB_SETCURSEL,
                   SendMessageW(application_state().controls.blend_combo,
                                CB_GETCURSEL, 0, 0),
                   0);
      state->check = child(window, WC_BUTTONW, L"Auto contrast (SDR only)",
                           WS_TABSTOP | BS_AUTOCHECKBOX, modal_check_id);
      SendMessageW(state->check, BM_SETCHECK,
                   checked(application_state().controls.auto_contrast_check)
                       ? BST_CHECKED
                       : BST_UNCHECKED,
                   0);
      MoveWindow(label, margin, margin, content_width, label_height, TRUE);
      MoveWindow(state->value, margin, margin + label_height, content_width,
                 row * 5, TRUE);
      MoveWindow(state->check, margin, margin + label_height + row + gap,
                 content_width, row, TRUE);
    } else {
      const HWND label = child(window, WC_STATICW, L"Tag", SS_LEFT, 0);
      std::string initial;
      if (state->session_index.has_value() &&
          *state->session_index <
              runtime_state().refresh_state.records.size()) {
        const auto &record =
            runtime_state().refresh_state.records[*state->session_index];
        initial = pano::app::application_session_tag(
                      application_state().application_settings,
                      wide_to_utf8(
                          window_text(application_state().controls.game_edit)),
                      record.session.session_id)
                      .value_or("");
      }
      state->value =
          child(window, WC_EDITW, utf8_to_wide(initial).c_str(),
                WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, modal_value_id);
      SendMessageW(state->value, EM_SETLIMITTEXT, 128, 0);
      state->character_count = child(window, WC_STATICW, L"", SS_RIGHT, 0);
      MoveWindow(label, margin, margin, content_width, label_height, TRUE);
      MoveWindow(state->value, margin, margin + label_height, content_width,
                 row, TRUE);
      MoveWindow(state->character_count, margin,
                 margin + label_height + row + scaled(4), content_width,
                 label_height, TRUE);
      const std::size_t characters = utf8_character_count(initial);
      const std::wstring remaining =
          std::to_wstring(characters >= 64U ? 0U : 64U - characters) +
          L" characters remaining";
      SetWindowTextW(state->character_count, remaining.c_str());
    }
    state->error = child(window, WC_STATICW, L"", SS_LEFT, 0);
    const HWND ok = child(window, WC_BUTTONW, L"OK", WS_TABSTOP | BS_OWNERDRAW,
                          modal_ok_id);
    const HWND cancel = child(window, WC_BUTTONW, L"Cancel",
                              WS_TABSTOP | BS_OWNERDRAW, modal_cancel_id);
    const int button_width = scaled(96);
    const int button_top = client.bottom - margin - row;
    const int ok_left = client.right - margin - button_width;
    const int cancel_left = ok_left - gap - button_width;
    MoveWindow(state->error, margin, button_top, cancel_left - margin - gap,
               row, TRUE);
    MoveWindow(cancel, cancel_left, button_top, button_width, row, TRUE);
    MoveWindow(ok, ok_left, button_top, button_width, row, TRUE);
    const auto *const shell = shell_state(state->owner);
    if (shell != nullptr &&
        shell->workflow.operation != pano::app::GuiOperation::idle) {
      if (state->value != nullptr)
        EnableWindow(state->value, FALSE);
      if (state->check != nullptr)
        EnableWindow(state->check, FALSE);
      EnableWindow(ok, FALSE);
      SetWindowTextW(state->error, L"Read-only while an operation is active");
    }
    SetFocus(state->value != nullptr ? state->value : state->check);
    return 0;
  }
  case WM_COMMAND:
    if (LOWORD(wparam) == modal_cancel_id) {
      DestroyWindow(window);
      return 0;
    }
    if (LOWORD(wparam) == modal_value_id && HIWORD(wparam) == EN_CHANGE &&
        state->kind == ModalKind::session_tag &&
        state->character_count != nullptr) {
      const std::size_t characters =
          utf8_character_count(wide_to_utf8(window_text(state->value)));
      const std::wstring remaining =
          std::to_wstring(characters >= 64U ? 0U : 64U - characters) +
          L" characters remaining";
      SetWindowTextW(state->character_count, remaining.c_str());
      return 0;
    }
    if (LOWORD(wparam) != modal_ok_id)
      break;
    if (state->kind == ModalKind::app_settings) {
      std::optional<unsigned> parsed;
      std::string error;
      if (!parse_unsigned_text(state->value, false, parsed, error) ||
          *parsed < 1024U || *parsed > 8192U) {
        SetWindowTextW(state->error, L"Enter a value from 1024 to 8192 MiB");
        return 0;
      }
      std::optional<unsigned> old_value;
      parse_unsigned_text(application_state().controls.memory_edit, false,
                          old_value, error);
      const bool preview_was_ready =
          runtime_state().active_preview_owner != nullptr;
      const bool budget_changed =
          !old_value.has_value() || *old_value != *parsed;
      const bool decreased = old_value.has_value() && *parsed < *old_value;
      SetWindowTextW(application_state().controls.memory_edit,
                     std::to_wstring(*parsed).c_str());
      application_state().application_settings.gpu_memory_mib = *parsed;
      application_state().application_settings.debug_coverage =
          checked(state->check);
      SendMessageW(application_state().controls.coverage_check, BM_SETCHECK,
                   application_state().application_settings.debug_coverage
                       ? BST_CHECKED
                       : BST_UNCHECKED,
                   0);
      save_gui_settings();
      if (budget_changed || runtime_state().validation_state.plan.has_value()) {
        if (decreased && preview_was_ready) {
          if (auto *const shell = shell_state(state->owner); shell != nullptr)
            shell->rebuild_preview_after_validation = true;
        }
        schedule_validation(state->owner, decreased);
      }
    } else if (state->kind == ModalKind::input_options) {
      SendMessageW(application_state().controls.incomplete_check, BM_SETCHECK,
                   checked(state->check) ? BST_CHECKED : BST_UNCHECKED, 0);
      schedule_validation(state->owner, false);
    } else if (state->kind == ModalKind::preview_options) {
      const LRESULT blend = SendMessageW(state->value, CB_GETCURSEL, 0, 0);
      const bool contrast = checked(state->check);
      const bool changed =
          blend != SendMessageW(application_state().controls.blend_combo,
                                CB_GETCURSEL, 0, 0) ||
          contrast != checked(application_state().controls.auto_contrast_check);
      SendMessageW(application_state().controls.blend_combo, CB_SETCURSEL,
                   blend, 0);
      SendMessageW(application_state().controls.auto_contrast_check,
                   BM_SETCHECK, contrast ? BST_CHECKED : BST_UNCHECKED, 0);
      application_state().application_settings.auto_contrast = contrast;
      save_gui_settings();
      if (changed)
        schedule_preview_option_validation(state->owner);
    } else if (state->session_index.has_value() &&
               *state->session_index <
                   runtime_state().refresh_state.records.size()) {
      const auto index = *state->session_index;
      const auto &record = runtime_state().refresh_state.records[index];
      std::string error;
      const std::string tag = wide_to_utf8(window_text(state->value));
      if (!pano::app::set_application_session_tag(
              application_state().application_settings,
              wide_to_utf8(window_text(application_state().controls.game_edit)),
              record.session.session_id, tag, error)) {
        SetWindowTextW(state->error, utf8_to_wide(error).c_str());
        return 0;
      }
      save_gui_settings();
      set_list_text(application_state().controls.session_combo,
                    static_cast<int>(index), 2, utf8_to_wide(tag));
      update_session_accessible_name(index);
    }
    DestroyWindow(window);
    return 0;
  case WM_CTLCOLORSTATIC:
    if (state->character_count != nullptr &&
        reinterpret_cast<HWND>(lparam) == state->character_count) {
      HDC context = reinterpret_cast<HDC>(wparam);
      SetTextColor(context, RGB(145, 148, 152));
      SetBkColor(context, RGB(3, 7, 18));
      return reinterpret_cast<LRESULT>(dark_background_brush());
    }
    return dark_control_color(message, wparam);
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORBTN:
  case WM_CTLCOLORLISTBOX:
    return dark_control_color(message, wparam);
  case WM_DRAWITEM: {
    const auto *const draw = reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
    if (draw != nullptr && draw_modal_button(*draw))
      return TRUE;
    break;
  }
  case WM_CLOSE:
    DestroyWindow(window);
    return 0;
  case WM_DESTROY:
    state->done = true;
    return 0;
  default:
    break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void run_modal(const HWND owner, const ModalKind kind,
               const std::optional<std::size_t> session_index = std::nullopt) {
  ModalState state;
  state.kind = kind;
  state.owner = owner;
  state.session_index = session_index;
  const wchar_t *const title =
      kind == ModalKind::app_settings      ? L"App Settings"
      : kind == ModalKind::input_options   ? L"Input Options"
      : kind == ModalKind::preview_options ? L"Preview Options"
                                           : L"Session Tag";
  RECT owner_bounds{};
  GetWindowRect(owner, &owner_bounds);
  const unsigned dpi = GetDpiForWindow(owner);
  const auto scaled = [dpi](const int value) {
    return MulDiv(value, static_cast<int>(dpi), 96);
  };
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE;
  constexpr DWORD extended_style = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  RECT modal_bounds{0, 0, scaled(500),
                    scaled(kind == ModalKind::input_options ? 190 : 240)};
  AdjustWindowRectExForDpi(&modal_bounds, style, FALSE, extended_style, dpi);
  const int width = modal_bounds.right - modal_bounds.left;
  const int height = modal_bounds.bottom - modal_bounds.top;
  HWND modal = CreateWindowExW(
      extended_style, modal_window_class, title, style,
      owner_bounds.left + (owner_bounds.right - owner_bounds.left - width) / 2,
      owner_bounds.top + (owner_bounds.bottom - owner_bounds.top - height) / 2,
      width, height, owner, nullptr, GetModuleHandleW(nullptr), &state);
  if (modal == nullptr)
    return;
  SetWindowTextW(modal, title);
  apply_dark_caption(modal);
  EnableWindow(owner, FALSE);
  MSG message{};
  bool quit = false;
  while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
      SendMessageW(
          modal, WM_COMMAND, MAKEWPARAM(modal_cancel_id, BN_CLICKED),
          reinterpret_cast<LPARAM>(GetDlgItem(modal, modal_cancel_id)));
      continue;
    }
    if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN &&
        (state.value == nullptr ||
         SendMessageW(state.value, CB_GETDROPPEDSTATE, 0, 0) == FALSE)) {
      SendMessageW(modal, WM_COMMAND, MAKEWPARAM(modal_ok_id, BN_CLICKED),
                   reinterpret_cast<LPARAM>(GetDlgItem(modal, modal_ok_id)));
      continue;
    }
    if (!IsDialogMessageW(modal, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  if (message.message == WM_QUIT)
    quit = true;
  EnableWindow(owner, TRUE);
  SetActiveWindow(owner);
  if (quit)
    PostQuitMessage(static_cast<int>(message.wParam));
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
    SetWindowTextW(application_state().controls.status_label,
                   utf8_to_wide(error).c_str());
    return;
  }
  if (!SetWindowPos(application_state().controls.preview_surface, HWND_TOP,
                    preview_bounds.left, preview_bounds.top,
                    preview_bounds.right - preview_bounds.left,
                    preview_bounds.bottom - preview_bounds.top,
                    SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
    hide_webview_preview(*shell);
    SetWindowTextW(application_state().controls.status_label,
                   L"Cannot position native preview surface");
    return;
  }
  shell->webview_preview_visible = true;
  if (!update_preview_surface() || !present_preview_view()) {
    hide_webview_preview(*shell);
    SetWindowTextW(application_state().controls.status_label,
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

void hide_legacy_webview_controls() {
  const Controls &legacy = application_state().controls;
  const std::array legacy_windows{legacy.input_stage_button,
                                  legacy.preview_stage_button,
                                  legacy.output_stage_button,
                                  legacy.settings_button,
                                  legacy.input_options_button,
                                  legacy.preview_options_button,
                                  legacy.exposure_panel_button,
                                  legacy.resolution_mode_button,
                                  legacy.game_label,
                                  legacy.game_edit,
                                  legacy.game_browse_button,
                                  legacy.refresh_button,
                                  legacy.session_label,
                                  legacy.session_combo,
                                  legacy.session_header_session,
                                  legacy.session_header_poses,
                                  legacy.session_header_tag,
                                  legacy.session_header_actions,
                                  legacy.image_label,
                                  legacy.image_edit,
                                  legacy.image_browse_button,
                                  legacy.output_label,
                                  legacy.output_edit,
                                  legacy.output_browse_button,
                                  legacy.output_name_label,
                                  legacy.output_name_edit,
                                  legacy.format_label,
                                  legacy.format_combo,
                                  legacy.format_jpeg,
                                  legacy.format_png,
                                  legacy.format_exr,
                                  legacy.quality_label,
                                  legacy.quality_edit,
                                  legacy.quality_slider,
                                  legacy.resolution_label,
                                  legacy.resolution_edit,
                                  legacy.resolution_slider,
                                  legacy.width_label,
                                  legacy.width_edit,
                                  legacy.blend_label,
                                  legacy.blend_combo,
                                  legacy.memory_label,
                                  legacy.memory_edit,
                                  legacy.workers_label,
                                  legacy.workers_edit,
                                  legacy.thumbnail_check,
                                  legacy.coverage_check,
                                  legacy.incomplete_check,
                                  legacy.auto_contrast_check,
                                  legacy.gpu_check,
                                  legacy.gpu_strict_check,
                                  legacy.automatic_exposure_button,
                                  legacy.match_exposure_button,
                                  legacy.discard_exposure_button,
                                  legacy.delete_session_button,
                                  legacy.delete_images_check,
                                  legacy.preview_button,
                                  legacy.preview_next_button,
                                  legacy.render_button,
                                  legacy.render_thumbnail_button,
                                  legacy.cancel_button,
                                  legacy.status_label,
                                  legacy.operation_progress};
  for (const HWND legacy_window : legacy_windows)
    ShowWindow(legacy_window, SW_HIDE);
}

void sync_webview_snapshot(const HWND window) {
  auto *const shell = shell_state(window);
  if (shell == nullptr || !shell->webview_enabled ||
      shell->webview == nullptr || !shell->webview->ready())
    return;
  if (!shell->webview_snapshot_stage.has_value() ||
      *shell->webview_snapshot_stage != shell->workflow.stage) {
    shell->webview_snapshot_stage = shell->workflow.stage;
    invalidate_webview_layout(*shell);
  }
  const Controls &legacy = application_state().controls;
  hide_legacy_webview_controls();
  if (!shell->webview_preview_visible)
    ShowWindow(legacy.preview_surface, SW_HIDE);
  std::wostringstream json;
  json << L"{\"version\":1,\"kind\":\"snapshot\",\"pageGeneration\":"
       << shell->webview->page_generation() << L",\"layoutGeneration\":"
       << shell->webview_layout_generation << L",\"stage\":"
       << json_string(shell->workflow.stage == pano::app::GuiStage::preview
                          ? L"preview"
                          : L"input")
       << L",\"gameDirectory\":"
       << json_string(window_text(application_state().controls.game_edit))
       << L",\"screenshotsDirectory\":"
       << json_string(window_text(application_state().controls.image_edit))
       << L",\"selectedIndex\":";
  if (shell->selected_record.has_value())
    json << *shell->selected_record;
  else
    json << L"null";
  json << L",\"status\":"
       << json_string(window_text(application_state().controls.status_label))
       << L",\"busy\":"
       << (runtime_state().preview_building ? L"true" : L"false")
       << L",\"previewEnabled\":"
       << (IsWindowEnabled(application_state().controls.preview_button)
               ? L"true"
               : L"false")
       << L",\"previewReady\":"
       << (retained_preview_ready() ? L"true" : L"false")
       << L",\"exposureOpen\":"
       << (shell->exposure_window != nullptr &&
                   IsWindowVisible(shell->exposure_window)
               ? L"true"
               : L"false")
       << L",\"exposureAdjusted\":"
       << (shell->exposure_edits_applied ? L"true" : L"false")
       << L",\"previewProgress\":" << shell->operation_progress_percent
       << L",\"previewMessage\":"
       << json_string(runtime_state().preview_building ? L"Loading..."
                                                       : L"No preview loaded")
       << L",\"sessions\":[";
  const std::string game_directory =
      wide_to_utf8(window_text(application_state().controls.game_edit));
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
         << json_string(utf8_to_wide(record.error)) << L'}';
  }
  json << L"]}";
  std::wstring error;
  if (!shell->webview->post_snapshot(json.str(), error) && !error.empty())
    SetWindowTextW(application_state().controls.status_label, error.c_str());
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
  json << L"],\"poseCount\":" << runtime_state().preview_hovered.size() << L'}';
  std::wstring error;
  if (!shell->exposure_webview->post_snapshot(json.str(), error) &&
      !error.empty())
    SetWindowTextW(application_state().controls.status_label, error.c_str());
}

void handle_exposure_webview_command(const HWND window,
                                     const pano::app::WebViewCommand &command) {
  auto *const shell = shell_state(window);
  if (shell == nullptr || !shell->webview_enabled)
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
  case pano::app::WebViewCommandKind::reset_exposure:
    if (!runtime_state().preview_building) {
      runtime_state().exposure_target.reset();
      runtime_state().exposure_selected.clear();
      std::fill(runtime_state().preview_hovered.begin(),
                runtime_state().preview_hovered.end(), std::uint8_t{0});
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
    MessageBoxW(window, command.value.c_str(), L"Exposure",
                MB_ICONERROR | MB_OK);
    return;
  default:
    return;
  }
  refresh_exposure_pose_labels();
  update_exposure_enablement();
  sync_exposure_webview_snapshot(window);
  sync_webview_snapshot(window);
  present_preview_view();
}

void handle_webview_command(const HWND window,
                            const pano::app::WebViewCommand &command) {
  auto *const shell = shell_state(window);
  if (shell == nullptr || !shell->webview_enabled)
    return;
  switch (command.kind) {
  case pano::app::WebViewCommandKind::ready:
    break;
  case pano::app::WebViewCommandKind::set_game_directory:
    SetWindowTextW(application_state().controls.game_edit,
                   command.value.c_str());
    break;
  case pano::app::WebViewCommandKind::set_screenshots_directory:
    SetWindowTextW(application_state().controls.image_edit,
                   command.value.c_str());
    break;
  case pano::app::WebViewCommandKind::browse_game_directory:
    if (const auto path = choose_path(window, true)) {
      SetWindowTextW(application_state().controls.game_edit, path->c_str());
      start_refresh();
    }
    break;
  case pano::app::WebViewCommandKind::browse_screenshots_directory:
    if (const auto path = choose_path(window, true))
      SetWindowTextW(application_state().controls.image_edit, path->c_str());
    break;
  case pano::app::WebViewCommandKind::refresh:
    start_refresh();
    break;
  case pano::app::WebViewCommandKind::select_session:
    if (command.session_index.has_value())
      apply_session_selection(window, *command.session_index);
    break;
  case pano::app::WebViewCommandKind::edit_tag:
    if (command.session_index.has_value())
      run_modal(window, ModalKind::session_tag, *command.session_index);
    break;
  case pano::app::WebViewCommandKind::delete_session:
    if (command.session_index.has_value()) {
      apply_session_selection(window, *command.session_index);
      confirm_delete_session(window);
    }
    break;
  case pano::app::WebViewCommandKind::navigate_input:
    navigate_stage(window, pano::app::GuiStage::input);
    break;
  case pano::app::WebViewCommandKind::navigate_preview:
    navigate_stage(window, pano::app::GuiStage::preview);
    break;
  case pano::app::WebViewCommandKind::open_settings:
    run_modal(window, ModalKind::app_settings);
    break;
  case pano::app::WebViewCommandKind::open_options:
    run_modal(window, shell->workflow.stage == pano::app::GuiStage::preview
                          ? ModalKind::preview_options
                          : ModalKind::input_options);
    break;
  case pano::app::WebViewCommandKind::open_exposure:
    show_exposure_panel(window);
    break;
  case pano::app::WebViewCommandKind::set_exposure_overlay:
  case pano::app::WebViewCommandKind::hover_exposure_pose:
  case pano::app::WebViewCommandKind::clear_exposure_hover:
  case pano::app::WebViewCommandKind::set_exposure_reference:
  case pano::app::WebViewCommandKind::toggle_exposure_selection:
  case pano::app::WebViewCommandKind::reset_exposure:
  case pano::app::WebViewCommandKind::equalize_exposure:
    return;
  case pano::app::WebViewCommandKind::abort_operation:
    SendMessageW(
        window, WM_COMMAND, MAKEWPARAM(cancel_id, BN_CLICKED),
        reinterpret_cast<LPARAM>(application_state().controls.cancel_button));
    break;
  case pano::app::WebViewCommandKind::start_preview:
    SendMessageW(
        window, WM_COMMAND, MAKEWPARAM(preview_id, BN_CLICKED),
        reinterpret_cast<LPARAM>(application_state().controls.preview_button));
    break;
  case pano::app::WebViewCommandKind::finalize_preview:
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(preview_next_id, BN_CLICKED),
                 reinterpret_cast<LPARAM>(
                     application_state().controls.preview_next_button));
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
  case WM_CREATE:
    if (!create_controls(window))
      return -1;
    apply_dark_caption(window);
    runtime_state().refresh_window.store(window, std::memory_order_release);
    if (auto *const shell = shell_state(window); shell != nullptr)
      shell->suppress_control_changes = true;
    load_gui_settings();
    if (auto *const shell = shell_state(window); shell != nullptr)
      shell->suppress_control_changes = false;
    if (!window_text(application_state().controls.game_edit).empty())
      start_refresh();
    if (auto *const shell = shell_state(window);
        shell != nullptr && shell->webview_enabled) {
      if (!pano::app::ensure_webview_runtime(window))
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
      hide_legacy_webview_controls();
      hide_webview_preview(*shell);
    }
    return 0;
  case WM_GETMINMAXINFO: {
    auto *const limits = reinterpret_cast<MINMAXINFO *>(lparam);
    const unsigned dpi = GetDpiForWindow(window);
    limits->ptMinTrackSize.x = MulDiv(920, static_cast<int>(dpi), 96);
    const auto *const shell = shell_state(window);
    int minimum_height =
        MulDiv(shell != nullptr && shell->webview_enabled ? 480 : 680,
               static_cast<int>(dpi), 96);
    if (shell != nullptr && shell->webview_enabled &&
        !shell->webview_resize_timer_active &&
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
    if (shell != nullptr && shell->webview_enabled &&
        proposed_bounds != nullptr &&
        preserve_webview_height_during_width_sizing(window, wparam,
                                                    *proposed_bounds))
      return TRUE;
    break;
  }
  case WM_ENTERSIZEMOVE:
    if (auto *const shell = shell_state(window);
        shell != nullptr && shell->webview_enabled) {
      shell->webview_sizing_active = true;
      shell->webview_sizing_changed_width = false;
    }
    return 0;
  case WM_EXITSIZEMOVE:
    if (auto *const shell = shell_state(window);
        shell != nullptr && shell->webview_enabled)
      finish_webview_sizing(window, *shell);
    return 0;
  case WM_PAINT: {
    PAINTSTRUCT paint{};
    HDC context = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    FillRect(context, &client, dark_background_brush());
    const unsigned dpi = GetDpiForWindow(window);
    const auto scaled = [dpi](const int value) {
      return MulDiv(value, static_cast<int>(dpi), 96);
    };
    const auto *const paint_shell = shell_state(window);
    const bool input = paint_shell != nullptr && paint_shell->workflow.stage ==
                                                     pano::app::GuiStage::input;
    const int margin = input ? scaled(16) : scaled(12);
    const int page_top = input ? scaled(74) : scaled(84);
    const int footer_height = input ? scaled(74) : scaled(76);
    const int page_bottom =
        input ? page_top + scaled(500) : client.bottom - footer_height;
    HPEN border = CreatePen(PS_SOLID, 1, RGB(107, 114, 128));
    const HGDIOBJ old_pen = SelectObject(context, border);
    const HGDIOBJ old_brush = SelectObject(context, GetStockObject(NULL_BRUSH));
    RoundRect(context, margin, page_top, client.right - margin, page_bottom,
              scaled(6), scaled(6));
    if (!input) {
      MoveToEx(context, margin, client.bottom - footer_height, nullptr);
      LineTo(context, client.right - margin, client.bottom - footer_height);
    }
    SelectObject(context, old_brush);
    SelectObject(context, old_pen);
    DeleteObject(border);
    EndPaint(window, &paint);
    return 0;
  }
  case WM_CTLCOLORSTATIC:
    if (reinterpret_cast<HWND>(lparam) ==
            application_state().controls.session_header_session ||
        reinterpret_cast<HWND>(lparam) ==
            application_state().controls.session_header_poses ||
        reinterpret_cast<HWND>(lparam) ==
            application_state().controls.session_header_tag ||
        reinterpret_cast<HWND>(lparam) ==
            application_state().controls.session_header_actions) {
      HDC context = reinterpret_cast<HDC>(wparam);
      SetTextColor(context, RGB(209, 213, 219));
      SetBkColor(context, RGB(75, 85, 99));
      return reinterpret_cast<LRESULT>(table_header_brush());
    }
    return dark_control_color(message, wparam);
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORBTN:
  case WM_CTLCOLORLISTBOX:
    return dark_control_color(message, wparam);
  case WM_MEASUREITEM: {
    auto *const measure = reinterpret_cast<MEASUREITEMSTRUCT *>(lparam);
    if (measure != nullptr && measure->CtlID == session_combo_id) {
      measure->itemHeight =
          static_cast<UINT>(MulDiv(45, GetDpiForWindow(window), 96));
      return TRUE;
    }
    break;
  }
  case WM_DRAWITEM: {
    const auto *const draw = reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
    if (draw != nullptr &&
        (draw_workflow_button(*draw, window) || draw_session_row(*draw)))
      return TRUE;
    break;
  }
  case WM_SIZE: {
    auto *const size_shell = shell_state(window);
    const int width = LOWORD(lparam);
    const int height = HIWORD(lparam);
    const bool width_changed =
        size_shell == nullptr || size_shell->window_width != width;
    const bool size_changed = size_shell == nullptr || width_changed ||
                              size_shell->window_height != height;
    if (size_shell != nullptr) {
      size_shell->window_width = width;
      size_shell->window_height = height;
      if (size_shell->webview != nullptr) {
        const RECT bounds{0, 0, width, height};
        size_shell->webview->resize(bounds);
        if (width_changed) {
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
    const bool webview_enabled =
        size_shell != nullptr && size_shell->webview_enabled;
    if (!webview_enabled) {
      pano::app::reset_gui_preview_view(runtime_state().preview_view);
      layout_controls(window);
      if (const auto *const shell = shell_state(window);
          shell != nullptr &&
          shell->workflow.stage == pano::app::GuiStage::preview &&
          !runtime_state().preview_building && size_changed) {
        const bool resized = update_preview_surface();
        const bool presented = resized && present_preview_view();
        if (!presented) {
          discard_active_preview();
          if (!recover_preview_surface())
            SetWindowTextW(application_state().controls.status_label,
                           L"D3D12 preview surface is unavailable");
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
    replace_ui_fonts(window);
    layout_controls(window);
    return 0;
  }
  case WM_CLOSE:
    DestroyWindow(window);
    return 0;
  case WM_NOTIFY: {
    const auto *const notification = reinterpret_cast<const NMHDR *>(lparam);
    if (notification == nullptr ||
        notification->hwndFrom != application_state().controls.session_combo)
      break;
    if (notification->code == LVN_ITEMCHANGED) {
      const auto *const changed = reinterpret_cast<const NMLISTVIEW *>(lparam);
      if ((changed->uNewState & LVIS_SELECTED) != 0U && changed->iItem >= 0) {
        const auto index = static_cast<std::size_t>(changed->iItem);
        const auto *const shell = shell_state(window);
        if (shell == nullptr || shell->selected_record != index)
          apply_session_selection(window, index);
      }
      return 0;
    }
    if (notification->code == LVN_GETINFOTIPW) {
      auto *const tip = reinterpret_cast<NMLVGETINFOTIPW *>(lparam);
      if (tip->iItem >= 0 && static_cast<std::size_t>(tip->iItem) <
                                 runtime_state().refresh_state.records.size()) {
        const auto &record =
            runtime_state()
                .refresh_state.records[static_cast<std::size_t>(tip->iItem)];
        const wchar_t *text = nullptr;
        if (!record.error.empty())
          text = L"Invalid";
        else if (!record.session.completed)
          text = L"Incomplete";
        if (text != nullptr)
          wcsncpy_s(tip->pszText, static_cast<std::size_t>(tip->cchTextMax),
                    text, _TRUNCATE);
      }
      return 0;
    }
    if (notification->code == NM_CLICK) {
      const auto *const click =
          reinterpret_cast<const NMITEMACTIVATE *>(lparam);
      if (click->iItem >= 0 && click->iSubItem == 2)
        run_modal(window, ModalKind::session_tag,
                  static_cast<std::size_t>(click->iItem));
      else if (click->iItem >= 0 && click->iSubItem == 3) {
        apply_session_selection(window, static_cast<std::size_t>(click->iItem));
        HMENU menu = CreatePopupMenu();
        if (menu != nullptr) {
          AppendMenuW(menu, MF_STRING, delete_session_id, L"Delete session...");
          POINT point{};
          GetCursorPos(&point);
          const UINT command =
              TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x,
                             point.y, 0, window, nullptr);
          DestroyMenu(menu);
          if (command == delete_session_id)
            confirm_delete_session(window);
        }
      }
      return 0;
    }
    if (notification->code == NM_CUSTOMDRAW) {
      auto *const draw = reinterpret_cast<NMLVCUSTOMDRAW *>(lparam);
      if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
        return CDRF_NOTIFYITEMDRAW;
      if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
        return CDRF_NOTIFYSUBITEMDRAW;
      if (draw->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
        const auto index = static_cast<std::size_t>(draw->nmcd.dwItemSpec);
        const bool selected =
            ListView_GetItemState(application_state().controls.session_combo,
                                  static_cast<int>(index), LVIS_SELECTED) != 0U;
        draw->clrTextBk = selected           ? RGB(35, 49, 63)
                          : index % 2U == 1U ? RGB(31, 41, 55)
                                             : RGB(17, 24, 39);
        draw->clrText = RGB(232, 234, 236);
        if (draw->iSubItem == 0 &&
            index < runtime_state().refresh_state.records.size()) {
          const auto &record = runtime_state().refresh_state.records[index];
          const bool stitched =
              pano::app::application_stitched_name(
                  application_state().application_settings,
                  wide_to_utf8(
                      window_text(application_state().controls.game_edit)),
                  record.session.session_id)
                  .has_value();
          switch (pano::app::gui_session_status(record, stitched)) {
          case pano::app::GuiSessionStatus::invalid:
            draw->clrText = RGB(239, 68, 68);
            break;
          case pano::app::GuiSessionStatus::incomplete:
            draw->clrText = RGB(205, 125, 35);
            break;
          case pano::app::GuiSessionStatus::stitched:
            draw->clrText = RGB(34, 197, 94);
            break;
          case pano::app::GuiSessionStatus::complete:
            break;
          }
        }
        return CDRF_NEWFONT;
      }
    }
    break;
  }
  case WM_HSCROLL: {
    const HWND slider = reinterpret_cast<HWND>(lparam);
    HWND value = nullptr;
    if (slider == application_state().controls.resolution_slider)
      value = application_state().controls.resolution_edit;
    else if (slider == application_state().controls.quality_slider)
      value = application_state().controls.quality_edit;
    if (value != nullptr) {
      const auto position =
          static_cast<unsigned>(SendMessageW(slider, TBM_GETPOS, 0, 0));
      if (auto *const shell = shell_state(window); shell != nullptr)
        shell->suppress_control_changes = true;
      SetWindowTextW(value, std::to_wstring(position).c_str());
      if (auto *const shell = shell_state(window); shell != nullptr)
        shell->suppress_control_changes = false;
      schedule_validation(window, false);
      return 0;
    }
    break;
  }
  case WM_COMMAND:
    switch (LOWORD(wparam)) {
    case settings_id:
      if (HIWORD(wparam) == BN_CLICKED)
        run_modal(window, ModalKind::app_settings);
      return 0;
    case input_options_id:
      if (HIWORD(wparam) == BN_CLICKED)
        run_modal(window, ModalKind::input_options);
      return 0;
    case preview_options_id:
      if (HIWORD(wparam) == BN_CLICKED)
        run_modal(window, ModalKind::preview_options);
      return 0;
    case exposure_panel_id:
      if (HIWORD(wparam) == BN_CLICKED)
        show_exposure_panel(window);
      return 0;
    case resolution_mode_id:
      if (HIWORD(wparam) == BN_CLICKED)
        if (auto *const shell = shell_state(window); shell != nullptr) {
          shell->resolution_pixels = !shell->resolution_pixels;
          SetWindowTextW(application_state().controls.resolution_mode_button,
                         shell->resolution_pixels ? L"Percent" : L"Pixels");
          layout_controls(window);
          schedule_validation(window, false);
        }
      return 0;
    case input_stage_id:
      if (HIWORD(wparam) == BN_CLICKED)
        navigate_stage(window, pano::app::GuiStage::input);
      return 0;
    case preview_stage_id:
      if (HIWORD(wparam) == BN_CLICKED)
        navigate_stage(window, pano::app::GuiStage::preview);
      return 0;
    case output_stage_id:
    case preview_next_id:
      if (HIWORD(wparam) == BN_CLICKED)
        navigate_stage(window, pano::app::GuiStage::output);
      return 0;
    case game_browse_id:
      if (HIWORD(wparam) == BN_CLICKED)
        if (const auto path = choose_path(window, true)) {
          SetWindowTextW(application_state().controls.game_edit, path->c_str());
          start_refresh();
        }
      return 0;
    case refresh_id:
      if (HIWORD(wparam) == BN_CLICKED)
        start_refresh();
      return 0;
    case image_browse_id:
      if (HIWORD(wparam) == BN_CLICKED)
        if (const auto path = choose_path(window, true))
          SetWindowTextW(application_state().controls.image_edit,
                         path->c_str());
      return 0;
    case output_browse_id:
      if (HIWORD(wparam) == BN_CLICKED)
        if (const auto path = choose_path(window, true))
          SetWindowTextW(application_state().controls.output_edit,
                         path->c_str());
      return 0;
    case format_combo_id:
      if (HIWORD(wparam) == CBN_SELCHANGE) {
        select_output_format(
            window, static_cast<int>(
                        SendMessageW(application_state().controls.format_combo,
                                     CB_GETCURSEL, 0, 0)));
      }
      return 0;
    case format_jpeg_id:
    case format_png_id:
    case format_exr_id:
      if (HIWORD(wparam) == BN_CLICKED)
        select_output_format(window, LOWORD(wparam) == format_jpeg_id  ? 0
                                     : LOWORD(wparam) == format_png_id ? 1
                                                                       : 2);
      return 0;
    case blend_combo_id:
      if (HIWORD(wparam) == CBN_SELCHANGE)
        schedule_preview_option_validation(window);
      return 0;
    case output_name_id:
    case output_edit_id:
    case width_edit_id:
    case memory_edit_id:
    case workers_edit_id:
      if (HIWORD(wparam) == EN_CHANGE) {
        const auto *const shell = shell_state(window);
        if (shell == nullptr || !shell->suppress_control_changes)
          schedule_validation(window, false);
      }
      return 0;
    case quality_edit_id:
    case resolution_edit_id:
      if (HIWORD(wparam) == EN_CHANGE) {
        auto *const shell = shell_state(window);
        if (shell == nullptr || !shell->suppress_control_changes) {
          const HWND edit = LOWORD(wparam) == quality_edit_id
                                ? application_state().controls.quality_edit
                                : application_state().controls.resolution_edit;
          const HWND slider =
              LOWORD(wparam) == quality_edit_id
                  ? application_state().controls.quality_slider
                  : application_state().controls.resolution_slider;
          std::optional<unsigned> value;
          std::string error;
          if (parse_unsigned_text(edit, false, value, error) &&
              value.has_value() && *value >= 1U && *value <= 100U)
            SendMessageW(slider, TBM_SETPOS, TRUE, *value);
          schedule_validation(window, false);
        }
      }
      return 0;
    case image_edit_id:
      if (HIWORD(wparam) == EN_CHANGE) {
        const auto *const shell = shell_state(window);
        if (shell == nullptr || !shell->suppress_control_changes)
          schedule_validation(window);
      }
      return 0;
    case game_edit_id:
      if (HIWORD(wparam) == EN_CHANGE) {
        const auto *const shell = shell_state(window);
        if (shell == nullptr || !shell->suppress_control_changes)
          reset_session_for_game_change(window);
      }
      return 0;
    case thumbnail_id:
    case coverage_id:
    case incomplete_id:
    case gpu_strict_id:
      if (HIWORD(wparam) == BN_CLICKED)
        schedule_validation(window, false);
      return 0;
    case auto_contrast_id:
      if (HIWORD(wparam) == BN_CLICKED)
        schedule_preview_option_validation(window);
      return 0;
    case gpu_id:
      if (HIWORD(wparam) == BN_CLICKED) {
        update_option_enablement();
        schedule_validation(window);
      }
      return 0;
    case preview_id:
      if (HIWORD(wparam) == BN_CLICKED &&
          runtime_state().validation_state.plan.has_value()) {
        navigate_stage(window, pano::app::GuiStage::preview);
        start_preview();
      }
      return 0;
    case automatic_exposure_id:
      if (HIWORD(wparam) == BN_CLICKED)
        start_exposure(ExposureCommand::automatic);
      return 0;
    case match_exposure_id:
      if (HIWORD(wparam) == BN_CLICKED)
        start_exposure(ExposureCommand::manual_match);
      return 0;
    case discard_exposure_id:
      if (HIWORD(wparam) == BN_CLICKED)
        start_exposure(ExposureCommand::discard);
      return 0;
    case render_id:
      if (HIWORD(wparam) == BN_CLICKED)
        request_render(window, false);
      return 0;
    case render_thumbnail_id:
      if (HIWORD(wparam) == BN_CLICKED)
        request_render(window, true);
      return 0;
    case delete_session_id:
      if (HIWORD(wparam) == BN_CLICKED)
        confirm_delete_session(window);
      return 0;
    case cancel_id:
      if (HIWORD(wparam) == BN_CLICKED && runtime_state().preview_building &&
          runtime_state().preview_cancellation != nullptr) {
        pano_gpu_cancellation_token_cancel(
            runtime_state().preview_cancellation);
        const auto *const shell = shell_state(window);
        const wchar_t *phase =
            shell != nullptr &&
                    shell->workflow.operation == pano::app::GuiOperation::render
                ? L"render"
            : shell != nullptr && shell->workflow.operation ==
                                      pano::app::GuiOperation::exposure
                ? L"exposure"
                : L"preview";
        const std::wstring status =
            std::wstring(L"Requesting ") + phase + L" cancellation...";
        SetWindowTextW(application_state().controls.status_label,
                       status.c_str());
      }
      return 0;
    default:
      break;
    }
    break;
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
    if (application_state().accessibility_properties != nullptr) {
      clear_session_accessible_names();
      application_state().accessibility_properties->Release();
      application_state().accessibility_properties = nullptr;
    }
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
  WNDCLASSEXW modal_descriptor{};
  modal_descriptor.cbSize = sizeof(modal_descriptor);
  modal_descriptor.lpfnWndProc = modal_window_procedure;
  modal_descriptor.hInstance = instance;
  modal_descriptor.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  modal_descriptor.hbrBackground = dark_background_brush();
  modal_descriptor.lpszClassName = modal_window_class;
  if (RegisterClassExW(&modal_descriptor) == 0)
    return false;
  WNDCLASSEXW preview_descriptor{};
  preview_descriptor.cbSize = sizeof(preview_descriptor);
  preview_descriptor.lpfnWndProc = preview_window_procedure;
  preview_descriptor.hInstance = instance;
  preview_descriptor.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  preview_descriptor.hbrBackground = dark_background_brush();
  preview_descriptor.lpszClassName = preview_window_class;
  if (RegisterClassExW(&preview_descriptor) == 0) {
    UnregisterClassW(modal_window_class, instance);
    return false;
  }
  WNDCLASSEXW exposure_descriptor{};
  exposure_descriptor.cbSize = sizeof(exposure_descriptor);
  exposure_descriptor.lpfnWndProc = exposure_window_procedure;
  exposure_descriptor.hInstance = instance;
  exposure_descriptor.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  exposure_descriptor.hbrBackground = dark_background_brush();
  exposure_descriptor.lpszClassName = exposure_window_class;
  if (RegisterClassExW(&exposure_descriptor) == 0) {
    UnregisterClassW(preview_window_class, instance);
    UnregisterClassW(modal_window_class, instance);
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
  UnregisterClassW(modal_window_class, instance);
  return false;
}

int self_test(const HINSTANCE instance, GuiShellState &shell) {
  application_state().self_test_allows_warp = true;
  HWND window = CreateWindowExW(
      WS_EX_CONTROLPARENT, window_class, L"Cyberpunk Panorama Stitcher",
      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 780, nullptr,
      nullptr, instance, &shell);
  if (window == nullptr)
    return 11;
  if (&application_state() != &shell || shell_state(window) != &shell ||
      shell.runtime == nullptr || shell.accessibility_properties == nullptr)
    return 28;
  layout_controls(window);
  navigate_stage(window, pano::app::GuiStage::preview);
  if (!update_preview_surface())
    return 19;
  pano::app::GuiLayoutMetrics dpi96;
  pano::app::GuiLayoutMetrics dpi144;
  pano::app::GuiLayoutMetrics dpi192;
  std::string error;
  const bool layout_ok =
      pano::app::calculate_gui_layout_metrics(96, 720, dpi96, error) &&
      pano::app::calculate_gui_layout_metrics(144, 1080, dpi144, error) &&
      pano::app::calculate_gui_layout_metrics(192, 1440, dpi192, error) &&
      dpi144.margin * 2 == dpi96.margin * 3 &&
      dpi192.margin == dpi96.margin * 2;
  navigate_stage(window, pano::app::GuiStage::input);
  RECT session_header_bounds{};
  RECT session_list_bounds{};
  RECT status_bounds{};
  RECT options_bounds{};
  RECT preview_bounds{};
  GetWindowRect(application_state().controls.session_header_session,
                &session_header_bounds);
  GetWindowRect(application_state().controls.session_combo,
                &session_list_bounds);
  GetWindowRect(application_state().controls.status_label, &status_bounds);
  GetWindowRect(application_state().controls.input_options_button,
                &options_bounds);
  GetWindowRect(application_state().controls.preview_button, &preview_bounds);
  const int expected_options_width = button_content_width(
      application_state().controls.input_options_button,
      options_icon_width(application_state().controls.input_options_button));
  const int expected_preview_width =
      button_content_width(application_state().controls.preview_button);
  const int expected_table_height =
      MulDiv(288, static_cast<int>(GetDpiForWindow(window)), 96);
  shell.webview_enabled = true;
  RECT original_preview_bounds{};
  GetWindowRect(application_state().controls.preview_surface,
                &original_preview_bounds);
  MoveWindow(application_state().controls.preview_surface, 0, 0, 0, 0, FALSE);
  shell.workflow.stage = pano::app::GuiStage::preview;
  shell.webview_preview_visible = false;
  layout_controls(window);
  RECT seeded_preview_client{};
  GetClientRect(application_state().controls.preview_surface,
                &seeded_preview_client);
  const bool webview_preview_seed_ok = seeded_preview_client.right > 0;
  MoveWindow(application_state().controls.preview_surface, 7, 11, 123, 61,
             FALSE);
  RECT webview_preview_before_layout{};
  RECT webview_preview_after_layout{};
  GetWindowRect(application_state().controls.preview_surface,
                &webview_preview_before_layout);
  shell.webview_preview_visible = true;
  layout_controls(window);
  shell.webview_preview_visible = false;
  GetWindowRect(application_state().controls.preview_surface,
                &webview_preview_after_layout);
  shell.workflow.stage = pano::app::GuiStage::input;
  const bool webview_preview_layout_ok =
      EqualRect(&webview_preview_before_layout,
                &webview_preview_after_layout) != FALSE;
  MapWindowPoints(HWND_DESKTOP, window,
                  reinterpret_cast<POINT *>(&original_preview_bounds), 2U);
  MoveWindow(application_state().controls.preview_surface,
             original_preview_bounds.left, original_preview_bounds.top,
             original_preview_bounds.right - original_preview_bounds.left,
             original_preview_bounds.bottom - original_preview_bounds.top,
             FALSE);
  shell.webview_content_height = 650.0;
  MINMAXINFO webview_limits{};
  SendMessageW(window, WM_GETMINMAXINFO, 0,
               reinterpret_cast<LPARAM>(&webview_limits));
  const std::optional<int> webview_height =
      webview_outer_height(window, *shell.webview_content_height);
  MONITORINFO monitor_info{sizeof(monitor_info)};
  const bool webview_minimum_ok =
      webview_height.has_value() &&
      GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
                      &monitor_info) &&
      webview_limits.ptMinTrackSize.y ==
          std::min(*webview_height,
                   static_cast<int>(monitor_info.rcWork.bottom -
                                    monitor_info.rcWork.top));
  shell.webview_resize_timer_active = true;
  MINMAXINFO resizing_webview_limits{};
  SendMessageW(window, WM_GETMINMAXINFO, 0,
               reinterpret_cast<LPARAM>(&resizing_webview_limits));
  const bool webview_resize_minimum_ok =
      resizing_webview_limits.ptMinTrackSize.y ==
      MulDiv(480, static_cast<int>(GetDpiForWindow(window)), 96);
  shell.webview_resize_timer_active = false;
  RECT current_window_bounds{};
  GetWindowRect(window, &current_window_bounds);
  RECT horizontal_sizing_bounds = current_window_bounds;
  horizontal_sizing_bounds.right += 100;
  horizontal_sizing_bounds.bottom += 100;
  const bool horizontal_sizing_ok =
      preserve_webview_height_during_width_sizing(window, WMSZ_RIGHT,
                                                  horizontal_sizing_bounds) &&
      horizontal_sizing_bounds.bottom - horizontal_sizing_bounds.top ==
          current_window_bounds.bottom - current_window_bounds.top;
  RECT vertical_sizing_bounds = current_window_bounds;
  vertical_sizing_bounds.bottom += 100;
  const bool vertical_sizing_ok =
      !preserve_webview_height_during_width_sizing(window, WMSZ_BOTTOM,
                                                   vertical_sizing_bounds) &&
      vertical_sizing_bounds.bottom == current_window_bounds.bottom + 100;
  shell.webview_resize_timer_active = true;
  shell.webview_width_resize_dirty = true;
  shell.webview_sizing_active = true;
  shell.webview_sizing_changed_width = true;
  const std::uint64_t generation_before_finish =
      shell.webview_layout_generation;
  finish_webview_sizing(window, shell);
  const std::uint64_t generation_after_finish = shell.webview_layout_generation;
  finish_webview_sizing(window, shell);
  const bool webview_resize_cleanup_ok =
      generation_after_finish == generation_before_finish + 1U &&
      shell.webview_layout_generation == generation_after_finish &&
      !shell.webview_resize_timer_active && !shell.webview_width_resize_dirty &&
      !shell.webview_sizing_active && !shell.webview_sizing_changed_width;
  runtime_state().preview_hovered.assign(3U, 0U);
  pano::app::WebViewCommand exposure_command;
  exposure_command.kind = pano::app::WebViewCommandKind::set_exposure_reference;
  exposure_command.pose_index = 1U;
  handle_exposure_webview_command(window, exposure_command);
  exposure_command.kind =
      pano::app::WebViewCommandKind::toggle_exposure_selection;
  exposure_command.pose_index = 2U;
  handle_exposure_webview_command(window, exposure_command);
  const bool exposure_selection_ok =
      runtime_state().exposure_target == 1U &&
      runtime_state().exposure_selected == std::vector<unsigned>{2U};
  exposure_command.kind = pano::app::WebViewCommandKind::set_exposure_reference;
  handle_exposure_webview_command(window, exposure_command);
  const bool exposure_exclusion_ok = runtime_state().exposure_target == 2U &&
                                     runtime_state().exposure_selected.empty();
  exposure_command.kind = pano::app::WebViewCommandKind::reset_exposure;
  exposure_command.pose_index.reset();
  handle_exposure_webview_command(window, exposure_command);
  const bool exposure_reset_ok = !runtime_state().exposure_target.has_value() &&
                                 runtime_state().exposure_selected.empty();
  runtime_state().preview_hovered = {0U, 1U, 0U};
  const bool preview_reference_ok = apply_exposure_preview_click(true) &&
                                    runtime_state().exposure_target == 1U;
  runtime_state().preview_hovered = {1U, 1U, 0U};
  const bool preview_overlap_ok = !apply_exposure_preview_click(true) &&
                                  runtime_state().exposure_target == 1U;
  runtime_state().preview_hovered = {1U, 1U, 1U};
  const bool preview_manual_ok =
      apply_exposure_preview_click(false) &&
      runtime_state().exposure_target == 1U &&
      runtime_state().exposure_selected == std::vector<unsigned>({0U, 2U});
  runtime_state().preview_hovered = {0U, 1U, 0U};
  runtime_state().preview_overlay_frames.assign(3U, 0U);
  const bool overlay_hover_only_ok =
      prepare_preview_overlay_frames(false) &&
      runtime_state().preview_overlay_frames ==
          std::vector<std::uint8_t>({0U, 1U, 0U});
  runtime_state().preview_hovered = {0U, 0U, 0U};
  const bool overlay_selected_fill_ok =
      prepare_preview_overlay_frames(true) &&
      runtime_state().preview_overlay_frames ==
          std::vector<std::uint8_t>({1U, 1U, 1U});
  runtime_state().preview_hovered.clear();
  runtime_state().preview_overlay_frames.clear();
  shell.webview_content_height.reset();
  shell.webview_enabled = false;
  DWORD_PTR subclass_data = 0U;
  RECT edit_client{};
  RECT edit_formatting{};
  GetClientRect(application_state().controls.game_edit, &edit_client);
  SendMessageW(application_state().controls.game_edit, EM_GETRECT, 0,
               reinterpret_cast<LPARAM>(&edit_formatting));
  const bool shared_rules_ok =
      GetWindowSubclass(application_state().controls.preview_button,
                        button_window_procedure, 2U, &subclass_data) != FALSE &&
      GetWindowSubclass(application_state().controls.game_edit,
                        edit_window_procedure, 3U, &subclass_data) != FALSE &&
      GetWindowSubclass(application_state().controls.session_combo,
                        session_list_window_procedure, 4U,
                        &subclass_data) != FALSE &&
      edit_formatting.left > edit_client.left &&
      edit_formatting.top > edit_client.top &&
      edit_formatting.right < edit_client.right &&
      edit_formatting.bottom < edit_client.bottom;
  SendMessageW(application_state().controls.preview_button, WM_MOUSEMOVE, 0,
               MAKELPARAM(2, 2));
  const bool hover_enters =
      button_is_hot(application_state().controls.preview_button);
  SendMessageW(application_state().controls.preview_button, WM_MOUSELEAVE, 0,
               0);
  const bool hover_leaves =
      !button_is_hot(application_state().controls.preview_button);
  if (!webview_preview_seed_ok)
    return 34;
  if (!webview_preview_layout_ok)
    return 35;
  if (window_text(application_state().controls.game_label) !=
          L"Game directory:" ||
      window_text(application_state().controls.session_header_poses) !=
          L"  #" ||
      window_text(application_state().controls.session_header_actions) != L"" ||
      window_text(application_state().controls.image_label) !=
          L"Screenshots directory:" ||
      window_text(application_state().controls.preview_button) != L"Preview" ||
      (GetWindowLongPtrW(application_state().controls.game_edit, GWL_STYLE) &
       ES_READONLY) != 0 ||
      (GetWindowLongPtrW(application_state().controls.image_edit, GWL_STYLE) &
       ES_READONLY) != 0 ||
      session_header_bounds.bottom - session_header_bounds.top +
              session_list_bounds.bottom - session_list_bounds.top !=
          expected_table_height ||
      !webview_minimum_ok || !webview_resize_minimum_ok ||
      !horizontal_sizing_ok || !vertical_sizing_ok ||
      !webview_resize_cleanup_ok || !exposure_selection_ok ||
      !exposure_exclusion_ok || !exposure_reset_ok || !preview_reference_ok ||
      !preview_overlap_ok || !preview_manual_ok || !overlay_hover_only_ok ||
      !overlay_selected_fill_ok || !shared_rules_ok || !hover_enters ||
      !hover_leaves || status_bounds.top != options_bounds.top ||
      options_bounds.top != preview_bounds.top ||
      options_bounds.right - options_bounds.left != expected_options_width ||
      preview_bounds.right - preview_bounds.left != expected_preview_width)
    return 31;
  const std::array<HWND, 44> tabbable{
      application_state().controls.input_stage_button,
      application_state().controls.preview_stage_button,
      application_state().controls.output_stage_button,
      application_state().controls.settings_button,
      application_state().controls.input_options_button,
      application_state().controls.preview_options_button,
      application_state().controls.exposure_panel_button,
      application_state().controls.resolution_mode_button,
      application_state().controls.game_edit,
      application_state().controls.game_browse_button,
      application_state().controls.refresh_button,
      application_state().controls.session_combo,
      application_state().controls.image_edit,
      application_state().controls.image_browse_button,
      application_state().controls.output_edit,
      application_state().controls.output_browse_button,
      application_state().controls.output_name_edit,
      application_state().controls.format_jpeg,
      application_state().controls.format_png,
      application_state().controls.format_exr,
      application_state().controls.quality_edit,
      application_state().controls.quality_slider,
      application_state().controls.resolution_edit,
      application_state().controls.resolution_slider,
      application_state().controls.width_edit,
      application_state().controls.blend_combo,
      application_state().controls.memory_edit,
      application_state().controls.workers_edit,
      application_state().controls.thumbnail_check,
      application_state().controls.coverage_check,
      application_state().controls.incomplete_check,
      application_state().controls.auto_contrast_check,
      application_state().controls.gpu_check,
      application_state().controls.gpu_strict_check,
      application_state().controls.automatic_exposure_button,
      application_state().controls.match_exposure_button,
      application_state().controls.discard_exposure_button,
      application_state().controls.delete_session_button,
      application_state().controls.delete_images_check,
      application_state().controls.preview_button,
      application_state().controls.preview_next_button,
      application_state().controls.render_button,
      application_state().controls.render_thumbnail_button,
      application_state().controls.cancel_button};
  if (!layout_ok)
    return 12;
  for (const HWND control : tabbable)
    if (control == nullptr ||
        (GetWindowLongPtrW(control, GWL_STYLE) & WS_TABSTOP) == 0)
      return 13;
  if (window_text(application_state().controls.output_name_edit) !=
          L"panorama.jpg" ||
      window_text(application_state().controls.quality_edit) != L"95" ||
      window_text(application_state().controls.resolution_edit) != L"100" ||
      window_text(application_state().controls.memory_edit) != L"1024" ||
      window_text(application_state().controls.workers_edit) != L"0" ||
      SendMessageW(application_state().controls.format_combo, CB_GETCURSEL, 0,
                   0) != 0 ||
      !output_format_selected(format_jpeg_id) ||
      output_format_selected(format_png_id) ||
      output_format_selected(format_exr_id) ||
      SendMessageW(application_state().controls.resolution_slider, TBM_GETPOS,
                   0, 0) != 100 ||
      SendMessageW(application_state().controls.quality_slider, TBM_GETPOS, 0,
                   0) != 95 ||
      SendMessageW(application_state().controls.blend_combo, CB_GETCURSEL, 0,
                   0) != 1 ||
      !checked(application_state().controls.auto_contrast_check) ||
      !checked(application_state().controls.gpu_check) ||
      checked(application_state().controls.thumbnail_check) ||
      checked(application_state().controls.coverage_check) ||
      checked(application_state().controls.incomplete_check) ||
      checked(application_state().controls.gpu_strict_check) ||
      IsWindowEnabled(application_state().controls.automatic_exposure_button) ||
      IsWindowEnabled(application_state().controls.match_exposure_button) ||
      IsWindowEnabled(application_state().controls.discard_exposure_button) ||
      IsWindowEnabled(application_state().controls.exposure_panel_button) ||
      IsWindowVisible(application_state().controls.automatic_exposure_button) ||
      IsWindowVisible(application_state().controls.match_exposure_button) ||
      IsWindowVisible(application_state().controls.discard_exposure_button) ||
      IsWindowEnabled(application_state().controls.delete_session_button) ||
      IsWindowEnabled(application_state().controls.render_thumbnail_button) ||
      checked(application_state().controls.delete_images_check))
    return 17;
  SetWindowTextW(application_state().controls.resolution_edit, L"42");
  SendMessageW(
      window, WM_COMMAND, MAKEWPARAM(format_png_id, BN_CLICKED),
      reinterpret_cast<LPARAM>(application_state().controls.format_png));
  if (SendMessageW(application_state().controls.resolution_slider, TBM_GETPOS,
                   0, 0) != 42)
    return 32;
  if (output_format_selected(format_jpeg_id) ||
      !output_format_selected(format_png_id) ||
      output_format_selected(format_exr_id))
    return 33;
  SetWindowTextW(application_state().controls.resolution_edit, L"100");
  SendMessageW(
      window, WM_COMMAND, MAKEWPARAM(format_jpeg_id, BN_CLICKED),
      reinterpret_cast<LPARAM>(application_state().controls.format_jpeg));
  const std::uint64_t stale_generation =
      pano::app::begin_gui_session_refresh(runtime_state().refresh_state);
  const std::uint64_t current_generation =
      pano::app::begin_gui_session_refresh(runtime_state().refresh_state);
  auto stale = std::make_unique<RefreshResult>();
  stale->generation = stale_generation;
  stale->records.push_back({"stale", {}, {}, {}});
  auto current = std::make_unique<RefreshResult>();
  current->generation = current_generation;
  current->records.resize(2);
  current->records[0].session.session_id = "complete";
  current->records[0].session.completed = true;
  current->records[1].session.session_id = "invalid";
  current->records[1].error = "broken";
  pano::app::mark_application_session_stitched(
      application_state().application_settings, "", "complete", "output.jpg");
  {
    std::lock_guard<std::mutex> lock(runtime_state().refresh_mutex);
    runtime_state().refresh_results.push_back(std::move(stale));
    runtime_state().refresh_results.push_back(std::move(current));
  }
  SendMessageW(window, refresh_complete_message, 0, 0);
  std::array<wchar_t, 256> first_label{};
  LVITEMW first_item{};
  first_item.iSubItem = 0;
  first_item.pszText = first_label.data();
  first_item.cchTextMax = static_cast<int>(first_label.size());
  SendMessageW(application_state().controls.session_combo, LVM_GETITEMTEXTW, 0,
               reinterpret_cast<LPARAM>(&first_item));
  const auto *const test_shell = shell_state(window);
  if (ListView_GetItemCount(application_state().controls.session_combo) != 2 ||
      Header_GetItemCount(ListView_GetHeader(
          application_state().controls.session_combo)) != 4 ||
      runtime_state().refresh_state.records.size() != 2U ||
      runtime_state().refresh_state.records[0].session.session_id !=
          "complete" ||
      std::wstring(first_label.data()) != L"complete" ||
      test_shell == nullptr || test_shell->selected_record != 0U ||
      pano::app::gui_session_status(runtime_state().refresh_state.records[0],
                                    true) !=
          pano::app::GuiSessionStatus::stitched ||
      !IsWindowEnabled(application_state().controls.delete_session_button))
    return 16;
  const auto stale_validation =
      pano::app::begin_gui_validation(runtime_state().validation_state);
  const auto current_validation =
      pano::app::begin_gui_validation(runtime_state().validation_state);
  auto stale_plan = std::make_unique<ValidationResult>();
  stale_plan->generation = stale_validation;
  stale_plan->error = "stale";
  auto current_plan = std::make_unique<ValidationResult>();
  current_plan->generation = current_validation;
  current_plan->plan.emplace();
  current_plan->plan->blend = "feather";
  {
    std::lock_guard<std::mutex> lock(runtime_state().validation_mutex);
    runtime_state().validation_results.push_back(std::move(stale_plan));
    runtime_state().validation_results.push_back(std::move(current_plan));
  }
  SendMessageW(window, validation_complete_message, 0, 0);
  if (!runtime_state().validation_state.plan.has_value() ||
      runtime_state().validation_state.plan->blend != "feather" ||
      !IsWindowEnabled(application_state().controls.preview_button) ||
      IsWindowEnabled(application_state().controls.render_button))
    return 18;
  if (runtime_state().preview_surface == nullptr)
    return 19;
  std::array<char, 512> gpu_error{};
  pano_gpu_preview_surface_diagnostics surface_diagnostics{};
  surface_diagnostics.size = sizeof(surface_diagnostics);
  surface_diagnostics.abi_version = PANO_GPU_ABI_VERSION;
  if (pano_gpu_preview_surface_query_diagnostics(
          runtime_state().preview_surface, &surface_diagnostics,
          gpu_error.data(),
          static_cast<uint32_t>(gpu_error.size())) != PANO_GPU_SUCCESS ||
      surface_diagnostics.width == 0 || surface_diagnostics.height == 0 ||
      surface_diagnostics.present_count == 0 ||
      surface_diagnostics.live_surface_count != 1U)
    return 20;
  constexpr float resized_color[4]{0.1F, 0.2F, 0.3F, 1.0F};
  if (pano_gpu_preview_surface_resize(
          runtime_state().preview_surface, 321, 123, gpu_error.data(),
          static_cast<uint32_t>(gpu_error.size())) != PANO_GPU_SUCCESS ||
      pano_gpu_preview_surface_clear_present(
          runtime_state().preview_surface, resized_color, gpu_error.data(),
          static_cast<uint32_t>(gpu_error.size())) != PANO_GPU_SUCCESS ||
      pano_gpu_preview_surface_query_diagnostics(
          runtime_state().preview_surface, &surface_diagnostics,
          gpu_error.data(),
          static_cast<uint32_t>(gpu_error.size())) != PANO_GPU_SUCCESS ||
      surface_diagnostics.width != 321U || surface_diagnostics.height != 123U ||
      surface_diagnostics.resize_count != 1U)
    return 25;
  if (pano_gpu_preview_surface_resize(
          runtime_state().preview_surface, 0, 0, gpu_error.data(),
          static_cast<uint32_t>(gpu_error.size())) != PANO_GPU_SUCCESS)
    return 26;
  pano_gpu_preview_surface_destroy(&runtime_state().preview_surface);
  pano_gpu_preview_surface_destroy(&runtime_state().preview_surface);
  if (!update_preview_surface() || runtime_state().preview_surface == nullptr)
    return 27;
  const BOOL first_close = DestroyWindow(window);
  if (first_close == FALSE)
    return 14;
  pano_gpu_diagnostics diagnostics{};
  diagnostics.size = sizeof(diagnostics);
  diagnostics.abi_version = PANO_GPU_ABI_VERSION;
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
      application_state().accessibility_properties != nullptr ||
      application_state().taskbar != nullptr ||
      pano_gpu_query_diagnostics(&diagnostics, gpu_error.data(),
                                 static_cast<uint32_t>(gpu_error.size())) !=
          PANO_GPU_SUCCESS ||
      diagnostics.live_device_count != 0U ||
      diagnostics.live_queue_count != 0U ||
      diagnostics.live_fence_count != 0U ||
      diagnostics.live_session_count != 0U ||
      diagnostics.live_output_count != 0U)
    return 29;
  SetLastError(ERROR_SUCCESS);
  const BOOL repeated_close = DestroyWindow(window);
  if (repeated_close != FALSE)
    return 15;
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
  bool native_ui_requested = false;
  for (int index = 1; arguments != nullptr && index < argument_count; ++index) {
    self_test_requested =
        self_test_requested || std::wstring(arguments[index]) == L"--self-test";
    native_ui_requested =
        native_ui_requested || std::wstring(arguments[index]) == L"--native-ui";
  }
  if (arguments != nullptr)
    LocalFree(arguments);
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const HRESULT com_result = CoInitializeEx(
      nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  if (FAILED(com_result))
    return 24;
  struct ComCleanup {
    ~ComCleanup() { CoUninitialize(); }
  } com_cleanup;
  INITCOMMONCONTROLSEX common_controls{};
  common_controls.dwSize = sizeof(common_controls);
  common_controls.dwICC = ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS;
  if (!InitCommonControlsEx(&common_controls))
    return 21;
  if (!register_window_class(instance))
    return 22;
  GuiShellState shell;
  shell.runtime = std::make_unique<GuiRuntimeState>();
  shell.webview_enabled = !native_ui_requested && !self_test_requested;
  active_application_state = &shell;
  if (self_test_requested) {
    const int result = self_test(instance, shell);
    UnregisterClassW(window_class, instance);
    UnregisterClassW(preview_window_class, instance);
    UnregisterClassW(exposure_window_class, instance);
    UnregisterClassW(modal_window_class, instance);
    if (application_state().body_font != nullptr)
      DeleteObject(application_state().body_font);
    if (application_state().heading_font != nullptr)
      DeleteObject(application_state().heading_font);
    application_state().body_font = nullptr;
    application_state().heading_font = nullptr;
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
  UnregisterClassW(modal_window_class, instance);
  if (application_state().body_font != nullptr)
    DeleteObject(application_state().body_font);
  if (application_state().heading_font != nullptr)
    DeleteObject(application_state().heading_font);
  application_state().body_font = nullptr;
  application_state().heading_font = nullptr;
  active_application_state = nullptr;
  return static_cast<int>(message.wParam);
}
