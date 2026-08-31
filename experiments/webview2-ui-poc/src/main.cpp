#include <windows.h>

#include <WebView2.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr wchar_t window_class_name[] = L"PanoramaWebView2UiPocWindow";
constexpr wchar_t base_title[] = L"Cyberpunk Panorama Stitcher — WebView2 POC";
constexpr UINT_PTR auto_close_timer = 1U;
constexpr unsigned benchmark_width = 2048U;
constexpr unsigned benchmark_height = 1024U;
constexpr LONG desired_client_width_dip = 1100;
constexpr LONG desired_client_height_dip = 780;
constexpr LONG minimum_client_width_dip = 920;
constexpr LONG minimum_client_height_dip = 680;

SIZE outer_size_for_client(const LONG width_dip, const LONG height_dip,
                           const UINT dpi) {
  RECT bounds{
      0, 0, MulDiv(width_dip, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI),
      MulDiv(height_dip, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI)};
  if (!AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0U, dpi))
    return {};
  return {bounds.right - bounds.left, bounds.bottom - bounds.top};
}

std::wstring argument_value(const std::vector<std::wstring> &arguments,
                            const std::wstring &name) {
  for (std::size_t index = 0; index + 1U < arguments.size(); ++index)
    if (arguments[index] == name)
      return arguments[index + 1U];
  return {};
}

bool has_argument(const std::vector<std::wstring> &arguments,
                  const std::wstring &name) {
  return std::find(arguments.begin(), arguments.end(), name) != arguments.end();
}

std::wstring module_directory() {
  std::vector<wchar_t> buffer(32768U);
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
  if (length == 0U || length >= buffer.size())
    return {};
  return std::filesystem::path(std::wstring(buffer.data(), length))
      .parent_path()
      .wstring();
}

std::wstring default_user_data_directory() {
  PWSTR local_app_data = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT,
                                  nullptr, &local_app_data)))
    return {};
  const std::filesystem::path result = std::filesystem::path(local_app_data) /
                                       L"PanoramaStitcher" / L"WebView2Poc";
  CoTaskMemFree(local_app_data);
  return result.wstring();
}

std::wstring file_url(const std::filesystem::path &path) {
  std::wstring result(32768U, L'\0');
  DWORD characters = static_cast<DWORD>(result.size());
  if (FAILED(UrlCreateFromPathW(path.c_str(), result.data(), &characters, 0U)))
    return {};
  if (characters > 0U && result[characters - 1U] == L'\0')
    --characters;
  result.resize(characters);
  return result;
}

double
elapsed_milliseconds(const std::chrono::steady_clock::time_point started) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - started)
      .count();
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

class Application {
public:
  Application(std::vector<std::wstring> arguments,
              const std::chrono::steady_clock::time_point started)
      : arguments_(std::move(arguments)), started_(started),
        benchmark_requested_(has_argument(arguments_, L"--benchmark")),
        native_probe_(has_argument(arguments_, L"--native-probe")),
        report_path_(argument_value(arguments_, L"--report")),
        user_data_directory_(argument_value(arguments_, L"--user-data-dir")) {
    if (user_data_directory_.empty())
      user_data_directory_ = default_user_data_directory();
  }

  bool create(const HINSTANCE instance) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground =
        reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = window_class_name;
    if (RegisterClassExW(&window_class) == 0U)
      return false;

    const UINT dpi = GetDpiForSystem();
    const SIZE window_size = outer_size_for_client(
        desired_client_width_dip, desired_client_height_dip, dpi);
    MONITORINFO monitor_info{sizeof(monitor_info)};
    const HMONITOR monitor =
        MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    const bool have_monitor = GetMonitorInfoW(monitor, &monitor_info) != FALSE;
    const int x = have_monitor
                      ? monitor_info.rcWork.left +
                            (monitor_info.rcWork.right -
                             monitor_info.rcWork.left - window_size.cx) /
                                2
                      : CW_USEDEFAULT;
    const int y = have_monitor
                      ? monitor_info.rcWork.top +
                            (monitor_info.rcWork.bottom -
                             monitor_info.rcWork.top - window_size.cy) /
                                2
                      : CW_USEDEFAULT;
    window_ = CreateWindowExW(0U, window_class_name, base_title,
                              WS_OVERLAPPEDWINDOW, x, y, window_size.cx,
                              window_size.cy, nullptr, nullptr, instance, this);
    if (window_ == nullptr)
      return false;
    if (native_probe_) {
      native_probe_window_ =
          CreateWindowExW(0U, L"STATIC", L"Native HWND\nreservation",
                          WS_CHILD | WS_VISIBLE | SS_CENTER | WS_BORDER, 0, 0,
                          0, 0, window_, nullptr, instance, nullptr);
    }
    ShowWindow(window_, SW_SHOWDEFAULT);
    UpdateWindow(window_);
    return initialize_webview();
  }

  int run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0U, 0U) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
  }

private:
  static LRESULT CALLBACK window_procedure(const HWND window,
                                           const UINT message,
                                           const WPARAM wparam,
                                           const LPARAM lparam) {
    Application *application = reinterpret_cast<Application *>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      const auto *const create = reinterpret_cast<CREATESTRUCTW *>(lparam);
      application = static_cast<Application *>(create->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(application));
    }
    if (application == nullptr)
      return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_DPICHANGED: {
      const auto *const suggested = reinterpret_cast<const RECT *>(lparam);
      SetWindowPos(window, nullptr, suggested->left, suggested->top,
                   suggested->right - suggested->left,
                   suggested->bottom - suggested->top,
                   SWP_NOACTIVATE | SWP_NOZORDER);
      return 0;
    }
    case WM_GETMINMAXINFO: {
      const UINT dpi = GetDpiForWindow(window);
      const SIZE minimum = outer_size_for_client(
          minimum_client_width_dip, minimum_client_height_dip, dpi);
      auto *const limits = reinterpret_cast<MINMAXINFO *>(lparam);
      limits->ptMinTrackSize = POINT{minimum.cx, minimum.cy};
      return 0;
    }
    case WM_SIZE:
      application->resize();
      return 0;
    case WM_CTLCOLORSTATIC: {
      HDC context = reinterpret_cast<HDC>(wparam);
      SetTextColor(context, RGB(209, 213, 219));
      SetBkColor(context, RGB(17, 24, 39));
      static HBRUSH brush = CreateSolidBrush(RGB(17, 24, 39));
      return reinterpret_cast<LRESULT>(brush);
    }
    case WM_TIMER:
      if (wparam == auto_close_timer)
        DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
    }
  }

  bool initialize_webview() {
    if (user_data_directory_.empty()) {
      fail(L"Cannot resolve local WebView2 profile directory", E_FAIL);
      return false;
    }
    const HRESULT started = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data_directory_.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](const HRESULT result,
                   ICoreWebView2Environment *const environment) -> HRESULT {
              if (FAILED(result) || environment == nullptr)
                return fail(L"Cannot create WebView2 environment", result);
              environment_ = environment;
              return environment_->CreateCoreWebView2Controller(
                  window_,
                  Callback<
                      ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                      [this](const HRESULT controller_result,
                             ICoreWebView2Controller *const controller)
                          -> HRESULT {
                        return create_controller(controller_result, controller);
                      })
                      .Get());
            })
            .Get());
    if (FAILED(started)) {
      fail(L"Cannot start WebView2 environment creation", started);
      return false;
    }
    return true;
  }

  HRESULT create_controller(const HRESULT result,
                            ICoreWebView2Controller *const controller) {
    if (FAILED(result) || controller == nullptr)
      return fail(L"Cannot create WebView2 controller", result);
    controller_ = controller;
    HRESULT status = controller_->put_ZoomFactor(1.0);
    if (FAILED(status))
      return fail(L"Cannot set WebView2 zoom", status);
    ComPtr<ICoreWebView2Controller3> controller3;
    if (SUCCEEDED(controller_.As(&controller3)))
      controller3->put_ShouldDetectMonitorScaleChanges(TRUE);
    status = controller_->get_CoreWebView2(&webview_);
    if (FAILED(status) || webview_ == nullptr)
      return fail(L"Cannot query WebView2", status);

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webview_->get_Settings(&settings))) {
      settings->put_AreDefaultContextMenusEnabled(FALSE);
      settings->put_IsStatusBarEnabled(FALSE);
      settings->put_AreDevToolsEnabled(FALSE);
    }
    status = webview_->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2 *,
                   ICoreWebView2WebMessageReceivedEventArgs *const event)
                -> HRESULT { return receive_message(event); })
            .Get(),
        &message_token_);
    if (FAILED(status))
      return fail(L"Cannot register WebView2 messages", status);

    resize();
    const auto page =
        std::filesystem::path(module_directory()) / L"ui" / L"index.html";
    const std::wstring url = file_url(page);
    if (url.empty())
      return fail(L"Cannot create POC page URL", E_FAIL);
    return webview_->Navigate(url.c_str());
  }

  HRESULT
  receive_message(ICoreWebView2WebMessageReceivedEventArgs *const event) {
    LPWSTR raw_message = nullptr;
    const HRESULT status = event->TryGetWebMessageAsString(&raw_message);
    if (FAILED(status) || raw_message == nullptr)
      return status;
    const std::wstring message(raw_message);
    CoTaskMemFree(raw_message);
    if (message.find(L"\"kind\":\"select-directory\"") != std::wstring::npos) {
      if (message.find(L"\"target\":\"game\"") != std::wstring::npos)
        return select_directory(L"game", L"Select game directory");
      if (message.find(L"\"target\":\"screenshots\"") != std::wstring::npos)
        return select_directory(L"screenshots",
                                L"Select screenshots directory");
      return S_OK;
    }
    if (message.find(L"\"kind\":\"dom-ready\"") != std::wstring::npos) {
      ready_milliseconds_ = elapsed_milliseconds(started_);
      std::wostringstream title;
      title << base_title << L" — Ready ("
            << static_cast<unsigned>(ready_milliseconds_ + 0.5) << L" ms)";
      SetWindowTextW(window_, title.str().c_str());
      if (benchmark_requested_)
        return send_preview_benchmark();
      write_report(L"null");
      return S_OK;
    }
    if (message.find(L"\"kind\":\"preview-transfer-complete\"") !=
        std::wstring::npos) {
      benchmark_total_milliseconds_ = elapsed_milliseconds(benchmark_started_);
      write_report(message);
      SetWindowTextW(
          window_,
          L"Cyberpunk Panorama Stitcher — WebView2 POC — Benchmark ready");
      shared_buffer_.Reset();
    }
    return S_OK;
  }

  HRESULT select_directory(const std::wstring_view target,
                           const wchar_t *const title) {
    ComPtr<IFileOpenDialog> dialog;
    HRESULT status =
        CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&dialog));
    if (FAILED(status))
      return interaction_error(L"Cannot create folder picker", status);
    FILEOPENDIALOGOPTIONS options{};
    status = dialog->GetOptions(&options);
    if (SUCCEEDED(status))
      status =
          dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                             FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    if (SUCCEEDED(status))
      status = dialog->SetTitle(title);
    if (FAILED(status))
      return interaction_error(L"Cannot configure folder picker", status);
    status = dialog->Show(window_);
    if (status == HRESULT_FROM_WIN32(ERROR_CANCELLED))
      return S_OK;
    if (FAILED(status))
      return interaction_error(L"Cannot show folder picker", status);
    ComPtr<IShellItem> selection;
    status = dialog->GetResult(&selection);
    if (FAILED(status))
      return interaction_error(L"Cannot read folder picker result", status);
    PWSTR selected_path = nullptr;
    status = selection->GetDisplayName(SIGDN_FILESYSPATH, &selected_path);
    if (FAILED(status) || selected_path == nullptr)
      return interaction_error(L"Selected item is not a file-system folder",
                               status);
    const std::wstring payload =
        L"{\"kind\":\"directory-selected\",\"target\":" + json_string(target) +
        L",\"path\":" + json_string(selected_path) + L"}";
    CoTaskMemFree(selected_path);
    status = webview_->PostWebMessageAsJson(payload.c_str());
    if (FAILED(status))
      return interaction_error(L"Cannot return selected directory", status);
    return S_OK;
  }

  HRESULT send_preview_benchmark() {
    ComPtr<ICoreWebView2Environment12> environment12;
    ComPtr<ICoreWebView2_17> webview17;
    HRESULT status = environment_.As(&environment12);
    if (FAILED(status))
      return fail(L"WebView2 runtime lacks shared-buffer environment API",
                  status);
    status = webview_.As(&webview17);
    if (FAILED(status))
      return fail(L"WebView2 runtime lacks shared-buffer script API", status);

    constexpr std::uint64_t bytes =
        static_cast<std::uint64_t>(benchmark_width) * benchmark_height * 4U;
    static_assert(bytes <= std::numeric_limits<std::uint32_t>::max());
    status = environment12->CreateSharedBuffer(bytes, &shared_buffer_);
    if (FAILED(status) || shared_buffer_ == nullptr)
      return fail(L"Cannot allocate WebView2 preview shared buffer", status);
    BYTE *pixels = nullptr;
    status = shared_buffer_->get_Buffer(&pixels);
    if (FAILED(status) || pixels == nullptr)
      return fail(L"Cannot map WebView2 preview shared buffer", status);

    benchmark_started_ = std::chrono::steady_clock::now();
    for (unsigned y = 0U; y < benchmark_height; ++y) {
      for (unsigned x = 0U; x < benchmark_width; ++x) {
        const std::size_t offset =
            (static_cast<std::size_t>(y) * benchmark_width + x) * 4U;
        pixels[offset] = static_cast<BYTE>(x * 255U / benchmark_width);
        pixels[offset + 1U] = static_cast<BYTE>(y * 255U / benchmark_height);
        pixels[offset + 2U] =
            static_cast<BYTE>((static_cast<std::uint64_t>(x + y) * 255U) /
                              (benchmark_width + benchmark_height));
        pixels[offset + 3U] = 255U;
      }
    }
    native_fill_milliseconds_ = elapsed_milliseconds(benchmark_started_);
    const auto post_started = std::chrono::steady_clock::now();
    status = webview17->PostSharedBufferToScript(
        shared_buffer_.Get(), COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY,
        L"{\"width\":2048,\"height\":1024}");
    native_post_milliseconds_ = elapsed_milliseconds(post_started);
    return status;
  }

  void resize() {
    if (window_ == nullptr)
      return;
    RECT bounds{};
    GetClientRect(window_, &bounds);
    constexpr LONG native_width = 160;
    if (native_probe_ && native_probe_window_ != nullptr) {
      const LONG width = std::max(1L, bounds.right - native_width);
      MoveWindow(native_probe_window_, width, 0, native_width, bounds.bottom,
                 TRUE);
      bounds.right = width;
    }
    if (controller_ != nullptr)
      controller_->put_Bounds(bounds);
  }

  HRESULT fail(const wchar_t *const message, const HRESULT status) {
    std::wostringstream detail;
    detail << message << L" (0x" << std::hex
           << static_cast<unsigned long>(status) << L")";
    SetWindowTextW(window_, L"WebView2 UI POC — Failed");
    MessageBoxW(window_, detail.str().c_str(), L"WebView2 UI POC",
                MB_ICONERROR | MB_OK);
    return FAILED(status) ? status : E_FAIL;
  }

  HRESULT interaction_error(const wchar_t *const message,
                            const HRESULT status) const {
    std::wostringstream detail;
    detail << message << L" (0x" << std::hex
           << static_cast<unsigned long>(status) << L")";
    MessageBoxW(window_, detail.str().c_str(), L"WebView2 UI POC",
                MB_ICONERROR | MB_OK);
    return S_OK;
  }

  void write_report(const std::wstring &script_result) {
    if (report_path_.empty())
      return;
    RECT client{};
    GetClientRect(window_, &client);
    const UINT dpi = GetDpiForWindow(window_);
    double rasterization_scale = 0.0;
    ComPtr<ICoreWebView2Controller3> controller3;
    if (SUCCEEDED(controller_.As(&controller3)))
      controller3->get_RasterizationScale(&rasterization_scale);
    std::ofstream output(std::filesystem::path(report_path_),
                         std::ios::binary | std::ios::trunc);
    output
        << "{\n"
        << "  \"dom_ready_ms\": " << ready_milliseconds_ << ",\n"
        << "  \"window_dpi\": " << dpi << ",\n"
        << "  \"client_width_pixels\": " << client.right << ",\n"
        << "  \"client_height_pixels\": " << client.bottom << ",\n"
        << "  \"client_width_dip\": "
        << MulDiv(client.right, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi))
        << ",\n"
        << "  \"client_height_dip\": "
        << MulDiv(client.bottom, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi))
        << ",\n"
        << "  \"webview_zoom\": 1.0,\n"
        << "  \"webview_rasterization_scale\": " << rasterization_scale << ",\n"
        << "  \"preview_width\": "
        << (benchmark_requested_ ? benchmark_width : 0U) << ",\n"
        << "  \"preview_height\": "
        << (benchmark_requested_ ? benchmark_height : 0U) << ",\n"
        << "  \"preview_bytes\": "
        << (benchmark_requested_ ? static_cast<std::uint64_t>(benchmark_width) *
                                       benchmark_height * 4U
                                 : 0U)
        << ",\n"
        << "  \"native_fill_ms\": " << native_fill_milliseconds_ << ",\n"
        << "  \"native_post_call_ms\": " << native_post_milliseconds_ << ",\n"
        << "  \"native_to_present_ms\": " << benchmark_total_milliseconds_
        << ",\n"
        << "  \"script_result\": ";
    if (script_result == L"null") {
      output << "null\n";
    } else {
      std::string utf8;
      const int bytes = WideCharToMultiByte(
          CP_UTF8, WC_ERR_INVALID_CHARS, script_result.data(),
          static_cast<int>(script_result.size()), nullptr, 0, nullptr, nullptr);
      if (bytes > 0) {
        utf8.resize(static_cast<std::size_t>(bytes));
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, script_result.data(),
                            static_cast<int>(script_result.size()), utf8.data(),
                            bytes, nullptr, nullptr);
      }
      output << utf8 << '\n';
    }
    output << "}\n";
  }

  std::vector<std::wstring> arguments_;
  std::chrono::steady_clock::time_point started_;
  std::chrono::steady_clock::time_point benchmark_started_{};
  bool benchmark_requested_ = false;
  bool native_probe_ = false;
  std::wstring report_path_;
  std::wstring user_data_directory_;
  HWND window_ = nullptr;
  HWND native_probe_window_ = nullptr;
  ComPtr<ICoreWebView2Environment> environment_;
  ComPtr<ICoreWebView2Controller> controller_;
  ComPtr<ICoreWebView2> webview_;
  ComPtr<ICoreWebView2SharedBuffer> shared_buffer_;
  EventRegistrationToken message_token_{};
  double ready_milliseconds_ = 0.0;
  double native_fill_milliseconds_ = 0.0;
  double native_post_milliseconds_ = 0.0;
  double benchmark_total_milliseconds_ = 0.0;
};
} // namespace

int WINAPI wWinMain(const HINSTANCE instance, HINSTANCE, PWSTR, int) {
  const auto started = std::chrono::steady_clock::now();
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    return 2;
  int argument_count = 0;
  LPWSTR *raw_arguments =
      CommandLineToArgvW(GetCommandLineW(), &argument_count);
  std::vector<std::wstring> arguments;
  if (raw_arguments != nullptr) {
    arguments.assign(raw_arguments, raw_arguments + argument_count);
    LocalFree(raw_arguments);
  }
  Application application(std::move(arguments), started);
  const int result = application.create(instance) ? application.run() : 3;
  CoUninitialize();
  return result;
}
