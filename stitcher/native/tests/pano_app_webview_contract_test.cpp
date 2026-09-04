#include "pano_app_resource.h"
#include "pano_app_webview.h"

#include <windows.h>

#include <array>
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
  using Domain = pano::app::WebViewCommandDomain;
  constexpr std::array<Domain, pano::app::webview_command_kind_count>
      command_domains{
          Domain::host,        Domain::directories, Domain::directories,
          Domain::directories, Domain::directories, Domain::directories,
          Domain::directories, Domain::sessions,    Domain::sessions,
          Domain::sessions,    Domain::sessions,    Domain::sessions,
          Domain::navigation,
          Domain::navigation,  Domain::navigation,  Domain::output,
          Domain::output,      Domain::output,      Domain::output,
          Domain::output,      Domain::output,      Domain::modal,
          Domain::modal,       Domain::exposure,    Domain::exposure,
          Domain::exposure,    Domain::exposure,    Domain::exposure,
          Domain::exposure,    Domain::exposure,    Domain::exposure,
          Domain::exposure,
          Domain::operation,   Domain::operation,   Domain::operation,
          Domain::operation,   Domain::operation,   Domain::modal,
          Domain::modal,       Domain::modal,       Domain::modal,
          Domain::layout,      Domain::layout,      Domain::host,
      };
  for (std::size_t index = 0; index < command_domains.size(); ++index)
    CHECK(pano::app::webview_command_domain(
              static_cast<pano::app::WebViewCommandKind>(index)) ==
          command_domains[index]);

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
  CHECK(html.find(L".shrink-0 { flex-shrink: 0; }") != std::wstring::npos);
  const std::size_t input_range_style = html.find(L"  .input-range {");
  const std::size_t input_radio_style = html.find(L"  .input-radio {");
  const std::size_t input_checkbox_style = html.find(L"  .input-checkbox {");
  const std::size_t pose_style = html.find(L"  .pose {");
  CHECK(input_range_style < input_radio_style);
  CHECK(input_radio_style < input_checkbox_style);
  CHECK(input_checkbox_style < pose_style);
  const std::wstring_view input_range_css(
      html.data() + input_range_style, input_radio_style - input_range_style);
  const std::wstring_view input_radio_css(
      html.data() + input_radio_style,
      input_checkbox_style - input_radio_style);
  const std::wstring_view input_checkbox_css(
      html.data() + input_checkbox_style, pose_style - input_checkbox_style);
  CHECK(html.find(L"--color-amber-700: oklch(55.5% 0.163 48.998)") !=
        std::wstring::npos);
  CHECK(html.find(L"--tw-shadow-alpha: 100%") != std::wstring::npos);
  CHECK(html.find(L"--tw-inset-shadow-alpha: 100%") !=
        std::wstring::npos);
  CHECK(input_range_css.find(L"transition-property: all") !=
        std::wstring_view::npos);
  CHECK(input_range_css.find(L"--tw-shadow: 0 4px 6px -1px") !=
        std::wstring_view::npos);
  CHECK(input_range_css.find(L"--tw-scale-x: 110%") !=
        std::wstring_view::npos);
  CHECK(input_radio_css.find(L"border-color: var(--color-amber-300)") !=
        std::wstring_view::npos);
  CHECK(input_radio_css.find(L"background-color: var(--color-gray-800)") !=
        std::wstring_view::npos);
  CHECK(input_radio_css.find(L"--tw-ring-color: var(--color-amber-700)") !=
        std::wstring_view::npos);
  CHECK(input_checkbox_css.find(L"height: calc(var(--spacing) * 5)") !=
        std::wstring_view::npos);
  CHECK(input_checkbox_css.find(L"border-right-width: 3px") !=
        std::wstring_view::npos);
  CHECK(html.find(L"id=\"ui-root\"") != std::wstring::npos);
  CHECK(html.find(L"function h(type, properties") != std::wstring::npos);
  CHECK(html.find(L"function hu(target, properties") != std::wstring::npos);
  CHECK(html.find(L"function App({ snapshot, bridgeFailed })") !=
        std::wstring::npos);
  CHECK(html.find(L"snapshot?.maximized && 'maximized'") != std::wstring::npos);
  CHECK(html.find(L"main.maximized { height: 100vh") != std::wstring::npos);
  CHECK(html.find(L"main.maximized > nav, main.maximized > footer") !=
        std::wstring::npos);
  CHECK(html.find(L"function InputView({ snapshot })") != std::wstring::npos);
  CHECK(html.find(L"function PreviewView({ snapshot })") != std::wstring::npos);
  CHECK(html.find(L"function OutputView({ snapshot })") != std::wstring::npos);
  CHECK(html.find(L"function ModalHost({ modal })") != std::wstring::npos);
  CHECK(html.find(L"modal?.kind === 'overwrite-output' || modal?.kind === 'notice'") !=
        std::wstring::npos);
  CHECK(html.find(L"function EditTagModal({ modal })") != std::wstring::npos);
  CHECK(html.find(L"function InputOptionsModal({ modal })") !=
        std::wstring::npos);
  CHECK(html.find(L"function PreviewOptionsModal({ modal })") !=
        std::wstring::npos);
  CHECK(html.find(L"function AppSettingsModal({ modal })") !=
        std::wstring::npos);
  CHECK(html.find(L"function DestructiveConfirmationModal({ modal })") !=
        std::wstring::npos);
  CHECK(html.find(L"id: 'modal-layer'") != std::wstring::npos);
  CHECK(html.find(L"id: 'modal-dialog'") != std::wstring::npos);
  CHECK(html.find(L"role: 'dialog'") != std::wstring::npos);
  CHECK(html.find(L"'aria-modal': 'true'") != std::wstring::npos);
  CHECK(html.find(L"'aria-labelledby': 'modal-title'") != std::wstring::npos);
  CHECK(html.find(L"'aria-describedby': describedBy") != std::wstring::npos);
  CHECK(html.find(L"const describedBy = modal?.kind === 'delete-session'") !=
        std::wstring::npos);
  CHECK(html.find(L"inert: modal !== null") != std::wstring::npos);
  CHECK(html.find(L"runtime.modalReturnFocus = active") != std::wstring::npos);
  CHECK(html.find(L"if (target?.isConnected) target.focus()") !=
        std::wstring::npos);
  CHECK(html.find(L"document.addEventListener('focusin'") !=
        std::wstring::npos);
  CHECK(html.find(L"document.addEventListener('keydown'") !=
        std::wstring::npos);
  CHECK(html.find(L"event.key === 'Escape'") != std::wstring::npos);
  CHECK(html.find(L"event.key !== 'Tab'") != std::wstring::npos);
  CHECK(html.find(L"event.target === event.currentTarget") !=
        std::wstring::npos);
  CHECK(html.find(L"post('dismiss-modal', { modalGeneration:") !=
        std::wstring::npos);
  CHECK(html.find(L"id: 'modal-tag'") != std::wstring::npos);
  CHECK(html.find(L"maxLength: 128") != std::wstring::npos);
  CHECK(html.find(L"characters remaining`") != std::wstring::npos);
  CHECK(html.find(L"id: 'modal-error'") != std::wstring::npos);
  CHECK(html.find(L"role: 'alert'") != std::wstring::npos);
  CHECK(html.find(L"id: 'modal-save'") != std::wstring::npos);
  CHECK(html.find(L"post('set-modal-value', {") != std::wstring::npos);
  CHECK(html.find(L"post('submit-modal', { modalGeneration:") !=
        std::wstring::npos);
  CHECK(html.find(L"id: 'modal-allow-incomplete'") != std::wstring::npos);
  CHECK(html.find(L"className: 'input-checkbox'") != std::wstring::npos);
  CHECK(html.find(L"Allow incomplete session") != std::wstring::npos);
  CHECK(html.find(L"post('set-modal-toggle', {") != std::wstring::npos);
  CHECK(html.find(L"h('fieldset', { className: 'flex flex-row items-center "
                  L"gap-4' }, () => {") != std::wstring::npos);
  CHECK(html.find(L"name: 'modal-blend'") != std::wstring::npos);
  CHECK(html.find(L"['hard', 'Hard'], ['feather', 'Feather']") !=
        std::wstring::npos);
  CHECK(html.find(L"id: 'modal-auto-contrast'") != std::wstring::npos);
  CHECK(html.find(L"Auto contrast (SDR only)") != std::wstring::npos);
  CHECK(html.find(L"id: 'modal-gpu-memory'") != std::wstring::npos);
  CHECK(html.find(L"D3D12 allocation (MiB):") != std::wstring::npos);
  CHECK(html.find(L"id: 'modal-debug-coverage'") != std::wstring::npos);
  CHECK(html.find(L"Write debug coverage image") != std::wstring::npos);
  CHECK(html.find(L"modal?.kind === 'app-settings'") != std::wstring::npos);
  CHECK(html.find(L"id: 'modal-delete-images'") != std::wstring::npos);
  CHECK(html.find(L"Also delete captured screenshots") != std::wstring::npos);
  CHECK(html.find(L"modal?.kind === 'delete-session'") != std::wstring::npos);
  CHECK(html.find(L"id: 'modal-file-list'") != std::wstring::npos);
  CHECK(html.find(L"description.split('\\n')") != std::wstring::npos);
  CHECK(html.find(L"deleteDescription.slice(1).forEach") !=
        std::wstring::npos);
  CHECK(html.find(L"modal?.kind === 'overwrite-output'") != std::wstring::npos);
  CHECK(html.find(L"? 'Delete'") != std::wstring::npos);
  CHECK(html.find(L"? 'Replace' : 'Save'") != std::wstring::npos);
  CHECK(html.find(L"targetPaths") == std::wstring::npos);
  CHECK(html.find(L"saves ? 'Cancel' : 'Close'") != std::wstring::npos);
  CHECK(html.find(L"snapshot.modal === null") != std::wstring::npos);
  CHECK(html.find(L".modal-layer {") != std::wstring::npos);
  CHECK(html.find(L"z-index: 30") != std::wstring::npos);
  CHECK(html.find(L"#modal-description { user-select: text; white-space: "
                  L"pre-wrap; }") != std::wstring::npos);
  CHECK(html.find(L"#modal-file-list {") != std::wstring::npos);
  CHECK(html.find(L"max-height: min(14rem, 40vh)") != std::wstring::npos);
  CHECK(html.find(L"overflow-x: hidden") != std::wstring::npos);
  CHECK(html.find(L"overflow-y: auto") != std::wstring::npos);
  CHECK(html.find(L"overflow-wrap: anywhere") != std::wstring::npos);
  CHECK(html.find(L"font-size: 0.75rem") != std::wstring::npos);
  CHECK(html.find(L"h('p', { id: 'modal-description' }, description);") !=
        std::wstring::npos);
  CHECK(html.find(L"h('p', { id: 'modal-description' }, description);\n"
                  L"          if (modal?.kind === 'edit-tag')") ==
        std::wstring::npos);
  CHECK(html.find(L"snapshot?.exposureAdjusted") != std::wstring::npos);
  CHECK(html.find(L"id: 'exposure-adjusted'") != std::wstring::npos);
  CHECK(html.find(L"Exposure has been adjusted") != std::wstring::npos);
  CHECK(html.find(L"function redrawChildren(children)") != std::wstring::npos);
  CHECK(html.find(L"function sessionFocusToken(element)") !=
        std::wstring::npos);
  CHECK(html.find(L"runtime.modalReturnFocus = restoredReturn") !=
        std::wstring::npos);
  CHECK(html.find(L"const restoredActive = sessionFocusTarget") !=
        std::wstring::npos);
  CHECK(html.find(L"'data-session-index': index") != std::wstring::npos);
  CHECK(html.find(L"'data-focus-target': 'actions'") != std::wstring::npos);
  CHECK(html.find(L"'data-focus-target': 'selection'") != std::wstring::npos);
  CHECK(html.find(L"'aria-disabled': disabled") != std::wstring::npos);
  CHECK(html.find(L"disabled: snapshot.busy") != std::wstring::npos);
  const std::size_t session_row = html.find(L"function SessionRow");
  const std::size_t session_table = html.find(L"function SessionTable");
  CHECK(session_row != std::wstring::npos &&
        session_table != std::wstring::npos);
  const std::wstring_view session_row_html(html.data() + session_row,
                                           session_table - session_row);
  CHECK(session_row_html.find(L"className: 'input-checkbox'") !=
        std::wstring_view::npos);
  CHECK(session_row_html.find(L"type: 'checkbox'") != std::wstring_view::npos);
  CHECK(session_row_html.find(L"checked: selected") != std::wstring_view::npos);
  CHECK(session_row_html.find(L"'aria-label': `Select ${session.name}`") !=
        std::wstring_view::npos);
  CHECK(session_row_html.find(L"onClick: event => event.preventDefault()") !=
        std::wstring_view::npos);
  CHECK(html.find(
            L"h('th', { className: 'w-12 p-2', 'aria-label': 'Selected' });") !=
        std::wstring::npos);
  CHECK(html.find(L"document.addEventListener('toggle'") !=
        std::wstring::npos);
  CHECK(html.find(L"document.addEventListener('pointerdown'") !=
        std::wstring::npos);
  CHECK(html.find(L"document.querySelectorAll('#sessions details[open]')") !=
        std::wstring::npos);
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
  const std::size_t resize_handler =
      html.find(L"function handleWindowResize()");
  const std::size_t resize_content =
      html.find(L"queueContentSize();", resize_handler);
  const std::size_t resize_geometry =
      html.find(L"queuePreviewGeometry();", resize_content);
  CHECK(resize_handler != std::wstring::npos &&
        resize_content != std::wstring::npos &&
        resize_geometry != std::wstring::npos);
  const std::size_t preview_visibility =
      html.find(L"const fullyVisible = snapshot?.stage === 'preview'");
  const std::size_t preview_readiness =
      html.find(L"snapshot.previewReady &&", preview_visibility);
  const std::size_t preview_modal =
      html.find(L"snapshot.modal === null", preview_readiness);
  CHECK(preview_visibility != std::wstring::npos &&
        preview_readiness != std::wstring::npos &&
        preview_modal != std::wstring::npos);
  CHECK(html.find(L"runtime.deviceScale !== window.devicePixelRatio") !=
        std::wstring::npos);
  CHECK(html.find(L"addEventListener('resize', handleWindowResize)") !=
        std::wstring::npos);
  CHECK(html.find(L"addEventListener('resize', queueContentSize)") ==
        std::wstring::npos);
  CHECK(html.find(L"id: 'input-view'") != std::wstring::npos);
  CHECK(html.find(L"id: 'preview-view'") != std::wstring::npos);
  CHECK(html.find(L"id: 'preview-frame'") != std::wstring::npos);
  CHECK(html.find(L"id: 'preview-placeholder'") != std::wstring::npos);
  CHECK(html.find(L"main.maximized #preview-placeholder { width: min(100%, "
                  L"200cqh); }") != std::wstring::npos);
  const std::size_t preview_view = html.find(L"function PreviewView");
  const std::size_t output_range =
      html.find(L"function OutputRange", preview_view);
  CHECK(preview_view != std::wstring::npos &&
        output_range != std::wstring::npos);
  const std::wstring_view preview_html(html.data() + preview_view,
                                       output_range - preview_view);
  const std::size_t exposure_actions =
      preview_html.find(L"className: 'flex items-center gap-4'");
  const std::size_t exposure_button =
      preview_html.find(L"id: 'adjust-exposure'", exposure_actions);
  const std::size_t exposure_button_end =
      preview_html.find(L"}, 'Adjust exposure", exposure_button);
  CHECK(exposure_actions != std::wstring_view::npos &&
        exposure_button != std::wstring_view::npos &&
        exposure_button_end != std::wstring_view::npos);
  const std::wstring_view exposure_actions_html = preview_html.substr(
      exposure_actions, exposure_button_end - exposure_actions);
  CHECK(exposure_actions_html.find(L"hidden:") == std::wstring_view::npos);
  CHECK(exposure_actions_html.find(L"disabled:") == std::wstring_view::npos);
  CHECK(html.find(L"id: 'output-view'") != std::wstring::npos);
  CHECK(html.find(L"hidden: !output") != std::wstring::npos);
  const std::size_t output_view = html.find(L"function OutputView");
  const std::size_t footer = html.find(L"function Footer", output_view);
  CHECK(output_view != std::wstring::npos && footer != std::wstring::npos);
  CHECK(
      html.substr(output_view, footer - output_view).find(L"debug coverage") ==
      std::wstring::npos);
  CHECK(html.find(L"snapshot?.resolutionPixels") != std::wstring::npos);
  CHECK(html.find(L"snapshot?.outputMaximumWidth") != std::wstring::npos);
  CHECK(html.find(L"max: maximum") != std::wstring::npos);
  CHECK(html.find(L"type: 'number'") == std::wstring::npos);
  CHECK(html.find(L"inputMode: 'numeric'") != std::wstring::npos);
  CHECK(html.find(L"pixels ? 99999") == std::wstring::npos);
  CHECK(html.find(L"if (format === 'jpeg')") != std::wstring::npos);
  CHECK(html.find(L"post('render-with-thumbnail')") != std::wstring::npos);
  const std::size_t footer_start = html.find(L"function Footer");
  const std::size_t footer_end =
      html.find(L"function BridgeError", footer_start);
  CHECK(footer_start != std::wstring::npos && footer_end != std::wstring::npos);
  const std::wstring_view footer_html(html.data() + footer_start,
                                      footer_end - footer_start);
  CHECK(footer_html.find(L"className: 'button sm shrink-0'") !=
        std::wstring_view::npos);
  CHECK(footer_html.find(L"className: 'button primary shrink-0'") !=
        std::wstring_view::npos);
  CHECK(footer_html.find(L"className: 'button shrink-0'") !=
        std::wstring_view::npos);
  std::size_t footer_shrink_count = 0;
  for (std::size_t offset = footer_html.find(L"shrink-0");
       offset != std::wstring_view::npos;
       offset = footer_html.find(L"shrink-0", offset + 1)) {
    ++footer_shrink_count;
  }
  CHECK(footer_shrink_count == 5);
  CHECK(html.find(L"className: 'input-radio'") != std::wstring::npos);
  CHECK(html.find(L"className: 'input-range flex-1'") != std::wstring::npos);
  CHECK(html.find(L"input-text flex-1 min-w-0") != std::wstring::npos);
  CHECK(html.find(L"progress: snapshot?.busy") != std::wstring::npos);
  CHECK(html.find(L"done: snapshot?.previewReady ?? false") !=
        std::wstring::npos);
  CHECK(html.find(L"snapshot?.outputComplete ?? false") != std::wstring::npos);
  CHECK(html.find(L"className: 'tag-cell'") == std::wstring::npos);
  CHECK(html.find(L"label: 'Copy coordinates'") != std::wstring::npos);
  CHECK(html.find(L"disabled: disabled || !session.hasCoordinates") !=
        std::wstring::npos);
  CHECK(html.find(
            L"onClick: () => post('copy-session-coordinates', { index })") !=
        std::wstring::npos);
  CHECK(html.find(L"onClick: () => post('edit-tag', { index })") !=
        std::wstring::npos);
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
  CHECK(exposure_html.find(L"class=\"input-checkbox\"") != std::wstring::npos);
  CHECK(exposure_html.find(L"set-exposure-reference") != std::wstring::npos);
  CHECK(exposure_html.find(L"toggle-exposure-selection") != std::wstring::npos);
  const auto final_exposure_divider =
      exposure_html.rfind(L"<hr class=\"border-gray-500\">");
  const auto final_exposure_label = exposure_html.find(L">Exposure:");
  CHECK(final_exposure_divider != std::wstring::npos &&
        final_exposure_label != std::wstring::npos &&
        final_exposure_divider < final_exposure_label);
  CHECK(exposure_html.find(L">Exposure:") != std::wstring::npos);
  CHECK(exposure_html.find(L"id=\"final-exposure\"") != std::wstring::npos);
  CHECK(exposure_html.find(L"min=\"-2\" max=\"2\" step=\"0.1\"") !=
        std::wstring::npos);
  CHECK(exposure_html.find(L"id=\"final-exposure-value\"") !=
        std::wstring::npos);
  CHECK(exposure_html.find(
            L"#final-exposure-value { flex: 0 0 calc(var(--spacing) * 16); "
            L"white-space: nowrap; }") != std::wstring::npos);
  CHECK(exposure_html.find(L"set-final-exposure") != std::wstring::npos);
  CHECK(exposure_html.find(L"pendingExposure ?? snapshot.finalExposure") !=
        std::wstring::npos);
  const auto exposure_input_listener =
      exposure_html.find(L"byId('final-exposure').addEventListener('input'");
  const auto pending_exposure_assignment =
      exposure_html.find(L"pendingExposure = value;", exposure_input_listener);
  const auto exposure_value_update = exposure_html.find(
      L"byId('final-exposure-value').textContent = formatEv(value);",
      pending_exposure_assignment);
  const auto exposure_change_listener =
      exposure_html.find(L"byId('final-exposure').addEventListener('change'",
                         exposure_value_update);
  const auto exposure_post = exposure_html.find(L"post('set-final-exposure'",
                                                exposure_change_listener);
  CHECK(exposure_input_listener != std::wstring::npos &&
        pending_exposure_assignment != std::wstring::npos &&
        exposure_value_update != std::wstring::npos &&
        exposure_change_listener != std::wstring::npos &&
        exposure_post != std::wstring::npos);
  CHECK(exposure_html.find(L"post('set-final-exposure'",
                           exposure_input_listener) == exposure_post);
  CHECK(exposure_html.find(L"queueExposure") == std::wstring::npos);
  CHECK(exposure_html.find(L"flushPendingExposure") == std::wstring::npos);
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
  command.page_generation = 4U;
  CHECK(pano::app::webview_command_is_current(true, 4U, command));
  CHECK(!pano::app::webview_command_is_current(false, 4U, command));
  CHECK(!pano::app::webview_command_is_current(true, 3U, command));
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"dismiss-modal","pageGeneration":4,"modalGeneration":9})",
      command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::dismiss_modal);
  CHECK(command.modal_generation == 9U);
  CHECK(pano::app::webview_command_is_current(true, 4U, command));
  CHECK(pano::app::webview_modal_command_is_current(true, 9U, command));
  CHECK(!pano::app::webview_modal_command_is_current(false, 9U, command));
  CHECK(!pano::app::webview_modal_command_is_current(true, 8U, command));
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"dismiss-modal","pageGeneration":4})", command,
      error));
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"dismiss-modal","pageGeneration":4,"modalGeneration":-1})",
      command, error));
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"dismiss-modal","pageGeneration":4,"modalGeneration":9,"value":"close"})",
      command, error));
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-modal-value","pageGeneration":4,"modalGeneration":9,"value":"favorite"})",
      command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::set_modal_value);
  CHECK(command.modal_generation == 9U && command.value == L"favorite");
  CHECK(pano::app::webview_modal_command_is_current(true, 9U, command));
  CHECK(!pano::app::webview_modal_command_is_current(true, 10U, command));
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"submit-modal","pageGeneration":4,"modalGeneration":9})",
      command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::submit_modal);
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-modal-toggle","pageGeneration":4,"modalGeneration":9,"enabled":true})",
      command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::set_modal_toggle);
  CHECK(command.modal_generation == 9U && command.enabled == true);
  CHECK(pano::app::webview_modal_command_is_current(true, 9U, command));
  CHECK(!pano::app::webview_modal_command_is_current(true, 10U, command));
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-modal-toggle","pageGeneration":4,"modalGeneration":9,"enabled":1})",
      command, error));
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-modal-toggle","pageGeneration":4,"modalGeneration":9,"enabled":true,"target":"incomplete"})",
      command, error));
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-modal-value","pageGeneration":4,"modalGeneration":9})",
      command, error));
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"submit-modal","pageGeneration":4,"modalGeneration":9,"value":"favorite"})",
      command, error));
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-directory","pageGeneration":3,"target":"game","value":"C:\\Games\\Cyberpunk 2077"})",
      command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::set_game_directory);
  CHECK(command.page_generation == 3U);
  CHECK(command.value == L"C:\\Games\\Cyberpunk 2077");
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-directory","pageGeneration":3,"target":"output","value":"C:\\Pictures"})",
      command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::set_output_directory);
  CHECK(command.value == L"C:\\Pictures");
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"navigate","pageGeneration":3,"target":"output"})",
      command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::navigate_output);
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-output-value","pageGeneration":3,"target":"format","value":"png"})",
      command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::set_output_format);
  CHECK(command.value == L"png");
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-output-value","pageGeneration":3,"target":"width","value":"18000"})",
      command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::set_output_width);
  CHECK(command.value == L"18000");
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"toggle-resolution-mode","pageGeneration":3})",
      command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::toggle_resolution_mode);
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"render-with-thumbnail","pageGeneration":3})",
      command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::render_with_thumbnail);
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-output-value","pageGeneration":3,"target":"workers","value":"2"})",
      command, error));
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-output-value","pageGeneration":3,"target":"format","value":"bmp"})",
      command, error));
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"select-session","pageGeneration":4,"index":2})",
      command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::select_session);
  CHECK(command.session_index == 2U);
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"copy-session-coordinates","pageGeneration":4,"index":2})",
      command, error));
  CHECK(command.kind ==
        pano::app::WebViewCommandKind::copy_session_coordinates);
  CHECK(command.session_index == 2U);
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"copy-session-coordinates","pageGeneration":4,"index":2,"value":"unexpected"})",
      command, error));
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-exposure-overlay","pageGeneration":2,"enabled":true})",
      command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::set_exposure_overlay);
  CHECK(command.enabled == true);
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-final-exposure","pageGeneration":2,"value":-1.7})",
      command, error));
  CHECK(command.kind == pano::app::WebViewCommandKind::set_final_exposure);
  CHECK(command.exposure_ev == -1.7);
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-final-exposure","pageGeneration":2,"value":-2})",
      command, error));
  CHECK(command.exposure_ev == -2.0);
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-final-exposure","pageGeneration":2,"value":1})",
      command, error));
  CHECK(command.exposure_ev == 1.0);
  CHECK(pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-final-exposure","pageGeneration":2,"value":2})",
      command, error));
  CHECK(command.exposure_ev == 2.0);
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-final-exposure","pageGeneration":2,"value":2.1})",
      command, error));
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-final-exposure","pageGeneration":2,"value":0.15})",
      command, error));
  CHECK(!pano::app::parse_webview_command_json(
      R"({"version":1,"kind":"set-final-exposure","pageGeneration":2,"value":"1.0"})",
      command, error));
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
