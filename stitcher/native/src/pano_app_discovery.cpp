#include "pano_app.h"

#include <algorithm>
#include <charconv>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <locale>
#include <sstream>
#include <unordered_set>

namespace pano::app {
namespace {
namespace fs = std::filesystem;

constexpr const char *session_prefix = "PanoramaCaptureBridge.pano-";
constexpr const char *session_suffix = ".json";

bool is_session_filename(const std::string &name) {
  return name.size() > std::char_traits<char>::length(session_prefix) +
                           std::char_traits<char>::length(session_suffix) &&
         name.compare(0, std::char_traits<char>::length(session_prefix),
                      session_prefix) == 0 &&
         name.compare(name.size() - std::char_traits<char>::length(session_suffix),
                      std::char_traits<char>::length(session_suffix),
                      session_suffix) == 0;
}

} // namespace

bool discover_gui_sessions(const std::string &game_directory,
                           std::vector<GuiSessionRecord> &records,
                           std::string &error) {
  if (game_directory.empty()) {
    error = "game directory is empty";
    return false;
  }
  const fs::path directory =
      fs::u8path(game_directory) / "bin" / "x64" / "plugins" /
      "cyber_engine_tweaks" / "mods" / "PanoramaCaptureProbe";
  std::error_code status_error;
  const bool exists = fs::exists(directory, status_error);
  if (status_error) {
    error = "cannot inspect game mod directory";
    return false;
  }
  if (!exists) {
    records.clear();
    error.clear();
    return true;
  }
  if (!fs::is_directory(directory, status_error) || status_error) {
    error = "game mod path is not a readable directory";
    return false;
  }
  std::vector<fs::path> paths;
  try {
    for (const auto &entry : fs::directory_iterator(directory))
      if (entry.is_regular_file() &&
          is_session_filename(entry.path().filename().u8string()))
        paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end(), [](const fs::path &left,
                                             const fs::path &right) {
      return left.filename().u8string() > right.filename().u8string();
    });
    std::vector<GuiSessionRecord> discovered;
    discovered.reserve(paths.size());
    for (const auto &path : paths) {
      GuiSessionRecord record;
      record.path = path.u8string();
      std::string detail;
      if (!load_session(record.path, directory.u8string(), record.session,
                        detail)) {
        const std::string filename = path.filename().u8string();
        record.session.session_id = filename.substr(
            std::char_traits<char>::length(session_prefix),
            filename.size() - std::char_traits<char>::length(session_prefix) -
                std::char_traits<char>::length(session_suffix));
        record.error = detail;
      } else {
        std::unordered_set<std::string> seen;
        for (const auto &frame : record.session.frames)
          if (seen.insert(frame.filename).second)
            record.image_paths.push_back(frame.filename);
      }
      discovered.push_back(std::move(record));
    }
    records.swap(discovered);
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate session discovery results";
    return false;
  } catch (const fs::filesystem_error &) {
    error = "cannot enumerate game mod directory";
    return false;
  }
}

GuiSessionStatus gui_session_status(const GuiSessionRecord &record,
                                    const bool stitched) noexcept {
  if (!record.error.empty()) return GuiSessionStatus::invalid;
  if (!record.session.completed) return GuiSessionStatus::incomplete;
  return stitched ? GuiSessionStatus::stitched : GuiSessionStatus::complete;
}

std::string gui_session_local_label(const std::string &session_id) {
  const auto separator = session_id.find('-');
  const auto timestamp_text = session_id.substr(0, separator);
  std::int64_t timestamp = 0;
  const auto parsed = std::from_chars(
      timestamp_text.data(), timestamp_text.data() + timestamp_text.size(),
      timestamp);
  if (timestamp_text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != timestamp_text.data() + timestamp_text.size())
    return session_id;
  const auto time = static_cast<std::time_t>(timestamp);
  std::tm local{};
#ifdef _WIN32
  if (localtime_s(&local, &time) != 0) return session_id;
#else
  if (localtime_r(&time, &local) == nullptr) return session_id;
#endif
  std::ostringstream label;
  label << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
  if (separator != std::string::npos)
    label << "  #" << session_id.substr(separator + 1U);
  return label.str();
}

std::optional<std::string>
gui_session_coordinates(const SessionSummary &session) {
  if (!session.location.has_value())
    return std::nullopt;
  std::ostringstream coordinates;
  coordinates.imbue(std::locale::classic());
  coordinates << std::fixed << std::setprecision(9)
              << session.location->position[0] << ", "
              << session.location->position[1] << ", "
              << session.location->position[2];
  return coordinates.str();
}

std::uint64_t begin_gui_session_refresh(GuiRefreshState &state) noexcept {
  return ++state.generation;
}

bool complete_gui_session_refresh(
    GuiRefreshState &state, const std::uint64_t generation,
    std::vector<GuiSessionRecord> records) noexcept {
  if (generation != state.generation) return false;
  state.records.swap(records);
  return true;
}

} // namespace pano::app
