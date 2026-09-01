#ifndef PANO_APP_WEBVIEW_H
#define PANO_APP_WEBVIEW_H

#ifdef _WIN32

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace pano::app {

struct WebViewRuntimeInfo {
  bool available = false;
  HRESULT status = E_FAIL;
  std::wstring version;
};

[[nodiscard]] WebViewRuntimeInfo query_webview_runtime() noexcept;
[[nodiscard]] std::filesystem::path webview_user_data_folder();
[[nodiscard]] const wchar_t *webview_runtime_download_url() noexcept;

// Returns true when the runtime is ready. Otherwise, gives the user a native
// download/retry/exit path without requiring WebView2 to create the prompt.
[[nodiscard]] bool ensure_webview_runtime(HWND owner);

enum class WebViewCommandKind {
  ready,
  set_game_directory,
  set_screenshots_directory,
  set_output_directory,
  browse_game_directory,
  browse_screenshots_directory,
  browse_output_directory,
  refresh,
  select_session,
  copy_session_coordinates,
  edit_tag,
  delete_session,
  navigate_input,
  navigate_preview,
  navigate_output,
  set_output_name,
  toggle_resolution_mode,
  set_resolution_percent,
  set_output_width,
  set_output_format,
  set_jpeg_quality,
  open_settings,
  open_options,
  open_exposure,
  set_exposure_overlay,
  hover_exposure_pose,
  clear_exposure_hover,
  set_exposure_reference,
  toggle_exposure_selection,
  set_final_exposure,
  reset_exposure,
  equalize_exposure,
  abort_operation,
  start_preview,
  finalize_preview,
  render,
  render_with_thumbnail,
  set_modal_value,
  set_modal_toggle,
  submit_modal,
  dismiss_modal,
  content_size,
  preview_geometry,
  host_failed
};

enum class WebViewCommandDomain {
  host,
  directories,
  sessions,
  navigation,
  output,
  modal,
  exposure,
  operation,
  layout
};

inline constexpr std::size_t webview_command_kind_count =
    static_cast<std::size_t>(WebViewCommandKind::host_failed) + 1U;

[[nodiscard]] constexpr WebViewCommandDomain
webview_command_domain(const WebViewCommandKind kind) noexcept {
  switch (kind) {
  case WebViewCommandKind::ready:
  case WebViewCommandKind::host_failed:
    return WebViewCommandDomain::host;
  case WebViewCommandKind::set_game_directory:
  case WebViewCommandKind::set_screenshots_directory:
  case WebViewCommandKind::set_output_directory:
  case WebViewCommandKind::browse_game_directory:
  case WebViewCommandKind::browse_screenshots_directory:
  case WebViewCommandKind::browse_output_directory:
    return WebViewCommandDomain::directories;
  case WebViewCommandKind::refresh:
  case WebViewCommandKind::select_session:
  case WebViewCommandKind::copy_session_coordinates:
  case WebViewCommandKind::edit_tag:
  case WebViewCommandKind::delete_session:
    return WebViewCommandDomain::sessions;
  case WebViewCommandKind::navigate_input:
  case WebViewCommandKind::navigate_preview:
  case WebViewCommandKind::navigate_output:
    return WebViewCommandDomain::navigation;
  case WebViewCommandKind::set_output_name:
  case WebViewCommandKind::toggle_resolution_mode:
  case WebViewCommandKind::set_resolution_percent:
  case WebViewCommandKind::set_output_width:
  case WebViewCommandKind::set_output_format:
  case WebViewCommandKind::set_jpeg_quality:
    return WebViewCommandDomain::output;
  case WebViewCommandKind::open_settings:
  case WebViewCommandKind::open_options:
  case WebViewCommandKind::set_modal_value:
  case WebViewCommandKind::set_modal_toggle:
  case WebViewCommandKind::submit_modal:
  case WebViewCommandKind::dismiss_modal:
    return WebViewCommandDomain::modal;
  case WebViewCommandKind::open_exposure:
  case WebViewCommandKind::set_exposure_overlay:
  case WebViewCommandKind::hover_exposure_pose:
  case WebViewCommandKind::clear_exposure_hover:
  case WebViewCommandKind::set_exposure_reference:
  case WebViewCommandKind::toggle_exposure_selection:
  case WebViewCommandKind::set_final_exposure:
  case WebViewCommandKind::reset_exposure:
  case WebViewCommandKind::equalize_exposure:
    return WebViewCommandDomain::exposure;
  case WebViewCommandKind::abort_operation:
  case WebViewCommandKind::start_preview:
  case WebViewCommandKind::finalize_preview:
  case WebViewCommandKind::render:
  case WebViewCommandKind::render_with_thumbnail:
    return WebViewCommandDomain::operation;
  case WebViewCommandKind::content_size:
  case WebViewCommandKind::preview_geometry:
    return WebViewCommandDomain::layout;
  }
  return WebViewCommandDomain::host;
}

struct WebViewPreviewGeometry {
  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;
  double device_scale = 0.0;
  std::uint64_t layout_generation = 0;
  bool visible = false;
};

[[nodiscard]] bool
calculate_webview_preview_bounds(const WebViewPreviewGeometry &geometry,
                                 double rasterization_scale, LONG client_width,
                                 LONG client_height, RECT &bounds,
                                 std::string &error) noexcept;

struct WebViewCommand {
  WebViewCommandKind kind = WebViewCommandKind::host_failed;
  std::uint64_t page_generation = 0;
  std::optional<std::size_t> session_index;
  std::optional<unsigned> pose_index;
  std::optional<double> exposure_ev;
  std::optional<bool> enabled;
  std::optional<std::uint64_t> modal_generation;
  std::optional<std::uint64_t> layout_generation;
  std::optional<double> content_height;
  std::optional<WebViewPreviewGeometry> preview;
  std::wstring value;
};

[[nodiscard]] bool parse_webview_command_json(std::string_view json,
                                              WebViewCommand &command,
                                              std::string &error);
[[nodiscard]] bool
webview_command_is_current(bool ready, std::uint64_t page_generation,
                           const WebViewCommand &command) noexcept;
[[nodiscard]] bool
webview_modal_command_is_current(bool modal_open,
                                 std::uint64_t modal_generation,
                                 const WebViewCommand &command) noexcept;
enum class WebViewPage { main, exposure };

[[nodiscard]] std::wstring
embedded_webview_html(WebViewPage page = WebViewPage::main);

struct WebViewHostState;

class WebViewHost {
public:
  using CommandHandler = std::function<void(const WebViewCommand &)>;

  WebViewHost(HWND owner, CommandHandler handler,
              WebViewPage page = WebViewPage::main);
  ~WebViewHost();
  WebViewHost(const WebViewHost &) = delete;
  WebViewHost &operator=(const WebViewHost &) = delete;

  [[nodiscard]] bool start(std::wstring &error);
  void resize(const RECT &bounds) noexcept;
  [[nodiscard]] bool post_snapshot(std::wstring_view json,
                                   std::wstring &error) const;
  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] std::uint64_t page_generation() const noexcept;
  [[nodiscard]] double rasterization_scale() const noexcept;
  void close() noexcept;

private:
  std::shared_ptr<WebViewHostState> state_;
};

} // namespace pano::app

#endif

#endif
