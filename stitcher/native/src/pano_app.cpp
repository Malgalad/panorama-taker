#include "pano_app.h"
#include "pano_app_version.h"

#include <ostream>

namespace pano::app {
namespace {
constexpr const char *version = PANO_APP_VERSION;

void write_help(std::ostream &output) {
  output
      << "Usage: pano-stitch-native [--help] [--version]\n"
         "       pano-stitch-native render SESSION --output PATH [options]\n"
         "\n"
         "Native panorama stitcher application host.\n"
         "\n"
         "Render-plan options:\n"
         "  --image-dir PATH              relocated screenshot directory\n"
         "  --width PIXELS                explicit panorama width\n"
         "  --resolution FRACTION         scale from 0 through 1 (for example "
         "1/4)\n"
         "  --format png|jpeg|exr         output format (default: jpeg)\n"
         "  --jpeg-quality 1..100         JPEG quality (default: 95)\n"
         "  --blend hard|feather          blend mode (default: hard)\n"
         "  --thumbnail                   plan a projected session thumbnail\n"
         "  --coverage                    plan a coverage PNG\n"
         "  --memory-budget-mib 1..8192   compositor budget (default: 1024)\n"
         "  --workers COUNT               worker count; 0 selects automatic\n"
         "  --no-gpu | --gpu-strict       backend admission intent\n"
         "  --gpu-memory-budget-mib MIB    explicit VRAM budget\n"
         "  --allow-incomplete            admit incomplete sessions\n"
         "  --auto-exposure               plan automatic exposure matching\n"
         "  --exposure-target INDEX       manual exposure reference\n"
         "  --exposure-source INDEX       repeatable manual exposure source\n"
         "  --no-auto-contrast             disable final SDR auto contrast\n";
}
} // namespace

bool calculate_gui_layout_metrics(const unsigned dpi, const int client_width,
                                  GuiLayoutMetrics &metrics,
                                  std::string &error) {
  if (dpi < 48U || dpi > 768U || client_width <= 0) {
    error = "invalid GUI layout dimensions";
    return false;
  }
  const auto scale = [dpi](const int value) {
    return static_cast<int>((static_cast<long long>(value) * dpi + 48) / 96);
  };
  GuiLayoutMetrics calculated;
  calculated.margin = scale(20);
  calculated.gap = scale(12);
  calculated.row_height = scale(34);
  calculated.label_width = scale(128);
  calculated.button_width = scale(112);
  calculated.content_width = client_width - 2 * calculated.margin;
  if (calculated.content_width <=
      calculated.label_width + calculated.button_width + 2 * calculated.gap) {
    error = "GUI client width is too small";
    return false;
  }
  metrics = calculated;
  error.clear();
  return true;
}

int run(const std::vector<std::string> &arguments, std::ostream &output,
        std::ostream &error) {
  if (arguments.empty() || arguments == std::vector<std::string>{"--help"} ||
      arguments == std::vector<std::string>{"-h"}) {
    write_help(output);
    return static_cast<int>(ExitCode::success);
  }
  if (arguments == std::vector<std::string>{"--version"}) {
    output << "pano-stitch-native " << version << '\n';
    return static_cast<int>(ExitCode::success);
  }
  if (arguments.front() == "render") {
    RenderOptions options;
    std::string detail;
    if (!parse_render_options(arguments, options, detail)) {
      error << "error: " << detail << '\n';
      return static_cast<int>(ExitCode::invalid_input);
    }
    RenderPlan plan;
    if (!make_render_plan(options, plan, detail)) {
      error << "error: " << detail << '\n';
      return static_cast<int>(ExitCode::invalid_input);
    }
    output << "render plan: session=" << plan.session.session_id
           << " frames=" << plan.session.frames.size()
           << " projection=" << plan.projection << " blend=" << plan.blend
           << " output=" << plan.outputs.panorama.final_path << '\n';
    return static_cast<int>(ExitCode::success);
  }
  error << "unknown option or command: " << arguments.front() << '\n';
  return static_cast<int>(ExitCode::invalid_input);
}

} // namespace pano::app
