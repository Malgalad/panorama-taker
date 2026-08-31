#include "pano_app_webview.h"

#ifdef _WIN32

#include <WebView2.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <array>
#include <system_error>

namespace pano::app {
namespace {

constexpr wchar_t runtime_download_url[] =
    L"https://developer.microsoft.com/microsoft-edge/webview2/";
constexpr int download_button = 101;
constexpr int retry_button = 102;

using TaskDialogFunction = HRESULT(WINAPI *)(const TASKDIALOGCONFIG *, int *,
                                             int *, BOOL *);

int show_missing_runtime_dialog(const HWND owner) {
  constexpr std::array<TASKDIALOG_BUTTON, 2> buttons{{
      {download_button, L"Download WebView2 Runtime"},
      {retry_button, L"Retry"},
  }};
  TASKDIALOGCONFIG dialog{};
  dialog.cbSize = sizeof(dialog);
  dialog.hwndParent = owner;
  dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
  dialog.dwCommonButtons = TDCBF_CLOSE_BUTTON;
  dialog.pszWindowTitle = L"Panorama Stitcher";
  dialog.pszMainIcon = TD_ERROR_ICON;
  dialog.pszMainInstruction = L"Microsoft Edge WebView2 Runtime is required";
  dialog.pszContent =
      L"Install the WebView2 Runtime, then restart Panorama Stitcher. You can "
      L"retry detection without closing this window.";
  dialog.cButtons = static_cast<UINT>(buttons.size());
  dialog.pButtons = buttons.data();
  dialog.nDefaultButton = download_button;

  HMODULE controls_module = LoadLibraryW(L"comctl32.dll");
  const auto task_dialog =
      controls_module == nullptr
          ? nullptr
          : reinterpret_cast<TaskDialogFunction>(
                GetProcAddress(controls_module, "TaskDialogIndirect"));
  int choice = IDCLOSE;
  const HRESULT result = task_dialog == nullptr
                             ? E_NOTIMPL
                             : task_dialog(&dialog, &choice, nullptr, nullptr);
  if (controls_module != nullptr)
    FreeLibrary(controls_module);
  if (SUCCEEDED(result))
    return choice;

  const int fallback = MessageBoxW(
      owner,
      L"Microsoft Edge WebView2 Runtime is required.\n\nChoose Yes to open "
      L"the download page, No to check again, or Cancel to exit.",
      L"Panorama Stitcher", MB_ICONERROR | MB_YESNOCANCEL | MB_DEFBUTTON1);
  return fallback == IDYES  ? download_button
         : fallback == IDNO ? retry_button
                            : IDCLOSE;
}

} // namespace

WebViewRuntimeInfo query_webview_runtime() noexcept {
  WebViewRuntimeInfo result;
  LPWSTR version = nullptr;
  result.status =
      GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
  if (SUCCEEDED(result.status) && version != nullptr && version[0] != L'\0') {
    result.available = true;
    result.version = version;
  } else if (SUCCEEDED(result.status)) {
    result.status = E_UNEXPECTED;
  }
  CoTaskMemFree(version);
  return result;
}

std::filesystem::path webview_user_data_folder() {
  PWSTR local_app_data = nullptr;
  const HRESULT result = SHGetKnownFolderPath(
      FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local_app_data);
  if (FAILED(result) || local_app_data == nullptr) {
    CoTaskMemFree(local_app_data);
    throw std::system_error(static_cast<int>(result), std::system_category(),
                            "cannot locate LocalAppData");
  }
  std::filesystem::path path(local_app_data);
  CoTaskMemFree(local_app_data);
  return path / L"PanoramaStitcher" / L"WebView2";
}

const wchar_t *webview_runtime_download_url() noexcept {
  return runtime_download_url;
}

bool ensure_webview_runtime(const HWND owner) {
  for (;;) {
    if (query_webview_runtime().available)
      return true;
    const int choice = show_missing_runtime_dialog(owner);
    if (choice == retry_button)
      continue;
    if (choice == download_button) {
      ShellExecuteW(owner, L"open", runtime_download_url, nullptr, nullptr,
                    SW_SHOWNORMAL);
    }
    return false;
  }
}

} // namespace pano::app

#endif
