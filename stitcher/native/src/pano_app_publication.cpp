#include "pano_app.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pano::app {
namespace {
namespace fs = std::filesystem;
constexpr const char *marker_contents = "pano-stitch-native-stage-v1\n";
std::atomic<std::uint64_t> next_stage_id{0};

bool fail(const CodecErrorCategory value, const std::string &message,
          CodecErrorCategory &category, std::string &error) {
  category = value;
  error = message;
  return false;
}

bool inject(const PublicationFaultCheck &fault, const char *boundary,
            CodecErrorCategory &category, std::string &error) {
  if (fault.callback == nullptr || !fault.callback(fault.user_data, boundary))
    return false;
  fail(CodecErrorCategory::io,
       std::string("injected publication failure at ") + boundary, category,
       error);
  return true;
}

bool read_marker(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return false;
  const std::string contents{std::istreambuf_iterator<char>(stream),
                             std::istreambuf_iterator<char>()};
  return contents == marker_contents;
}

bool create_marker_exclusive(const fs::path &path) {
  const std::string contents = marker_contents;
#ifdef _WIN32
  const HANDLE handle = CreateFileW(
      path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool success =
      WriteFile(handle, contents.data(), static_cast<DWORD>(contents.size()),
                &written, nullptr) != FALSE &&
      written == static_cast<DWORD>(contents.size()) &&
      FlushFileBuffers(handle) != FALSE;
  CloseHandle(handle);
#else
  const int descriptor =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (descriptor < 0) return false;
  const ssize_t written = ::write(descriptor, contents.data(), contents.size());
  const bool success = written == static_cast<ssize_t>(contents.size()) &&
                       ::fsync(descriptor) == 0;
  ::close(descriptor);
#endif
  if (!success) {
    std::error_code ignored;
    fs::remove(path, ignored);
  }
  return success;
}
} // namespace

class OutputStage {
public:
  OutputStage(std::string final, std::string staged, std::string marker)
      : destination(std::move(final)), stage(std::move(staged)),
        marker_path(std::move(marker)) {}
  ~OutputStage() { cleanup(); }

  void cleanup() noexcept {
    if (!cleanup_owned) return;
    std::error_code ignored;
    fs::remove(fs::u8path(stage), ignored);
    fs::remove(fs::u8path(marker_path), ignored);
    cleanup_owned = false;
  }

  void preserve_for_recovery() noexcept { cleanup_owned = false; }

  std::string destination;
  std::string stage;
  std::string marker_path;
  bool cleanup_owned = true;
};

bool create_output_stage(const std::string &destination,
                         OutputStage **const stage,
                         CodecErrorCategory &category, std::string &error) {
  if (stage == nullptr)
    return fail(CodecErrorCategory::invalid_request,
                "output stage out-handle is null", category, error);
  *stage = nullptr;
  if (destination.empty())
    return fail(CodecErrorCategory::invalid_request,
                "output stage destination is empty", category, error);
  try {
    const fs::path final_path = fs::u8path(destination);
    const fs::path parent = final_path.parent_path().empty()
                                ? fs::current_path()
                                : final_path.parent_path();
    std::error_code status_error;
    if (!fs::is_directory(parent, status_error) || status_error)
      return fail(CodecErrorCategory::io,
                  "output stage directory does not exist", category, error);
    const auto nonce = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
      const auto id = next_stage_id.fetch_add(1, std::memory_order_relaxed);
      const std::string token = std::to_string(nonce) + "-" +
                                std::to_string(id) + "-" +
                                std::to_string(attempt);
      const fs::path staged =
          parent / fs::u8path("." + final_path.filename().u8string() + "." +
                              token + ".partial");
      const fs::path marker = fs::u8path(staged.u8string() + ".owner");
      if (fs::exists(staged, status_error) || status_error) {
        status_error.clear();
        continue;
      }
      if (fs::exists(marker, status_error) || status_error) {
        status_error.clear();
        continue;
      }
      if (!create_marker_exclusive(marker)) continue;
      *stage = new OutputStage(final_path.u8string(), staged.u8string(),
                               marker.u8string());
      category = CodecErrorCategory::none;
      error.clear();
      return true;
    }
    return fail(CodecErrorCategory::io,
                "cannot allocate a collision-free output stage", category,
                error);
  } catch (const std::bad_alloc &) {
    return fail(CodecErrorCategory::allocation,
                "cannot allocate output stage", category, error);
  } catch (...) {
    return fail(CodecErrorCategory::io,
                "unexpected output stage creation failure", category, error);
  }
}

const std::string &output_stage_path(const OutputStage *const stage) noexcept {
  static const std::string empty;
  return stage == nullptr ? empty : stage->stage;
}

bool publish_output_stages(const std::vector<OutputStage *> &stages,
                           const PublicationFaultCheck &fault,
                           CodecErrorCategory &category, std::string &error) {
  if (stages.empty())
    return fail(CodecErrorCategory::invalid_request,
                "publication transaction has no outputs", category, error);
  std::unordered_set<std::string> destinations;
  for (const OutputStage *stage : stages) {
    if (stage == nullptr || !stage->cleanup_owned ||
        !destinations.insert(stage->destination).second ||
        !fs::is_regular_file(fs::u8path(stage->stage)) ||
        !read_marker(fs::u8path(stage->marker_path)))
      return fail(CodecErrorCategory::invalid_request,
                  "invalid or incomplete owned output stage", category,
                  error);
  }

  std::vector<OutputStage *> backups;
  std::vector<OutputStage *> published;
  bool transaction_failed = false;
  try {
    for (OutputStage *source : stages) {
      if (!fs::exists(fs::u8path(source->destination))) continue;
      if (inject(fault, "before_backup", category, error)) {
        transaction_failed = true;
        break;
      }
      OutputStage *backup = nullptr;
      if (!create_output_stage(source->destination, &backup, category, error)) {
        transaction_failed = true;
        break;
      }
      fs::rename(fs::u8path(source->destination), fs::u8path(backup->stage));
      backups.push_back(backup);
      if (inject(fault, "after_backup", category, error)) {
        transaction_failed = true;
        break;
      }
    }
    if (!transaction_failed)
      for (OutputStage *source : stages) {
        if (inject(fault, "before_publish", category, error)) {
          transaction_failed = true;
          break;
        }
        fs::rename(fs::u8path(source->stage),
                   fs::u8path(source->destination));
        published.push_back(source);
        if (inject(fault, "after_publish", category, error)) {
          transaction_failed = true;
          break;
        }
      }
    if (!transaction_failed &&
        inject(fault, "before_cleanup", category, error))
      transaction_failed = true;
  } catch (const fs::filesystem_error &failure) {
    category = CodecErrorCategory::io;
    error = "output publication failed: " + std::string(failure.what());
    transaction_failed = true;
  } catch (const std::bad_alloc &) {
    category = CodecErrorCategory::allocation;
    error = "cannot allocate output publication transaction";
    transaction_failed = true;
  } catch (...) {
    category = CodecErrorCategory::io;
    error = "unexpected output publication failure";
    transaction_failed = true;
  }

  if (transaction_failed) {
    std::error_code ignored;
    for (auto iterator = published.rbegin(); iterator != published.rend();
         ++iterator)
      fs::remove(fs::u8path((*iterator)->destination), ignored);
    bool restored = true;
    for (auto iterator = backups.rbegin(); iterator != backups.rend();
         ++iterator) {
      std::error_code restore_error;
      fs::rename(fs::u8path((*iterator)->stage),
                 fs::u8path((*iterator)->destination), restore_error);
      if (restore_error) {
        (*iterator)->preserve_for_recovery();
        restored = false;
      }
    }
    for (OutputStage *backup : backups) delete backup;
    if (!restored) error += "; an original output backup remains recoverable";
    return false;
  }

  for (OutputStage *backup : backups) delete backup;
  for (OutputStage *source : stages) {
    std::error_code ignored;
    fs::remove(fs::u8path(source->marker_path), ignored);
    source->cleanup_owned = false;
  }
  category = CodecErrorCategory::none;
  error.clear();
  return true;
}

void abort_output_stage(OutputStage **const stage) noexcept {
  if (stage == nullptr || *stage == nullptr) return;
  delete *stage;
  *stage = nullptr;
}

bool recover_stale_output_stages(
    const std::vector<std::string> &destinations,
    CodecErrorCategory &category, std::string &error) {
  try {
    const auto stale_before = fs::file_time_type::clock::now() -
                              std::chrono::hours(24);
    for (const std::string &destination : destinations) {
      const fs::path final_path = fs::u8path(destination);
      const fs::path parent = final_path.parent_path().empty()
                                  ? fs::current_path()
                                  : final_path.parent_path();
      const std::string prefix = "." + final_path.filename().u8string() + ".";
      const std::string suffix = ".partial.owner";
      std::error_code iteration_error;
      for (const auto &entry : fs::directory_iterator(parent, iteration_error)) {
        if (iteration_error)
          return fail(CodecErrorCategory::io,
                      "cannot scan output stage directory", category, error);
        const std::string name = entry.path().filename().u8string();
        if (name.size() <= prefix.size() + suffix.size() ||
            name.compare(0, prefix.size(), prefix) != 0 ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) !=
                0 ||
            !read_marker(entry.path()))
          continue;
        std::error_code time_error;
        const auto modified = fs::last_write_time(entry.path(), time_error);
        if (time_error || modified > stale_before) continue;
        const fs::path staged = fs::u8path(
            entry.path().u8string().substr(0, entry.path().u8string().size() -
                                                 std::string(".owner").size()));
        std::error_code ignored;
        fs::remove(staged, ignored);
        fs::remove(entry.path(), ignored);
      }
      if (iteration_error)
        return fail(CodecErrorCategory::io,
                    "cannot scan output stage directory", category, error);
    }
    category = CodecErrorCategory::none;
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    return fail(CodecErrorCategory::allocation,
                "cannot allocate stale-stage recovery", category, error);
  } catch (...) {
    return fail(CodecErrorCategory::io,
                "unexpected stale-stage recovery failure", category, error);
  }
}

} // namespace pano::app
