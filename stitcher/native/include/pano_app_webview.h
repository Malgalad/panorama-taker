#ifndef PANO_APP_WEBVIEW_H
#define PANO_APP_WEBVIEW_H

#ifdef _WIN32

#include <windows.h>

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
  browse_game_directory,
  browse_screenshots_directory,
  refresh,
  select_session,
  edit_tag,
  delete_session,
  navigate_input,
  navigate_preview,
  open_settings,
  open_options,
  open_exposure,
  set_exposure_overlay,
  hover_exposure_pose,
  clear_exposure_hover,
  set_exposure_reference,
  toggle_exposure_selection,
  reset_exposure,
  equalize_exposure,
  abort_operation,
  start_preview,
  finalize_preview,
  content_size,
  preview_geometry,
  host_failed
};

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
  std::optional<bool> enabled;
  std::optional<std::uint64_t> layout_generation;
  std::optional<double> content_height;
  std::optional<WebViewPreviewGeometry> preview;
  std::wstring value;
};

[[nodiscard]] bool parse_webview_command_json(std::string_view json,
                                              WebViewCommand &command,
                                              std::string &error);
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
