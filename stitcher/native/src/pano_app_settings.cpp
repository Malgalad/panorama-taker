#include "pano_app.h"

#include "yyjson.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <unordered_set>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace pano::app {
namespace {
namespace fs = std::filesystem;

std::string normalized_path(const std::string &path) {
  std::error_code ignored;
  auto absolute = fs::absolute(fs::u8path(path), ignored);
  if (ignored)
    absolute = fs::u8path(path);
  return absolute.lexically_normal().u8string();
}

bool replace_file(const fs::path &source, const fs::path &destination,
                  std::string &error) {
#ifdef _WIN32
  if (MoveFileExW(source.c_str(), destination.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE)
    return true;
  error = "cannot replace native application settings";
  return false;
#else
  std::error_code move_error;
  fs::rename(source, destination, move_error);
  if (!move_error)
    return true;
  error = "cannot replace native application settings: " + move_error.message();
  return false;
#endif
}

bool valid_tag(const std::string &tag) noexcept {
  std::size_t offset = 0;
  unsigned characters = 0;
  while (offset < tag.size()) {
    const auto lead = static_cast<unsigned char>(tag[offset]);
    unsigned continuation = 0;
    std::uint32_t value = 0;
    if (lead < 0x80U) {
      value = lead;
    } else if (lead >= 0xC2U && lead <= 0xDFU) {
      continuation = 1;
      value = lead & 0x1FU;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
      continuation = 2;
      value = lead & 0x0FU;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
      continuation = 3;
      value = lead & 0x07U;
    } else {
      return false;
    }
    if (offset + continuation >= tag.size())
      return false;
    for (unsigned index = 0; index < continuation; ++index) {
      const auto byte = static_cast<unsigned char>(tag[offset + index + 1U]);
      if ((byte & 0xC0U) != 0x80U)
        return false;
      value = (value << 6U) | (byte & 0x3FU);
    }
    if ((continuation == 2U && value < 0x800U) ||
        (continuation == 3U && value < 0x10000U) ||
        (value >= 0xD800U && value <= 0xDFFFU) || value > 0x10FFFFU)
      return false;
    offset += continuation + 1U;
    if (++characters > 64U)
      return false;
  }
  return true;
}
} // namespace

bool load_application_settings(const std::string &path,
                               ApplicationSettings &settings,
                               std::string &error) {
  ApplicationSettings loaded;
  if (path.empty()) {
    error = "application settings path is empty";
    return false;
  }
  try {
    std::ifstream stream(fs::u8path(path), std::ios::binary);
    if (!stream) {
      std::error_code exists_error;
      const bool exists = fs::exists(fs::u8path(path), exists_error);
      if (!exists && !exists_error) {
        settings = std::move(loaded);
        error.clear();
        return true;
      }
      error = "cannot read existing native application settings";
      return false;
    }
    const std::string contents{std::istreambuf_iterator<char>(stream),
                               std::istreambuf_iterator<char>()};
    yyjson_read_err read_error{};
    yyjson_doc *document =
        yyjson_read_opts(const_cast<char *>(contents.data()), contents.size(),
                         YYJSON_READ_NOFLAG, nullptr, &read_error);
    if (document == nullptr) {
      error = "cannot parse existing native application settings";
      return false;
    }
    struct DocumentCleanup {
      yyjson_doc *document;
      ~DocumentCleanup() { yyjson_doc_free(document); }
    } cleanup{document};
    yyjson_val *root = yyjson_doc_get_root(document);
    if (!yyjson_is_obj(root)) {
      error = "native application settings root must be an object";
      return false;
    }
    const auto read_string = [&](const char *key, std::string &value) {
      yyjson_val *node = yyjson_obj_get(root, key);
      if (yyjson_is_str(node))
        value = yyjson_get_str(node);
    };
    read_string("game_dir", loaded.game_directory);
    read_string("image_dir", loaded.image_directory);
    read_string("output_dir", loaded.output_directory);
    if (yyjson_val *contrast = yyjson_obj_get(root, "auto_contrast");
        yyjson_is_bool(contrast))
      loaded.auto_contrast = yyjson_get_bool(contrast);
    if (yyjson_val *budget = yyjson_obj_get(root, "gpu_memory_mib");
        yyjson_is_uint(budget) && yyjson_get_uint(budget) <= 8192U)
      loaded.gpu_memory_mib = static_cast<unsigned>(yyjson_get_uint(budget));
    if (yyjson_val *coverage = yyjson_obj_get(root, "debug_coverage");
        yyjson_is_bool(coverage))
      loaded.debug_coverage = yyjson_get_bool(coverage);
    if (yyjson_val *history = yyjson_obj_get(root, "stitched_sessions");
        yyjson_is_obj(history)) {
      auto iterator = yyjson_obj_iter_with(history);
      while (yyjson_val *key = yyjson_obj_iter_next(&iterator)) {
        yyjson_val *entry = yyjson_obj_iter_get_val(key);
        yyjson_val *name = yyjson_is_obj(entry)
                               ? yyjson_obj_get(entry, "output_name")
                               : nullptr;
        if (yyjson_is_str(name))
          loaded.stitched_sessions.push_back(
              {yyjson_get_str(key), yyjson_get_str(name)});
      }
    }
    if (yyjson_val *tags = yyjson_obj_get(root, "session_tags");
        yyjson_is_obj(tags)) {
      auto iterator = yyjson_obj_iter_with(tags);
      while (yyjson_val *key = yyjson_obj_iter_next(&iterator)) {
        yyjson_val *value = yyjson_obj_iter_get_val(key);
        if (yyjson_is_str(value) && valid_tag(yyjson_get_str(value)))
          loaded.session_tags.push_back(
              {yyjson_get_str(key), yyjson_get_str(value)});
      }
    }
    settings = std::move(loaded);
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate native application settings";
    return false;
  } catch (...) {
    error = "cannot load native application settings";
    return false;
  }
}

bool save_application_settings(const std::string &path,
                               const ApplicationSettings &settings,
                               std::string &error) {
  if (path.empty()) {
    error = "application settings path is empty";
    return false;
  }
  const fs::path destination = fs::u8path(path);
  const fs::path partial = fs::u8path(path + ".partial");
  try {
    std::error_code directory_error;
    fs::create_directories(destination.parent_path(), directory_error);
    if (directory_error) {
      error = "cannot create native settings directory: " +
              directory_error.message();
      return false;
    }
    yyjson_mut_doc *document = yyjson_mut_doc_new(nullptr);
    if (document == nullptr)
      throw std::bad_alloc{};
    struct DocumentCleanup {
      yyjson_mut_doc *document;
      ~DocumentCleanup() { yyjson_mut_doc_free(document); }
    } cleanup{document};
    yyjson_mut_val *root = yyjson_mut_obj(document);
    yyjson_mut_doc_set_root(document, root);
    const auto add_string = [&](yyjson_mut_val *object, const char *key,
                                const std::string &value) {
      return yyjson_mut_obj_add_strncpy(document, object, key, value.data(),
                                        value.size());
    };
    yyjson_mut_val *history = yyjson_mut_obj(document);
    yyjson_mut_val *tags = yyjson_mut_obj(document);
    bool built = root != nullptr && history != nullptr && tags != nullptr &&
                 add_string(root, "game_dir", settings.game_directory) &&
                 add_string(root, "image_dir", settings.image_directory) &&
                 add_string(root, "output_dir", settings.output_directory) &&
                 yyjson_mut_obj_add_uint(document, root, "gpu_memory_mib",
                                         settings.gpu_memory_mib) &&
                 yyjson_mut_obj_add_bool(document, root, "debug_coverage",
                                         settings.debug_coverage) &&
                 yyjson_mut_obj_add_bool(document, root, "auto_contrast",
                                         settings.auto_contrast);
    for (const auto &entry : settings.stitched_sessions) {
      yyjson_mut_val *value = yyjson_mut_obj(document);
      built =
          built && value != nullptr &&
          add_string(value, "output_name", entry.output_name) &&
          yyjson_mut_obj_add_val(document, history, entry.key.c_str(), value);
    }
    built = built && yyjson_mut_obj_add_val(document, root, "stitched_sessions",
                                            history);
    for (const auto &entry : settings.session_tags)
      built = built && valid_tag(entry.tag) &&
              add_string(tags, entry.key.c_str(), entry.tag);
    built =
        built && yyjson_mut_obj_add_val(document, root, "session_tags", tags);
    std::size_t json_bytes = 0;
    yyjson_write_err write_error{};
    char *json = built
                     ? yyjson_mut_write_opts(document,
                                             YYJSON_WRITE_PRETTY_TWO_SPACES |
                                                 YYJSON_WRITE_NEWLINE_AT_END,
                                             nullptr, &json_bytes, &write_error)
                     : nullptr;
    struct JsonCleanup {
      char *json;
      ~JsonCleanup() { std::free(json); }
    } json_cleanup{json};
    std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
    if (json == nullptr || !stream ||
        !stream.write(json, static_cast<std::streamsize>(json_bytes)) ||
        !stream.flush()) {
      error = "cannot write native application settings";
      std::error_code ignored;
      fs::remove(partial, ignored);
      return false;
    }
    stream.close();
    if (!replace_file(partial, destination, error)) {
      std::error_code ignored;
      fs::remove(partial, ignored);
      return false;
    }
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate native application settings output";
    return false;
  } catch (const fs::filesystem_error &failure) {
    error = "cannot save native application settings: " +
            std::string(failure.what());
    return false;
  }
}

std::string application_history_key(const std::string &game_directory,
                                    const std::string &session_id) {
  return normalized_path(game_directory) + "::" + session_id;
}

void mark_application_session_stitched(ApplicationSettings &settings,
                                       const std::string &game_directory,
                                       const std::string &session_id,
                                       const std::string &output_name) {
  const std::string key = application_history_key(game_directory, session_id);
  const auto found = std::find_if(
      settings.stitched_sessions.begin(), settings.stitched_sessions.end(),
      [&](const StitchedSessionEntry &entry) { return entry.key == key; });
  if (found == settings.stitched_sessions.end())
    settings.stitched_sessions.push_back({key, output_name});
  else
    found->output_name = output_name;
}

std::optional<std::string>
application_stitched_name(const ApplicationSettings &settings,
                          const std::string &game_directory,
                          const std::string &session_id) {
  const std::string key = application_history_key(game_directory, session_id);
  const auto found = std::find_if(
      settings.stitched_sessions.begin(), settings.stitched_sessions.end(),
      [&](const StitchedSessionEntry &entry) { return entry.key == key; });
  return found == settings.stitched_sessions.end()
             ? std::nullopt
             : std::optional<std::string>(found->output_name);
}

bool set_application_session_tag(ApplicationSettings &settings,
                                 const std::string &game_directory,
                                 const std::string &session_id,
                                 const std::string &tag, std::string &error) {
  if (!valid_tag(tag)) {
    error = "session tag must be valid UTF-8 with at most 64 characters";
    return false;
  }
  const std::string key = application_history_key(game_directory, session_id);
  const auto found = std::find_if(
      settings.session_tags.begin(), settings.session_tags.end(),
      [&](const SessionTagEntry &entry) { return entry.key == key; });
  if (tag.empty()) {
    if (found != settings.session_tags.end())
      settings.session_tags.erase(found);
  } else if (found == settings.session_tags.end()) {
    try {
      settings.session_tags.push_back({key, tag});
    } catch (const std::bad_alloc &) {
      error = "cannot allocate session tag";
      return false;
    }
  } else {
    found->tag = tag;
  }
  error.clear();
  return true;
}

std::optional<std::string>
application_session_tag(const ApplicationSettings &settings,
                        const std::string &game_directory,
                        const std::string &session_id) {
  const std::string key = application_history_key(game_directory, session_id);
  const auto found = std::find_if(
      settings.session_tags.begin(), settings.session_tags.end(),
      [&](const SessionTagEntry &entry) { return entry.key == key; });
  return found == settings.session_tags.end()
             ? std::nullopt
             : std::optional<std::string>(found->tag);
}

std::vector<std::string>
application_deletion_targets(const GuiSessionRecord &record,
                             const bool include_images) {
  std::vector<std::string> targets;
  std::unordered_set<std::string> unique;
  if (unique.insert(record.path).second)
    targets.push_back(record.path);
  if (include_images)
    for (const auto &path : record.image_paths)
      if (unique.insert(path).second)
        targets.push_back(path);
  return targets;
}

bool delete_application_files(const std::vector<std::string> &paths,
                              DeletionResult &result, std::string &error) {
  DeletionResult deleted;
  for (const auto &path : paths) {
    std::error_code remove_error;
    const bool removed = fs::remove(fs::u8path(path), remove_error);
    if (remove_error) {
      error = "cannot delete application file: " + path + ": " +
              remove_error.message();
      return false;
    }
    if (removed)
      ++deleted.deleted;
    else
      ++deleted.missing;
  }
  result = deleted;
  error.clear();
  return true;
}

} // namespace pano::app
