#include "pano_app.h"
#include "pano_app_version.h"
#include "pano_gpu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
int failures = 0;

void expect(const bool condition, const int line) {
  if (!condition) {
    std::cerr << "application contract check failed at line " << line << '\n';
    ++failures;
  }
}

#define EXPECT(condition) expect((condition), __LINE__)

fs::path fixtures() { return fs::u8path(PANO_APP_FIXTURE_DIR); }
fs::path codec_fixtures() { return fs::u8path(PANO_CODEC_FIXTURE_DIR); }

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = fs::temp_directory_path() /
            ("pano-app-contract-" + std::to_string(nonce));
    fs::create_directories(path_);
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  const fs::path &path() const { return path_; }

private:
  fs::path path_;
};

void write_text(const fs::path &path, const std::string &text) {
  std::ofstream stream(path, std::ios::binary);
  stream << text;
}

std::vector<std::uint8_t> read_bytes(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

struct NativeRenderProgress {
  unsigned calls = 0;
  unsigned completed = 0;
  unsigned total = 0;
  std::vector<unsigned> values;
  std::vector<std::string> phases;
};

[[maybe_unused]] void record_native_render_progress(void *const context,
                                                    const unsigned completed,
                                                    const unsigned total,
                                                    const char *const phase) {
  auto &progress = *static_cast<NativeRenderProgress *>(context);
  ++progress.calls;
  progress.completed = completed;
  progress.total = total;
  progress.values.push_back(completed);
  progress.phases.emplace_back(phase == nullptr ? "" : phase);
}

void cancel_gpu_on_progress(void *const context, unsigned, unsigned,
                            const char *) {
  pano_gpu_cancellation_token_cancel(
      static_cast<pano_gpu_cancellation_token *>(context));
}

void write_bytes(const fs::path &path, const std::vector<std::uint8_t> &bytes) {
  std::ofstream stream(path, std::ios::binary);
  stream.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

std::string read_text(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

std::uint32_t test_crc32(const std::uint8_t *data, const std::size_t size) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1U) ^
            (0xedb88320U &
             static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & 1U)));
  }
  return crc ^ 0xffffffffU;
}

bool parse(const std::vector<std::string> &arguments,
           pano::app::RenderOptions &options, std::string &error) {
  return pano::app::parse_render_options(arguments, options, error);
}

void test_dispatch() {
  std::ostringstream output;
  std::ostringstream error;
  EXPECT(pano::app::run({}, output, error) == 0);
  EXPECT(output.str().find("Usage:") != std::string::npos);
  EXPECT(output.str().find("--auto-exposure") != std::string::npos);
  output.str({});
  EXPECT(pano::app::run({"--version"}, output, error) == 0);
  EXPECT(output.str().find(PANO_APP_VERSION) != std::string::npos);
  EXPECT(pano::app::run({"--unknown"}, output, error) ==
         static_cast<int>(pano::app::ExitCode::invalid_input));
  EXPECT(error.str().find("--unknown") != std::string::npos);
}

void test_path_and_size_options() {
  pano::app::RenderOptions options;
  std::string error;
  EXPECT(parse(
      {"render", u8"сессия с пробелами.json", "--output", u8"панорама.jpg"},
      options, error));
  EXPECT(options.session == u8"сессия с пробелами.json");
  EXPECT(options.output == u8"панорама.jpg");
  EXPECT(options.resolution == 1.0);
  EXPECT(!options.width.has_value());
  EXPECT(parse({"render", "session.json", "--output", "out.jpg", "--image-dir",
                "moved", "--resolution", "1/4"},
               options, error));
  EXPECT(options.image_dir == "moved");
  EXPECT(std::abs(options.resolution - 0.25) < 1.0e-12);
  EXPECT(parse(
      {"render", "session.json", "--output", "out.jpg", "--width", "4096"},
      options, error));
  EXPECT(options.width == 4096U);
  EXPECT(!parse({"render", "session.json"}, options, error));
  EXPECT(!parse({"render", "session.json", "--output", "out", "--width", "0"},
                options, error));
  EXPECT(!parse(
      {"render", "session.json", "--output", "out", "--resolution", "1junk"},
      options, error));
  EXPECT(!parse(
      {"render", "session.json", "--output", "out", "--resolution", "2/0"},
      options, error));
  EXPECT(!parse(
      {"render", "session.json", "--output", "out", "--resolution", "1.5/2"},
      options, error));
  EXPECT(!parse({"render", "session.json", "--output", "out", "--resolution",
                 "1/2", "--width", "100"},
                options, error));
}

void test_format_and_render_options() {
  pano::app::RenderOptions options;
  std::string error;
  EXPECT(parse({"render", "session", "--output", "out", "--format", "png",
                "--blend", "feather", "--thumbnail", "--coverage"},
               options, error));
  EXPECT(options.format == "png");
  EXPECT(options.blend == "feather");
  EXPECT(options.thumbnail && options.coverage);
  EXPECT(!parse({"render", "session", "--output", "out", "--format", "tiff"},
                options, error));
  EXPECT(!parse({"render", "session", "--output", "out", "--format", "png",
                 "--jpeg-quality", "80"},
                options, error));
  EXPECT(!parse({"render", "session", "--output", "out", "--format", "png",
                 "--jpeg-quality", "95"},
                options, error));
  EXPECT(
      !parse({"render", "session", "--output", "out", "--jpeg-quality", "101"},
             options, error));
}

void test_memory_backend_and_exposure_options() {
  pano::app::RenderOptions options;
  std::string error;
  EXPECT(parse({"render", "session", "--output", "out", "--memory-budget-mib",
                "8192", "--workers", "0", "--gpu-memory-budget-mib", "2048",
                "--gpu-strict", "--allow-incomplete", "--no-auto-contrast"},
               options, error));
  EXPECT(options.memory_mib == 8192U);
  EXPECT(options.workers == 0U);
  EXPECT(options.gpu_memory_mib == 2048U);
  EXPECT(options.gpu && options.gpu_strict && options.allow_incomplete);
  EXPECT(!options.auto_contrast);
  EXPECT(!parse(
      {"render", "session", "--output", "out", "--memory-budget-mib", "8193"},
      options, error));
  EXPECT(!parse(
      {"render", "session", "--output", "out", "--no-gpu", "--gpu-strict"},
      options, error));
  EXPECT(!parse({"render", "session", "--output", "out", "--no-gpu",
                 "--gpu-memory-budget-mib", "1"},
                options, error));
  EXPECT(parse({"render", "session", "--output", "out", "--exposure-target",
                "0", "--exposure-source", "1", "--exposure-source", "2"},
               options, error));
  EXPECT(options.exposure_target == 0U);
  EXPECT(options.exposure_sources == std::vector<unsigned>({1, 2}));
  EXPECT(parse({"render", "session", "--output", "out", "--auto-exposure"},
               options, error));
  EXPECT(options.automatic_exposure);
  EXPECT(
      !parse({"render", "session", "--output", "out", "--exposure-source", "1"},
             options, error));
  EXPECT(
      !parse({"render", "session", "--output", "out", "--exposure-target", "0"},
             options, error));
  EXPECT(!parse({"render", "session", "--output", "out", "--auto-exposure",
                 "--exposure-target", "0", "--exposure-source", "1"},
                options, error));
  EXPECT(!parse({"render", "session", "--output", "out", "--exposure-target",
                 "0", "--exposure-source", "0"},
                options, error));
  EXPECT(!parse({"render", "session", "--output", "out", "--exposure-target",
                 "0", "--exposure-source", "1", "--exposure-source", "2",
                 "--exposure-source", "1"},
                options, error));
}

void test_shared_session() {
  pano::app::SessionSummary session;
  std::string error;
  EXPECT(pano::app::load_session((fixtures() / "shared-valid.json").u8string(),
                                 std::nullopt, session, error));
  EXPECT(session.schema_version == 1U);
  EXPECT(session.session_id == "shared-valid");
  EXPECT(session.capture_mode == "full_sphere");
  EXPECT(session.projection == "rectilinear");
  EXPECT(session.completed);
  EXPECT(session.frames.size() == 1);
  EXPECT(session.frames[0].filename == "shared.png");
  EXPECT(session.image_encoding.sample_type == "uint8");
  EXPECT(session.image_encoding.transfer_function == "srgb");
  EXPECT(!pano::app::load_session(
      (fixtures() / "shared-invalid-viewport.json").u8string(), std::nullopt,
      session, error));
  EXPECT(error.find("viewport.width") != std::string::npos);

  EXPECT(pano::app::load_session(
      (fixtures() / "shared-default-encoding.json").u8string(), std::nullopt,
      session, error));
  EXPECT(session.session_id == "shared-default-encoding");
  EXPECT(session.capture_mode == "horizontal");
  EXPECT(session.horizontal_fov_deg == 80.0);
  EXPECT(session.vertical_fov_deg == 50.0);
  EXPECT(session.overlap_fraction == 0.1);
  EXPECT(!session.completed);
  EXPECT(session.image_encoding.sample_type == "uint8");
  EXPECT(session.image_encoding.color_primaries == "srgb");
  EXPECT(session.image_encoding.transfer_function == "srgb");
  EXPECT(session.image_encoding.reference_white_nits == 100.0);
  EXPECT(session.frames[0].camera_basis_row_major.has_value());
}

void test_cet_session_and_paths() {
  TemporaryDirectory temporary;
  const auto moved = temporary.path() / fs::u8path(u8"moved images");
  fs::create_directories(moved);
  write_text(moved / "complete.png", "fixture");
  pano::app::SessionSummary session;
  std::string error;
  EXPECT(pano::app::load_session((fixtures() / "cet-complete.json").u8string(),
                                 moved.u8string(), session, error));
  EXPECT(session.completed);
  EXPECT(session.frames.size() == 1);
  EXPECT(session.frames[0].filename == (moved / "complete.png").u8string());
  EXPECT(session.frames[0].camera_basis_row_major.has_value());
  const std::array<double, 9> identity = {1.0, 0.0, 0.0, 0.0, 1.0,
                                          0.0, 0.0, 0.0, 1.0};
  for (std::size_t index = 0; index < identity.size(); ++index) {
    EXPECT(std::abs((*session.frames[0].camera_basis_row_major)[index] -
                    identity[index]) < 1.0e-12);
  }
  EXPECT(session.image_encoding.transfer_function == "pq");
  EXPECT(
      pano::app::load_session((fixtures() / "cet-incomplete.json").u8string(),
                              std::nullopt, session, error));
  EXPECT(!session.completed);
  EXPECT(session.frames[0].filename == "incomplete.png");
  EXPECT(!pano::app::load_session((fixtures() / "cet-invalid.json").u8string(),
                                  std::nullopt, session, error));
  EXPECT(error.find("state") != std::string::npos);

  const auto sibling_json = temporary.path() / "sibling.json";
  write_text(temporary.path() / fs::u8path(u8"кадр.png"), "fixture");
  write_text(
      sibling_json,
      u8R"({"schema_version":1,"session_id":"юникод","horizontal_fov_deg":90,"vertical_fov_deg":60,"state":"completed","poses":[{"index":0,"commanded_yaw_deg":0,"commanded_pitch_deg":0,"forward":[0,1,0],"right":[1,0,0],"up":[0,0,1],"screenshot_path":"C:\\captures\\кадр.png"}]})");
  EXPECT(pano::app::load_session(sibling_json.u8string(), std::nullopt, session,
                                 error));
  EXPECT(session.session_id == u8"юникод");
  EXPECT(session.frames[0].filename ==
         (temporary.path() / fs::u8path(u8"кадр.png")).u8string());

  const auto direct_image = temporary.path() / "direct.png";
  write_text(direct_image, "fixture");
  const auto direct_json = temporary.path() / "direct.json";
  write_text(
      direct_json,
      std::string(
          R"({"schema_version":1,"session_id":"direct","horizontal_fov_deg":90,"vertical_fov_deg":60,"state":"completed","poses":[{"index":0,"commanded_yaw_deg":0,"commanded_pitch_deg":0,"forward":[0,1,0],"right":[1,0,0],"up":[0,0,1],"screenshot_path":")") +
          direct_image.generic_u8string() + R"("}]})");
  EXPECT(pano::app::load_session(direct_json.u8string(), std::nullopt, session,
                                 error));
  EXPECT(session.frames[0].filename == direct_image.u8string());

  const auto malformed = temporary.path() / "malformed.json";
  write_text(malformed, "{]");
  EXPECT(!pano::app::load_session(malformed.u8string(), std::nullopt, session,
                                  error));
  EXPECT(error.find("byte") != std::string::npos);
  const auto non_object = temporary.path() / "non-object.json";
  write_text(non_object, "[]");
  EXPECT(!pano::app::load_session(non_object.u8string(), std::nullopt, session,
                                  error));
  EXPECT(error.find("root") != std::string::npos);
  const auto invalid_utf8 = temporary.path() / "invalid-utf8.json";
  write_text(invalid_utf8,
             std::string("{\"bad\":\"") + static_cast<char>(0xff) + "\"}");
  EXPECT(!pano::app::load_session(invalid_utf8.u8string(), std::nullopt,
                                  session, error));

  const auto unknown = temporary.path() / "unknown.json";
  write_text(
      unknown,
      R"({"schema_version":1,"session_id":"unknown","capture_mode":"horizontal","projection":"rectilinear","viewport":{"width":1,"height":1},"fov":{"horizontal_deg":90,"vertical_deg":60,"source":"configured_value"},"base_pose":{"position":[0,0,0],"orientation_xyzw":[0,0,0,1]},"planner":{"overlap_fraction":0.1,"yaw_step_deg":80,"pitch_step_deg":50},"frames":[],"completed":true,"surprise":1})");
  EXPECT(!pano::app::load_session(unknown.u8string(), std::nullopt, session,
                                  error));
  EXPECT(error.find("surprise") != std::string::npos);
}

void test_output_and_render_plans() {
  TemporaryDirectory temporary;
  const auto requested = temporary.path() / "result.wrong";
  const auto coverage = temporary.path() / "result-coverage.png";
  write_text(coverage, "existing");
  pano::app::RenderOptions options;
  options.session = (fixtures() / "shared-valid.json").u8string();
  options.output = requested.u8string();
  options.format = "png";
  options.coverage = true;
  options.thumbnail = true;
  options.blend = "feather";
  options.width = 4097;
  options.memory_mib = 768U;
  options.gpu_memory_mib = 3072U;
  options.automatic_exposure = true;
  pano::app::OutputPlan outputs;
  std::string error;
  EXPECT(pano::app::plan_outputs(options, outputs, error));
  EXPECT(outputs.panorama.final_path ==
         (temporary.path() / "result.png").u8string());
  EXPECT(outputs.coverage.has_value());
  EXPECT(outputs.coverage->final_path == coverage.u8string());
  EXPECT(outputs.coverage->exists);
  EXPECT(outputs.thumbnail.has_value());
  EXPECT(outputs.thumbnail->final_path ==
         (temporary.path() / "result-thumbnail.png").u8string());
  EXPECT(outputs.panorama.stage_pattern.find(".<unique>.partial") !=
         std::string::npos);

  pano::app::RenderPlan plan;
  EXPECT(pano::app::make_render_plan(options, plan, error));
  EXPECT(plan.session.session_id == "shared-valid");
  EXPECT(plan.output_width == 4096U);
  EXPECT(plan.output_height == 2048U);
  EXPECT(plan.projection == "equirectangular");
  EXPECT(plan.blend == "feather");
  EXPECT(plan.memory_mib == 768U && plan.gpu_memory_mib == 3072U);
  EXPECT(plan.automatic_exposure);

  options.session = (fixtures() / "cet-incomplete.json").u8string();
  options.image_dir.clear();
  EXPECT(!pano::app::make_render_plan(options, plan, error));
  EXPECT(error.find("incomplete") != std::string::npos);
  options.allow_incomplete = true;
  EXPECT(pano::app::make_render_plan(options, plan, error));
  options.automatic_exposure = false;
  options.exposure_target = 0;
  options.exposure_sources = {0};
  EXPECT(!pano::app::make_render_plan(options, plan, error));
}

void test_render_dispatch() {
  TemporaryDirectory temporary;
  std::ostringstream output;
  std::ostringstream error;
  EXPECT(
      pano::app::run({"render", (fixtures() / "shared-valid.json").u8string(),
                      "--output", (temporary.path() / "result.jpg").u8string()},
                     output, error) == 0);
  EXPECT(output.str().find("session=shared-valid") != std::string::npos);
  EXPECT(output.str().find("projection=equirectangular") != std::string::npos);
  EXPECT(!fs::exists(temporary.path() / "result.jpg"));
}

bool always_cancelled(void *) { return true; }

void test_sdr_codec_contracts() {
  pano::app::ImageInfo png;
  pano::app::ImageInfo jpeg;
  pano::app::CodecErrorCategory category =
      pano::app::CodecErrorCategory::codec_failure;
  std::string error;
  EXPECT(pano::app::inspect_image(
      (codec_fixtures() / "rgb8-srgb.png").u8string(), png, category, error));
  EXPECT(category == pano::app::CodecErrorCategory::none);
  EXPECT(png.container == pano::app::ImageContainer::png);
  EXPECT(png.width == 3U && png.height == 2U && png.channels == 3U);
  EXPECT(png.encoding.sample_type == "uint8");
  EXPECT(png.encoding.color_primaries == "srgb");
  EXPECT(png.encoding.transfer_function == "srgb");
  EXPECT(png.encoding.reference_white_nits == 100.0);
  EXPECT(!png.png_cicp.has_value());

  EXPECT(pano::app::inspect_image(
      (codec_fixtures() / "rgb8-srgb.jpg").u8string(), jpeg, category, error));
  EXPECT(jpeg.container == pano::app::ImageContainer::jpeg);
  EXPECT(jpeg.width == 8U && jpeg.height == 4U && jpeg.channels == 3U);
  EXPECT(jpeg.encoding.sample_type == "uint8");
  EXPECT(jpeg.encoding.transfer_function == "srgb");
  EXPECT(jpeg.jpeg_subsampling == "4:4:4");

  TemporaryDirectory temporary;
  const auto truncated = temporary.path() / "truncated.png";
  write_text(truncated, "\x89PNG\r\n\x1a\n");
  EXPECT(!pano::app::inspect_image(truncated.u8string(), png, category, error));
  EXPECT(category == pano::app::CodecErrorCategory::malformed);
  const auto unsupported = temporary.path() / "unsupported.bin";
  write_text(unsupported, "not an image");
  EXPECT(
      !pano::app::inspect_image(unsupported.u8string(), png, category, error));
  EXPECT(category == pano::app::CodecErrorCategory::unsupported);
  EXPECT(!pano::app::inspect_image(
      (temporary.path() / "missing.png").u8string(), png, category, error));
  EXPECT(category == pano::app::CodecErrorCategory::io);

  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(png.width) *
                                   png.height * 3U);
#ifdef _WIN32
  EXPECT(pano::app::decode_image(
      (codec_fixtures() / "rgb8-srgb.png").u8string(), png, pixels.data(),
      png.width * 3U, pixels.size(), {}, category, error));
  const std::vector<std::uint8_t> expected_png = {
      0, 64, 255, 10, 20, 30, 255, 128, 1, 3, 2, 1, 17, 34, 51, 250, 251, 252};
  EXPECT(pixels == expected_png);
  pixels.resize(static_cast<std::size_t>(jpeg.width) * jpeg.height * 3U);
  EXPECT(pano::app::decode_image(
      (codec_fixtures() / "rgb8-srgb.jpg").u8string(), jpeg, pixels.data(),
      jpeg.width * 3U, pixels.size(), {}, category, error));
  const std::vector<std::uint8_t> expected_jpeg = {
      0,   1,   0,   34,  2,   25,  65,  0,   40,  95,  0,   52,  123, 4,
      72,  159, 0,   99,  182, 3,   107, 217, 2,   132, 0,   68,  42,  31,
      68,  60,  62,  64,  87,  93,  66,  101, 118, 70,  112, 154, 62,  135,
      181, 65,  166, 220, 65,  183, 0,   134, 88,  30,  135, 104, 64,  135,
      129, 94,  137, 143, 122, 135, 177, 159, 134, 176, 189, 139, 202, 225,
      127, 212, 2,   199, 127, 32,  199, 143, 63,  202, 160, 91,  199, 183,
      121, 199, 203, 160, 197, 226, 188, 197, 240, 212, 209, 10};
  EXPECT(pixels == expected_jpeg);
#else
  EXPECT(!pano::app::decode_image(
      (codec_fixtures() / "rgb8-srgb.png").u8string(), png, pixels.data(),
      png.width * 3U, pixels.size(), {}, category, error));
  EXPECT(category == pano::app::CodecErrorCategory::unavailable);
#endif
  EXPECT(!pano::app::decode_image(
      (codec_fixtures() / "rgb8-srgb.png").u8string(), png, pixels.data(),
      png.width * 3U, 1U, {}, category, error));
  EXPECT(category == pano::app::CodecErrorCategory::invalid_request);
  const pano::app::CancellationCheck cancellation{always_cancelled, nullptr};
  EXPECT(!pano::app::decode_image(
      (codec_fixtures() / "rgb8-srgb.png").u8string(), png, pixels.data(),
      png.width * 3U, static_cast<std::uint64_t>(png.width) * png.height * 3U,
      cancellation, category, error));
  EXPECT(category == pano::app::CodecErrorCategory::cancelled);
}

void test_pq_png_codec_contracts() {
  const auto path = codec_fixtures() / "rgb16-rec2020-pq.png";
  pano::app::ImageInfo info;
  pano::app::CodecErrorCategory category{};
  std::string error;
  EXPECT(pano::app::inspect_image(path.u8string(), info, category, error));
  EXPECT(info.container == pano::app::ImageContainer::png);
  EXPECT(info.width == 3U && info.height == 2U && info.channels == 3U);
  EXPECT(info.encoding.sample_type == "uint16");
  EXPECT(info.encoding.color_primaries == "rec2020");
  EXPECT(info.encoding.transfer_function == "pq");
  EXPECT(info.encoding.reference_white_nits == 203.0);
  EXPECT((info.png_cicp == std::array<std::uint8_t, 4>{9, 16, 0, 1}));

  TemporaryDirectory temporary;
  auto unsupported_bytes = read_bytes(path);
  const std::array<std::uint8_t, 4> cicp_type{'c', 'I', 'C', 'P'};
  const auto type =
      std::search(unsupported_bytes.begin(), unsupported_bytes.end(),
                  cicp_type.begin(), cicp_type.end());
  EXPECT(type != unsupported_bytes.end());
  if (type != unsupported_bytes.end() &&
      std::distance(type, unsupported_bytes.end()) >= 12) {
    const auto offset = static_cast<std::size_t>(
        std::distance(unsupported_bytes.begin(), type));
    unsupported_bytes[offset + 4] = 1;
    const auto crc = test_crc32(unsupported_bytes.data() + offset, 8);
    for (unsigned byte = 0; byte < 4; ++byte)
      unsupported_bytes[offset + 8 + byte] =
          static_cast<std::uint8_t>(crc >> ((3U - byte) * 8U));
    const auto unsupported = temporary.path() / "unsupported-cicp.png";
    write_bytes(unsupported, unsupported_bytes);
    EXPECT(!pano::app::inspect_image(unsupported.u8string(), info, category,
                                     error));
    EXPECT(category == pano::app::CodecErrorCategory::unsupported);
    unsupported_bytes[offset + 8] ^= 1U;
    const auto malformed = temporary.path() / "malformed-cicp.png";
    write_bytes(malformed, unsupported_bytes);
    EXPECT(
        !pano::app::inspect_image(malformed.u8string(), info, category, error));
    EXPECT(category == pano::app::CodecErrorCategory::malformed);
  }

  std::vector<std::uint16_t> pixels(static_cast<std::size_t>(info.width) *
                                    info.height * 3U);
#ifdef _WIN32
  EXPECT(pano::app::decode_image(path.u8string(), info, pixels.data(),
                                 info.width * 3U * sizeof(std::uint16_t),
                                 pixels.size() * sizeof(std::uint16_t), {},
                                 category, error));
  const std::vector<std::uint16_t> expected{
      0, 32768, 65535, 1000,  2000,  3000,  65535, 50000, 40000,
      1, 2,     3,     16384, 24576, 32768, 60000, 45000, 12345};
  EXPECT(pixels == expected);
#else
  EXPECT(!pano::app::decode_image(path.u8string(), info, pixels.data(),
                                  info.width * 3U * sizeof(std::uint16_t),
                                  pixels.size() * sizeof(std::uint16_t), {},
                                  category, error));
  EXPECT(category == pano::app::CodecErrorCategory::unavailable);
#endif
}

void test_exr_codec_contracts() {
  const auto path = codec_fixtures() / "rgb32-rec2020-linear.exr";
  pano::app::ImageInfo info;
  pano::app::CodecErrorCategory category{};
  std::string error;
  EXPECT(pano::app::inspect_image(path.u8string(), info, category, error));
  EXPECT(info.container == pano::app::ImageContainer::exr);
  EXPECT(info.width == 3U && info.height == 2U && info.channels == 3U);
  EXPECT(info.encoding.sample_type == "float32");
  EXPECT(info.encoding.color_primaries == "rec2020");
  EXPECT(info.encoding.transfer_function == "linear");
  EXPECT(info.encoding.reference_white_nits == 203.0);
  EXPECT((info.exr_data_window == std::array<std::int32_t, 4>{0, 0, 2, 1}));
  EXPECT((info.exr_display_window == std::array<std::int32_t, 4>{0, 0, 2, 1}));
  EXPECT(info.exr_compression == "PIZ");

  std::vector<float> pixels(static_cast<std::size_t>(info.width) * info.height *
                            3U);
  EXPECT(pano::app::decode_image(
      path.u8string(), info, pixels.data(), info.width * 3U * sizeof(float),
      pixels.size() * sizeof(float), {}, category, error));
  const std::vector<float> expected{0.0F,  0.25F, 1.0F, 2.0F,  4.0F,   8.0F,
                                    -0.5F, 0.5F,  1.5F, 16.0F, 0.125F, 3.0F,
                                    0.75F, 1.25F, 2.5F, 6.0F,  7.0F,   9.0F};
  EXPECT(pixels == expected);
  const pano::app::CancellationCheck cancellation{always_cancelled, nullptr};
  EXPECT(!pano::app::decode_image(
      path.u8string(), info, pixels.data(), info.width * 3U * sizeof(float),
      pixels.size() * sizeof(float), cancellation, category, error));
  EXPECT(category == pano::app::CodecErrorCategory::cancelled);

  TemporaryDirectory temporary;
  auto truncated_bytes = read_bytes(path);
  truncated_bytes.resize(32);
  const auto truncated = temporary.path() / "truncated.exr";
  write_bytes(truncated, truncated_bytes);
  EXPECT(
      !pano::app::inspect_image(truncated.u8string(), info, category, error));
  EXPECT(category == pano::app::CodecErrorCategory::malformed ||
         category == pano::app::CodecErrorCategory::io);
}

void test_sdr_writer_contracts() {
  TemporaryDirectory temporary;
  pano::app::CodecErrorCategory category{};
  std::string error;
  pano::app::ImageWriter *writer =
      reinterpret_cast<pano::app::ImageWriter *>(1);
  pano::app::ImageWriterOptions invalid;
  invalid.path = (temporary.path() / "invalid.jpg").u8string();
  invalid.container = pano::app::ImageContainer::jpeg;
  invalid.width = 3;
  invalid.height = 2;
  invalid.jpeg_quality = 0;
  EXPECT(!pano::app::create_image_writer(invalid, &writer, category, error));
  EXPECT(writer == nullptr);
  EXPECT(category == pano::app::CodecErrorCategory::invalid_request);
  EXPECT(!fs::exists(fs::u8path(invalid.path)));

  pano::app::ImageWriterOptions png;
  png.path =
      (temporary.path() / fs::u8path(u8"полосатый output.png")).u8string();
  png.container = pano::app::ImageContainer::png;
  png.width = 3;
  png.height = 2;
#ifdef _WIN32
  const std::array<std::uint8_t, 18> png_pixels{
      0, 64, 255, 10, 20, 30, 255, 128, 1, 3, 2, 1, 17, 34, 51, 250, 251, 252};
  EXPECT(pano::app::create_image_writer(png, &writer, category, error));
  EXPECT(pano::app::write_image_rows(writer, png_pixels.data(), 1, 9, {},
                                     category, error));
  EXPECT(pano::app::write_image_rows(writer, png_pixels.data() + 9, 1, 9, {},
                                     category, error));
  EXPECT(pano::app::finish_image_writer(&writer, {}, category, error));
  EXPECT(writer == nullptr);
  pano::app::ImageInfo png_info;
  EXPECT(pano::app::inspect_image(png.path, png_info, category, error));
  EXPECT(png_info.width == 3U && png_info.height == 2U);
  EXPECT(png_info.encoding.sample_type == "uint8");
  std::array<std::uint8_t, 18> decoded_png{};
  EXPECT(pano::app::decode_image(png.path, png_info, decoded_png.data(), 9,
                                 decoded_png.size(), {}, category, error));
  EXPECT(decoded_png == png_pixels);

  pano::app::ImageWriterOptions jpeg = png;
  jpeg.path = (temporary.path() / "striped.jpg").u8string();
  jpeg.container = pano::app::ImageContainer::jpeg;
  jpeg.width = 8;
  jpeg.height = 4;
  jpeg.jpeg_quality = 95;
  std::array<std::uint8_t, 96> jpeg_pixels{};
  for (unsigned y = 0; y < 4; ++y)
    for (unsigned x = 0; x < 8; ++x) {
      const std::size_t offset = (y * 8U + x) * 3U;
      jpeg_pixels[offset] = static_cast<std::uint8_t>(x * 31U);
      jpeg_pixels[offset + 1] = static_cast<std::uint8_t>(y * 67U);
      jpeg_pixels[offset + 2] =
          static_cast<std::uint8_t>((x * 19U + y * 43U) % 256U);
    }
  EXPECT(pano::app::create_image_writer(jpeg, &writer, category, error));
  EXPECT(pano::app::write_image_rows(writer, jpeg_pixels.data(), 1, 24, {},
                                     category, error));
  EXPECT(pano::app::write_image_rows(writer, jpeg_pixels.data() + 24, 3, 24, {},
                                     category, error));
  EXPECT(pano::app::finish_image_writer(&writer, {}, category, error));
  pano::app::ImageInfo jpeg_info;
  EXPECT(pano::app::inspect_image(jpeg.path, jpeg_info, category, error));
  EXPECT(jpeg_info.jpeg_subsampling == "4:4:4");
  std::array<std::uint8_t, 96> decoded_jpeg{};
  EXPECT(pano::app::decode_image(jpeg.path, jpeg_info, decoded_jpeg.data(), 24,
                                 decoded_jpeg.size(), {}, category, error));
  int maximum_jpeg_error = 0;
  for (std::size_t index = 0; index < jpeg_pixels.size(); ++index)
    maximum_jpeg_error = std::max(
        maximum_jpeg_error, std::abs(static_cast<int>(decoded_jpeg[index]) -
                                     static_cast<int>(jpeg_pixels[index])));
  EXPECT(maximum_jpeg_error <= 15);

  png.path = (temporary.path() / "cancelled.png").u8string();
  EXPECT(pano::app::create_image_writer(png, &writer, category, error));
  const pano::app::CancellationCheck cancellation{always_cancelled, nullptr};
  EXPECT(!pano::app::write_image_rows(writer, png_pixels.data(), 1, 9,
                                      cancellation, category, error));
  EXPECT(category == pano::app::CodecErrorCategory::cancelled);
  pano::app::abort_image_writer(&writer);
  pano::app::abort_image_writer(&writer);
  EXPECT(!fs::exists(fs::u8path(png.path)));

  png.path = (temporary.path() / "incomplete.png").u8string();
  EXPECT(pano::app::create_image_writer(png, &writer, category, error));
  EXPECT(pano::app::write_image_rows(writer, png_pixels.data(), 1, 9, {},
                                     category, error));
  EXPECT(!pano::app::finish_image_writer(&writer, {}, category, error));
  EXPECT(category == pano::app::CodecErrorCategory::invalid_request);
  pano::app::abort_image_writer(&writer);
  EXPECT(!fs::exists(fs::u8path(png.path)));
#else
  EXPECT(!pano::app::create_image_writer(png, &writer, category, error));
  EXPECT(writer == nullptr);
  EXPECT(category == pano::app::CodecErrorCategory::unavailable);
  EXPECT(!fs::exists(fs::u8path(png.path)));
#endif
}

void test_exr_writer_contracts() {
  TemporaryDirectory temporary;
  pano::app::CodecErrorCategory category{};
  std::string error;
  pano::app::ImageWriterOptions options;
  options.path =
      (temporary.path() / fs::u8path(u8"линейный output.exr")).u8string();
  options.container = pano::app::ImageContainer::exr;
  options.width = 3;
  options.height = 33;
  options.sample_type = "float32";
  options.encoding.sample_type = "float32";
  options.encoding.color_primaries = "rec2020";
  options.encoding.transfer_function = "linear";
  options.encoding.reference_white_nits = 203.0;
  std::vector<float> pixels(static_cast<std::size_t>(options.width) *
                            options.height * 3U);
  for (std::size_t index = 0; index < pixels.size(); ++index)
    pixels[index] =
        static_cast<float>(static_cast<int>(index % 29U) - 7) / 3.0F;

  pano::app::ImageWriter *writer = nullptr;
  EXPECT(pano::app::create_image_writer(options, &writer, category, error));
  const std::uint64_t stride = options.width * 3U * sizeof(float);
  EXPECT(pano::app::write_image_rows(writer, pixels.data(), 1, stride, {},
                                     category, error));
  EXPECT(pano::app::write_image_rows(writer, pixels.data() + 9, 31, stride, {},
                                     category, error));
  EXPECT(pano::app::write_image_rows(writer, pixels.data() + 32U * 9U, 1,
                                     stride, {}, category, error));
  EXPECT(pano::app::finish_image_writer(&writer, {}, category, error));
  EXPECT(writer == nullptr);
  pano::app::ImageInfo info;
  EXPECT(pano::app::inspect_image(options.path, info, category, error));
  EXPECT(info.width == 3U && info.height == 33U);
  EXPECT(info.exr_compression == "PIZ");
  std::vector<float> decoded(pixels.size());
  EXPECT(pano::app::decode_image(options.path, info, decoded.data(), stride,
                                 decoded.size() * sizeof(float), {}, category,
                                 error));
  EXPECT(decoded == pixels);

  options.path = (temporary.path() / "nonfinite.exr").u8string();
  options.height = 1;
  EXPECT(pano::app::create_image_writer(options, &writer, category, error));
  std::array<float, 9> nonfinite{};
  nonfinite[4] = std::numeric_limits<float>::infinity();
  EXPECT(!pano::app::write_image_rows(writer, nonfinite.data(), 1, stride, {},
                                      category, error));
  EXPECT(category == pano::app::CodecErrorCategory::invalid_request);
  pano::app::abort_image_writer(&writer);
  EXPECT(!fs::exists(fs::u8path(options.path)));

  pano::app::ImageWriterOptions invalid = options;
  invalid.path = (temporary.path() / "invalid.exr").u8string();
  invalid.sample_type = "uint8";
  EXPECT(!pano::app::create_image_writer(invalid, &writer, category, error));
  EXPECT(writer == nullptr);
  EXPECT(category == pano::app::CodecErrorCategory::invalid_request);
  EXPECT(!fs::exists(fs::u8path(invalid.path)));
}

struct PublicationFaultState {
  std::string boundary;
  unsigned occurrence = 0;
};

bool publication_fault(void *data, const char *boundary) {
  auto &state = *static_cast<PublicationFaultState *>(data);
  if (state.boundary != boundary)
    return false;
  if (state.occurrence != 0U) {
    --state.occurrence;
    return false;
  }
  return true;
}

void test_output_publication_contracts() {
  TemporaryDirectory temporary;
  pano::app::CodecErrorCategory category{};
  std::string error;
  const std::array<fs::path, 3> destinations{
      temporary.path() / "panorama.png",
      temporary.path() / "panorama-coverage.png",
      temporary.path() / "panorama-thumbnail.png"};

  pano::app::OutputStage *first = nullptr;
  pano::app::OutputStage *second = nullptr;
  EXPECT(pano::app::create_output_stage(destinations[0].u8string(), &first,
                                        category, error));
  EXPECT(pano::app::create_output_stage(destinations[0].u8string(), &second,
                                        category, error));
  EXPECT(pano::app::output_stage_path(first) !=
         pano::app::output_stage_path(second));
  write_text(fs::u8path(pano::app::output_stage_path(first)), "new-one");
  write_text(destinations[0], "old-one");
  EXPECT(pano::app::publish_output_stages({first}, {}, category, error));
  EXPECT(read_text(destinations[0]) == "new-one");
  pano::app::abort_output_stage(&first);
  pano::app::abort_output_stage(&first);
  pano::app::abort_output_stage(&second);
  EXPECT(read_text(destinations[0]) == "new-one");

  const auto verify_rollback = [&](const std::string &boundary,
                                   const unsigned occurrence) {
    std::vector<pano::app::OutputStage *> stages;
    for (std::size_t index = 0; index < destinations.size(); ++index) {
      write_text(destinations[index], "old-" + std::to_string(index));
      pano::app::OutputStage *stage = nullptr;
      EXPECT(pano::app::create_output_stage(destinations[index].u8string(),
                                            &stage, category, error));
      write_text(fs::u8path(pano::app::output_stage_path(stage)),
                 "new-" + std::to_string(index));
      stages.push_back(stage);
    }
    PublicationFaultState state{boundary, occurrence};
    const pano::app::PublicationFaultCheck fault{publication_fault, &state};
    EXPECT(!pano::app::publish_output_stages(stages, fault, category, error));
    EXPECT(category == pano::app::CodecErrorCategory::io);
    for (std::size_t index = 0; index < destinations.size(); ++index) {
      EXPECT(read_text(destinations[index]) == "old-" + std::to_string(index));
      pano::app::abort_output_stage(&stages[index]);
    }
  };
  for (const std::string boundary :
       {"before_backup", "after_backup", "before_publish", "after_publish"})
    for (unsigned occurrence = 0; occurrence < 3; ++occurrence)
      verify_rollback(boundary, occurrence);
  verify_rollback("before_cleanup", 0);

  std::vector<pano::app::OutputStage *> stages;
  for (std::size_t index = 0; index < destinations.size(); ++index) {
    pano::app::OutputStage *stage = nullptr;
    EXPECT(pano::app::create_output_stage(destinations[index].u8string(),
                                          &stage, category, error));
    write_text(fs::u8path(pano::app::output_stage_path(stage)),
               "committed-" + std::to_string(index));
    stages.push_back(stage);
  }
  EXPECT(pano::app::publish_output_stages(stages, {}, category, error));
  for (std::size_t index = 0; index < destinations.size(); ++index) {
    EXPECT(read_text(destinations[index]) ==
           "committed-" + std::to_string(index));
    pano::app::abort_output_stage(&stages[index]);
  }

  const auto absent = temporary.path() / "absent.png";
  pano::app::OutputStage *absent_stage = nullptr;
  EXPECT(pano::app::create_output_stage(absent.u8string(), &absent_stage,
                                        category, error));
  write_text(fs::u8path(pano::app::output_stage_path(absent_stage)), "new");
  PublicationFaultState after_publish{"after_publish", 0};
  EXPECT(!pano::app::publish_output_stages(
      {absent_stage}, {publication_fault, &after_publish}, category, error));
  EXPECT(!fs::exists(absent));
  pano::app::abort_output_stage(&absent_stage);

  const auto stale_destination = temporary.path() / "stale.png";
  pano::app::OutputStage *stale = nullptr;
  EXPECT(pano::app::create_output_stage(stale_destination.u8string(), &stale,
                                        category, error));
  const fs::path stale_path = fs::u8path(pano::app::output_stage_path(stale));
  const fs::path marker = fs::u8path(stale_path.u8string() + ".owner");
  write_text(stale_path, "partial");
  fs::last_write_time(marker, fs::file_time_type::clock::now() -
                                  std::chrono::hours(48));
  const auto unrelated = temporary.path() / ".stale.png.user.partial";
  write_text(unrelated, "keep");
  const auto false_marker = temporary.path() / ".stale.png.user.partial.owner";
  write_text(false_marker, "not owned");
  fs::last_write_time(false_marker, fs::file_time_type::clock::now() -
                                        std::chrono::hours(48));
  EXPECT(pano::app::recover_stale_output_stages({stale_destination.u8string()},
                                                category, error));
  EXPECT(!fs::exists(stale_path));
  EXPECT(!fs::exists(marker));
  EXPECT(fs::exists(unrelated));
  EXPECT(fs::exists(false_marker));
  pano::app::abort_output_stage(&stale);
}

void test_cpu_render_planning() {
  pano::app::CpuRenderPlanRequest request;
  request.source_width = 64;
  request.source_height = 64;
  request.output_width = 64;
  request.output_height = 32;
  request.memory_budget_bytes = 201516032;
  request.worker_count = 1;
  pano::app::CpuRenderPlan plan;
  std::string error;
  EXPECT(pano::app::plan_cpu_render(request, plan, error));
  EXPECT(plan.source_working_set_bytes == 147456U);
  EXPECT(plan.available_strip_bytes == 41984U);
  EXPECT(plan.bytes_per_worker_row == 10496U);
  EXPECT(plan.worker_count == 1U);
  EXPECT(plan.strip_height == 4U);
  EXPECT(plan.scratch_bytes == 32768U);
  request.worker_count = 2;
  EXPECT(pano::app::plan_cpu_render(request, plan, error));
  EXPECT(plan.worker_count == 2U && plan.strip_height == 2U);

  request.source_width = 3840;
  request.source_height = 2160;
  request.output_width = 21274;
  request.output_height = std::numeric_limits<unsigned>::max() / 2U;
  request.memory_budget_bytes = 1024ULL * 1024ULL * 1024ULL;
  request.worker_count = 1;
  EXPECT(pano::app::plan_cpu_render(request, plan, error));
  EXPECT(plan.source_working_set_bytes == 298598400U);
  EXPECT(plan.available_strip_bytes == 573816832U);
  EXPECT(plan.bytes_per_worker_row == 3488936U);
  EXPECT(plan.strip_height == 164U);
  request.worker_count = 0;
  EXPECT(pano::app::plan_cpu_render(request, plan, error));
  EXPECT(plan.worker_count >= 1U && plan.worker_count <= 8U);

  request.memory_budget_bytes = 8192ULL * 1024ULL * 1024ULL + 1U;
  EXPECT(!pano::app::plan_cpu_render(request, plan, error));
  EXPECT(error.find("8192 MiB") != std::string::npos);
  request.memory_budget_bytes = 192ULL * 1024ULL * 1024ULL;
  EXPECT(!pano::app::plan_cpu_render(request, plan, error));
  EXPECT(error.find("too small") != std::string::npos);
  request.memory_budget_bytes = 1024ULL * 1024ULL * 1024ULL;
  request.output_width = 0;
  EXPECT(!pano::app::plan_cpu_render(request, plan, error));
  request.output_width = std::numeric_limits<unsigned>::max();
  request.output_height = std::numeric_limits<unsigned>::max();
  request.source_width = std::numeric_limits<unsigned>::max();
  request.source_height = std::numeric_limits<unsigned>::max();
  request.memory_budget_bytes = 8192ULL * 1024ULL * 1024ULL;
  EXPECT(!pano::app::plan_cpu_render(request, plan, error));
  EXPECT(error.find("overflows") != std::string::npos);
}

void test_cpu_render_storage() {
  TemporaryDirectory temporary;
  pano::app::CpuRenderPlanRequest request;
  request.source_width = 64;
  request.source_height = 64;
  request.output_width = 64;
  request.output_height = 32;
  request.memory_budget_bytes = 201516032;
  request.worker_count = 2;
  pano::app::CpuRenderPlan plan;
  std::string error;
  EXPECT(pano::app::plan_cpu_render(request, plan, error));
  pano::app::CpuRenderStorageOptions options;
  options.directory = temporary.path().u8string();
  options.plan = plan;
  options.output_width = request.output_width;
  options.output_height = request.output_height;
  pano::app::CpuRenderStorage *storage = nullptr;
  EXPECT(pano::app::create_cpu_render_storage(options, &storage, error));
  pano::app::CpuRenderStorageDiagnostics diagnostics;
  EXPECT(pano::app::query_cpu_render_storage(storage, diagnostics, error));
  EXPECT(diagnostics.mapped_scratch_bytes == 32768U);
  EXPECT(diagnostics.worker_strip_bytes == 41984U);
  EXPECT(diagnostics.live_bytes == 74752U);
  EXPECT(diagnostics.peak_live_bytes == diagnostics.live_bytes);
  float *color = pano::app::cpu_render_color_scratch(storage);
  float *weight = pano::app::cpu_render_weight_scratch(storage);
  auto *worker_zero = static_cast<std::uint8_t *>(
      pano::app::cpu_render_worker_strip(storage, 0));
  auto *worker_one = static_cast<std::uint8_t *>(
      pano::app::cpu_render_worker_strip(storage, 1));
  EXPECT(color != nullptr && weight != nullptr && worker_zero != nullptr &&
         worker_one != nullptr);
  EXPECT(reinterpret_cast<std::uint8_t *>(weight) -
             reinterpret_cast<std::uint8_t *>(color) ==
         64 * 32 * 3 * static_cast<std::ptrdiff_t>(sizeof(float)));
  EXPECT(worker_one - worker_zero == 20992);
  EXPECT(pano::app::cpu_render_worker_strip(storage, 2) == nullptr);
  color[0] = 1.25F;
  weight[0] = 2.5F;
  worker_zero[0] = 17;
  EXPECT(color[0] == 1.25F && weight[0] == 2.5F && worker_zero[0] == 17U);
  pano::app::destroy_cpu_render_storage(&storage);
  pano::app::destroy_cpu_render_storage(&storage);
  EXPECT(storage == nullptr);
  EXPECT(fs::is_empty(temporary.path()));

  for (const std::string boundary : {"after_directory", "after_file",
                                     "after_mapping", "before_worker_strips"}) {
    PublicationFaultState state{boundary, 0};
    options.fault = {publication_fault, &state};
    EXPECT(!pano::app::create_cpu_render_storage(options, &storage, error));
    EXPECT(storage == nullptr);
    EXPECT(error.find(boundary) != std::string::npos);
    EXPECT(fs::is_empty(temporary.path()));
  }
  options.fault = {};
  ++options.plan.scratch_bytes;
  EXPECT(!pano::app::create_cpu_render_storage(options, &storage, error));
  EXPECT(error.find("invalid") != std::string::npos);
  EXPECT(fs::is_empty(temporary.path()));
}

struct CpuTaskState {
  std::array<std::atomic<unsigned>, 32> visits{};
  std::atomic<bool> cancel{false};
  bool fail = false;
};

bool cpu_task(void *data, const unsigned worker, const unsigned task) {
  auto &state = *static_cast<CpuTaskState *>(data);
  if (worker >= 4U || task >= state.visits.size())
    return false;
  state.visits[task].fetch_add(1, std::memory_order_relaxed);
  if (task == 3U)
    state.cancel.store(true, std::memory_order_relaxed);
  return !state.fail || task != 2U;
}

bool cpu_cancelled(void *data) {
  return static_cast<CpuTaskState *>(data)->cancel.load(
      std::memory_order_relaxed);
}

void test_cpu_worker_pool() {
  std::string error;
  pano::app::CpuWorkerPool *pool = nullptr;
  EXPECT(!pano::app::create_cpu_worker_pool(0, &pool, error));
  EXPECT(!pano::app::create_cpu_worker_pool(9, &pool, error));
  EXPECT(pano::app::create_cpu_worker_pool(4, &pool, error));
  CpuTaskState state;
  EXPECT(pano::app::run_cpu_tasks(pool, 32, cpu_task, &state, {}, error));
  for (const auto &visit : state.visits)
    EXPECT(visit.load(std::memory_order_relaxed) == 1U);
  for (auto &visit : state.visits)
    visit.store(0, std::memory_order_relaxed);
  state.fail = true;
  EXPECT(!pano::app::run_cpu_tasks(pool, 32, cpu_task, &state, {}, error));
  EXPECT(error.find("callback") != std::string::npos);
  state.fail = false;
  state.cancel.store(true, std::memory_order_relaxed);
  EXPECT(!pano::app::run_cpu_tasks(pool, 32, cpu_task, &state,
                                   {cpu_cancelled, &state}, error));
  EXPECT(error.find("cancelled") != std::string::npos);
  state.cancel.store(false, std::memory_order_relaxed);
  for (auto &visit : state.visits)
    visit.store(0, std::memory_order_relaxed);
  EXPECT(!pano::app::run_cpu_tasks(pool, 32, cpu_task, &state,
                                   {cpu_cancelled, &state}, error));
  EXPECT(error.find("cancelled") != std::string::npos);
  pano::app::destroy_cpu_worker_pool(&pool);
  pano::app::destroy_cpu_worker_pool(&pool);
  EXPECT(pool == nullptr);
}

void test_cpu_one_frame_hard_composition() {
  std::string error;
  pano::app::CpuRayRequest rays;
  rays.output_width = 4;
  rays.output_height = 2;
  rays.row_count = 2;
  std::array<float, 24> world{};
  EXPECT(pano::app::generate_cpu_world_rays(rays, world.data(), sizeof(world),
                                            error));
  constexpr float pi = 3.14159265358979323846F;
  for (unsigned row = 0; row < 2; ++row) {
    for (unsigned column = 0; column < 4; ++column) {
      const float longitude = ((column + 0.5F) / 4.0F - 0.5F) * 2.0F * pi;
      const float latitude = (0.5F - (row + 0.5F) / 2.0F) * pi;
      const auto offset = (row * 4U + column) * 3U;
      EXPECT(std::fabs(world[offset] -
                       std::cos(latitude) * std::sin(longitude)) < 1.0e-6F);
      EXPECT(std::fabs(world[offset + 1U] - std::sin(latitude)) < 1.0e-6F);
      EXPECT(std::fabs(world[offset + 2U] -
                       std::cos(latitude) * std::cos(longitude)) < 1.0e-6F);
    }
  }
  rays.projection = pano::app::CpuOutputProjection::rectilinear;
  rays.output_width = 3;
  rays.output_height = 3;
  rays.row_count = 3;
  rays.rectilinear_vertical_fov_degrees = 90.0F;
  std::array<float, 27> thumbnail_world{};
  EXPECT(pano::app::generate_cpu_world_rays(rays, thumbnail_world.data(),
                                            sizeof(thumbnail_world), error));
  EXPECT(std::fabs(thumbnail_world[12]) < 1.0e-7F);
  EXPECT(std::fabs(thumbnail_world[13]) < 1.0e-7F);
  EXPECT(std::fabs(thumbnail_world[14] - 1.0F) < 1.0e-7F);
  const float corner_inverse_length = 1.0F / std::sqrt(17.0F / 9.0F);
  EXPECT(std::fabs(thumbnail_world[0] + (2.0F / 3.0F) * corner_inverse_length) <
         1.0e-6F);
  EXPECT(std::fabs(thumbnail_world[1] - (2.0F / 3.0F) * corner_inverse_length) <
         1.0e-6F);
  EXPECT(std::fabs(thumbnail_world[2] - corner_inverse_length) < 1.0e-6F);
  rays.row_start = 1;
  rays.row_count = 1;
  std::array<float, 9> middle_row{};
  EXPECT(pano::app::generate_cpu_world_rays(rays, middle_row.data(),
                                            sizeof(middle_row), error));
  EXPECT(std::equal(middle_row.begin(), middle_row.end(),
                    thumbnail_world.begin() + 9));
  rays.row_start = 3;
  EXPECT(!pano::app::generate_cpu_world_rays(rays, middle_row.data(),
                                             sizeof(middle_row), error));

  pano::app::CpuProjectionRequest projection;
  projection.pixel_count = 4;
  projection.source_width = 4;
  projection.source_height = 4;
  projection.horizontal_fov_degrees = 90.0F;
  projection.vertical_fov_degrees = 90.0F;
  projection.world_to_camera = {1.0F, 0.0F, 0.0F, 0.0F, 1.0F,
                                0.0F, 0.0F, 0.0F, 1.0F};
  std::array<float, 12> projection_rays{
      0.0F, 0.0F, 1.0F, -1.0F, 0.0F,
      1.0F, 0.0F, 0.0F, -1.0F, std::numeric_limits<float>::quiet_NaN(),
      0.0F, 1.0F};
  std::array<float, 8> coordinates{};
  std::array<std::uint8_t, 4> validity{};
  std::array<float, 4> edge_distances{};
  EXPECT(pano::app::project_cpu_world_rays(
      projection, projection_rays.data(), sizeof(projection_rays),
      coordinates.data(), sizeof(coordinates), validity.data(),
      sizeof(validity), edge_distances.data(), sizeof(edge_distances), error));
  EXPECT((validity == std::array<std::uint8_t, 4>{1, 1, 0, 0}));
  EXPECT(std::fabs(coordinates[0] - 1.5F) < 1.0e-6F &&
         std::fabs(coordinates[1] - 1.5F) < 1.0e-6F);
  EXPECT(coordinates[2] == 0.0F && coordinates[3] == 1.5F);
  EXPECT(std::fabs(edge_distances[0] - 1.5F) < 1.0e-6F);
  projection.world_to_camera[0] = std::numeric_limits<float>::infinity();
  EXPECT(!pano::app::project_cpu_world_rays(
      projection, projection_rays.data(), sizeof(projection_rays),
      coordinates.data(), sizeof(coordinates), validity.data(),
      sizeof(validity), edge_distances.data(), sizeof(edge_distances), error));

  const std::array<std::uint8_t, 12> source8{0, 0,   0, 255, 0,   0,
                                             0, 255, 0, 255, 255, 255};
  pano::app::CpuSampleRequest sample;
  sample.source_width = 2;
  sample.source_height = 2;
  sample.source_row_stride_bytes = 6;
  sample.pixel_count = 3;
  const std::array<float, 6> sample_coordinates{0.5F, 0.5F, 0.0F,
                                                0.0F, 1.0F, 1.0F};
  const std::array<std::uint8_t, 3> sample_validity{1, 1, 0};
  std::array<float, 9> sampled{};
  EXPECT(pano::app::sample_cpu_bilinear(
      sample, source8.data(), sizeof(source8), sample_coordinates.data(),
      sizeof(sample_coordinates), sample_validity.data(),
      sizeof(sample_validity), sampled.data(), sizeof(sampled), error));
  EXPECT(std::fabs(sampled[0] - 0.5F) < 1.0e-6F);
  EXPECT(std::fabs(sampled[1] - 0.5F) < 1.0e-6F);
  EXPECT(std::fabs(sampled[2] - 0.25F) < 1.0e-6F);
  EXPECT(sampled[3] == 0.0F && sampled[4] == 0.0F && sampled[5] == 0.0F);
  EXPECT(sampled[6] == 1.0F && sampled[7] == 1.0F && sampled[8] == 1.0F);

  const std::array<std::uint16_t, 12> source16{
      0, 0, 0, 65535, 65535, 65535, 32768, 32768, 32768, 16384, 16384, 16384};
  sample.sample_type = pano::app::CpuSampleType::uint16;
  sample.transfer_function = pano::app::CpuTransferFunction::pq;
  sample.source_row_stride_bytes = 12;
  EXPECT(pano::app::sample_cpu_bilinear(
      sample, source16.data(), sizeof(source16), sample_coordinates.data(),
      sizeof(sample_coordinates), sample_validity.data(),
      sizeof(sample_validity), sampled.data(), sizeof(sampled), error));
  const float pq_powered = std::pow((16384.0F / 65535.0F), 32.0F / 2523.0F);
  const float expected_pq =
      std::pow(std::max(pq_powered - 3424.0F / 4096.0F, 0.0F) /
                   std::max(2413.0F / 128.0F - 2392.0F / 128.0F * pq_powered,
                            std::numeric_limits<float>::min()),
               16384.0F / 2610.0F);
  EXPECT(sampled[3] == 0.0F && std::fabs(sampled[6] - expected_pq) < 1.0e-7F);

  const std::array<float, 12> source_float{-2.0F, 2.0F, 4.0F, 0.0F,
                                           4.0F,  8.0F, 2.0F, 6.0F,
                                           12.0F, 4.0F, 8.0F, 16.0F};
  sample.sample_type = pano::app::CpuSampleType::float32;
  sample.transfer_function = pano::app::CpuTransferFunction::linear;
  sample.source_row_stride_bytes = 24;
  EXPECT(pano::app::sample_cpu_bilinear(
      sample, source_float.data(), sizeof(source_float),
      sample_coordinates.data(), sizeof(sample_coordinates),
      sample_validity.data(), sizeof(sample_validity), sampled.data(),
      sizeof(sampled), error));
  EXPECT(sampled[0] == 1.0F && sampled[1] == 5.0F && sampled[2] == 10.0F);
  EXPECT(sampled[3] == -2.0F && sampled[6] == 4.0F && sampled[8] == 16.0F);

  const std::array<float, 9> candidates{1.0F, 2.0F, 3.0F, 4.0F, 5.0F,
                                        6.0F, 7.0F, 8.0F, 9.0F};
  const std::array<std::uint8_t, 3> candidate_validity{1, 1, 0};
  const std::array<float, 3> candidate_edges{0.0F, 2.0F, 3.0F};
  std::array<float, 9> color{10.0F, 11.0F, 12.0F, 13.0F, 14.0F,
                             15.0F, 16.0F, 17.0F, 18.0F};
  std::array<float, 3> weight{0.0F, 2.0F, 0.0F};
  std::array<std::uint8_t, 3> coverage{};
  EXPECT(pano::app::select_cpu_hard(
      3, candidates.data(), candidate_validity.data(), candidate_edges.data(),
      color.data(), weight.data(), coverage.data(), error));
  EXPECT(color[0] == 1.0F && color[1] == 2.0F && color[2] == 3.0F);
  EXPECT(color[3] == 13.0F && color[6] == 16.0F);
  EXPECT(std::fabs(weight[0] - 1.0e-6F) < 1.0e-9F && weight[1] == 2.0F &&
         weight[2] == 0.0F);
  EXPECT((coverage == std::array<std::uint8_t, 3>{1, 1, 0}));
}

struct CpuFrameState {
  std::vector<unsigned> events;
  bool live = false;
  unsigned fail_compose = std::numeric_limits<unsigned>::max();
};

bool acquire_cpu_frame(void *data, const unsigned frame) {
  auto &state = *static_cast<CpuFrameState *>(data);
  if (state.live)
    return false;
  state.live = true;
  state.events.push_back(frame * 3U);
  return true;
}

bool compose_cpu_frame(void *data, const unsigned frame) {
  auto &state = *static_cast<CpuFrameState *>(data);
  if (!state.live)
    return false;
  state.events.push_back(frame * 3U + 1U);
  return frame != state.fail_compose;
}

void release_cpu_frame(void *data, const unsigned frame) {
  auto &state = *static_cast<CpuFrameState *>(data);
  state.events.push_back(frame * 3U + 2U);
  state.live = false;
}

void test_cpu_multiframe_feather_and_gains() {
  std::string error;
  CpuFrameState frame_state;
  const pano::app::CpuFrameCallbacks callbacks{
      acquire_cpu_frame, compose_cpu_frame, release_cpu_frame, &frame_state};
  EXPECT(pano::app::iterate_cpu_frames(3, callbacks, {}, error));
  EXPECT(
      (frame_state.events == std::vector<unsigned>{0, 1, 2, 3, 4, 5, 6, 7, 8}));
  EXPECT(!frame_state.live);
  frame_state.events.clear();
  frame_state.fail_compose = 1;
  EXPECT(!pano::app::iterate_cpu_frames(3, callbacks, {}, error));
  EXPECT((frame_state.events == std::vector<unsigned>{0, 1, 2, 3, 4, 5}));
  EXPECT(!frame_state.live);

  const std::array<float, 6> first_rgb{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
  const std::array<float, 6> tied_rgb{7.0F, 8.0F, 9.0F, 10.0F, 11.0F, 12.0F};
  const std::array<std::uint8_t, 2> valid{1, 1};
  const std::array<float, 2> edges{1.0F, 2.0F};
  std::array<float, 6> hard_color{};
  std::array<float, 2> hard_weight{};
  std::array<std::uint8_t, 2> hard_coverage{};
  EXPECT(pano::app::select_cpu_hard(
      2, first_rgb.data(), valid.data(), edges.data(), hard_color.data(),
      hard_weight.data(), hard_coverage.data(), error));
  EXPECT(pano::app::select_cpu_hard(
      2, tied_rgb.data(), valid.data(), edges.data(), hard_color.data(),
      hard_weight.data(), hard_coverage.data(), error));
  EXPECT(hard_color == first_rgb);

  std::array<float, 6> feather_color{};
  std::array<float, 2> feather_weight{};
  const std::array<float, 2> feather_edges{8.0F, 0.0F};
  const std::array<std::uint8_t, 2> second_valid{1, 0};
  EXPECT(pano::app::accumulate_cpu_feather(
      2, 100, 100, first_rgb.data(), valid.data(), feather_edges.data(),
      feather_color.data(), feather_weight.data(), error));
  EXPECT(pano::app::accumulate_cpu_feather(
      2, 100, 100, tied_rgb.data(), second_valid.data(), feather_edges.data(),
      feather_color.data(), feather_weight.data(), error));
  EXPECT(feather_weight[0] == 2.0F &&
         std::fabs(feather_weight[1] - 1.0e-6F) < 1.0e-9F);
  std::array<std::uint8_t, 2> feather_coverage{};
  EXPECT(pano::app::normalize_cpu_feather(2, feather_color.data(),
                                          feather_weight.data(),
                                          feather_coverage.data(), error));
  EXPECT(feather_color[0] == 4.0F && feather_color[1] == 5.0F &&
         feather_color[2] == 6.0F);
  EXPECT(feather_color[3] == 4.0F && feather_color[5] == 6.0F);
  feather_weight[1] = 0.0F;
  EXPECT(pano::app::mark_cpu_incomplete(2, feather_color.data(),
                                        feather_weight.data(), error));
  EXPECT(feather_color[3] == 1.0F && feather_color[4] == 0.0F &&
         feather_color[5] == 1.0F);

  std::array<float, 12> gained{1.0F, 2.0F, 3.0F, 1.0F, 2.0F, 3.0F,
                               1.0F, 2.0F, 3.0F, 1.0F, 2.0F, 3.0F};
  EXPECT(pano::app::apply_cpu_global_gain(4, 2.0F, gained.data(), error));
  EXPECT(gained[0] == 2.0F && gained[11] == 6.0F);
  pano::app::CpuLocalGainRequest local;
  local.output_width = 2;
  local.output_height = 2;
  local.row_count = 2;
  local.field_width = 2;
  local.field_height = 2;
  const std::array<float, 4> local_log_gain{0.0F, std::log(2.0F),
                                            std::log(4.0F), std::log(8.0F)};
  EXPECT(pano::app::apply_cpu_local_gain(local, local_log_gain.data(),
                                         gained.data(), error));
  EXPECT(std::fabs(gained[0] - 2.0F) < 1.0e-6F);
  EXPECT(std::fabs(gained[3] - 4.0F) < 1.0e-5F);
  EXPECT(std::fabs(gained[6] - 8.0F) < 1.0e-5F);
  EXPECT(std::fabs(gained[9] - 16.0F) < 1.0e-5F);
}

void test_cpu_exposure_proxies_and_pairs() {
  std::string error;
  const std::array<float, 18> source{0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F,
                                     2.0F, 2.0F, 2.0F, 3.0F, 3.0F, 3.0F,
                                     4.0F, 4.0F, 4.0F, 5.0F, 5.0F, 5.0F};
  pano::app::CpuExposureProxyRequest proxy_request;
  proxy_request.source.sample_type = pano::app::CpuSampleType::float32;
  proxy_request.source.transfer_function =
      pano::app::CpuTransferFunction::linear;
  proxy_request.source.source_width = 3;
  proxy_request.source.source_height = 2;
  proxy_request.source.source_row_stride_bytes = 9 * sizeof(float);
  proxy_request.proxy_width = 2;
  proxy_request.proxy_height = 1;
  std::array<float, 6> proxy{};
  EXPECT(pano::app::build_cpu_exposure_proxy(proxy_request, source.data(),
                                             sizeof(source), proxy.data(),
                                             sizeof(proxy), error));
  EXPECT(std::fabs(proxy[0] - 1.8333333F) < 1.0e-6F);
  EXPECT(std::fabs(proxy[3] - 3.1666667F) < 1.0e-6F);
  const std::array<std::uint8_t, 3> white8{255, 255, 255};
  proxy_request.source.sample_type = pano::app::CpuSampleType::uint8;
  proxy_request.source.transfer_function = pano::app::CpuTransferFunction::srgb;
  proxy_request.source.source_width = 1;
  proxy_request.source.source_height = 1;
  proxy_request.source.source_row_stride_bytes = 3;
  proxy_request.proxy_width = 1;
  std::array<float, 3> endpoint{};
  EXPECT(pano::app::build_cpu_exposure_proxy(proxy_request, white8.data(),
                                             sizeof(white8), endpoint.data(),
                                             sizeof(endpoint), error));
  EXPECT((endpoint == std::array<float, 3>{1.0F, 1.0F, 1.0F}));
  const std::array<std::uint16_t, 3> white16{65535, 65535, 65535};
  proxy_request.source.sample_type = pano::app::CpuSampleType::uint16;
  proxy_request.source.transfer_function = pano::app::CpuTransferFunction::pq;
  proxy_request.source.source_row_stride_bytes = 6;
  EXPECT(pano::app::build_cpu_exposure_proxy(proxy_request, white16.data(),
                                             sizeof(white16), endpoint.data(),
                                             sizeof(endpoint), error));
  EXPECT(std::fabs(endpoint[0] - 1.0F) < 1.0e-5F);

  std::array<pano::app::CpuFramePair, 3> pairs{};
  unsigned pair_count = 0;
  EXPECT(pano::app::enumerate_cpu_exposure_pairs(
      3, pairs.data(), static_cast<unsigned>(pairs.size()), pair_count, error));
  EXPECT(pair_count == 3U && pairs[0].left == 0U && pairs[0].right == 1U &&
         pairs[1].left == 0U && pairs[1].right == 2U && pairs[2].left == 1U &&
         pairs[2].right == 2U);
  EXPECT(pano::app::enumerate_cpu_exposure_pairs(1, nullptr, 0, pair_count,
                                                 error));
  EXPECT(pair_count == 0U);

  pano::app::CpuExposurePairRequest pair_request;
  pair_request.sample_width = 1;
  pair_request.sample_height = 1;
  pair_request.proxy_width = 3;
  pair_request.proxy_height = 3;
  pair_request.horizontal_fov_degrees = 90.0F;
  pair_request.vertical_fov_degrees = 90.0F;
  pair_request.left_world_to_camera = {1.0F, 0.0F, 0.0F, 0.0F, 1.0F,
                                       0.0F, 0.0F, 0.0F, 1.0F};
  pair_request.right_world_to_camera = pair_request.left_world_to_camera;
  std::array<float, 4> paired_coordinates{};
  std::array<std::uint8_t, 1> overlap{};
  EXPECT(pano::app::project_cpu_exposure_pair(
      pair_request, paired_coordinates.data(), overlap.data(), error));
  EXPECT(overlap[0] == 1U && paired_coordinates[0] == 1.0F &&
         paired_coordinates[1] == 1.0F && paired_coordinates[2] == 1.0F &&
         paired_coordinates[3] == 1.0F);
  pair_request.right_world_to_camera = {-1.0F, 0.0F, 0.0F, 0.0F, 1.0F,
                                        0.0F,  0.0F, 0.0F, -1.0F};
  EXPECT(pano::app::project_cpu_exposure_pair(
      pair_request, paired_coordinates.data(), overlap.data(), error));
  EXPECT(overlap[0] == 0U);

  pair_request.right_world_to_camera = pair_request.left_world_to_camera;
  paired_coordinates = {0.5F, 0.5F, 1.5F, 1.5F};
  std::array<float, 27> left_proxy{};
  std::array<float, 27> right_proxy{};
  for (unsigned pixel = 0; pixel < 9U; ++pixel) {
    for (unsigned channel = 0; channel < 3U; ++channel) {
      left_proxy[pixel * 3U + channel] = static_cast<float>(pixel);
      right_proxy[pixel * 3U + channel] = static_cast<float>(pixel + 10U);
    }
  }
  std::array<float, 6> samples{};
  EXPECT(pano::app::sample_cpu_exposure_pair(
      pair_request, left_proxy.data(), right_proxy.data(),
      paired_coordinates.data(), samples.data(), error));
  EXPECT(samples[0] == 2.0F && samples[2] == 2.0F && samples[3] == 16.0F &&
         samples[5] == 16.0F);
}

void test_cpu_exposure_classification_and_reduction() {
  std::string error;
  std::array<float, 36> samples{};
  samples.fill(0.5F);
  samples[6] = samples[7] = samples[8] = 1.0e-6F;
  samples[12] = 1.0F;
  samples[18] = std::numeric_limits<float>::quiet_NaN();
  samples[30] = samples[31] = samples[32] = 2.0F;
  std::array<std::uint8_t, 6> overlap{1, 1, 1, 1, 0, 1};
  std::array<float, 12> luminance{};
  std::array<std::uint8_t, 6> accepted{};
  EXPECT(pano::app::classify_cpu_exposure_samples(
      6, pano::app::CpuTransferFunction::srgb, samples.data(), overlap.data(),
      luminance.data(), accepted.data(), error));
  EXPECT((accepted == std::array<std::uint8_t, 6>{1, 0, 0, 0, 0, 0}));
  EXPECT(pano::app::classify_cpu_exposure_samples(
      6, pano::app::CpuTransferFunction::linear, samples.data(), overlap.data(),
      luminance.data(), accepted.data(), error));
  EXPECT((accepted == std::array<std::uint8_t, 6>{1, 0, 1, 0, 0, 1}));

  std::array<float, 18> gradient_luminance{};
  gradient_luminance.fill(0.5F);
  std::array<float, 18> gradients{};
  EXPECT(pano::app::calculate_cpu_exposure_gradients(
      3, 3, gradient_luminance.data(), gradients.data(), error));
  EXPECT(std::all_of(gradients.begin(), gradients.end(),
                     [](const float value) { return value == 0.0F; }));
  for (unsigned index = 0; index < 9U; ++index) {
    const unsigned x = index % 3U;
    gradient_luminance[index * 2U] = x == 0U ? 0.1F : 0.8F;
    gradient_luminance[index * 2U + 1U] = 0.2F + 0.05F * index;
  }
  EXPECT(pano::app::calculate_cpu_exposure_gradients(
      3, 3, gradient_luminance.data(), gradients.data(), error));
  EXPECT(gradients[0] > 0.0F && gradients[1] > 0.0F);
  std::array<std::uint8_t, 9> gradient_accepted{};
  gradient_accepted.fill(1);
  gradients[4] = std::numeric_limits<float>::quiet_NaN();
  std::array<float, 2> limits{};
  const auto expected_p90 = [&](const unsigned channel) {
    std::vector<float> finite;
    for (unsigned index = 0; index < 9U; ++index)
      if (std::isfinite(gradients[index * 2U + channel]))
        finite.push_back(gradients[index * 2U + channel]);
    std::sort(finite.begin(), finite.end());
    const double position = static_cast<double>(finite.size() - 1U) * 0.9;
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    const float fraction = static_cast<float>(position - lower);
    return finite[lower] * (1.0F - fraction) + finite[upper] * fraction;
  };
  const std::array<float, 2> expected_limits{expected_p90(0), expected_p90(1)};
  EXPECT(pano::app::filter_cpu_exposure_gradients(
      9, gradients.data(), gradient_accepted.data(), limits, error));
  EXPECT(std::fabs(limits[0] - expected_limits[0]) < 1.0e-6F &&
         std::fabs(limits[1] - expected_limits[1]) < 1.0e-6F);
  EXPECT(gradient_accepted[2] == 0U);

  std::array<float, 64> reduction_luminance{};
  std::array<std::uint8_t, 32> reduction_accepted{};
  reduction_accepted.fill(1);
  for (unsigned index = 0; index < 32U; ++index) {
    reduction_luminance[index * 2U] = 2.0F;
    reduction_luminance[index * 2U + 1U] = 1.0F;
  }
  pano::app::CpuExposurePairReduction reduction;
  EXPECT(pano::app::reduce_cpu_exposure_pair(32, reduction_luminance.data(),
                                             reduction_accepted.data(),
                                             reduction, error));
  EXPECT(reduction.rejection == pano::app::CpuExposurePairRejection::accepted);
  EXPECT(reduction.valid_count == 32U && reduction.inlier_count == 32U);
  EXPECT(std::fabs(reduction.difference - std::log(2.0F)) < 1.0e-6F);
  EXPECT(reduction.mad == 0.0F &&
         std::fabs(reduction.weight - std::sqrt(32.0F)) < 1.0e-6F);
  for (unsigned index = 23U; index < 32U; ++index)
    reduction_accepted[index] = 0;
  EXPECT(pano::app::reduce_cpu_exposure_pair(32, reduction_luminance.data(),
                                             reduction_accepted.data(),
                                             reduction, error));
  EXPECT(reduction.rejection ==
         pano::app::CpuExposurePairRejection::insufficient_valid);
  reduction_accepted.fill(1);
  for (unsigned index = 0; index < 32U; ++index)
    reduction_luminance[index * 2U] = std::exp(static_cast<float>(index));
  EXPECT(pano::app::reduce_cpu_exposure_pair(32, reduction_luminance.data(),
                                             reduction_accepted.data(),
                                             reduction, error));
  EXPECT(reduction.rejection ==
         pano::app::CpuExposurePairRejection::excessive_mad);
  EXPECT(reduction.weight == 0.0F);
}

void test_cpu_exposure_graph_report_and_cache() {
  std::string error;
  std::array<pano::app::CpuExposurePairMeasurement, 3> measurements{};
  measurements[0].pair = {0, 1};
  measurements[0].reduction.rejection =
      pano::app::CpuExposurePairRejection::accepted;
  measurements[0].reduction.difference = 0.25F;
  measurements[0].reduction.weight = 2.0F;
  measurements[0].geometric_count = 24;
  measurements[1].pair = {0, 2};
  measurements[1].reduction.rejection =
      pano::app::CpuExposurePairRejection::insufficient_valid;
  measurements[1].geometric_count = 24;
  measurements[2].pair = {1, 2};
  measurements[2].reduction.rejection =
      pano::app::CpuExposurePairRejection::insufficient_valid;
  std::vector<pano::app::CpuExposureEquation> equations;
  EXPECT(pano::app::build_cpu_exposure_solve_graph(
      3, measurements.data(), static_cast<unsigned>(measurements.size()),
      equations, error));
  EXPECT(equations.size() == 2U);
  EXPECT(equations[0].left == 0U && equations[0].right == 1U &&
         equations[0].difference == 0.25 && equations[0].weight == 2.0);
  EXPECT(equations[1].left == 0U && equations[1].right == 2U &&
         equations[1].difference == 0.0 && equations[1].weight == 1.0);
  pano::app::CpuExposureSolveResult solved;
  EXPECT(pano::app::solve_cpu_exposure_graph(3, equations, solved, error));
  EXPECT(solved.anchor_frame == 0U && solved.edge_count == 2U);
  EXPECT(std::fabs(solved.log_gains[0]) < 1.0e-7F);
  EXPECT(std::fabs(solved.log_gains[1] - 0.25F) < 1.0e-6F);
  EXPECT(std::fabs(solved.log_gains[2]) < 1.0e-7F);

  equations = {{0, 1, 0.2, 1.0}, {0, 2, 0.0, 1.0}, {1, 2, 0.4, 3.0}};
  EXPECT(pano::app::solve_cpu_exposure_graph(3, equations, solved, error));
  EXPECT(solved.anchor_frame == 0U && solved.edge_count == 3U);
  EXPECT(std::fabs(solved.log_gains[0]) < 1.0e-7F);
  EXPECT(std::fabs(solved.log_gains[1] + 0.057142857F) < 1.0e-6F);
  EXPECT(std::fabs(solved.log_gains[2] - 0.257142857F) < 1.0e-6F);
  pano::app::CpuExposureSolveResult single;
  EXPECT(pano::app::solve_cpu_exposure_graph(1, {}, single, error));
  EXPECT(single.anchor_frame == 0U && single.edge_count == 0U &&
         single.log_gains == std::vector<float>{0.0F});
  equations = {{0, 1, 10.0, 1.0}};
  EXPECT(pano::app::solve_cpu_exposure_graph(2, equations, solved, error));
  EXPECT(std::fabs(solved.log_gains[0] + std::log(2.0F)) < 1.0e-6F);
  EXPECT(std::fabs(solved.log_gains[1] - std::log(2.0F)) < 1.0e-6F);

  pano::app::CpuExposureReport report;
  const std::vector<float> manual{1.0F, 2.0F};
  EXPECT(pano::app::make_cpu_exposure_report(solved, manual, report, error));
  EXPECT(report.anchor_frame == solved.anchor_frame &&
         report.edge_count == 1U && !report.warning);
  EXPECT(std::fabs(report.gains[0] - 0.5F) < 1.0e-6F &&
         std::fabs(report.gains[1] - 4.0F) < 1.0e-6F);
  pano::app::CpuExposureEdits edits{{1.0F, 2.0F, 3.0F}, 0U, {1U, 2U}};
  EXPECT(pano::app::apply_cpu_exposure_match(edits, 0.5F, error));
  EXPECT((edits.gains == std::vector<float>{1.0F, 1.0F, 1.5F}));
  EXPECT(pano::app::discard_cpu_exposure_edits(edits, error));
  EXPECT((edits.gains == std::vector<float>{1.0F, 1.0F, 1.0F}));
  EXPECT(
      (edits.target == 0U && edits.selected == std::vector<unsigned>{1U, 2U}));

  pano::app::CpuExposureCache *cache = nullptr;
  EXPECT(pano::app::create_cpu_exposure_cache(&cache, error));
  bool hit = true;
  pano::app::CpuExposureReport cached;
  EXPECT(pano::app::query_cpu_exposure_cache(cache, "session/options/a", cached,
                                             hit, error));
  EXPECT(!hit);
  EXPECT(pano::app::store_cpu_exposure_cache(cache, "session/options/a", report,
                                             error));
  EXPECT(pano::app::query_cpu_exposure_cache(cache, "session/options/a", cached,
                                             hit, error));
  EXPECT(hit && cached.gains == report.gains);
  cached.gains[0] = 99.0F;
  EXPECT(pano::app::query_cpu_exposure_cache(cache, "session/options/a", cached,
                                             hit, error));
  EXPECT(hit && cached.gains[0] == report.gains[0]);
  EXPECT(pano::app::query_cpu_exposure_cache(cache, "session/options/b", cached,
                                             hit, error));
  EXPECT(!hit);
  pano::app::invalidate_cpu_exposure_cache(cache);
  EXPECT(pano::app::query_cpu_exposure_cache(cache, "session/options/a", cached,
                                             hit, error));
  EXPECT(!hit);
  pano::app::destroy_cpu_exposure_cache(&cache);
  pano::app::destroy_cpu_exposure_cache(&cache);
  EXPECT(cache == nullptr);
}

void test_reference_exposure_propagation() {
  std::string error;
  std::vector<float> gains;
  const std::vector<pano::app::CpuExposureEquation> frontier{
      {0, 1, 0.2, 100.0}, {0, 2, 0.0, 1.0}, {1, 2, 0.4, 1000.0}};
  EXPECT(pano::app::solve_reference_exposure_gains(
      3, 0, frontier, {1.0F, 1.0F, 1.0F}, gains, error));
  EXPECT(gains.size() == 3U && std::fabs(gains[0] - 1.0F) < 1.0e-7F &&
         std::fabs(gains[1] - std::exp(0.2F)) < 1.0e-6F &&
         std::fabs(gains[2] - 1.0F) < 1.0e-7F);

  EXPECT(pano::app::solve_reference_exposure_gains(2, 0, {{0, 1, 2.0, 1.0}},
                                                   {1.0F, 1.0F}, gains, error));
  EXPECT(gains.size() == 2U && std::fabs(gains[1] - std::exp(2.0F)) < 1.0e-5F);

  EXPECT(pano::app::solve_reference_exposure_gains(2, 0, {{0, 1, 0.2, 1.0}},
                                                   {2.0F, 0.5F}, gains, error));
  EXPECT(gains.size() == 2U && std::fabs(gains[0] - 2.0F) < 1.0e-7F &&
         std::fabs(gains[1] - 2.0F * std::exp(0.2F)) < 1.0e-6F);

  gains = {7.0F};
  EXPECT(!pano::app::solve_reference_exposure_gains(
      3, 0, {{0, 1, 0.2, 1.0}}, {1.0F, 1.0F, 1.0F}, gains, error));
  EXPECT(gains == std::vector<float>{7.0F});
  EXPECT(error.find("disconnected") != std::string::npos);
}

void test_cpu_conversion_and_writer_bands() {
  std::string error;
  pano::app::CpuSdrConversionRequest request;
  request.pixel_count = 4;
  const std::array<float, 12> linear{
      0.0F, 0.0F, 0.0F, 1.0F, 1.0F,
      1.0F, 0.5F, 0.5F, 0.5F, std::numeric_limits<float>::quiet_NaN(),
      0.0F, 0.0F};
  const std::array<std::uint8_t, 4> coverage{1, 1, 1, 1};
  std::array<std::uint64_t, 4096> histogram{};
  EXPECT(pano::app::accumulate_cpu_auto_contrast_histogram(
      request, linear.data(), coverage.data(), histogram, error));
  EXPECT(histogram[0] == 1U && histogram[4095] == 1U);
  EXPECT(std::accumulate(histogram.begin(), histogram.end(),
                         std::uint64_t{0}) == 3U);
  std::array<std::uint64_t, 4096> frozen_histogram{};
  frozen_histogram[0] = 1;
  frozen_histogram[4095] = 1;
  pano::app::CpuAutoContrastLevels levels;
  EXPECT(pano::app::select_cpu_auto_contrast_levels(frozen_histogram, levels,
                                                    error));
  EXPECT(!levels.valid && levels.processed_pixels == 2U);
  frozen_histogram[0] = 100;
  frozen_histogram[4095] = 100;
  EXPECT(pano::app::select_cpu_auto_contrast_levels(frozen_histogram, levels,
                                                    error));
  EXPECT(levels.valid && levels.processed_pixels == 200U);
  EXPECT(std::fabs(levels.black - 0.00995F / 4096.0F) < 1.0e-9F);
  EXPECT(std::fabs(levels.white - 4095.98005F / 4096.0F) < 1.0e-6F);

  request.pixel_count = 3;
  const std::array<float, 9> sdr_linear{0.0F, 0.0F, 0.0F, 1.0F, 1.0F,
                                        1.0F, 0.5F, 0.5F, 0.5F};
  std::array<std::uint8_t, 9> sdr8{};
  EXPECT(pano::app::convert_cpu_sdr8_band(request, sdr_linear.data(),
                                          sdr8.data(), error));
  EXPECT((sdr8 ==
          std::array<std::uint8_t, 9>{0, 0, 0, 255, 255, 255, 188, 188, 188}));
  request.pixel_count = 1;
  request.apply_auto_contrast = true;
  request.levels = {0.25F, 0.75F, 2U, true};
  const float encoded_half_linear = std::pow((0.5F + 0.055F) / 1.055F, 2.4F);
  const std::array<float, 3> encoded_half{
      encoded_half_linear, encoded_half_linear, encoded_half_linear};
  EXPECT(pano::app::convert_cpu_sdr8_band(request, encoded_half.data(),
                                          sdr8.data(), error));
  EXPECT(sdr8[0] == 128U && sdr8[1] == 128U && sdr8[2] == 128U);
  request.apply_auto_contrast = false;
  request.source_transfer = pano::app::CpuTransferFunction::linear;
  request.source_primaries = pano::app::CpuColorPrimaries::rec2020;
  const std::array<float, 3> linear_rec2020{0.5F, 0.25F, 0.125F};
  EXPECT(pano::app::convert_cpu_sdr8_band(
      request, linear_rec2020.data(), sdr8.data(), error));
  const std::array<std::uint8_t, 3> linear_rec2020_sdr{
      sdr8[0], sdr8[1], sdr8[2]};
  request.source_primaries = pano::app::CpuColorPrimaries::srgb;
  EXPECT(pano::app::convert_cpu_sdr8_band(
      request, linear_rec2020.data(), sdr8.data(), error));
  EXPECT(std::equal(linear_rec2020_sdr.begin(), linear_rec2020_sdr.end(),
                    sdr8.begin()));
  request.source_transfer = pano::app::CpuTransferFunction::pq;
  request.source_primaries = pano::app::CpuColorPrimaries::rec2020;
  request.reference_white_nits = 203.0F;
  const std::array<float, 3> pq_white{1.0F, 1.0F, 1.0F};
  EXPECT(pano::app::convert_cpu_sdr8_band(request, pq_white.data(), sdr8.data(),
                                          error));
  EXPECT(sdr8[0] >= 252U && sdr8[0] <= 254U && sdr8[1] >= 252U &&
         sdr8[1] <= 254U && sdr8[2] >= 252U && sdr8[2] <= 254U);

  const std::array<float, 6> float_input{-2.0F, 0.5F, 4.0F, 8.0F, 16.0F, 32.0F};
  std::array<float, 6> float_output{};
  EXPECT(pano::app::copy_cpu_float_band(2, float_input.data(),
                                        float_output.data(), error));
  EXPECT(float_output == float_input);

  TemporaryDirectory temporary;
  pano::app::CodecErrorCategory category{};
  pano::app::ImageWriterOptions exr;
  exr.path = (temporary.path() / "cpu-band.exr").u8string();
  exr.container = pano::app::ImageContainer::exr;
  exr.width = 2;
  exr.height = 1;
  exr.sample_type = "float32";
  exr.encoding.sample_type = "float32";
  exr.encoding.color_primaries = "rec2020";
  exr.encoding.transfer_function = "linear";
  exr.encoding.reference_white_nits = 203.0;
  pano::app::ImageWriter *writer = nullptr;
  EXPECT(pano::app::create_image_writer(exr, &writer, category, error));
  request.pixel_count = 2;
  EXPECT(pano::app::convert_and_write_cpu_band(
      writer, pano::app::CpuOutputBandSample::linear_float32, request, 2, 1,
      float_input.data(), nullptr, 0, {}, category, error));
  EXPECT(pano::app::finish_image_writer(&writer, {}, category, error));
  pano::app::ImageInfo info;
  EXPECT(pano::app::inspect_image(exr.path, info, category, error));
  std::array<float, 6> decoded_float{};
  EXPECT(pano::app::decode_image(exr.path, info, decoded_float.data(),
                                 6 * sizeof(float), sizeof(decoded_float), {},
                                 category, error));
  EXPECT(decoded_float == float_input);
#ifdef _WIN32
  pano::app::ImageWriterOptions png;
  png.path = (temporary.path() / "cpu-band.png").u8string();
  png.container = pano::app::ImageContainer::png;
  png.width = 2;
  png.height = 1;
  EXPECT(pano::app::create_image_writer(png, &writer, category, error));
  request.source_transfer = pano::app::CpuTransferFunction::srgb;
  request.source_primaries = pano::app::CpuColorPrimaries::srgb;
  request.reference_white_nits = 100.0F;
  std::array<std::uint8_t, 6> band_scratch{};
  EXPECT(pano::app::convert_and_write_cpu_band(
      writer, pano::app::CpuOutputBandSample::srgb8, request, 2, 1,
      sdr_linear.data(), band_scratch.data(), band_scratch.size(), {}, category,
      error));
  EXPECT(pano::app::finish_image_writer(&writer, {}, category, error));
  EXPECT(pano::app::inspect_image(png.path, info, category, error));
  std::array<std::uint8_t, 6> decoded_sdr{};
  EXPECT(pano::app::decode_image(png.path, info, decoded_sdr.data(), 6,
                                 decoded_sdr.size(), {}, category, error));
  EXPECT(decoded_sdr == band_scratch);
#endif
}

struct CpuPipelineState {
  std::vector<pano::app::CpuRenderPhase> phases;
  std::optional<pano::app::CpuRenderPhase> fail;
  std::optional<pano::app::CpuRenderPhase> cancel_after;
  std::atomic<bool> cancelled{false};
  std::atomic<bool> entered_decode{false};
  std::atomic<bool> release_decode{true};
  unsigned cleanups = 0;
  std::string destination = "old";
};

bool run_cpu_pipeline_phase(void *data, const pano::app::CpuRenderPhase phase) {
  auto &state = *static_cast<CpuPipelineState *>(data);
  state.phases.push_back(phase);
  if (phase == pano::app::CpuRenderPhase::decode &&
      !state.release_decode.load(std::memory_order_acquire)) {
    state.entered_decode.store(true, std::memory_order_release);
    while (!state.release_decode.load(std::memory_order_acquire))
      std::this_thread::yield();
  }
  if (state.cancel_after == phase)
    state.cancelled.store(true, std::memory_order_release);
  if (state.fail == phase)
    return false;
  if (phase == pano::app::CpuRenderPhase::publish)
    state.destination = "new";
  return true;
}

void cleanup_cpu_pipeline(void *data) {
  auto &state = *static_cast<CpuPipelineState *>(data);
  ++state.cleanups;
  state.destination = "old";
}

bool cpu_pipeline_cancelled(void *data) {
  return static_cast<CpuPipelineState *>(data)->cancelled.load(
      std::memory_order_acquire);
}

void test_cpu_failure_cancellation_and_concurrency() {
  std::string error;
  pano::app::CpuRenderCoordinator *coordinator = nullptr;
  EXPECT(pano::app::create_cpu_render_coordinator(&coordinator, error));
  constexpr std::array<pano::app::CpuRenderPhase, 5> phases{
      pano::app::CpuRenderPhase::allocation, pano::app::CpuRenderPhase::decode,
      pano::app::CpuRenderPhase::compose, pano::app::CpuRenderPhase::encode,
      pano::app::CpuRenderPhase::publish};
  for (const auto failed_phase : phases) {
    CpuPipelineState state;
    state.fail = failed_phase;
    const pano::app::CpuRenderPipelineCallbacks callbacks{
        run_cpu_pipeline_phase, cleanup_cpu_pipeline, &state};
    EXPECT(
        !pano::app::run_cpu_render_pipeline(coordinator, callbacks, {}, error));
    EXPECT(state.phases.back() == failed_phase && state.cleanups == 1U &&
           state.destination == "old");
    CpuPipelineState retry_state;
    const pano::app::CpuRenderPipelineCallbacks retry{
        run_cpu_pipeline_phase, cleanup_cpu_pipeline, &retry_state};
    EXPECT(pano::app::run_cpu_render_pipeline(coordinator, retry, {}, error));
    EXPECT(retry_state.phases.size() == phases.size() &&
           retry_state.cleanups == 0U && retry_state.destination == "new");
  }
  for (const auto cancelled_after : phases) {
    CpuPipelineState state;
    state.cancel_after = cancelled_after;
    const pano::app::CpuRenderPipelineCallbacks callbacks{
        run_cpu_pipeline_phase, cleanup_cpu_pipeline, &state};
    EXPECT(!pano::app::run_cpu_render_pipeline(
        coordinator, callbacks, {cpu_pipeline_cancelled, &state}, error));
    EXPECT(state.phases.back() == cancelled_after && state.cleanups == 1U &&
           state.destination == "old");
  }

  CpuPipelineState held;
  held.release_decode.store(false, std::memory_order_release);
  const pano::app::CpuRenderPipelineCallbacks held_callbacks{
      run_cpu_pipeline_phase, cleanup_cpu_pipeline, &held};
  bool first_result = false;
  std::thread first([&] {
    std::string thread_error;
    first_result = pano::app::run_cpu_render_pipeline(
        coordinator, held_callbacks, {}, thread_error);
  });
  while (!held.entered_decode.load(std::memory_order_acquire))
    std::this_thread::yield();
  CpuPipelineState rejected;
  const pano::app::CpuRenderPipelineCallbacks rejected_callbacks{
      run_cpu_pipeline_phase, cleanup_cpu_pipeline, &rejected};
  EXPECT(!pano::app::run_cpu_render_pipeline(coordinator, rejected_callbacks,
                                             {}, error));
  EXPECT(error.find("already active") != std::string::npos &&
         rejected.phases.empty() && rejected.cleanups == 0U);
  held.release_decode.store(true, std::memory_order_release);
  first.join();
  EXPECT(first_result && held.destination == "new" && held.cleanups == 0U);
  CpuPipelineState repeated;
  const pano::app::CpuRenderPipelineCallbacks repeated_callbacks{
      run_cpu_pipeline_phase, cleanup_cpu_pipeline, &repeated};
  EXPECT(pano::app::run_cpu_render_pipeline(coordinator, repeated_callbacks, {},
                                            error));
  pano::app::destroy_cpu_render_coordinator(&coordinator);
  pano::app::destroy_cpu_render_coordinator(&coordinator);
  EXPECT(coordinator == nullptr);
}

void test_gui_session_discovery_state() {
  TemporaryDirectory temporary;
  const fs::path game = temporary.path() / fs::u8path(u8"Игра");
  const fs::path directory = game / "bin" / "x64" / "plugins" /
                             "cyber_engine_tweaks" / "mods" /
                             "PanoramaCaptureProbe";
  fs::create_directories(directory);
  const fs::path contracts = fixtures();
  fs::copy_file(contracts / "cet-complete.json",
                directory / "PanoramaCaptureBridge.pano-1700000000-1.json");
  fs::copy_file(contracts / "cet-incomplete.json",
                directory / "PanoramaCaptureBridge.pano-1700000100-1.json");
  fs::copy_file(contracts / "cet-invalid.json",
                directory / "PanoramaCaptureBridge.pano-1700000200-1.json");
  write_text(directory / "unrelated.json", "{}");
  std::vector<pano::app::GuiSessionRecord> records;
  std::string error;
  EXPECT(pano::app::discover_gui_sessions(game.u8string(), records, error));
  EXPECT(records.size() == 3U);
  EXPECT(records[0].path.find("1700000200-1") != std::string::npos &&
         !records[0].error.empty());
  EXPECT(records[1].path.find("1700000100-1") != std::string::npos &&
         records[1].error.empty() && !records[1].session.completed);
  EXPECT(records[2].path.find("1700000000-1") != std::string::npos &&
         records[2].error.empty() && records[2].session.completed);
  EXPECT(pano::app::gui_session_status(records[0], true) ==
         pano::app::GuiSessionStatus::invalid);
  EXPECT(pano::app::gui_session_status(records[1], true) ==
         pano::app::GuiSessionStatus::incomplete);
  EXPECT(pano::app::gui_session_status(records[2], false) ==
         pano::app::GuiSessionStatus::complete);
  EXPECT(pano::app::gui_session_status(records[2], true) ==
         pano::app::GuiSessionStatus::stitched);
  EXPECT(pano::app::gui_session_local_label("not-a-timestamp") ==
         "not-a-timestamp");
  const auto local_label = pano::app::gui_session_local_label("1700000000-1");
  EXPECT(local_label != "1700000000-1" &&
         local_label.find("#1") != std::string::npos);
  std::vector<pano::app::GuiSessionRecord> missing{{"stale", {}, {}, {}}};
  EXPECT(pano::app::discover_gui_sessions(
      (temporary.path() / "missing").u8string(), missing, error));
  EXPECT(missing.empty());

  pano::app::GuiRefreshState state;
  const std::uint64_t first = pano::app::begin_gui_session_refresh(state);
  const std::uint64_t second = pano::app::begin_gui_session_refresh(state);
  EXPECT(second == first + 1U);
  EXPECT(!pano::app::complete_gui_session_refresh(state, first, records));
  EXPECT(state.records.empty());
  EXPECT(pano::app::complete_gui_session_refresh(state, second,
                                                 std::move(records)));
  EXPECT(state.records.size() == 3U);
}

void test_gui_request_and_validation_state() {
  pano::app::GuiRenderRequestState request;
  EXPECT(request.output_name == "panorama.jpg");
  EXPECT(request.resolution_percent == 100U);
  EXPECT(request.format == "jpeg" && request.jpeg_quality == 95U);
  EXPECT(request.blend == "feather");
  EXPECT(!request.thumbnail && !request.coverage);
  EXPECT(request.memory_mib == 1024U && request.workers == 0U);
  EXPECT(request.gpu && !request.gpu_memory_mib.has_value() &&
         !request.gpu_strict);
  EXPECT(!request.allow_incomplete && request.auto_contrast);

  auto enablement = pano::app::gui_option_enablement(request);
  EXPECT(enablement.jpeg_quality && enablement.cpu_memory &&
         enablement.workers && enablement.gpu_strict);
  request.format = "png";
  request.gpu = false;
  enablement = pano::app::gui_option_enablement(request);
  EXPECT(!enablement.jpeg_quality && !enablement.gpu_strict);

  TemporaryDirectory temporary;
  request.session = (fixtures() / "shared-valid.json").u8string();
  request.session_id = u8"сессия-1";
  request.image_dir = u8"изображения";
  request.output_directory = temporary.path().u8string();
  request.output_name = "panorama.png";
  request.format = "jpeg";
  request.width = 4096U;
  request.resolution_percent = 75U;
  request.thumbnail = true;
  request.coverage = true;
  request.gpu = false;
  request.gpu_strict = true;
  pano::app::RenderOptions options;
  std::string error;
  EXPECT(pano::app::snapshot_gui_render_request(request, options, error));
  EXPECT(options.session == request.session &&
         options.image_dir == request.image_dir);
  EXPECT(fs::u8path(options.output).filename() ==
         fs::u8path(u8"panorama-сессия-1.jpg"));
  EXPECT(options.width == 4096U &&
         std::abs(options.resolution - 0.75) < 1.0e-12);
  EXPECT(options.blend == "feather" && options.thumbnail && options.coverage);
  EXPECT(!options.gpu && !options.gpu_strict);
  EXPECT(!options.allow_incomplete && options.auto_contrast);

  request.gpu = true;
  request.memory_mib = 768U;
  request.gpu_memory_mib = 3072U;
  EXPECT(pano::app::snapshot_gui_render_request(request, options, error));
  EXPECT(options.memory_mib == 768U && options.gpu_memory_mib == 3072U);
  request.gpu_memory_mib = 8193U;
  EXPECT(!pano::app::snapshot_gui_render_request(request, options, error));
  EXPECT(error.find("GPU memory") != std::string::npos);
  request.gpu_memory_mib = 3072U;
  request.gpu = false;
  EXPECT(!pano::app::snapshot_gui_render_request(request, options, error));
  EXPECT(error.find("requires GPU") != std::string::npos);
  request.gpu = true;
  request.gpu_memory_mib.reset();
  request.memory_mib = 1024U;

  request.resolution_percent = 0U;
  EXPECT(!pano::app::snapshot_gui_render_request(request, options, error));
  EXPECT(error.find("resolution") != std::string::npos);
  request.resolution_percent = 100U;
  request.memory_mib = 8193U;
  EXPECT(!pano::app::snapshot_gui_render_request(request, options, error));
  EXPECT(error.find("memory") != std::string::npos);
  request.memory_mib = 1024U;

  pano::app::GuiValidationState validation;
  const auto first = pano::app::begin_gui_validation(validation);
  const auto second = pano::app::begin_gui_validation(validation);
  pano::app::RenderPlan stale;
  stale.blend = "stale";
  EXPECT(!pano::app::complete_gui_validation(validation, first,
                                             std::move(stale), {}));
  EXPECT(!validation.plan.has_value());
  pano::app::RenderPlan current;
  current.blend = "feather";
  current.outputs.panorama = {"panorama.jpg", {}, true};
  current.outputs.coverage = pano::app::OutputTarget{"coverage.png", {}, true};
  current.outputs.thumbnail =
      pano::app::OutputTarget{"thumbnail.jpg", {}, false};
  EXPECT(pano::app::complete_gui_validation(validation, second,
                                            std::move(current), {}));
  EXPECT(validation.plan.has_value() && validation.error.empty());
  EXPECT(pano::app::gui_existing_output_paths(*validation.plan) ==
         std::vector<std::string>({"panorama.jpg", "coverage.png"}));

  const auto third = pano::app::begin_gui_validation(validation);
  EXPECT(!validation.plan.has_value() && validation.error.empty());
  EXPECT(pano::app::complete_gui_validation(validation, third, std::nullopt,
                                            "invalid session"));
  EXPECT(!validation.plan.has_value() && validation.error == "invalid session");
}

void test_gui_preview_crop_state() {
  pano::app::GuiPreviewViewState state;
  std::string error;
  EXPECT(pano::app::calculate_gui_preview_crop(2000, 1000, 500, 250, 0.5, 0.5,
                                               state, error));
  EXPECT(!state.overview && state.crop.left == 750U && state.crop.top == 375U &&
         state.crop.width == 500U && state.crop.height == 250U);
  EXPECT(pano::app::calculate_gui_preview_crop(2000, 1000, 500, 250, 0.0, 0.0,
                                               state, error));
  EXPECT(state.crop.left == 0U && state.crop.top == 0U);
  EXPECT(pano::app::calculate_gui_preview_crop(2000, 1000, 500, 250, 1.2, 1.2,
                                               state, error));
  EXPECT(state.crop.left == 1500U && state.crop.top == 750U);
  EXPECT(!pano::app::calculate_gui_preview_crop(100, 100, 101, 50, 0.5, 0.5,
                                                state, error));
  EXPECT(!pano::app::calculate_gui_preview_crop(
      100, 100, 50, 50, std::numeric_limits<double>::quiet_NaN(), 0.5, state,
      error));
  pano::app::reset_gui_preview_view(state);
  EXPECT(state.overview && state.crop.width == 0U && state.crop.height == 0U);

  pano::app::GuiPreviewHitRequest hit;
  hit.source_width = 400;
  hit.source_height = 200;
  hit.mask_width = 4;
  hit.mask_height = 2;
  hit.frame_count = 2;
  hit.pointer_x = 0.5;
  hit.pointer_y = 0.5;
  hit.view = {false, {200, 100, 100, 50}};
  std::vector<std::uint8_t> masks(16, 0);
  masks[6] = 1;
  masks[14] = 1;
  std::vector<unsigned> candidates;
  EXPECT(pano::app::gui_preview_hit_test(hit, masks, candidates, error));
  EXPECT(candidates == std::vector<unsigned>({0, 1}));
  hit.target = 0U;
  EXPECT(pano::app::gui_preview_hit_test(hit, masks, candidates, error));
  EXPECT(candidates == std::vector<unsigned>({1}));
  hit.target_mode = true;
  hit.selected = {0U};
  EXPECT(pano::app::gui_preview_hit_test(hit, masks, candidates, error));
  EXPECT(candidates == std::vector<unsigned>({1}));
  masks.pop_back();
  EXPECT(!pano::app::gui_preview_hit_test(hit, masks, candidates, error));
}

void test_gui_workflow_state() {
  EXPECT(pano::app::select_gui_backend(true, false, true, true) ==
         pano::app::GuiBackendDecision::d3d12);
  EXPECT(pano::app::select_gui_backend(false, false, true, true) ==
         pano::app::GuiBackendDecision::cpu_forced);
  EXPECT(pano::app::select_gui_backend(true, false, false, true) ==
         pano::app::GuiBackendDecision::cpu_fallback);
  EXPECT(pano::app::select_gui_backend(true, true, false, true) ==
         pano::app::GuiBackendDecision::strict_d3d12_rejection);
  EXPECT(pano::app::select_gui_backend(true, false, false, false) ==
         pano::app::GuiBackendDecision::unavailable);

  pano::app::GuiWorkflowState state;
  EXPECT(state.stage == pano::app::GuiStage::input);
  EXPECT(state.operation == pano::app::GuiOperation::idle);
  auto presentation = pano::app::derive_gui_presentation(
      state, false, false, false, false, 0U, false);
  EXPECT(!presentation.busy && presentation.input_enabled);
  EXPECT(!presentation.preview_enabled && !presentation.preview_ready);
  EXPECT(!presentation.exposure_enabled && !presentation.output_enabled &&
         !presentation.render_enabled && !presentation.output_complete);
  state.session_selected = true;
  state.validation_ready = true;
  presentation = pano::app::derive_gui_presentation(state, false, false, false,
                                                    false, 0U, false);
  EXPECT(presentation.preview_enabled && !presentation.render_enabled);
  state.preview_ready = true;
  presentation = pano::app::derive_gui_presentation(state, false, true, true,
                                                    true, 0U, true);
  EXPECT(!presentation.exposure_enabled && presentation.output_enabled &&
         presentation.render_enabled);
  presentation = pano::app::derive_gui_presentation(state, true, true, true,
                                                    true, 0U, true);
  EXPECT(presentation.preview_ready && presentation.exposure_enabled);
  EXPECT(presentation.automatic_exposure_enabled &&
         presentation.match_exposure_enabled &&
         presentation.discard_exposure_enabled);
  EXPECT(presentation.output_enabled && presentation.render_enabled &&
         presentation.output_complete);

  pano::app::navigate_gui_stage(state, pano::app::GuiStage::preview);
  pano::app::navigate_gui_stage(state, pano::app::GuiStage::output);
  EXPECT(state.stage == pano::app::GuiStage::output);
  EXPECT(state.validation_ready && state.preview_ready);

  auto invalidation =
      pano::app::apply_gui_change(state, pano::app::GuiChange::output_options);
  EXPECT(invalidation.revalidate && !invalidation.discard_preview &&
         !invalidation.rebuild_preview);
  EXPECT(!state.validation_ready && state.preview_ready);
  state.validation_ready = true;

  invalidation = pano::app::apply_gui_change(
      state, pano::app::GuiChange::gpu_budget_increase);
  EXPECT(invalidation.revalidate && !invalidation.discard_preview);
  EXPECT(state.preview_ready);
  state.validation_ready = true;

  invalidation = pano::app::apply_gui_change(
      state, pano::app::GuiChange::gpu_budget_decrease);
  EXPECT(invalidation.revalidate && invalidation.discard_preview &&
         invalidation.rebuild_preview);
  EXPECT(!state.preview_ready);
  state.preview_ready = true;
  invalidation =
      pano::app::apply_gui_change(state, pano::app::GuiChange::preview_options);
  EXPECT(invalidation.discard_preview && invalidation.rebuild_preview);

  state.preview_ready = true;
  invalidation =
      pano::app::apply_gui_change(state, pano::app::GuiChange::session);
  EXPECT(invalidation.revalidate && invalidation.discard_preview &&
         !invalidation.rebuild_preview && state.session_selected);
  state.session_selected = true;
  state.preview_ready = true;
  invalidation =
      pano::app::apply_gui_change(state, pano::app::GuiChange::game_directory);
  EXPECT(invalidation.reset_session && invalidation.discard_preview &&
         !invalidation.rebuild_preview);
  EXPECT(!state.session_selected && !state.validation_ready &&
         !state.preview_ready);

  std::string error;
  std::uint64_t generation = 0;
  EXPECT(pano::app::begin_gui_operation(state, pano::app::GuiOperation::preview,
                                        generation, error));
  EXPECT(state.operation == pano::app::GuiOperation::preview);
  presentation = pano::app::derive_gui_presentation(state, true, true, true,
                                                    true, 37U, true);
  EXPECT(presentation.busy && !presentation.input_enabled &&
         !presentation.preview_enabled && !presentation.exposure_enabled &&
         !presentation.output_enabled && !presentation.render_enabled);
  EXPECT(presentation.preview_progress == 37U &&
         presentation.output_progress == 0U && presentation.output_complete);
  std::uint64_t rejected = 0;
  EXPECT(!pano::app::begin_gui_operation(state, pano::app::GuiOperation::render,
                                         rejected, error));
  EXPECT(!pano::app::complete_gui_operation(state, generation + 1U));
  EXPECT(pano::app::complete_gui_operation(state, generation));
  EXPECT(state.operation == pano::app::GuiOperation::idle);
  EXPECT(!pano::app::begin_gui_operation(state, pano::app::GuiOperation::idle,
                                         rejected, error));
  EXPECT(pano::app::begin_gui_operation(state, pano::app::GuiOperation::render,
                                        generation, error));
  presentation = pano::app::derive_gui_presentation(state, true, true, true,
                                                    true, 61U, true);
  EXPECT(presentation.busy && presentation.rendering);
  EXPECT(presentation.preview_progress == 0U &&
         presentation.output_progress == 61U && !presentation.output_complete);
  pano::app::cancel_gui_operation(state);
  EXPECT(state.operation == pano::app::GuiOperation::idle);
  EXPECT(!pano::app::complete_gui_operation(state, generation));
}

void test_gui_exposure_state() {
  pano::app::GuiExposureState state;
  std::string error;
  std::uint64_t first = 0;
  EXPECT(pano::app::begin_gui_exposure_operation(
      state, pano::app::GuiExposureOperation::automatic, 3, 1U, {}, first,
      error));
  EXPECT(state.busy &&
         state.edits.gains == std::vector<float>({1.0F, 1.0F, 1.0F}));
  EXPECT(pano::app::update_gui_exposure_progress(state, first, 40U));
  EXPECT(state.progress_percent == 40U);
  pano::app::cancel_gui_exposure_operation(state);
  EXPECT(!state.busy && state.progress_percent == 0U);
  pano::app::CpuExposureReport stale;
  stale.gains = {2.0F, 2.0F, 2.0F};
  EXPECT(!pano::app::complete_gui_exposure_operation(state, first, stale, {}));
  EXPECT(state.edits.gains == std::vector<float>({1.0F, 1.0F, 1.0F}));

  std::uint64_t current = 0;
  EXPECT(pano::app::begin_gui_exposure_operation(
      state, pano::app::GuiExposureOperation::automatic, 3, std::nullopt, {},
      current, error));
  EXPECT(!pano::app::update_gui_exposure_progress(state, first, 80U));
  pano::app::CpuExposureReport report;
  report.gains = {1.0F, 0.75F, 1.25F};
  report.warning = true;
  EXPECT(pano::app::complete_gui_exposure_operation(state, current, report,
                                                    "disconnected frames"));
  EXPECT(!state.busy && state.progress_percent == 100U &&
         state.edits.gains == report.gains &&
         state.warning == "disconnected frames");

  EXPECT(!pano::app::begin_gui_exposure_operation(
      state, pano::app::GuiExposureOperation::manual_match, 3, 0U, {}, current,
      error));
  EXPECT(pano::app::begin_gui_exposure_operation(
      state, pano::app::GuiExposureOperation::manual_match, 3, 0U, {1U, 2U},
      current, error));
  EXPECT(!pano::app::apply_gui_exposure_match(state, 0.5F, error));
  EXPECT(!pano::app::discard_gui_exposure_edits(state, error));
  EXPECT(pano::app::complete_gui_exposure_operation(
      state, current, std::nullopt, "manual estimate unavailable"));
  EXPECT(pano::app::apply_gui_exposure_match(state, 0.5F, error));
  EXPECT(state.edits.gains == std::vector<float>({1.0F, 0.375F, 0.625F}));
  EXPECT(pano::app::discard_gui_exposure_edits(state, error));
  EXPECT(state.edits.gains == std::vector<float>({1.0F, 1.0F, 1.0F}) &&
         state.edits.target == 0U &&
         state.edits.selected == std::vector<unsigned>({1U, 2U}) &&
         state.warning.empty());
}

void test_sdr_d3d12_upload() {
#ifdef _WIN32
  std::array<char, 512> gpu_error{};
  pano_gpu_probe_options probe{};
  probe.size = sizeof(probe);
  probe.abi_version = PANO_GPU_ABI_VERSION;
  probe.allow_warp = 1;
  pano_gpu_adapter_info adapter{};
  adapter.size = sizeof(adapter);
  adapter.abi_version = PANO_GPU_ABI_VERSION;
  const auto probe_result =
      pano_gpu_probe_adapter(&probe, &adapter, gpu_error.data(),
                             static_cast<std::uint32_t>(gpu_error.size()));
  EXPECT(probe_result == PANO_GPU_SUCCESS);
  if (probe_result != PANO_GPU_SUCCESS)
    return;

  pano_gpu_device *device = nullptr;
  const auto device_result =
      pano_gpu_device_create(&probe, &device, gpu_error.data(),
                             static_cast<std::uint32_t>(gpu_error.size()));
  EXPECT(device_result == PANO_GPU_SUCCESS);
  if (device_result != PANO_GPU_SUCCESS)
    return;

  pano::app::ImageInfo info;
  pano::app::CodecErrorCategory category{};
  std::string error;
  EXPECT(pano::app::inspect_image(
      (codec_fixtures() / "rgb8-srgb.png").u8string(), info, category, error));
  pano_gpu_session_create_options options{};
  options.size = sizeof(options);
  options.abi_version = PANO_GPU_ABI_VERSION;
  options.frame_count = 2;
  options.source_width = info.width;
  options.source_height = info.height;
  options.source_sample_type = PANO_GPU_SAMPLE_UINT8;
  options.transfer_function = PANO_GPU_TRANSFER_SRGB;
  options.source_row_stride_bytes = info.width * 3U;
  options.device_luid = adapter.luid;
  pano_gpu_session *session = nullptr;
  const auto session_result =
      pano_gpu_session_create(device, &options, &session, gpu_error.data(),
                              static_cast<std::uint32_t>(gpu_error.size()));
  EXPECT(session_result == PANO_GPU_SUCCESS);
  if (session_result == PANO_GPU_SUCCESS) {
    const std::vector<std::string> paths{
        (codec_fixtures() / "rgb8-srgb.png").u8string(),
        (codec_fixtures() / "rgb8-srgb.png").u8string()};
    EXPECT(pano::app::decode_and_upload_images(session, paths, info, {},
                                               nullptr, category, error));
    EXPECT(category == pano::app::CodecErrorCategory::none);
    pano_gpu_session_diagnostics diagnostics{};
    diagnostics.size = sizeof(diagnostics);
    diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_query_diagnostics(
               session, &diagnostics, gpu_error.data(),
               static_cast<std::uint32_t>(gpu_error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(diagnostics.upload_count == 2U);
    EXPECT(diagnostics.uploaded_bytes == 36U);
    EXPECT(diagnostics.last_completed_upload_fence != 0U);

    const pano::app::CancellationCheck cancellation{always_cancelled, nullptr};
    EXPECT(!pano::app::decode_and_upload_images(
        session, paths, info, cancellation, nullptr, category, error));
    EXPECT(category == pano::app::CodecErrorCategory::cancelled);
    pano_gpu_cancellation_token *token = nullptr;
    EXPECT(pano_gpu_cancellation_token_create(
               &token, gpu_error.data(),
               static_cast<std::uint32_t>(gpu_error.size())) ==
           PANO_GPU_SUCCESS);
    pano_gpu_cancellation_token_cancel(token);
    EXPECT(!pano::app::decode_and_upload_images(session, paths, info, {}, token,
                                                category, error));
    EXPECT(category == pano::app::CodecErrorCategory::cancelled);
    pano_gpu_cancellation_token_destroy(&token);
  }
  pano_gpu_session_destroy(&session);

  EXPECT(pano::app::inspect_image(
      (codec_fixtures() / "rgb16-rec2020-pq.png").u8string(), info, category,
      error));
  options.source_width = info.width;
  options.source_height = info.height;
  options.source_sample_type = PANO_GPU_SAMPLE_UINT16;
  options.transfer_function = PANO_GPU_TRANSFER_PQ;
  options.source_row_stride_bytes = info.width * 3U * sizeof(std::uint16_t);
  EXPECT(pano_gpu_session_create(
             device, &options, &session, gpu_error.data(),
             static_cast<std::uint32_t>(gpu_error.size())) == PANO_GPU_SUCCESS);
  if (session != nullptr) {
    const std::vector<std::string> paths{
        (codec_fixtures() / "rgb16-rec2020-pq.png").u8string(),
        (codec_fixtures() / "rgb16-rec2020-pq.png").u8string()};
    EXPECT(pano::app::decode_and_upload_images(session, paths, info, {},
                                               nullptr, category, error));
    pano_gpu_session_diagnostics diagnostics{};
    diagnostics.size = sizeof(diagnostics);
    diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_query_diagnostics(
               session, &diagnostics, gpu_error.data(),
               static_cast<std::uint32_t>(gpu_error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(diagnostics.upload_count == 2U);
    EXPECT(diagnostics.uploaded_bytes == 72U);
    EXPECT(diagnostics.last_completed_upload_fence != 0U);
  }
  pano_gpu_session_destroy(&session);

  EXPECT(pano::app::inspect_image(
      (codec_fixtures() / "rgb32-rec2020-linear.exr").u8string(), info,
      category, error));
  options.source_width = info.width;
  options.source_height = info.height;
  options.source_sample_type = PANO_GPU_SAMPLE_FLOAT32;
  options.transfer_function = PANO_GPU_TRANSFER_LINEAR;
  options.source_row_stride_bytes = info.width * 3U * sizeof(float);
  EXPECT(pano_gpu_session_create(
             device, &options, &session, gpu_error.data(),
             static_cast<std::uint32_t>(gpu_error.size())) == PANO_GPU_SUCCESS);
  if (session != nullptr) {
    const std::vector<std::string> paths{
        (codec_fixtures() / "rgb32-rec2020-linear.exr").u8string(),
        (codec_fixtures() / "rgb32-rec2020-linear.exr").u8string()};
    EXPECT(pano::app::decode_and_upload_images(session, paths, info, {},
                                               nullptr, category, error));
    pano_gpu_session_diagnostics diagnostics{};
    diagnostics.size = sizeof(diagnostics);
    diagnostics.abi_version = PANO_GPU_ABI_VERSION;
    EXPECT(pano_gpu_session_query_diagnostics(
               session, &diagnostics, gpu_error.data(),
               static_cast<std::uint32_t>(gpu_error.size())) ==
           PANO_GPU_SUCCESS);
    EXPECT(diagnostics.upload_count == 2U);
    EXPECT(diagnostics.uploaded_bytes == 144U);
    EXPECT(diagnostics.last_completed_upload_fence != 0U);
  }
  pano_gpu_session_destroy(&session);

  pano_gpu_diagnostics before_preview{};
  before_preview.size = sizeof(before_preview);
  before_preview.abi_version = PANO_GPU_ABI_VERSION;
  EXPECT(pano_gpu_query_diagnostics(
             &before_preview, gpu_error.data(),
             static_cast<std::uint32_t>(gpu_error.size())) == PANO_GPU_SUCCESS);
  pano::app::RenderPlan preview_plan;
  preview_plan.session.capture_mode = "full_sphere";
  preview_plan.session.horizontal_fov_deg = 90.0;
  preview_plan.session.vertical_fov_deg = 90.0;
  preview_plan.session.completed = true;
  preview_plan.session.image_encoding = info.encoding;
  pano::app::FrameSummary preview_frame;
  preview_frame.filename = (codec_fixtures() / "rgb8-srgb.png").u8string();
  preview_frame.status = "captured";
  preview_frame.camera_basis_row_major =
      std::array<double, 9>{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
  preview_plan.session.frames.push_back(preview_frame);
  preview_plan.session.frames.push_back(preview_frame);
  preview_plan.blend = "hard";
  preview_plan.allow_incomplete = true;
  preview_plan.auto_contrast = false;
  TemporaryDirectory rendered;
  preview_plan.output_width = 8U;
  preview_plan.output_height = 4U;
  preview_plan.outputs.panorama.final_path =
      (rendered.path() / "native-render.png").u8string();
  preview_plan.outputs.coverage = pano::app::OutputTarget{
      (rendered.path() / "native-render-coverage.png").u8string(), {}, false};
  preview_plan.outputs.thumbnail = pano::app::OutputTarget{
      (rendered.path() / "native-render-thumbnail.png").u8string(), {}, false};
  pano::app::NativePreviewOptions preview_options;
  preview_options.viewport_width = 2;
  NativeRenderProgress preview_progress;
  preview_options.progress = record_native_render_progress;
  preview_options.progress_user_data = &preview_progress;
  pano::app::NativePreview *native_preview = nullptr;
  const bool preview_created = pano::app::create_native_preview(
      device, preview_plan, preview_options, &native_preview, error);
  if (!preview_created)
    std::cerr << "native preview creation failed: " << error << '\n';
  EXPECT(preview_created);
  EXPECT(!preview_progress.values.empty() &&
         preview_progress.values.front() == 0U &&
         preview_progress.values.back() ==
             pano::app::native_preview_retain_progress_end &&
         std::is_sorted(preview_progress.values.begin(),
                        preview_progress.values.end()));
  if (native_preview != nullptr) {
    pano::app::NativePreviewDiagnostics preview_diagnostics;
    EXPECT(pano::app::query_native_preview(native_preview, preview_diagnostics,
                                           error));
    EXPECT(preview_diagnostics.frame_count == 2U);
    EXPECT(preview_diagnostics.preview_width == 8U);
    EXPECT(preview_diagnostics.preview_height == 4U);
    EXPECT(preview_diagnostics.overview_width == 2U);
    EXPECT(preview_diagnostics.overview_height == 1U);
    EXPECT(preview_diagnostics.mask_width == 2U);
    EXPECT(preview_diagnostics.mask_height == 1U);
    EXPECT(pano::app::native_preview_handle(native_preview) != nullptr);
    EXPECT(pano::app::native_preview_masks(native_preview).size() == 4U);
    unsigned render_width = 0U;
    unsigned render_height = 0U;
    EXPECT(pano::app::query_native_render_dimensions(
        native_preview, render_width, render_height, error));
    EXPECT(render_width == 8U && render_height == 4U);
    unsigned maximum_render_width = 0U;
    EXPECT(pano::app::query_native_maximum_render_width(
        native_preview, maximum_render_width, error));
    EXPECT(maximum_render_width != 0U);
    auto recomposed_plan = preview_plan;
    recomposed_plan.blend = "feather";
    recomposed_plan.auto_contrast = true;
    NativeRenderProgress recomposition_progress;
    auto recomposition_options = preview_options;
    recomposition_options.progress_user_data = &recomposition_progress;
    const auto preview_before_recomposition =
        pano::app::native_preview_handle(native_preview);
    EXPECT(pano::app::rebuild_native_preview(native_preview, recomposed_plan,
                                             recomposition_options, error));
    EXPECT(pano::app::native_preview_handle(native_preview) != nullptr &&
           pano::app::native_preview_handle(native_preview) !=
               preview_before_recomposition);
    EXPECT(!recomposition_progress.values.empty() &&
           recomposition_progress.values.front() ==
               pano::app::native_preview_compose_progress_begin &&
           recomposition_progress.values.back() ==
               pano::app::native_preview_retain_progress_end &&
           std::is_sorted(recomposition_progress.values.begin(),
                          recomposition_progress.values.end()) &&
           std::none_of(recomposition_progress.phases.begin(),
                        recomposition_progress.phases.end(),
                        [](const std::string &phase) {
                          return phase == "Loading source images";
                        }));
    preview_plan = std::move(recomposed_plan);
    NativeRenderProgress thumbnail_progress;
    pano::app::NativeRenderOptions thumbnail_render_options;
    thumbnail_render_options.progress = record_native_render_progress;
    thumbnail_render_options.progress_user_data = &thumbnail_progress;
    pano::app::NativeRenderResult thumbnail_render_result;
    EXPECT(pano::app::render_native_session(native_preview,
                                            thumbnail_render_options,
                                            thumbnail_render_result, error));
    EXPECT(!thumbnail_progress.values.empty() &&
           thumbnail_progress.values.front() == 0U &&
           thumbnail_progress.values.back() == 100U &&
           std::is_sorted(thumbnail_progress.values.begin(),
                          thumbnail_progress.values.end()));
    EXPECT(std::find(thumbnail_progress.phases.begin(),
                     thumbnail_progress.phases.end(),
                     "render") != thumbnail_progress.phases.end() &&
           std::find(thumbnail_progress.phases.begin(),
                     thumbnail_progress.phases.end(),
                     "thumbnail") != thumbnail_progress.phases.end());
    EXPECT(
        (thumbnail_render_result.published_paths ==
         std::vector<std::string>{preview_plan.outputs.coverage->final_path,
                                  preview_plan.outputs.thumbnail->final_path,
                                  preview_plan.outputs.panorama.final_path}));
    EXPECT(fs::remove(fs::u8path(preview_plan.outputs.thumbnail->final_path)));
    auto updated_plan = preview_plan;
    updated_plan.outputs.panorama.final_path =
        (rendered.path() / "native-render-updated.png").u8string();
    updated_plan.outputs.thumbnail.reset();
    updated_plan.jpeg_quality = 92U;
    EXPECT(pano::app::update_native_preview_render_plan(native_preview,
                                                        updated_plan, error));
    EXPECT(pano::app::query_native_render_dimensions(
        native_preview, render_width, render_height, error));
    EXPECT(render_width == 8U && render_height == 4U);
    updated_plan.output_width.reset();
    updated_plan.output_height.reset();
    updated_plan.resolution = 0.5;
    EXPECT(pano::app::update_native_preview_render_plan(native_preview,
                                                        updated_plan, error));
    EXPECT(pano::app::query_native_render_dimensions(
        native_preview, render_width, render_height, error));
    EXPECT(render_width == 4U && render_height == 2U);
    updated_plan.output_width = 7U;
    EXPECT(pano::app::update_native_preview_render_plan(native_preview,
                                                        updated_plan, error));
    EXPECT(pano::app::query_native_render_dimensions(
        native_preview, render_width, render_height, error));
    EXPECT(render_width == 6U && render_height == 3U);
    updated_plan.output_width = 8U;
    updated_plan.output_height = 4U;
    updated_plan.resolution = 1.0;
    EXPECT(pano::app::update_native_preview_render_plan(native_preview,
                                                        updated_plan, error));
    auto incompatible_plan = updated_plan;
    incompatible_plan.session.session_id = "different-session";
    EXPECT(!pano::app::update_native_preview_render_plan(
        native_preview, incompatible_plan, error));
    EXPECT(error.find("retained session") != std::string::npos);
    preview_plan = std::move(updated_plan);
    pano::app::NativeExposureResult exposure;
    EXPECT(pano::app::discard_native_exposure_edits(
        native_preview, preview_options, exposure, error));
    EXPECT(exposure.gains == std::vector<float>({1.0F, 1.0F}));
    EXPECT(pano::app::native_preview_handle(native_preview) != nullptr);
    EXPECT(pano::app::native_preview_masks(native_preview).size() == 4U);
    NativeRenderProgress exposure_progress;
    auto exposure_options = preview_options;
    exposure_options.progress_user_data = &exposure_progress;
    EXPECT(pano::app::apply_native_automatic_exposure(
        native_preview, 0U, exposure_options, exposure, error));
    EXPECT(!exposure_progress.values.empty() &&
           exposure_progress.values.front() == 0U &&
           exposure_progress.values.back() ==
               pano::app::native_preview_retain_progress_end &&
           std::is_sorted(exposure_progress.values.begin(),
                          exposure_progress.values.end()) &&
           std::find(exposure_progress.phases.begin(),
                     exposure_progress.phases.end(),
                     "Sampling poses") != exposure_progress.phases.end());
    const auto retained_before_cancel =
        pano::app::native_preview_handle(native_preview);
    pano::app::NativePreviewOptions cancelled_exposure = preview_options;
    cancelled_exposure.cancellation = {always_cancelled, nullptr};
    EXPECT(!pano::app::discard_native_exposure_edits(
        native_preview, cancelled_exposure, exposure, error));
    EXPECT(pano::app::native_preview_handle(native_preview) ==
           retained_before_cancel);
    EXPECT(!pano::app::apply_native_automatic_exposure(
        native_preview, 2U, preview_options, exposure, error));
    EXPECT(!pano::app::apply_native_manual_exposure_match(
        native_preview, 0U, {0U}, preview_options, exposure, error));
    NativeRenderProgress progress;
    pano::app::NativeRenderOptions render_options;
    render_options.progress = record_native_render_progress;
    render_options.progress_user_data = &progress;
    pano::app::NativeRenderResult render_result;
    EXPECT(pano::app::render_native_session(native_preview, render_options,
                                            render_result, error));
    EXPECT(render_result.width == 8U && render_result.height == 4U);
    EXPECT(
        render_result.published_paths ==
        std::vector<std::string>({preview_plan.outputs.coverage->final_path,
                                  preview_plan.outputs.panorama.final_path}));
    EXPECT(progress.calls > 0U && progress.completed == 100U &&
           progress.total == 100U && !progress.values.empty() &&
           progress.values.front() == 0U &&
           std::is_sorted(progress.values.begin(), progress.values.end()));
    pano::app::ImageInfo rendered_info;
    EXPECT(pano::app::inspect_image(preview_plan.outputs.panorama.final_path,
                                    rendered_info, category, error));
    EXPECT(rendered_info.width == 8U && rendered_info.height == 4U &&
           rendered_info.channels == 3U);
    EXPECT(pano::app::inspect_image(preview_plan.outputs.coverage->final_path,
                                    rendered_info, category, error));
    EXPECT(rendered_info.width == 8U && rendered_info.height == 4U &&
           rendered_info.channels == 1U);
    EXPECT(!fs::exists(rendered.path() / "native-render-thumbnail.png"));
    const auto panorama_before_cancel =
        read_bytes(fs::u8path(preview_plan.outputs.panorama.final_path));
    pano::app::NativeRenderOptions cancelled_render = render_options;
    cancelled_render.cancellation = {always_cancelled, nullptr};
    EXPECT(!pano::app::render_native_session(native_preview, cancelled_render,
                                             render_result, error));
    EXPECT(read_bytes(fs::u8path(preview_plan.outputs.panorama.final_path)) ==
           panorama_before_cancel);

    auto uncovered_exr = preview_plan;
    uncovered_exr.allow_incomplete = false;
    uncovered_exr.outputs.panorama.final_path =
        (rendered.path() / "native-uncovered.exr").u8string();
    uncovered_exr.outputs.coverage.reset();
    EXPECT(pano::app::update_native_preview_render_plan(
        native_preview, uncovered_exr, error));
    EXPECT(!pano::app::render_native_session(native_preview, render_options,
                                             render_result, error));
    EXPECT(error.find("cover every output pixel") != std::string::npos);
  }
  pano::app::destroy_native_preview(&native_preview);
  pano::app::destroy_native_preview(&native_preview);
  EXPECT(native_preview == nullptr);
  EXPECT(pano::app::native_preview_handle(native_preview) == nullptr);
  EXPECT(pano::app::native_preview_masks(native_preview).empty());
  pano::app::NativePreviewDiagnostics missing_preview_diagnostics;
  EXPECT(!pano::app::query_native_preview(native_preview,
                                          missing_preview_diagnostics, error));

  pano::app::NativePreviewOptions cancelled_preview_options = preview_options;
  cancelled_preview_options.cancellation = {always_cancelled, nullptr};
  EXPECT(!pano::app::create_native_preview(
      device, preview_plan, cancelled_preview_options, &native_preview, error));
  EXPECT(native_preview == nullptr);
  pano_gpu_diagnostics after_preview{};
  after_preview.size = sizeof(after_preview);
  after_preview.abi_version = PANO_GPU_ABI_VERSION;
  EXPECT(pano_gpu_query_diagnostics(
             &after_preview, gpu_error.data(),
             static_cast<std::uint32_t>(gpu_error.size())) == PANO_GPU_SUCCESS);
  EXPECT(after_preview.live_session_count == before_preview.live_session_count);
  pano_gpu_device_destroy(&device);
#endif
}

void test_application_settings_history_and_deletion() {
  TemporaryDirectory temporary;
  const auto settings_path = temporary.path() / fs::u8path(u8"настройки.json");
  pano::app::ApplicationSettings settings;
  std::string error;
  unsigned gpu_memory_mib = 0;
  EXPECT(pano::app::parse_application_gpu_memory_mib("1024", gpu_memory_mib,
                                                     error));
  EXPECT(gpu_memory_mib == 1024U);
  EXPECT(pano::app::parse_application_gpu_memory_mib("8192", gpu_memory_mib,
                                                     error));
  EXPECT(gpu_memory_mib == 8192U);
  for (const std::string_view invalid :
       {"", "1023", "8193", "+1024", "1024 MiB", "42949672960"}) {
    EXPECT(!pano::app::parse_application_gpu_memory_mib(invalid, gpu_memory_mib,
                                                        error));
    EXPECT(error.find("1024") != std::string::npos &&
           error.find("8192") != std::string::npos);
  }
  EXPECT(pano::app::load_application_settings(settings_path.u8string(),
                                              settings, error));
  EXPECT(settings.game_directory.empty() && settings.image_directory.empty() &&
         settings.output_directory.empty() && settings.auto_contrast &&
         settings.stitched_sessions.empty() && settings.session_tags.empty() &&
         settings.gpu_memory_mib == 0U && !settings.debug_coverage);
  write_text(settings_path, "not json");
  settings.game_directory = "stale";
  settings.session_tags.push_back({"preserved-key", "preserved-tag"});
  EXPECT(!pano::app::load_application_settings(settings_path.u8string(),
                                               settings, error));
  EXPECT(settings.game_directory == "stale" &&
         settings.session_tags.size() == 1U &&
         settings.session_tags[0].key == "preserved-key" &&
         settings.session_tags[0].tag == "preserved-tag" &&
         error.find("parse") != std::string::npos);
  write_text(
      settings_path,
      R"({"game_dir":"game","image_dir":"images","output_dir":"output","stitched_sessions":"invalid","auto_contrast":false})");
  EXPECT(pano::app::load_application_settings(settings_path.u8string(),
                                              settings, error));
  EXPECT(settings.game_directory == "game" &&
         settings.image_directory == "images" &&
         settings.output_directory == "output" && !settings.auto_contrast &&
         settings.stitched_sessions.empty());
  write_text(
      settings_path,
      R"({"game_dir":"legacy-game","image_dir":"legacy-images","output_dir":"legacy-output","stitched_sessions":{"legacy-key":{"output_name":"legacy-panorama.jpg"}},"auto_contrast":true})");
  EXPECT(pano::app::load_application_settings(settings_path.u8string(),
                                              settings, error));
  EXPECT(settings.game_directory == "legacy-game" &&
         settings.image_directory == "legacy-images" &&
         settings.output_directory == "legacy-output" &&
         settings.auto_contrast && settings.stitched_sessions.size() == 1U &&
         settings.stitched_sessions[0].key == "legacy-key" &&
         settings.stitched_sessions[0].output_name == "legacy-panorama.jpg");
  settings.stitched_sessions.clear();
  settings.gpu_memory_mib = 3072U;
  settings.debug_coverage = true;
  pano::app::mark_application_session_stitched(settings, "game", "session",
                                               u8"первый.png");
  pano::app::mark_application_session_stitched(settings, "game", "session",
                                               u8"готово.png");
  EXPECT(settings.stitched_sessions.size() == 1U);
  EXPECT(pano::app::application_stitched_name(settings, "game", "session") ==
         std::optional<std::string>(u8"готово.png"));
  EXPECT(pano::app::set_application_session_tag(settings, "game", "session",
                                                u8"любимый", error));
  EXPECT(pano::app::application_session_tag(settings, "game", "session") ==
         std::optional<std::string>(u8"любимый"));
  EXPECT(pano::app::set_application_session_tag(settings, "game", "session",
                                                std::string(64U, 'x'), error));
  EXPECT(!pano::app::set_application_session_tag(settings, "game", "session",
                                                 std::string(65U, 'x'), error));
  EXPECT(error.find("64") != std::string::npos);
  EXPECT(!pano::app::set_application_session_tag(
      settings, "game", "session", std::string("\xC0\xAF", 2), error));
  EXPECT(pano::app::set_application_session_tag(settings, "game", "session",
                                                u8"любимый", error));
  EXPECT(pano::app::set_and_save_application_session_tag(
      settings, "game", "session", std::string(64U, 'x'), std::nullopt, error));
  EXPECT(pano::app::application_session_tag(settings, "game", "session") ==
         std::optional<std::string>(std::string(64U, 'x')));
  EXPECT(pano::app::set_and_save_application_session_tag(
      settings, "game", "session", "", std::nullopt, error));
  EXPECT(!pano::app::application_session_tag(settings, "game", "session")
              .has_value());
  EXPECT(pano::app::set_and_save_application_session_tag(
      settings, "game", "session", u8"любимый", std::nullopt, error));
  EXPECT(!pano::app::set_and_save_application_session_tag(
      settings, "game", "session", std::string(65U, 'x'), std::nullopt, error));
  EXPECT(pano::app::application_session_tag(settings, "game", "session") ==
         std::optional<std::string>(u8"любимый"));
  const auto blocked_settings_parent = temporary.path() / "blocked-settings";
  write_text(blocked_settings_parent, "not a directory");
  EXPECT(!pano::app::set_and_save_application_session_tag(
      settings, "game", "session", "replacement",
      (blocked_settings_parent / "gui-settings.json").u8string(), error));
  EXPECT(pano::app::application_session_tag(settings, "game", "session") ==
         std::optional<std::string>(u8"любимый"));
  EXPECT(pano::app::set_and_save_application_session_tag(
      settings, "game", "session", u8"любимый", settings_path.u8string(),
      error));
  const bool settings_saved = pano::app::save_application_settings(
      settings_path.u8string(), settings, error);
  if (!settings_saved)
    std::cerr << "native settings save failed: " << error << '\n';
  EXPECT(settings_saved);
  EXPECT(!fs::exists(fs::u8path(settings_path.u8string() + ".partial")));
  pano::app::ApplicationSettings round_trip;
  EXPECT(pano::app::load_application_settings(settings_path.u8string(),
                                              round_trip, error));
  EXPECT(round_trip.game_directory == settings.game_directory &&
         round_trip.image_directory == settings.image_directory &&
         round_trip.output_directory == settings.output_directory &&
         round_trip.auto_contrast == settings.auto_contrast &&
         round_trip.gpu_memory_mib == 3072U && round_trip.debug_coverage &&
         round_trip.session_tags.size() == 1U &&
         round_trip.session_tags[0].tag == u8"любимый" &&
         round_trip.stitched_sessions.size() == 1U &&
         round_trip.stitched_sessions[0].output_name == u8"готово.png");
  EXPECT(pano::app::set_application_session_tag(round_trip, "game", "session",
                                                "", error));
  EXPECT(!pano::app::application_session_tag(round_trip, "game", "session")
              .has_value());

  const auto session = temporary.path() / "session.json";
  const auto image = temporary.path() / "image.png";
  write_text(session, "session");
  write_text(image, "image");
  pano::app::GuiSessionRecord record;
  record.path = session.u8string();
  record.image_paths = {image.u8string(), image.u8string(),
                        (temporary.path() / "missing.png").u8string()};
  EXPECT(pano::app::application_deletion_targets(record, false) ==
         std::vector<std::string>({session.u8string()}));
  const auto targets = pano::app::application_deletion_targets(record, true);
  EXPECT(targets.size() == 3U && fs::exists(session) && fs::exists(image));
  const auto partial_file = temporary.path() / "partial-delete.txt";
  const auto blocked_directory = temporary.path() / "non-empty-directory";
  write_text(partial_file, "delete first");
  fs::create_directories(blocked_directory);
  write_text(blocked_directory / "keep.txt", "keep");
  pano::app::DeletionResult partial_deletion;
  EXPECT(!pano::app::delete_application_files(
      {partial_file.u8string(), blocked_directory.u8string()}, partial_deletion,
      error));
  EXPECT(!fs::exists(partial_file) && fs::exists(blocked_directory));
  EXPECT(error.find(blocked_directory.u8string()) != std::string::npos);
  pano::app::DeletionResult deletion;
  EXPECT(pano::app::delete_application_files(targets, deletion, error));
  EXPECT(deletion.deleted == 2U && deletion.missing == 1U);
  EXPECT(!fs::exists(session) && !fs::exists(image));
}

void test_cpu_native_session() {
  TemporaryDirectory temporary;
  pano::app::RenderPlan plan;
  plan.session.schema_version = 5U;
  plan.session.session_id = "cpu-session";
  plan.session.capture_mode = "full_sphere";
  plan.session.horizontal_fov_deg = 90.0;
  plan.session.vertical_fov_deg = 90.0;
  plan.session.overlap_fraction = 0.25;
  plan.session.completed = true;
  plan.session.image_encoding =
#ifdef _WIN32
      {"uint8", "srgb", "srgb", 100.0};
#else
      {"float32", "rec2020", "linear", 203.0};
#endif
  pano::app::FrameSummary frame;
  frame.filename =
#ifdef _WIN32
      (codec_fixtures() / "rgb8-srgb.png").u8string();
#else
      (codec_fixtures() / "rgb32-rec2020-linear.exr").u8string();
#endif
  frame.status = "captured";
  frame.camera_basis_row_major =
      std::array<double, 9>{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0};
  plan.session.frames = {frame, frame};
  plan.output_width = 8U;
  plan.output_height = 4U;
  plan.blend = "hard";
  plan.allow_incomplete = true;
  plan.auto_contrast = false;
  plan.use_gpu = false;
  plan.memory_mib = 2048U;
  plan.outputs.panorama.final_path =
#ifdef _WIN32
      (temporary.path() / "cpu-hard.png").u8string();
#else
      (temporary.path() / "cpu-hard.exr").u8string();
#endif
#ifdef _WIN32
  plan.outputs.coverage = pano::app::OutputTarget{
      (temporary.path() / "cpu-hard-coverage.png").u8string(), {}, false};
  plan.outputs.thumbnail = pano::app::OutputTarget{
      (temporary.path() / "cpu-hard-thumbnail.png").u8string(), {}, false};
#endif
  pano::app::NativePreviewOptions preview_options;
  preview_options.viewport_width = 8U;
  pano::app::CpuNativePreview *preview = nullptr;
  std::string error;
  auto uncovered = plan;
  uncovered.allow_incomplete = false;
  pano::app::CpuNativePreview *uncovered_preview = nullptr;
  EXPECT(!pano::app::create_cpu_native_preview(
      uncovered, preview_options, &uncovered_preview, error));
  EXPECT(uncovered_preview == nullptr &&
         error.find("cover every output pixel") != std::string::npos);

  pano_gpu_cancellation_token *preview_token = nullptr;
  std::array<char, 256> gpu_error{};
  EXPECT(pano_gpu_cancellation_token_create(
             &preview_token, gpu_error.data(),
             static_cast<std::uint32_t>(gpu_error.size())) == PANO_GPU_SUCCESS);
  pano::app::NativePreviewOptions token_cancelled_preview = preview_options;
  token_cancelled_preview.gpu_cancellation = preview_token;
  token_cancelled_preview.progress = cancel_gpu_on_progress;
  token_cancelled_preview.progress_user_data = preview_token;
  EXPECT(!pano::app::create_cpu_native_preview(
      plan, token_cancelled_preview, &uncovered_preview, error));
  EXPECT(uncovered_preview == nullptr && error.find("cancel") != std::string::npos);
  pano_gpu_cancellation_token_destroy(&preview_token);

  const bool cpu_preview_created = pano::app::create_cpu_native_preview(
      plan, preview_options, &preview, error);
  if (!cpu_preview_created)
    std::cerr << "CPU native preview creation failed: " << error << '\n';
  EXPECT(cpu_preview_created);
  pano::app::NativePreviewDiagnostics diagnostics;
  EXPECT(pano::app::query_cpu_native_preview(preview, diagnostics, error));
  EXPECT(diagnostics.preview_width == 8U);
  EXPECT(diagnostics.preview_height == 4U);
  EXPECT(pano::app::cpu_native_preview_pixels(preview).size() == 128U);
  pano::app::NativeExposureResult exposure;
  EXPECT(pano::app::discard_cpu_native_exposure_edits(
      preview, preview_options, exposure, error));
  EXPECT(exposure.gains == std::vector<float>({1.0F, 1.0F}));
  EXPECT(pano::app::apply_cpu_native_automatic_exposure(
      preview, 0U, preview_options, exposure, error));
  EXPECT(exposure.anchor_frame == 0U && exposure.gains.size() == 2U);
  EXPECT(pano::app::apply_cpu_native_manual_exposure_match(
      preview, 0U, {1U}, preview_options, exposure, error));
  EXPECT(exposure.anchor_frame == 0U && exposure.gains.size() == 2U);
  EXPECT(pano::app::discard_cpu_native_exposure_edits(
      preview, preview_options, exposure, error));
  EXPECT(exposure.gains == std::vector<float>({1.0F, 1.0F}));
  unsigned render_width = 0U;
  unsigned render_height = 0U;
  EXPECT(pano::app::query_cpu_native_render_dimensions(preview, render_width,
                                                       render_height, error));
  EXPECT(render_width == 8U && render_height == 4U);
  unsigned maximum_render_width = 0U;
  EXPECT(pano::app::query_cpu_native_maximum_render_width(
      preview, maximum_render_width, error));
  EXPECT(maximum_render_width != 0U);
  NativeRenderProgress progress;
  pano::app::NativeRenderOptions render_options;
  render_options.progress = record_native_render_progress;
  render_options.progress_user_data = &progress;
  pano::app::NativeRenderResult result;
  EXPECT(pano::app::render_cpu_native_session(preview, render_options, result,
                                              error));
#ifdef _WIN32
  constexpr std::size_t expected_cpu_paths = 3U;
#else
  constexpr std::size_t expected_cpu_paths = 1U;
#endif
  EXPECT(result.width == 8U && result.height == 4U &&
         result.published_paths.size() == expected_cpu_paths);
  EXPECT(!progress.values.empty() && progress.values.front() == 0U &&
         progress.values.back() == 100U && progress.total == 100U &&
         std::is_sorted(progress.values.begin(), progress.values.end()));
  for (const auto &path : result.published_paths)
    EXPECT(fs::exists(fs::u8path(path)));
  auto feather = plan;
  feather.blend = "feather";
  feather.outputs.panorama.final_path =
#ifdef _WIN32
      (temporary.path() / "cpu-feather.png").u8string();
#else
      (temporary.path() / "cpu-feather.exr").u8string();
#endif
  feather.outputs.coverage.reset();
  feather.outputs.thumbnail.reset();
  EXPECT(pano::app::update_cpu_native_preview_render_plan(preview, feather,
                                                          error));
  auto incompatible = feather;
  incompatible.session.session_id = "other";
  EXPECT(!pano::app::update_cpu_native_preview_render_plan(
      preview, incompatible, error));
  progress = {};
  EXPECT(pano::app::render_cpu_native_session(preview, render_options, result,
                                              error));
  EXPECT(!progress.values.empty() && progress.values.front() == 0U &&
         progress.values.back() == 100U && progress.total == 100U &&
         std::is_sorted(progress.values.begin(), progress.values.end()));
  EXPECT(result.published_paths ==
         std::vector<std::string>({feather.outputs.panorama.final_path}));
#ifdef _WIN32
  auto jpeg = feather;
  jpeg.outputs.panorama.final_path =
      (temporary.path() / "cpu-feather.jpg").u8string();
  jpeg.jpeg_quality = 87U;
  EXPECT(
      pano::app::update_cpu_native_preview_render_plan(preview, jpeg, error));
  EXPECT(pano::app::render_cpu_native_session(preview, render_options, result,
                                              error));
  pano::app::ImageInfo jpeg_info;
  pano::app::CodecErrorCategory jpeg_category{};
  EXPECT(pano::app::inspect_image(jpeg.outputs.panorama.final_path, jpeg_info,
                                  jpeg_category, error));
  EXPECT(jpeg_info.container == pano::app::ImageContainer::jpeg &&
         jpeg_info.width == 8U && jpeg_info.height == 4U);
#endif
  pano::app::NativeRenderOptions cancelled = render_options;
  pano_gpu_cancellation_token *render_token = nullptr;
  EXPECT(pano_gpu_cancellation_token_create(
             &render_token, gpu_error.data(),
             static_cast<std::uint32_t>(gpu_error.size())) == PANO_GPU_SUCCESS);
  cancelled.gpu_cancellation = render_token;
  cancelled.progress = cancel_gpu_on_progress;
  cancelled.progress_user_data = render_token;
  EXPECT(
      !pano::app::render_cpu_native_session(preview, cancelled, result, error));
  EXPECT(error.find("cancel") != std::string::npos);
  pano_gpu_cancellation_token_destroy(&render_token);
  pano::app::destroy_cpu_native_preview(&preview);
  pano::app::destroy_cpu_native_preview(&preview);
  EXPECT(preview == nullptr);
  EXPECT(!pano::app::query_cpu_native_preview(preview, diagnostics, error));
}
} // namespace

int main() {
  if (!fs::is_directory(fixtures())) {
    std::cerr << "application contract fixture directory does not exist: "
              << fixtures() << '\n';
    return 1;
  }
  if (!fs::is_directory(codec_fixtures())) {
    std::cerr << "codec contract fixture directory does not exist: "
              << codec_fixtures() << '\n';
    return 1;
  }
  test_dispatch();
  test_path_and_size_options();
  test_format_and_render_options();
  test_memory_backend_and_exposure_options();
  test_shared_session();
  test_cet_session_and_paths();
  test_output_and_render_plans();
  test_render_dispatch();
  test_sdr_codec_contracts();
  test_pq_png_codec_contracts();
  test_exr_codec_contracts();
  test_sdr_writer_contracts();
  test_exr_writer_contracts();
  test_output_publication_contracts();
  test_cpu_render_planning();
  test_cpu_render_storage();
  test_cpu_worker_pool();
  test_cpu_one_frame_hard_composition();
  test_cpu_multiframe_feather_and_gains();
  test_cpu_exposure_proxies_and_pairs();
  test_cpu_exposure_classification_and_reduction();
  test_cpu_exposure_graph_report_and_cache();
  test_reference_exposure_propagation();
  test_cpu_conversion_and_writer_bands();
  test_cpu_failure_cancellation_and_concurrency();
  test_gui_session_discovery_state();
  test_gui_request_and_validation_state();
  test_gui_workflow_state();
  test_gui_preview_crop_state();
  test_gui_exposure_state();
  test_application_settings_history_and_deletion();
  test_cpu_native_session();
  test_sdr_d3d12_upload();
  return failures == 0 ? 0 : 1;
}
