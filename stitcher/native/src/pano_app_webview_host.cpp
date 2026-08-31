#include "pano_app_webview.h"

#ifdef _WIN32

#include "pano_app_resource.h"

#include <WebView2.h>
#include <wrl.h>

#include "yyjson.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <utility>

namespace pano::app {
namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

std::string wide_to_utf8(const std::wstring_view value) {
  if (value.empty())
    return {};
  const int bytes = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (bytes <= 0)
    return {};
  std::string result(static_cast<std::size_t>(bytes), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), result.data(), bytes,
                          nullptr, nullptr) != bytes)
    return {};
  return result;
}

std::wstring utf8_to_wide(const std::string_view value) {
  if (value.empty())
    return {};
  const int characters =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), nullptr, 0);
  if (characters <= 0)
    return {};
  std::wstring result(static_cast<std::size_t>(characters), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), result.data(),
                          characters) != characters)
    return {};
  return result;
}

std::wstring hresult_message(const wchar_t *const action,
                             const HRESULT status) {
  std::wostringstream message;
  message << action << L" (0x" << std::hex << static_cast<unsigned long>(status)
          << L")";
  return message.str();
}

bool exact_object_shape(yyjson_val *const root,
                        const std::initializer_list<std::string_view> keys) {
  if (!yyjson_is_obj(root) || yyjson_obj_size(root) != keys.size())
    return false;
  yyjson_obj_iter iterator = yyjson_obj_iter_with(root);
  yyjson_val *key = nullptr;
  while ((key = yyjson_obj_iter_next(&iterator)) != nullptr) {
    const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
    bool allowed = false;
    for (const std::string_view candidate : keys)
      allowed = allowed || name == candidate;
    if (!allowed)
      return false;
  }
  return true;
}

} // namespace

bool parse_webview_command_json(const std::string_view json,
                                WebViewCommand &command, std::string &error) {
  command = {};
  yyjson_read_err read_error{};
  yyjson_doc *const document =
      yyjson_read_opts(const_cast<char *>(json.data()), json.size(),
                       YYJSON_READ_NOFLAG, nullptr, &read_error);
  if (document == nullptr) {
    error = "invalid WebView command JSON";
    return false;
  }
  struct DocumentCleanup {
    yyjson_doc *document;
    ~DocumentCleanup() { yyjson_doc_free(document); }
  } cleanup{document};
  yyjson_val *const root = yyjson_doc_get_root(document);
  yyjson_val *const version = yyjson_obj_get(root, "version");
  yyjson_val *const kind = yyjson_obj_get(root, "kind");
  yyjson_val *const generation = yyjson_obj_get(root, "pageGeneration");
  if (!yyjson_is_uint(version) || yyjson_get_uint(version) != 1U ||
      !yyjson_is_str(kind) || !yyjson_is_uint(generation)) {
    error = "invalid WebView command envelope";
    return false;
  }
  command.page_generation = yyjson_get_uint(generation);
  const std::string_view name(yyjson_get_str(kind), yyjson_get_len(kind));
  const auto simple = [&](const std::string_view expected,
                          const WebViewCommandKind parsed) {
    if (name != expected ||
        !exact_object_shape(root, {"version", "kind", "pageGeneration"}))
      return false;
    command.kind = parsed;
    return true;
  };
  if (simple("ready", WebViewCommandKind::ready) ||
      simple("refresh", WebViewCommandKind::refresh) ||
      simple("open-settings", WebViewCommandKind::open_settings) ||
      simple("open-options", WebViewCommandKind::open_options) ||
      simple("open-exposure", WebViewCommandKind::open_exposure) ||
      simple("clear-exposure-hover",
             WebViewCommandKind::clear_exposure_hover) ||
      simple("reset-exposure", WebViewCommandKind::reset_exposure) ||
      simple("equalize-exposure", WebViewCommandKind::equalize_exposure) ||
      simple("abort", WebViewCommandKind::abort_operation) ||
      simple("start-preview", WebViewCommandKind::start_preview) ||
      simple("finalize", WebViewCommandKind::finalize_preview)) {
    error.clear();
    return true;
  }
  if (name == "set-exposure-overlay") {
    if (!exact_object_shape(root,
                            {"version", "kind", "pageGeneration", "enabled"}) ||
        !yyjson_is_bool(yyjson_obj_get(root, "enabled"))) {
      error = "invalid WebView exposure overlay command";
      return false;
    }
    command.kind = WebViewCommandKind::set_exposure_overlay;
    command.enabled = yyjson_get_bool(yyjson_obj_get(root, "enabled"));
    error.clear();
    return true;
  }
  if (name == "hover-exposure-pose" || name == "set-exposure-reference" ||
      name == "toggle-exposure-selection") {
    if (!exact_object_shape(root,
                            {"version", "kind", "pageGeneration", "index"}) ||
        !yyjson_is_uint(yyjson_obj_get(root, "index")) ||
        yyjson_get_uint(yyjson_obj_get(root, "index")) >
            std::numeric_limits<unsigned>::max()) {
      error = "invalid WebView exposure pose command";
      return false;
    }
    command.pose_index =
        static_cast<unsigned>(yyjson_get_uint(yyjson_obj_get(root, "index")));
    command.kind = name == "hover-exposure-pose"
                       ? WebViewCommandKind::hover_exposure_pose
                   : name == "set-exposure-reference"
                       ? WebViewCommandKind::set_exposure_reference
                       : WebViewCommandKind::toggle_exposure_selection;
    error.clear();
    return true;
  }
  if (name == "set-directory" || name == "browse-directory" ||
      name == "navigate") {
    const bool sets_value = name == "set-directory";
    const bool valid_shape =
        sets_value
            ? exact_object_shape(root, {"version", "kind", "pageGeneration",
                                        "target", "value"})
            : exact_object_shape(
                  root, {"version", "kind", "pageGeneration", "target"});
    if (!valid_shape) {
      error = "unexpected WebView command fields";
      return false;
    }
    yyjson_val *const target = yyjson_obj_get(root, "target");
    yyjson_val *const value = yyjson_obj_get(root, "value");
    if (!yyjson_is_str(target) || (sets_value && !yyjson_is_str(value))) {
      error = "invalid WebView command value";
      return false;
    }
    const std::string_view target_name(yyjson_get_str(target),
                                       yyjson_get_len(target));
    if (name == "navigate") {
      if (target_name == "input")
        command.kind = WebViewCommandKind::navigate_input;
      else if (target_name == "preview")
        command.kind = WebViewCommandKind::navigate_preview;
      else {
        error = "unknown WebView navigation target";
        return false;
      }
    } else if (target_name == "game") {
      command.kind = sets_value ? WebViewCommandKind::set_game_directory
                                : WebViewCommandKind::browse_game_directory;
    } else if (target_name == "screenshots") {
      command.kind = sets_value
                         ? WebViewCommandKind::set_screenshots_directory
                         : WebViewCommandKind::browse_screenshots_directory;
    } else {
      error = "unknown WebView directory target";
      return false;
    }
    if (sets_value) {
      const std::string_view bytes(yyjson_get_str(value),
                                   yyjson_get_len(value));
      command.value = utf8_to_wide(bytes);
      if (!bytes.empty() && command.value.empty()) {
        error = "invalid UTF-8 WebView command value";
        return false;
      }
    }
    error.clear();
    return true;
  }
  if (name == "select-session" || name == "edit-tag" ||
      name == "delete-session") {
    if (!exact_object_shape(root,
                            {"version", "kind", "pageGeneration", "index"})) {
      error = "unexpected WebView session command fields";
      return false;
    }
    yyjson_val *const index = yyjson_obj_get(root, "index");
    if (!yyjson_is_uint(index) ||
        yyjson_get_uint(index) > std::numeric_limits<std::size_t>::max()) {
      error = "invalid WebView session index";
      return false;
    }
    command.session_index = static_cast<std::size_t>(yyjson_get_uint(index));
    command.kind = name == "select-session" ? WebViewCommandKind::select_session
                   : name == "edit-tag"     ? WebViewCommandKind::edit_tag
                                        : WebViewCommandKind::delete_session;
    error.clear();
    return true;
  }
  if (name == "content-size") {
    if (!exact_object_shape(root, {"version", "kind", "pageGeneration",
                                   "layoutGeneration", "height"})) {
      error = "unexpected WebView content size fields";
      return false;
    }
    yyjson_val *const layout = yyjson_obj_get(root, "layoutGeneration");
    yyjson_val *const height = yyjson_obj_get(root, "height");
    if (!yyjson_is_uint(layout) || !yyjson_is_num(height)) {
      error = "invalid WebView content size value";
      return false;
    }
    const double parsed_height = yyjson_get_num(height);
    if (!std::isfinite(parsed_height) || parsed_height <= 0.0 ||
        parsed_height > 100'000.0) {
      error = "invalid WebView content size value";
      return false;
    }
    command.kind = WebViewCommandKind::content_size;
    command.layout_generation = yyjson_get_uint(layout);
    command.content_height = parsed_height;
    error.clear();
    return true;
  }
  if (name == "preview-geometry") {
    if (!exact_object_shape(root, {"version", "kind", "pageGeneration",
                                   "layoutGeneration", "x", "y", "width",
                                   "height", "deviceScale", "visible"})) {
      error = "unexpected WebView preview geometry fields";
      return false;
    }
    yyjson_val *const layout = yyjson_obj_get(root, "layoutGeneration");
    yyjson_val *const x = yyjson_obj_get(root, "x");
    yyjson_val *const y = yyjson_obj_get(root, "y");
    yyjson_val *const width = yyjson_obj_get(root, "width");
    yyjson_val *const height = yyjson_obj_get(root, "height");
    yyjson_val *const scale = yyjson_obj_get(root, "deviceScale");
    yyjson_val *const visible = yyjson_obj_get(root, "visible");
    if (!yyjson_is_uint(layout) || !yyjson_is_num(x) || !yyjson_is_num(y) ||
        !yyjson_is_num(width) || !yyjson_is_num(height) ||
        !yyjson_is_num(scale) || !yyjson_is_bool(visible)) {
      error = "invalid WebView preview geometry value";
      return false;
    }
    WebViewPreviewGeometry geometry;
    geometry.layout_generation = yyjson_get_uint(layout);
    geometry.x = yyjson_get_num(x);
    geometry.y = yyjson_get_num(y);
    geometry.width = yyjson_get_num(width);
    geometry.height = yyjson_get_num(height);
    geometry.device_scale = yyjson_get_num(scale);
    geometry.visible = yyjson_get_bool(visible);
    command.kind = WebViewCommandKind::preview_geometry;
    command.preview = geometry;
    error.clear();
    return true;
  }
  error = "unknown WebView command";
  return false;
}

bool calculate_webview_preview_bounds(const WebViewPreviewGeometry &geometry,
                                      const double rasterization_scale,
                                      const LONG client_width,
                                      const LONG client_height, RECT &bounds,
                                      std::string &error) noexcept {
  bounds = {};
  const std::array values{geometry.x,
                          geometry.y,
                          geometry.width,
                          geometry.height,
                          geometry.device_scale,
                          rasterization_scale};
  if (!geometry.visible ||
      !std::all_of(values.begin(), values.end(),
                   [](const double value) { return std::isfinite(value); }) ||
      rasterization_scale <= 0.0 || client_width <= 0 || client_height <= 0 ||
      geometry.x < 0.0 || geometry.y < 0.0 || geometry.width < 64.0 ||
      geometry.height < 32.0 ||
      std::abs(geometry.device_scale - rasterization_scale) > 0.05 ||
      std::abs(geometry.width / geometry.height - 2.0) > 0.05 ||
      geometry.x + geometry.width >
          static_cast<double>(client_width + 1) / rasterization_scale ||
      geometry.y + geometry.height >
          static_cast<double>(client_height + 1) / rasterization_scale) {
    error = "invalid WebView preview geometry";
    return false;
  }
  const LONG left =
      static_cast<LONG>(std::lround(geometry.x * rasterization_scale));
  const LONG top =
      static_cast<LONG>(std::lround(geometry.y * rasterization_scale));
  const LONG width =
      static_cast<LONG>(std::lround(geometry.width * rasterization_scale));
  const LONG height =
      static_cast<LONG>(std::lround(geometry.height * rasterization_scale));
  if (left < 0 || top < 0 || width <= 0 || height <= 0 ||
      left + width > client_width + 1 || top + height > client_height + 1) {
    error = "WebView preview geometry is outside the client";
    return false;
  }
  bounds = {left, top, left + width, top + height};
  error.clear();
  return true;
}

std::wstring embedded_webview_html(const WebViewPage page) {
  const HMODULE module = GetModuleHandleW(nullptr);
  const HRSRC resource = FindResourceW(
      module,
      MAKEINTRESOURCEW(page == WebViewPage::main ? PANO_APP_UI_HTML
                                                 : PANO_APP_EXPOSURE_HTML),
      RT_RCDATA);
  if (resource == nullptr)
    return {};
  const HGLOBAL loaded = LoadResource(module, resource);
  const DWORD size = SizeofResource(module, resource);
  const void *const bytes = loaded == nullptr ? nullptr : LockResource(loaded);
  if (bytes == nullptr || size == 0U)
    return {};
  return utf8_to_wide(std::string_view(static_cast<const char *>(bytes), size));
}

struct WebViewHostState {
  HWND owner = nullptr;
  WebViewHost::CommandHandler handler;
  ComPtr<ICoreWebView2Environment> environment;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> webview;
  EventRegistrationToken message_token{};
  EventRegistrationToken navigation_token{};
  EventRegistrationToken navigation_completed_token{};
  RECT pending_bounds{};
  std::uint64_t page_generation = 0;
  bool ready = false;
  bool closed = false;
  WebViewPage page = WebViewPage::main;
};

namespace {

void report_host_failure(const std::shared_ptr<WebViewHostState> &state,
                         std::wstring error) {
  if (state->closed || !state->handler)
    return;
  WebViewCommand command;
  command.kind = WebViewCommandKind::host_failed;
  command.page_generation = state->page_generation;
  command.value = std::move(error);
  state->handler(command);
}

HRESULT create_controller(const std::shared_ptr<WebViewHostState> &state,
                          const HRESULT result,
                          ICoreWebView2Controller *const controller) {
  if (state->closed)
    return S_OK;
  if (FAILED(result) || controller == nullptr) {
    report_host_failure(state,
                        hresult_message(L"Cannot create WebView2 controller",
                                        FAILED(result) ? result : E_POINTER));
    return S_OK;
  }
  state->controller = controller;
  HRESULT status = state->controller->put_ZoomFactor(1.0);
  if (SUCCEEDED(status))
    status = state->controller->put_Bounds(state->pending_bounds);
  if (SUCCEEDED(status))
    status = state->controller->put_IsVisible(TRUE);
  if (SUCCEEDED(status))
    status = state->controller->get_CoreWebView2(&state->webview);
  if (FAILED(status) || state->webview == nullptr) {
    report_host_failure(
        state, hresult_message(L"Cannot initialize WebView2 controller",
                               FAILED(status) ? status : E_POINTER));
    return S_OK;
  }
  ComPtr<ICoreWebView2Controller2> controller2;
  if (SUCCEEDED(state->controller.As(&controller2))) {
    constexpr COREWEBVIEW2_COLOR background{255U, 3U, 7U, 18U};
    controller2->put_DefaultBackgroundColor(background);
  }
  ComPtr<ICoreWebView2Controller3> controller3;
  if (SUCCEEDED(state->controller.As(&controller3)))
    controller3->put_ShouldDetectMonitorScaleChanges(TRUE);
  ComPtr<ICoreWebView2Settings> settings;
  if (SUCCEEDED(state->webview->get_Settings(&settings))) {
    settings->put_AreDefaultContextMenusEnabled(FALSE);
    settings->put_AreDevToolsEnabled(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
  }
  status = state->webview->add_NavigationStarting(
      Callback<ICoreWebView2NavigationStartingEventHandler>(
          [state](ICoreWebView2 *,
                  ICoreWebView2NavigationStartingEventArgs *const event) {
            LPWSTR uri = nullptr;
            const HRESULT uri_status = event->get_Uri(&uri);
            const std::wstring_view value = uri == nullptr ? L"" : uri;
            const bool allowed = SUCCEEDED(uri_status) &&
                                 (!state->ready || value == L"about:blank");
            CoTaskMemFree(uri);
            if (!allowed)
              event->put_Cancel(TRUE);
            return S_OK;
          })
          .Get(),
      &state->navigation_token);
  if (FAILED(status)) {
    report_host_failure(
        state, hresult_message(L"Cannot secure WebView2 navigation", status));
    return S_OK;
  }
  status = state->webview->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
          [state](ICoreWebView2 *,
                  ICoreWebView2NavigationCompletedEventArgs *const event) {
            if (state->closed)
              return S_OK;
            BOOL succeeded = FALSE;
            if (FAILED(event->get_IsSuccess(&succeeded)) || succeeded == FALSE)
              report_host_failure(state,
                                  L"Embedded WebView2 page navigation failed");
            return S_OK;
          })
          .Get(),
      &state->navigation_completed_token);
  if (FAILED(status)) {
    report_host_failure(
        state, hresult_message(L"Cannot monitor WebView2 navigation", status));
    return S_OK;
  }
  status = state->webview->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [state](ICoreWebView2 *,
                  ICoreWebView2WebMessageReceivedEventArgs *const event) {
            if (state->closed)
              return S_OK;
            LPWSTR json = nullptr;
            const HRESULT message_status = event->get_WebMessageAsJson(&json);
            if (FAILED(message_status) || json == nullptr) {
              CoTaskMemFree(json);
              report_host_failure(state, L"Cannot read WebView2 command");
              return S_OK;
            }
            const std::string utf8 = wide_to_utf8(json);
            CoTaskMemFree(json);
            WebViewCommand command;
            std::string error;
            if (!parse_webview_command_json(utf8, command, error)) {
              report_host_failure(state, utf8_to_wide(error));
              return S_OK;
            }
            if (command.kind == WebViewCommandKind::ready) {
              state->ready = true;
              command.page_generation = state->page_generation;
            } else if (!state->ready ||
                       command.page_generation != state->page_generation) {
              return S_OK;
            }
            if (state->handler)
              state->handler(command);
            return S_OK;
          })
          .Get(),
      &state->message_token);
  if (FAILED(status)) {
    report_host_failure(
        state, hresult_message(L"Cannot register WebView2 bridge", status));
    return S_OK;
  }
  const std::wstring html = embedded_webview_html(state->page);
  if (html.empty()) {
    report_host_failure(state, L"Embedded WebView2 page is unavailable");
    return S_OK;
  }
  ++state->page_generation;
  status = state->webview->NavigateToString(html.c_str());
  if (FAILED(status))
    report_host_failure(
        state,
        hresult_message(L"Cannot navigate embedded WebView2 page", status));
  return S_OK;
}

} // namespace

WebViewHost::WebViewHost(const HWND owner, CommandHandler handler,
                         const WebViewPage page)
    : state_(std::make_shared<WebViewHostState>()) {
  state_->owner = owner;
  state_->handler = std::move(handler);
  state_->page = page;
  GetClientRect(owner, &state_->pending_bounds);
}

WebViewHost::~WebViewHost() { close(); }

bool WebViewHost::start(std::wstring &error) {
  if (state_->closed || state_->owner == nullptr) {
    error = L"WebView2 host is closed";
    return false;
  }
  const std::wstring profile = webview_user_data_folder().wstring();
  const std::shared_ptr<WebViewHostState> state = state_;
  const HRESULT status = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, profile.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [state](const HRESULT result,
                  ICoreWebView2Environment *const environment) {
            if (state->closed)
              return S_OK;
            if (FAILED(result) || environment == nullptr) {
              report_host_failure(
                  state, hresult_message(L"Cannot create WebView2 environment",
                                         FAILED(result) ? result : E_POINTER));
              return S_OK;
            }
            state->environment = environment;
            return environment->CreateCoreWebView2Controller(
                state->owner,
                Callback<
                    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [state](const HRESULT controller_result,
                            ICoreWebView2Controller *const controller) {
                      return create_controller(state, controller_result,
                                               controller);
                    })
                    .Get());
          })
          .Get());
  if (FAILED(status)) {
    error = hresult_message(L"Cannot start WebView2 environment", status);
    return false;
  }
  error.clear();
  return true;
}

void WebViewHost::resize(const RECT &bounds) noexcept {
  state_->pending_bounds = bounds;
  if (state_->controller != nullptr)
    state_->controller->put_Bounds(bounds);
}

bool WebViewHost::post_snapshot(const std::wstring_view json,
                                std::wstring &error) const {
  if (!state_->ready || state_->webview == nullptr) {
    error = L"WebView2 page is not ready";
    return false;
  }
  const std::wstring payload(json);
  const HRESULT status = state_->webview->PostWebMessageAsJson(payload.c_str());
  if (FAILED(status)) {
    error = hresult_message(L"Cannot send WebView2 snapshot", status);
    return false;
  }
  error.clear();
  return true;
}

bool WebViewHost::ready() const noexcept { return state_->ready; }

std::uint64_t WebViewHost::page_generation() const noexcept {
  return state_->page_generation;
}

double WebViewHost::rasterization_scale() const noexcept {
  ComPtr<ICoreWebView2Controller3> controller3;
  double scale = 0.0;
  if (state_->controller != nullptr &&
      SUCCEEDED(state_->controller.As(&controller3)))
    controller3->get_RasterizationScale(&scale);
  return scale;
}

void WebViewHost::close() noexcept {
  if (state_->closed)
    return;
  state_->closed = true;
  state_->ready = false;
  state_->handler = {};
  if (state_->webview != nullptr) {
    state_->webview->remove_WebMessageReceived(state_->message_token);
    state_->webview->remove_NavigationStarting(state_->navigation_token);
    state_->webview->remove_NavigationCompleted(
        state_->navigation_completed_token);
  }
  if (state_->controller != nullptr)
    state_->controller->Close();
  state_->webview.Reset();
  state_->controller.Reset();
  state_->environment.Reset();
  state_->owner = nullptr;
}

} // namespace pano::app

#endif
