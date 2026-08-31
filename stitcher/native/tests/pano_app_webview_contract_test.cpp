#include "pano_app_resource.h"
#include "pano_app_webview.h"

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int fail(const int line, const char *const expression) {
  std::cerr << "contract check failed at line " << line << ": " << expression
            << '\n';
  return 1;
}

#define CHECK(expression)                                                      \
  do {                                                                         \
    if (!(expression))                                                         \
      return fail(__LINE__, #expression);                                      \
  } while (false)

} // namespace

int main() {
  const HINSTANCE module = GetModuleHandleW(nullptr);
  CHECK(module != nullptr);
  CHECK(FindResourceW(module, MAKEINTRESOURCEW(PANO_APP_ICON),
                      MAKEINTRESOURCEW(14)) != nullptr);
  CHECK(LoadIconW(module, MAKEINTRESOURCEW(PANO_APP_ICON)) != nullptr);
  CHECK(FindResourceW(module, MAKEINTRESOURCEW(PANO_APP_EXPOSURE_HTML),
                      MAKEINTRESOURCEW(10)) != nullptr);

  const pano::app::WebViewRuntimeInfo runtime =
      pano::app::query_webview_runtime();
  CHECK(runtime.available == SUCCEEDED(runtime.status));
  CHECK(runtime.available == !runtime.version.empty());

  const std::filesystem::path profile = pano::app::webview_user_data_folder();
  CHECK(profile.filename() == L"WebView2");
  CHECK(profile.parent_path().filename() == L"PanoramaStitcher");
  CHECK(profile.is_absolute());

  const std::wstring_view download_url =
      pano::app::webview_runtime_download_url();
  CHECK(download_url.rfind(L"https://", 0) == 0);
  CHECK(download_url.find(L"webview2") != std::wstring_view::npos);

  const std::wstring html = pano::app::embedded_webview_html();
  CHECK(html.find(L"Content-Security-Policy") != std::wstring::npos);
  CHECK(html.find(L"<style id=\"_style\">") != std::wstring::npos);
  CHECK(html.find(L"<script>") != std::wstring::npos);
  CHECK(html.find(L"tailwindcss v4.3.3") != std::wstring::npos);
  CHECK(html.find(L"id=\"ui-root\"") != std::wstring::npos);
  CHECK(html.find(L"function h(type, properties") != std::wstring::npos);
  CHECK(html.find(L"function hu(target, properties") != std::wstring::npos);
  CHECK(html.find(L"function App({ snapshot, bridgeFailed })") !=
        std::wstring::npos);
  CHECK(html.find(L"function InputView({ snapshot })") != std::wstring::npos);
  CHECK(html.find(L"function PreviewView({ snapshot })") != std::wstring::npos);
  CHECK(html.find(L"snapshot?.exposureAdjusted") != std::wstring::npos);
  CHECK(html.find(L"id: 'exposure-adjusted'") != std::wstring::npos);
  CHECK(html.find(L"Exposure has been adjusted") != std::wstring::npos);
  CHECK(html.find(L"function redrawChildren(children)") != std::wstring::npos);
  CHECK(html.find(L"snapshot?.sessions.forEach") != std::wstring::npos);
  CHECK(html.find(L"Object.is(previous[name], value)") != std::wstring::npos);
  CHECK(html.find(L"nodeKeys") == std::wstring::npos);
  CHECK(html.find(L"post('content-size', {") != std::wstring::npos);
  CHECK(html.find(L"runtime.lastContentHeight === height &&") !=
        std::wstring::npos);
  CHECK(
      html.find(L"runtime.lastContentLayoutGeneration === layoutGeneration") !=
      std::wstring::npos);
  CHECK(html.find(L"runtime.naturalContentHeight = height") !=
        std::wstring::npos);
  CHECK(html.find(L"document.documentElement.clientHeight + 1") !=
        std::wstring::npos);
  CHECK(html.find(L"contentFits ? 'hidden' : 'auto'") != std::wstring::npos);
  CHECK(html.find(L"runtime.lastPreviewGeometry === signature") !=
        std::wstring::npos);
  CHECK(html.find(L"runtime.deviceScale !== window.devicePixelRatio") !=
        std::wstring::npos);
  CHECK(html.find(L"addEventListener('resize', handleWindowResize)") !=
        std::wstring::npos);
  CHECK(html.find(L"addEventListener('resize', queueContentSize)") ==
        std::wstring::npos);
  CHECK(html.find(L"id: 'input-view'") != std::wstring::npos);
  CHECK(html.find(L"id: 'preview-view'") != std::wstring::npos);
  CHECK(html.find(L"id: 'preview-placeholder'") != std::wstring::npos);
  CHECK(html.find(L"progress: snapshot?.busy") != std::wstring::npos);
  CHECK(html.find(L"done: !(snapshot?.busy ?? false)") != std::wstring::npos);
  CHECK(html.find(L"className: 'tag-cell'") == std::wstring::npos);
  CHECK(html.find(L"onClick: () => post('edit-tag', { index })") !=
        std::wstring::npos);
  CHECK(html.find(L"bg-gray-900 p-2") != std::wstring::npos);
  CHECK(html.find(L"pano_app_ui.css") == std::wstring::npos);
  CHECK(html.find(L"pano_app_ui.js") == std::wstring::npos);
  CHECK(html.find(L"https://cdn") == std::wstring::npos);
  CHECK(html.find(L"data:image") == std::wstring::npos);
  CHECK(html.find(L"type=\"file\"") == std::wstring::npos);
  CHECK(html.find(L"<img") == std::wstring::npos);
  const std::wstring exposure_html =
      pano::app::embedded_webview_html(pano::app::WebViewPage::exposure);
  CHECK(exposure_html.find(L"class=\"exposure-page\"") != std::wstring::npos);
  CHECK(exposure_html.find(L"id=\"show-overlay\"") != std::wstring::npos);
  CHECK(exposure_html.find(L"set-exposure-reference") != std::wstring::npos);
  CHECK(exposure_html.find(L"toggle-exposure-selection") != std::wstring::npos);
  CHECK(exposure_html.find(L"post('content-size', {") != std::wstring::npos);
  CHECK(exposure_html.find(L"lastContentHeight = null") != std::wstring::npos);
  CHECK(exposure_html.find(L"aria-disabled") != std::wstring::npos);
  CHECK(exposure_html.find(L"addEventListener('resize'") != std::wstring::npos);
  CHECK(exposure_html.find(L"pano_app_exposure.js") == std::wstring::npos);
  CHECK(exposure_html.find(L"https://cdn") == std::wstring::npos);

  pano::app::WebViewCommand command;
  std::string error;
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"ready","pageGeneration":0})", command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::ready);
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-directory","pageGeneration":3,"target":"game","value":"C:\\Games\\Cyberpunk 2077"})",
      command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::set_game_directory);
  CHECK(command.page_generation == 3U);
  CHECK(command.value == L"C:\\Games\\Cyberpunk 2077");
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"select-session","pageGeneration":4,"index":2})",
      command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::select_session);
  CHECK(command.session_index == 2U);
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-exposure-overlay","pageGeneration":2,"enabled":true})",
      command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::set_exposure_overlay);
  CHECK(command.enabled == true);
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-exposure-reference","pageGeneration":2,"index":12})",
      command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::set_exposure_reference);
  CHECK(command.pose_index == 12U);
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-exposure-overlay","pageGeneration":2,"enabled":1})",
      command, error));
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"hover-exposure-pose","pageGeneration":2,"index":-1})",
      command, error));
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"content-size","pageGeneration":5,"layoutGeneration":8,"height":612.5})",
      command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::content_size);
  CHECK(command.layout_generation == 8U);
  CHECK(command.content_height == 612.5);
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"content-size","pageGeneration":5,"layoutGeneration":8,"height":0})",
      command, error));
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"content-size","pageGeneration":5,"height":612.5})",
      command, error));
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"content-size","pageGeneration":5,"layoutGeneration":8,"height":612.5,"width":920})",
      command, error));
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"preview-geometry","pageGeneration":5,"layoutGeneration":8,"x":16.25,"y":104,"width":868,"height":434,"deviceScale":1.5,"visible":true})",
      command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::preview_geometry);
  CHECK(command.preview.has_value());
  CHECK(command.preview->layout_generation == 8U);
  CHECK(command.preview->device_scale == 1.5);
  RECT bounds{};
  pano::app::WebViewPreviewGeometry geometry = *command.preview;
  geometry.x = 16.0;
  geometry.y = 104.0;
  geometry.width = 868.0;
  geometry.height = 434.0;
  geometry.device_scale = 1.0;
  CHECK(pano::app::calculate_webview_preview_bounds(geometry, 1.0, 920, 680,
                                                    bounds, error));
  CHECK(bounds.left == 16 && bounds.top == 104 && bounds.right == 884 &&
        bounds.bottom == 538);
  geometry.x = 16.25;
  geometry.device_scale = 1.5;
  CHECK(pano::app::calculate_webview_preview_bounds(geometry, 1.5, 1380, 1020,
                                                    bounds, error));
  CHECK(bounds.left == 24 && bounds.top == 156 && bounds.right == 1326 &&
        bounds.bottom == 807);
  geometry.device_scale = 1.0;
  CHECK(!pano::app::calculate_webview_preview_bounds(geometry, 1.5, 1380, 1020,
                                                     bounds, error));
  geometry.device_scale = 1.5;
  geometry.width = 1000.0;
  CHECK(!pano::app::calculate_webview_preview_bounds(geometry, 1.5, 1380, 1020,
                                                     bounds, error));
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"preview-geometry","pageGeneration":5,"layoutGeneration":8,"x":16,"y":104,"width":868,"height":434,"deviceScale":1.5})",
      command, error));
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"preview-geometry","pageGeneration":5,"layoutGeneration":8,"x":16,"y":104,"width":868,"height":434,"deviceScale":1.5,"visible":true,"hwnd":42})",
      command, error));
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":2,"kind":"ready","pageGeneration":0})", command, error));
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"ready","pageGeneration":0,"extra":true})",
      command, error));
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"shell","pageGeneration":0,"value":"cmd.exe"})",
      command, error));

  std::wcout << L"WebView2 runtime: "
             << (runtime.available ? runtime.version : L"not installed")
             << L'\n';
  return 0;
}
