#include "pano_app.h"

#include "yyjson.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace pano::app {
namespace {
namespace fs = std::filesystem;

class Document {
public:
  explicit Document(yyjson_doc *document) : document_(document) {}
  ~Document() { yyjson_doc_free(document_); }
  Document(const Document &) = delete;
  Document &operator=(const Document &) = delete;
  yyjson_val *root() const { return yyjson_doc_get_root(document_); }

private:
  yyjson_doc *document_;
};

bool fail(std::string &error, const std::string &location,
          const std::string &detail) {
  error = "invalid session metadata at " + location + ": " + detail;
  return false;
}

bool allowed_keys(yyjson_val *object,
                  const std::initializer_list<const char *> allowed,
                  std::string &error, const std::string &location) {
  auto iterator = yyjson_obj_iter_with(object);
  while (auto *key = yyjson_obj_iter_next(&iterator)) {
    const std::string name = yyjson_get_str(key);
    if (std::find_if(allowed.begin(), allowed.end(),
                     [&name](const char *candidate) {
                       return name == candidate;
                     }) == allowed.end()) {
      return fail(error, location + "." + name, "is not an allowed property");
    }
  }
  return true;
}

bool required_object(yyjson_val *parent, const char *key, yyjson_val *&value,
                     std::string &error, const std::string &location) {
  value = yyjson_obj_get(parent, key);
  return yyjson_is_obj(value) || fail(error, location, "must be an object");
}

bool required_array(yyjson_val *parent, const char *key, yyjson_val *&value,
                    std::string &error, const std::string &location) {
  value = yyjson_obj_get(parent, key);
  return yyjson_is_arr(value) || fail(error, location, "must be an array");
}

bool required_string(yyjson_val *parent, const char *key, std::string &value,
                     std::string &error, const std::string &location) {
  auto *node = yyjson_obj_get(parent, key);
  if (!yyjson_is_str(node) || yyjson_get_len(node) == 0) {
    return fail(error, location, "must be a non-empty string");
  }
  value = yyjson_get_str(node);
  return true;
}

bool required_unsigned(yyjson_val *parent, const char *key, unsigned &value,
                       std::string &error, const std::string &location,
                       const bool positive = false) {
  auto *node = yyjson_obj_get(parent, key);
  if (!yyjson_is_uint(node) ||
      yyjson_get_uint(node) > std::numeric_limits<unsigned>::max() ||
      (positive && yyjson_get_uint(node) == 0)) {
    return fail(error, location,
                positive ? "must be a positive integer"
                         : "must be a non-negative integer");
  }
  value = static_cast<unsigned>(yyjson_get_uint(node));
  return true;
}

bool required_number(yyjson_val *parent, const char *key, double &value,
                     std::string &error, const std::string &location) {
  auto *node = yyjson_obj_get(parent, key);
  if (!yyjson_is_num(node))
    return fail(error, location, "must be a number");
  value = yyjson_get_num(node);
  return std::isfinite(value) || fail(error, location, "must be finite");
}

bool number_in_range(yyjson_val *parent, const char *key, double &value,
                     std::string &error, const std::string &location,
                     const double minimum, const double maximum,
                     const bool minimum_inclusive) {
  if (!required_number(parent, key, value, error, location))
    return false;
  const bool valid_minimum =
      minimum_inclusive ? value >= minimum : value > minimum;
  return (valid_minimum && value < maximum) ||
         fail(error, location, "is outside its allowed range");
}

bool required_bool(yyjson_val *parent, const char *key, bool &value,
                   std::string &error, const std::string &location) {
  auto *node = yyjson_obj_get(parent, key);
  if (!yyjson_is_bool(node))
    return fail(error, location, "must be a boolean");
  value = yyjson_get_bool(node);
  return true;
}

bool optional_nonempty_string(yyjson_val *parent, const char *key,
                              std::string &error, const std::string &location) {
  auto *node = yyjson_obj_get(parent, key);
  return node == nullptr || (yyjson_is_str(node) && yyjson_get_len(node) > 0) ||
         fail(error, location, "must be a non-empty string");
}

bool optional_string(yyjson_val *parent, const char *key, std::string &error,
                     const std::string &location) {
  auto *node = yyjson_obj_get(parent, key);
  return node == nullptr || yyjson_is_str(node) ||
         fail(error, location, "must be a string");
}

bool string_choice(yyjson_val *parent, const char *key, std::string &value,
                   const std::vector<std::string> &choices, std::string &error,
                   const std::string &location) {
  if (!required_string(parent, key, value, error, location))
    return false;
  return std::find(choices.begin(), choices.end(), value) != choices.end() ||
         fail(error, location, "has an unsupported value");
}

template <std::size_t Size>
bool number_array(yyjson_val *value, std::array<double, Size> &result,
                  std::string &error, const std::string &location) {
  if (!yyjson_is_arr(value) || yyjson_arr_size(value) != Size) {
    return fail(error, location, "has the wrong number of values");
  }
  std::size_t index = 0;
  std::size_t count = 0;
  yyjson_val *item = nullptr;
  yyjson_arr_foreach(value, index, count, item) {
    if (!yyjson_is_num(item) || !std::isfinite(yyjson_get_num(item))) {
      return fail(error, location, "must contain only finite numbers");
    }
    result[index] = yyjson_get_num(item);
  }
  return true;
}

bool parse_encoding(yyjson_val *root, ImageEncoding &encoding,
                    std::string &error) {
  auto *value = yyjson_obj_get(root, "image_encoding");
  if (value == nullptr)
    return true;
  if (!yyjson_is_obj(value))
    return fail(error, "image_encoding", "must be an object");
  return allowed_keys(value,
                      {"sample_type", "color_primaries", "transfer_function",
                       "reference_white_nits"},
                      error, "image_encoding") &&
         string_choice(value, "sample_type", encoding.sample_type,
                       {"uint8", "uint16", "float16", "float32"}, error,
                       "image_encoding.sample_type") &&
         string_choice(value, "color_primaries", encoding.color_primaries,
                       {"srgb", "rec2020"}, error,
                       "image_encoding.color_primaries") &&
         string_choice(value, "transfer_function", encoding.transfer_function,
                       {"srgb", "pq", "linear"}, error,
                       "image_encoding.transfer_function") &&
         required_number(value, "reference_white_nits",
                         encoding.reference_white_nits, error,
                         "image_encoding.reference_white_nits") &&
         (encoding.reference_white_nits > 0.0 ||
          fail(error, "image_encoding.reference_white_nits",
               "must be greater than 0"));
}

bool parse_shared_frame(yyjson_val *value, const std::size_t position,
                        FrameSummary &frame, std::string &error) {
  const auto prefix = "frames." + std::to_string(position);
  if (!yyjson_is_obj(value))
    return fail(error, prefix, "must be an object");
  if (!allowed_keys(value,
                    {"index", "filename", "yaw_deg", "pitch_deg", "roll_deg",
                     "position", "orientation_xyzw", "camera_basis_row_major",
                     "observed_forward", "observed_right", "observed_up",
                     "view_matrix_row_major", "projection_matrix_row_major",
                     "settled_real_frames", "real_fps_before_capture",
                     "status"},
                    error, prefix) ||
      !required_unsigned(value, "index", frame.index, error,
                         prefix + ".index") ||
      !required_string(value, "filename", frame.filename, error,
                       prefix + ".filename") ||
      !required_number(value, "yaw_deg", frame.yaw_deg, error,
                       prefix + ".yaw_deg") ||
      !required_number(value, "pitch_deg", frame.pitch_deg, error,
                       prefix + ".pitch_deg") ||
      !required_number(value, "roll_deg", frame.roll_deg, error,
                       prefix + ".roll_deg") ||
      !string_choice(value, "status", frame.status,
                     {"planned", "captured", "failed"}, error,
                     prefix + ".status")) {
    return false;
  }
  if (auto *basis = yyjson_obj_get(value, "camera_basis_row_major");
      basis != nullptr) {
    std::array<double, 9> parsed{};
    if (!number_array(basis, parsed, error, prefix + ".camera_basis_row_major"))
      return false;
    frame.camera_basis_row_major = parsed;
  }
  for (const auto *key :
       {"position", "observed_forward", "observed_right", "observed_up"}) {
    if (auto *vector = yyjson_obj_get(value, key); vector != nullptr) {
      std::array<double, 3> ignored{};
      if (!number_array(vector, ignored, error, prefix + "." + key))
        return false;
    }
  }
  if (auto *orientation = yyjson_obj_get(value, "orientation_xyzw");
      orientation != nullptr) {
    std::array<double, 4> ignored{};
    if (!number_array(orientation, ignored, error,
                      prefix + ".orientation_xyzw"))
      return false;
  }
  for (const auto *key :
       {"view_matrix_row_major", "projection_matrix_row_major"}) {
    if (auto *matrix = yyjson_obj_get(value, key); matrix != nullptr) {
      std::array<double, 16> ignored{};
      if (!number_array(matrix, ignored, error, prefix + "." + key))
        return false;
    }
  }
  if (auto *settled = yyjson_obj_get(value, "settled_real_frames");
      settled != nullptr && !yyjson_is_uint(settled)) {
    return fail(error, prefix + ".settled_real_frames",
                "must be a non-negative integer");
  }
  if (auto *fps = yyjson_obj_get(value, "real_fps_before_capture");
      fps != nullptr &&
      (!yyjson_is_num(fps) || !std::isfinite(yyjson_get_num(fps)))) {
    return fail(error, prefix + ".real_fps_before_capture",
                "must be a finite number");
  }
  return true;
}

bool validate_render_timing(yyjson_val *root, std::string &error) {
  auto *timing = yyjson_obj_get(root, "render_timing");
  if (timing == nullptr)
    return true;
  if (!yyjson_is_obj(timing))
    return fail(error, "render_timing", "must be an object");
  if (!allowed_keys(timing,
                    {"required_real_settle_frames", "real_fps_at_start",
                     "presented_fps_at_start", "frame_generation"},
                    error, "render_timing"))
    return false;
  if (auto *settled = yyjson_obj_get(timing, "required_real_settle_frames");
      settled != nullptr && !yyjson_is_uint(settled)) {
    return fail(error, "render_timing.required_real_settle_frames",
                "must be a non-negative integer");
  }
  for (const auto *key : {"real_fps_at_start", "presented_fps_at_start"}) {
    if (auto *number = yyjson_obj_get(timing, key);
        number != nullptr &&
        (!yyjson_is_num(number) || !std::isfinite(yyjson_get_num(number)))) {
      return fail(error, "render_timing." + std::string(key),
                  "must be a finite number");
    }
  }
  auto *generation = yyjson_obj_get(timing, "frame_generation");
  if (generation == nullptr)
    return true;
  if (!yyjson_is_obj(generation)) {
    return fail(error, "render_timing.frame_generation", "must be an object");
  }
  if (!allowed_keys(generation, {"enabled", "active_backend", "raw_settings"},
                    error, "render_timing.frame_generation"))
    return false;
  if (auto *enabled = yyjson_obj_get(generation, "enabled");
      enabled != nullptr && !yyjson_is_bool(enabled)) {
    return fail(error, "render_timing.frame_generation.enabled",
                "must be a boolean");
  }
  if (auto *backend = yyjson_obj_get(generation, "active_backend");
      backend != nullptr && !yyjson_is_str(backend)) {
    return fail(error, "render_timing.frame_generation.active_backend",
                "must be a string");
  }
  if (auto *settings = yyjson_obj_get(generation, "raw_settings");
      settings != nullptr && !yyjson_is_obj(settings)) {
    return fail(error, "render_timing.frame_generation.raw_settings",
                "must be an object");
  }
  return true;
}

bool parse_shared(yyjson_val *root, SessionSummary &session,
                  std::string &error) {
  std::string projection;
  std::string fov_source;
  yyjson_val *viewport = nullptr;
  yyjson_val *fov = nullptr;
  yyjson_val *planner = nullptr;
  yyjson_val *frames = nullptr;
  yyjson_val *base_pose = nullptr;
  unsigned viewport_width = 0;
  unsigned viewport_height = 0;
  if (!allowed_keys(root,
                    {"schema_version", "session_id", "game_version",
                     "red4ext_version", "mod_version", "created_utc",
                     "capture_mode", "projection", "viewport", "image_encoding",
                     "fov", "base_pose", "location", "planner", "render_timing",
                     "frames", "completed"},
                    error, "root")) {
    return false;
  }
  if (!required_unsigned(root, "schema_version", session.schema_version, error,
                         "schema_version")) {
    return false;
  }
  if (session.schema_version != 1)
    return fail(error, "schema_version", "must be 1");
  if (!required_string(root, "session_id", session.session_id, error,
                       "session_id") ||
      !string_choice(root, "capture_mode", session.capture_mode,
                     {"horizontal", "full_sphere"}, error, "capture_mode") ||
      !string_choice(root, "projection", projection, {"rectilinear"}, error,
                     "projection") ||
      !required_object(root, "viewport", viewport, error, "viewport") ||
      !allowed_keys(viewport, {"width", "height"}, error, "viewport") ||
      !required_unsigned(viewport, "width", viewport_width, error,
                         "viewport.width", true) ||
      !required_unsigned(viewport, "height", viewport_height, error,
                         "viewport.height", true) ||
      !required_object(root, "fov", fov, error, "fov") ||
      !allowed_keys(fov, {"horizontal_deg", "vertical_deg", "source"}, error,
                    "fov") ||
      !number_in_range(fov, "horizontal_deg", session.horizontal_fov_deg, error,
                       "fov.horizontal_deg", 0.0, 180.0, false) ||
      !number_in_range(fov, "vertical_deg", session.vertical_fov_deg, error,
                       "fov.vertical_deg", 0.0, 180.0, false) ||
      !string_choice(fov, "source", fov_source,
                     {"render_projection_matrix", "configured_value"}, error,
                     "fov.source") ||
      !required_object(root, "base_pose", base_pose, error, "base_pose") ||
      !allowed_keys(base_pose, {"position", "orientation_xyzw"}, error,
                    "base_pose") ||
      !required_object(root, "planner", planner, error, "planner") ||
      !allowed_keys(planner,
                    {"overlap_fraction", "yaw_step_deg", "pitch_step_deg"},
                    error, "planner") ||
      !number_in_range(planner, "overlap_fraction", session.overlap_fraction,
                       error, "planner.overlap_fraction", 0.0, 1.0, true) ||
      !required_array(root, "frames", frames, error, "frames") ||
      !required_bool(root, "completed", session.completed, error,
                     "completed") ||
      !parse_encoding(root, session.image_encoding, error) ||
      !validate_render_timing(root, error)) {
    return false;
  }
  for (const auto *key : {"game_version", "red4ext_version", "mod_version"}) {
    if (!optional_nonempty_string(root, key, error, key))
      return false;
  }
  if (!optional_string(root, "created_utc", error, "created_utc"))
    return false;
  std::array<double, 3> base_position{};
  std::array<double, 4> base_orientation{};
  if (!number_array(yyjson_obj_get(base_pose, "position"), base_position, error,
                    "base_pose.position") ||
      !number_array(yyjson_obj_get(base_pose, "orientation_xyzw"),
                    base_orientation, error, "base_pose.orientation_xyzw")) {
    return false;
  }
  if (auto *location = yyjson_obj_get(root, "location"); location != nullptr) {
    if (!yyjson_is_obj(location) ||
        !allowed_keys(location, {"position", "yaw_deg"}, error, "location")) {
      if (!yyjson_is_obj(location))
        fail(error, "location", "must be an object");
      return false;
    }
    std::array<double, 3> position{};
    double yaw = 0.0;
    if (!number_array(yyjson_obj_get(location, "position"), position, error,
                      "location.position") ||
        !required_number(location, "yaw_deg", yaw, error, "location.yaw_deg")) {
      return false;
    }
  }
  double ignored_step = 0.0;
  if (!required_number(planner, "yaw_step_deg", ignored_step, error,
                       "planner.yaw_step_deg") ||
      ignored_step <= 0.0 || ignored_step > 360.0) {
    if (error.empty())
      fail(error, "planner.yaw_step_deg", "is outside its allowed range");
    return false;
  }
  if (!required_number(planner, "pitch_step_deg", ignored_step, error,
                       "planner.pitch_step_deg") ||
      ignored_step <= 0.0 || ignored_step > 180.0) {
    if (error.empty())
      fail(error, "planner.pitch_step_deg", "is outside its allowed range");
    return false;
  }
  session.projection = projection;
  std::size_t index = 0;
  std::size_t count = 0;
  yyjson_val *frame_value = nullptr;
  yyjson_arr_foreach(frames, index, count, frame_value) {
    FrameSummary frame;
    if (!parse_shared_frame(frame_value, index, frame, error))
      return false;
    session.frames.push_back(std::move(frame));
  }
  return true;
}

bool cet_vector(yyjson_val *pose, const char *key,
                std::array<double, 3> &vector, std::string &error,
                const std::string &location) {
  auto *value = yyjson_obj_get(pose, key);
  return number_array(value, vector, error, location);
}

std::array<double, 3> converted(const std::array<double, 3> &value) {
  return {value[0], value[2], value[1]};
}

double dot(const std::array<double, 3> &left,
           const std::array<double, 3> &right) {
  return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

std::array<double, 3> relative(const std::array<double, 3> &raw,
                               const std::array<double, 3> &capture_right,
                               const std::array<double, 3> &capture_forward) {
  const auto world = converted(raw);
  return {dot(world, capture_right), world[1], dot(world, capture_forward)};
}

std::string native_path_string(fs::path path) {
  path.make_preferred();
  return path.u8string();
}

std::string
portable_filename(const std::string &raw, const fs::path &metadata_path,
                  const std::optional<std::string> &image_directory) {
  const auto native = fs::u8path(raw);
  std::error_code status_error;
  if (fs::is_regular_file(native, status_error))
    return native_path_string(native);
  const auto separator = raw.find_last_of("/\\");
  const auto basename =
      separator == std::string::npos ? raw : raw.substr(separator + 1);
  if (image_directory.has_value()) {
    const auto candidate = fs::u8path(*image_directory) / fs::u8path(basename);
    status_error.clear();
    if (fs::is_regular_file(candidate, status_error))
      return native_path_string(candidate);
  }
  const auto sibling = metadata_path.parent_path() / fs::u8path(basename);
  status_error.clear();
  if (fs::is_regular_file(sibling, status_error))
    return native_path_string(sibling);
  return basename;
}

bool parse_cet(yyjson_val *root, const fs::path &path,
               const std::optional<std::string> &image_directory,
               SessionSummary &session, std::string &error) {
  std::string state;
  yyjson_val *poses = nullptr;
  if (!required_unsigned(root, "schema_version", session.schema_version, error,
                         "schema_version")) {
    return false;
  }
  if (session.schema_version != 1)
    return fail(error, "schema_version", "must be 1");
  if (!required_string(root, "session_id", session.session_id, error,
                       "session_id") ||
      !number_in_range(root, "horizontal_fov_deg", session.horizontal_fov_deg,
                       error, "horizontal_fov_deg", 0.0, 180.0, false) ||
      !number_in_range(root, "vertical_fov_deg", session.vertical_fov_deg,
                       error, "vertical_fov_deg", 0.0, 180.0, false) ||
      !string_choice(root, "state", state,
                     {"active", "completed", "aborted", "failed"}, error,
                     "state") ||
      !required_array(root, "poses", poses, error, "poses")) {
    return false;
  }
  if (yyjson_arr_size(poses) == 0)
    return fail(error, "poses", "must contain at least one pose");

  yyjson_val *reference = nullptr;
  double reference_distance = std::numeric_limits<double>::infinity();
  std::size_t index = 0;
  std::size_t count = 0;
  yyjson_val *pose = nullptr;
  yyjson_arr_foreach(poses, index, count, pose) {
    double yaw = 0.0;
    if (!yyjson_is_obj(pose) ||
        !required_number(pose, "commanded_yaw_deg", yaw, error,
                         "poses." + std::to_string(index) +
                             ".commanded_yaw_deg")) {
      return false;
    }
    const auto distance = std::abs(std::remainder(yaw, 360.0));
    if (distance < reference_distance) {
      reference = pose;
      reference_distance = distance;
    }
  }
  std::array<double, 3> raw_reference_right{};
  if (!cet_vector(reference, "right", raw_reference_right, error,
                  "poses.reference.right")) {
    return false;
  }
  const auto world_right = converted(raw_reference_right);
  const auto horizontal_length = std::hypot(world_right[0], world_right[2]);
  if (horizontal_length < 1.0e-6) {
    return fail(error, "poses.reference.right",
                "yaw-zero camera right vector is not horizontal");
  }
  const std::array<double, 3> capture_right = {
      world_right[0] / horizontal_length, 0.0,
      world_right[2] / horizontal_length};
  const std::array<double, 3> capture_forward = {-capture_right[2], 0.0,
                                                 capture_right[0]};

  yyjson_arr_foreach(poses, index, count, pose) {
    const auto prefix = "poses." + std::to_string(index);
    FrameSummary frame;
    std::array<double, 3> right{};
    std::array<double, 3> up{};
    std::array<double, 3> forward{};
    std::string screenshot_path;
    if (!required_unsigned(pose, "index", frame.index, error,
                           prefix + ".index") ||
        !required_string(pose, "screenshot_path", screenshot_path, error,
                         prefix + ".screenshot_path") ||
        !required_number(pose, "commanded_yaw_deg", frame.yaw_deg, error,
                         prefix + ".commanded_yaw_deg") ||
        !required_number(pose, "commanded_pitch_deg", frame.pitch_deg, error,
                         prefix + ".commanded_pitch_deg") ||
        !cet_vector(pose, "right", right, error, prefix + ".right") ||
        !cet_vector(pose, "up", up, error, prefix + ".up") ||
        !cet_vector(pose, "forward", forward, error, prefix + ".forward")) {
      return false;
    }
    frame.filename = portable_filename(screenshot_path, path, image_directory);
    frame.status = "captured";
    const auto relative_right = relative(right, capture_right, capture_forward);
    const auto relative_up = relative(up, capture_right, capture_forward);
    const auto relative_forward =
        relative(forward, capture_right, capture_forward);
    frame.camera_basis_row_major = std::array<double, 9>{
        relative_right[0],   relative_right[1],   relative_right[2],
        relative_up[0],      relative_up[1],      relative_up[2],
        relative_forward[0], relative_forward[1], relative_forward[2]};
    session.frames.push_back(std::move(frame));
  }
  session.capture_mode = "full_sphere";
  session.overlap_fraction = 0.08;
  session.completed = state == "completed";
  session.image_encoding = {"uint16", "rec2020", "pq", 203.0};
  return true;
}
} // namespace

bool load_session(const std::string &path_text,
                  const std::optional<std::string> &image_directory,
                  SessionSummary &session, std::string &error) {
  error.clear();
  const auto path = fs::u8path(path_text);
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    error = "cannot open session metadata: " + path_text;
    return false;
  }
  std::string json{std::istreambuf_iterator<char>(stream),
                   std::istreambuf_iterator<char>()};
  yyjson_read_err read_error{};
  auto *raw_document = yyjson_read_opts(
      json.data(), json.size(), YYJSON_READ_NOFLAG, nullptr, &read_error);
  if (raw_document == nullptr) {
    error = "invalid session JSON at byte " + std::to_string(read_error.pos) +
            ": " +
            (read_error.msg == nullptr ? "parse failure" : read_error.msg);
    return false;
  }
  Document document(raw_document);
  auto *root = document.root();
  if (!yyjson_is_obj(root))
    return fail(error, "root", "must be an object");

  SessionSummary parsed;
  const bool is_cet = yyjson_obj_get(root, "poses") != nullptr &&
                      yyjson_obj_get(root, "state") != nullptr;
  if (!(is_cet ? parse_cet(root, path, image_directory, parsed, error)
               : parse_shared(root, parsed, error))) {
    return false;
  }
  session = std::move(parsed);
  return true;
}
} // namespace pano::app
