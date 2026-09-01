#include "pano_app.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace pano::app {
namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t maximum_memory_budget = 8192ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t reserved_runtime_bytes = 192ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t tile_bytes_per_pixel = 164;
constexpr unsigned maximum_auto_workers = 8;
std::atomic<std::uint64_t> next_storage_id{0};

bool checked_multiply(const std::uint64_t left, const std::uint64_t right,
                      std::uint64_t &result) {
  if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left)
    return false;
  result = left * right;
  return true;
}

bool exact_buffer(const void *const data, const std::uint64_t actual,
                  const std::uint64_t expected) {
  return data != nullptr && actual == expected;
}

float decode_transfer(const float value,
                      const CpuTransferFunction transfer) {
  if (transfer == CpuTransferFunction::srgb) {
    return value <= 0.04045F
               ? value / 12.92F
               : std::pow(std::max((value + 0.055F) / 1.055F, 0.0F), 2.4F);
  }
  if (transfer == CpuTransferFunction::pq) {
    const float powered = std::pow(std::max(value, 0.0F), 32.0F / 2523.0F);
    const float numerator = std::max(powered - 3424.0F / 4096.0F, 0.0F);
    const float denominator = std::max(
        2413.0F / 128.0F - 2392.0F / 128.0F * powered,
        std::numeric_limits<float>::min());
    return std::pow(numerator / denominator, 16384.0F / 2610.0F);
  }
  return value;
}

float source_channel(const CpuSampleRequest &request, const void *source,
                     const unsigned x, const unsigned y,
                     const unsigned channel) {
  const auto byte_offset = static_cast<std::uint64_t>(y) *
                               request.source_row_stride_bytes +
                           (static_cast<std::uint64_t>(x) * 3U + channel) *
                               (request.sample_type == CpuSampleType::uint8
                                    ? 1U
                                    : request.sample_type == CpuSampleType::uint16
                                          ? 2U
                                          : 4U);
  float encoded = 0.0F;
  const auto *bytes = static_cast<const std::uint8_t *>(source);
  if (request.sample_type == CpuSampleType::uint8) {
    encoded = bytes[byte_offset] * (1.0F / 255.0F);
  } else if (request.sample_type == CpuSampleType::uint16) {
    std::uint16_t value = 0;
    std::memcpy(&value, bytes + byte_offset, sizeof(value));
    encoded = value * (1.0F / 65535.0F);
  } else {
    std::memcpy(&encoded, bytes + byte_offset, sizeof(encoded));
  }
  return decode_transfer(encoded, request.transfer_function);
}

float linear_to_srgb(const float value) {
  const float nonnegative = std::max(value, 0.0F);
  return std::clamp(nonnegative <= 0.0031308F
                        ? nonnegative * 12.92F
                        : 1.055F * std::pow(nonnegative, 1.0F / 2.4F) - 0.055F,
                    0.0F, 1.0F);
}

bool convert_sdr_pixel(const CpuSdrConversionRequest &request,
                       const float *linear_rgb,
                       std::array<float, 3> &encoded) {
  if (!std::isfinite(request.exposure_multiplier) ||
      request.exposure_multiplier <= 0.0F ||
      !std::all_of(linear_rgb, linear_rgb + 3U,
                   [](const float value) { return std::isfinite(value); }))
    return false;
  std::array<float, 3> working{linear_rgb[0], linear_rgb[1], linear_rgb[2]};
  for (float &value : working)
    value *= request.exposure_multiplier;
  if (request.source_transfer == CpuTransferFunction::pq) {
    const float scale = 10000.0F / request.reference_white_nits;
    for (float &value : working) value = std::max(value, 0.0F) * scale;
    const float luminance = working[0] * 0.2627F + working[1] * 0.6780F +
                            working[2] * 0.0593F;
    const float mapped = luminance / (1.0F + luminance);
    const float tone_scale = luminance > 0.0F ? mapped / luminance : 0.0F;
    for (float &value : working) value *= tone_scale;
  }
  if (request.source_transfer == CpuTransferFunction::pq &&
      request.source_primaries == CpuColorPrimaries::rec2020) {
    working = {
        working[0] * 1.660491F - working[1] * 0.587641F -
            working[2] * 0.072850F,
        working[0] * -0.124550F + working[1] * 1.132900F -
            working[2] * 0.008349F,
        working[0] * -0.018151F - working[1] * 0.100579F +
            working[2] * 1.118730F,
    };
  }
  for (unsigned channel = 0; channel < 3U; ++channel) {
    encoded[channel] = linear_to_srgb(working[channel]);
    if (request.apply_auto_contrast && request.levels.valid &&
        request.levels.white > request.levels.black)
      encoded[channel] = std::clamp(
          (encoded[channel] - request.levels.black) /
              (request.levels.white - request.levels.black),
          0.0F, 1.0F);
  }
  return true;
}

bool inject(const CpuStorageFaultCheck &fault, const char *boundary,
            std::string &error) {
  if (fault.callback == nullptr || !fault.callback(fault.user_data, boundary))
    return false;
  error = std::string("injected CPU storage failure at ") + boundary;
  return true;
}
} // namespace

class CpuRenderStorage {
public:
  ~CpuRenderStorage() { cleanup(); }

  void cleanup() noexcept {
#ifdef _WIN32
    if (mapping != nullptr) {
      UnmapViewOfFile(mapping);
      mapping = nullptr;
    }
    if (mapping_handle != nullptr) {
      CloseHandle(mapping_handle);
      mapping_handle = nullptr;
    }
    if (file_handle != INVALID_HANDLE_VALUE) {
      CloseHandle(file_handle);
      file_handle = INVALID_HANDLE_VALUE;
    }
#else
    if (mapping != nullptr) {
      ::munmap(mapping, static_cast<std::size_t>(diagnostics.mapped_scratch_bytes));
      mapping = nullptr;
    }
    if (file_descriptor >= 0) {
      ::close(file_descriptor);
      file_descriptor = -1;
    }
#endif
    worker_strips.clear();
    worker_strips.shrink_to_fit();
    std::error_code ignored;
    if (!scratch_file.empty()) fs::remove(scratch_file, ignored);
    if (!owned_directory.empty()) fs::remove(owned_directory, ignored);
    diagnostics.live_bytes = 0;
  }

  fs::path owned_directory;
  fs::path scratch_file;
  void *mapping = nullptr;
  std::uint64_t color_bytes = 0;
  std::uint64_t per_worker_strip_bytes = 0;
  std::vector<std::uint8_t> worker_strips;
  CpuRenderStorageDiagnostics diagnostics;
#ifdef _WIN32
  HANDLE file_handle = INVALID_HANDLE_VALUE;
  HANDLE mapping_handle = nullptr;
#else
  int file_descriptor = -1;
#endif
};

class CpuWorkerPool {
public:
  explicit CpuWorkerPool(const unsigned count) {
    workers.reserve(count);
    try {
      for (unsigned index = 0; index < count; ++index)
        workers.emplace_back([this, index] { worker(index); });
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
        ++generation;
      }
      work_ready.notify_all();
      for (auto &thread : workers)
        if (thread.joinable()) thread.join();
      throw;
    }
  }
  ~CpuWorkerPool() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
      ++generation;
    }
    work_ready.notify_all();
    for (auto &thread : workers)
      if (thread.joinable()) thread.join();
  }

  void worker(const unsigned worker_index) {
    std::uint64_t observed_generation = 0;
    for (;;) {
      std::unique_lock<std::mutex> lock(mutex);
      work_ready.wait(lock, [&] {
        return stopping || generation != observed_generation;
      });
      if (stopping) return;
      observed_generation = generation;
      lock.unlock();
      for (;;) {
        if (cancellation.callback != nullptr &&
            cancellation.callback(cancellation.user_data)) {
          was_cancelled.store(true, std::memory_order_relaxed);
          break;
        }
        const unsigned task = next_task.fetch_add(1, std::memory_order_relaxed);
        if (task >= task_count) break;
        if (!callback(user_data, worker_index, task)) {
          callback_failed.store(true, std::memory_order_relaxed);
          break;
        }
      }
      lock.lock();
      if (--active_workers == 0U) finished.notify_one();
    }
  }

  std::mutex mutex;
  std::condition_variable work_ready;
  std::condition_variable finished;
  std::vector<std::thread> workers;
  bool stopping = false;
  bool running = false;
  std::uint64_t generation = 0;
  unsigned active_workers = 0;
  unsigned task_count = 0;
  std::atomic<unsigned> next_task{0};
  CpuTaskCallback callback = nullptr;
  void *user_data = nullptr;
  CancellationCheck cancellation;
  std::atomic<bool> callback_failed{false};
  std::atomic<bool> was_cancelled{false};
};

class CpuExposureCache {
public:
  mutable std::mutex mutex;
  std::string identity;
  std::optional<CpuExposureReport> report;
};

class CpuRenderCoordinator {
public:
  std::atomic<bool> busy{false};
};

bool plan_cpu_render(const CpuRenderPlanRequest &request, CpuRenderPlan &plan,
                     std::string &error) {
  if (request.source_width == 0U || request.source_height == 0U ||
      request.output_width == 0U || request.output_height == 0U) {
    error = "CPU render dimensions must be positive";
    return false;
  }
  if (request.memory_budget_bytes == 0U ||
      request.memory_budget_bytes > maximum_memory_budget) {
    error = "CPU memory budget must be between 1 byte and 8192 MiB";
    return false;
  }
  CpuRenderPlan parsed;
  std::uint64_t source_pixels = 0;
  if (!checked_multiply(request.source_width, request.source_height,
                        source_pixels) ||
      !checked_multiply(source_pixels, 3U * sizeof(float) * 3U,
                        parsed.source_working_set_bytes) ||
      !checked_multiply(request.output_width, tile_bytes_per_pixel,
                        parsed.bytes_per_worker_row)) {
    error = "CPU render memory plan overflows";
    return false;
  }
  if (request.memory_budget_bytes <= reserved_runtime_bytes ||
      parsed.source_working_set_bytes >
          request.memory_budget_bytes - reserved_runtime_bytes) {
    error = "CPU memory budget is too small for one output row and one decoded source";
    return false;
  }
  parsed.available_strip_bytes =
      request.memory_budget_bytes - reserved_runtime_bytes -
      parsed.source_working_set_bytes;
  if (parsed.available_strip_bytes < parsed.bytes_per_worker_row) {
    error = "CPU memory budget is too small for one output row and one decoded source";
    return false;
  }
  const std::uint64_t maximum_workers =
      std::max<std::uint64_t>(1U, parsed.available_strip_bytes /
                                     parsed.bytes_per_worker_row);
  unsigned requested_workers = request.worker_count;
  if (requested_workers == 0U) {
    requested_workers = std::thread::hardware_concurrency();
    if (requested_workers == 0U) requested_workers = 1U;
    requested_workers = std::min(requested_workers, maximum_auto_workers);
  }
  parsed.worker_count = static_cast<unsigned>(
      std::min<std::uint64_t>(requested_workers, maximum_workers));
  std::uint64_t all_worker_row_bytes = 0;
  if (!checked_multiply(parsed.worker_count, parsed.bytes_per_worker_row,
                        all_worker_row_bytes)) {
    error = "CPU render worker memory plan overflows";
    return false;
  }
  const std::uint64_t planned_rows =
      std::max<std::uint64_t>(1U, parsed.available_strip_bytes /
                                     all_worker_row_bytes);
  parsed.strip_height = static_cast<unsigned>(std::min<std::uint64_t>(
      planned_rows, request.output_height));
  std::uint64_t output_pixels = 0;
  if (!checked_multiply(request.output_width, request.output_height,
                        output_pixels) ||
      !checked_multiply(output_pixels, 4U * sizeof(float),
                        parsed.scratch_bytes)) {
    error = "CPU render scratch plan overflows";
    return false;
  }
  plan = parsed;
  error.clear();
  return true;
}

bool create_cpu_render_storage(const CpuRenderStorageOptions &options,
                               CpuRenderStorage **const storage,
                               std::string &error) {
  if (storage == nullptr) {
    error = "CPU render storage out-handle is null";
    return false;
  }
  *storage = nullptr;
  std::uint64_t output_pixels = 0;
  std::uint64_t expected_scratch = 0;
  std::uint64_t expected_row_bytes = 0;
  std::uint64_t expected_worker_bytes = 0;
  std::uint64_t color_bytes = 0;
  if (options.directory.empty() || options.output_width == 0U ||
      options.output_height == 0U || options.plan.worker_count == 0U ||
      options.plan.strip_height == 0U ||
      !checked_multiply(options.output_width, options.output_height,
                        output_pixels) ||
      !checked_multiply(output_pixels, 4U * sizeof(float), expected_scratch) ||
      !checked_multiply(output_pixels, 3U * sizeof(float), color_bytes) ||
      !checked_multiply(options.output_width, tile_bytes_per_pixel,
                        expected_row_bytes) ||
      !checked_multiply(expected_row_bytes, options.plan.strip_height,
                        expected_worker_bytes) ||
      !checked_multiply(expected_worker_bytes, options.plan.worker_count,
                        expected_worker_bytes) ||
      options.plan.scratch_bytes != expected_scratch ||
      options.plan.bytes_per_worker_row != expected_row_bytes ||
      options.plan.strip_height > options.output_height ||
      expected_scratch > std::numeric_limits<std::size_t>::max() ||
      expected_scratch >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      expected_worker_bytes > std::numeric_limits<std::size_t>::max()) {
    error = "invalid CPU render storage plan";
    return false;
  }
  try {
    auto created = std::make_unique<CpuRenderStorage>();
    const fs::path root = fs::u8path(options.directory);
    std::error_code status_error;
    if (!fs::is_directory(root, status_error) || status_error) {
      error = "CPU scratch root does not exist";
      return false;
    }
    const auto nonce = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
      const auto id = next_storage_id.fetch_add(1, std::memory_order_relaxed);
      created->owned_directory =
          root / fs::u8path("pano-cpu-" + std::to_string(nonce) + "-" +
                            std::to_string(id) + "-" +
                            std::to_string(attempt));
      if (fs::create_directory(created->owned_directory, status_error)) break;
      created->owned_directory.clear();
      status_error.clear();
    }
    if (created->owned_directory.empty()) {
      error = "cannot create collision-free CPU scratch directory";
      return false;
    }
    if (inject(options.fault, "after_directory", error)) return false;
    created->scratch_file = created->owned_directory / "color-weight.f32";
#ifdef _WIN32
    created->file_handle = CreateFileW(
        created->scratch_file.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (created->file_handle == INVALID_HANDLE_VALUE) {
      error = "cannot create CPU scratch file";
      return false;
    }
    LARGE_INTEGER size{};
    size.QuadPart = static_cast<LONGLONG>(expected_scratch);
    if (SetFilePointerEx(created->file_handle, size, nullptr, FILE_BEGIN) ==
            FALSE ||
        SetEndOfFile(created->file_handle) == FALSE) {
      error = "cannot size CPU scratch file";
      return false;
    }
#else
    created->file_descriptor = ::open(created->scratch_file.c_str(),
                                      O_RDWR | O_CREAT | O_EXCL, 0600);
    if (created->file_descriptor < 0 ||
        ::ftruncate(created->file_descriptor,
                    static_cast<off_t>(expected_scratch)) != 0) {
      error = "cannot create or size CPU scratch file";
      return false;
    }
#endif
    if (inject(options.fault, "after_file", error)) return false;
#ifdef _WIN32
    created->mapping_handle = CreateFileMappingW(
        created->file_handle, nullptr, PAGE_READWRITE,
        static_cast<DWORD>(expected_scratch >> 32U),
        static_cast<DWORD>(expected_scratch & 0xffffffffU), nullptr);
    if (created->mapping_handle != nullptr)
      created->mapping = MapViewOfFile(created->mapping_handle,
                                       FILE_MAP_ALL_ACCESS, 0, 0,
                                       static_cast<SIZE_T>(expected_scratch));
#else
    created->mapping = ::mmap(nullptr, static_cast<std::size_t>(expected_scratch),
                              PROT_READ | PROT_WRITE, MAP_SHARED,
                              created->file_descriptor, 0);
    if (created->mapping == MAP_FAILED) created->mapping = nullptr;
#endif
    if (created->mapping == nullptr) {
      error = "cannot map CPU scratch file";
      return false;
    }
    created->diagnostics.mapped_scratch_bytes = expected_scratch;
    created->diagnostics.live_bytes = expected_scratch;
    created->diagnostics.peak_live_bytes = expected_scratch;
    if (inject(options.fault, "after_mapping", error)) return false;
    if (inject(options.fault, "before_worker_strips", error)) return false;
    created->worker_strips.resize(
        static_cast<std::size_t>(expected_worker_bytes));
    created->color_bytes = color_bytes;
    created->per_worker_strip_bytes =
        expected_worker_bytes / options.plan.worker_count;
    created->diagnostics.worker_strip_bytes = expected_worker_bytes;
    created->diagnostics.live_bytes += expected_worker_bytes;
    created->diagnostics.peak_live_bytes = created->diagnostics.live_bytes;
    *storage = created.release();
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate CPU render storage";
    return false;
  } catch (...) {
    error = "unexpected CPU render storage failure";
    return false;
  }
}

bool query_cpu_render_storage(
    const CpuRenderStorage *const storage,
    CpuRenderStorageDiagnostics &diagnostics, std::string &error) {
  if (storage == nullptr) {
    error = "invalid CPU render storage handle";
    return false;
  }
  diagnostics = storage->diagnostics;
  error.clear();
  return true;
}

float *cpu_render_color_scratch(CpuRenderStorage *const storage) noexcept {
  return storage == nullptr ? nullptr : static_cast<float *>(storage->mapping);
}

float *cpu_render_weight_scratch(CpuRenderStorage *const storage) noexcept {
  return storage == nullptr
             ? nullptr
             : reinterpret_cast<float *>(
                   static_cast<std::uint8_t *>(storage->mapping) +
                   storage->color_bytes);
}

void *cpu_render_worker_strip(CpuRenderStorage *const storage,
                              const unsigned worker_index) noexcept {
  if (storage == nullptr || storage->per_worker_strip_bytes == 0U ||
      worker_index >= storage->diagnostics.worker_strip_bytes /
                          storage->per_worker_strip_bytes)
    return nullptr;
  return storage->worker_strips.data() +
         static_cast<std::size_t>(worker_index *
                                  storage->per_worker_strip_bytes);
}

void destroy_cpu_render_storage(CpuRenderStorage **const storage) noexcept {
  if (storage == nullptr || *storage == nullptr) return;
  delete *storage;
  *storage = nullptr;
}

bool create_cpu_worker_pool(const unsigned worker_count,
                            CpuWorkerPool **const pool, std::string &error) {
  if (pool == nullptr) {
    error = "CPU worker-pool out-handle is null";
    return false;
  }
  *pool = nullptr;
  if (worker_count == 0U || worker_count > maximum_auto_workers) {
    error = "CPU worker count must be between 1 and 8";
    return false;
  }
  try {
    *pool = new CpuWorkerPool(worker_count);
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate CPU worker pool";
    return false;
  } catch (...) {
    error = "cannot create CPU worker threads";
    return false;
  }
}

bool run_cpu_tasks(CpuWorkerPool *const pool, const unsigned task_count,
                   const CpuTaskCallback callback, void *const user_data,
                   const CancellationCheck &cancellation,
                   std::string &error) {
  if (pool == nullptr || task_count == 0U || callback == nullptr) {
    error = "invalid CPU task batch";
    return false;
  }
  std::unique_lock<std::mutex> lock(pool->mutex);
  if (pool->running) {
    error = "CPU worker pool already has an active batch";
    return false;
  }
  if (cancellation.callback != nullptr &&
      cancellation.callback(cancellation.user_data)) {
    error = "CPU task batch cancelled";
    return false;
  }
  pool->running = true;
  pool->task_count = task_count;
  pool->next_task.store(0, std::memory_order_relaxed);
  pool->callback = callback;
  pool->user_data = user_data;
  pool->cancellation = cancellation;
  pool->callback_failed.store(false, std::memory_order_relaxed);
  pool->was_cancelled.store(false, std::memory_order_relaxed);
  pool->active_workers = static_cast<unsigned>(pool->workers.size());
  ++pool->generation;
  pool->work_ready.notify_all();
  pool->finished.wait(lock, [&] { return pool->active_workers == 0U; });
  pool->running = false;
  if (pool->was_cancelled.load(std::memory_order_relaxed)) {
    error = "CPU task batch cancelled";
    return false;
  }
  if (pool->callback_failed.load(std::memory_order_relaxed)) {
    error = "CPU task callback failed";
    return false;
  }
  error.clear();
  return true;
}

void destroy_cpu_worker_pool(CpuWorkerPool **const pool) noexcept {
  if (pool == nullptr || *pool == nullptr) return;
  delete *pool;
  *pool = nullptr;
}

bool generate_cpu_world_rays(const CpuRayRequest &request, float *world_rays,
                             const std::uint64_t world_ray_bytes,
                             std::string &error) {
  std::uint64_t pixel_count = 0;
  std::uint64_t expected_bytes = 0;
  if (request.output_width == 0U || request.output_height == 0U ||
      request.row_count == 0U || request.row_start > request.output_height ||
      request.row_count > request.output_height - request.row_start ||
      !checked_multiply(request.output_width, request.row_count, pixel_count) ||
      !checked_multiply(pixel_count, 3U * sizeof(float), expected_bytes) ||
      !exact_buffer(world_rays, world_ray_bytes, expected_bytes)) {
    error = "invalid CPU world-ray buffer or row range";
    return false;
  }
  if (request.projection == CpuOutputProjection::equirectangular) {
    if (!std::isfinite(request.latitude_span_degrees) ||
        request.latitude_span_degrees <= 0.0F ||
        request.latitude_span_degrees > 180.0F) {
      error = "invalid CPU equirectangular latitude span";
      return false;
    }
  } else if (request.projection != CpuOutputProjection::rectilinear ||
             !std::isfinite(request.rectilinear_vertical_fov_degrees) ||
             request.rectilinear_vertical_fov_degrees <= 0.0F ||
             request.rectilinear_vertical_fov_degrees >= 180.0F) {
    error = "invalid CPU rectilinear field of view";
    return false;
  }
  constexpr float pi = 3.14159265358979323846F;
  const float rectilinear_focal_x = request.output_width * 0.5F;
  const float rectilinear_focal_y =
      request.output_height /
      (2.0F * std::tan(request.rectilinear_vertical_fov_degrees * pi / 360.0F));
  for (unsigned row = 0; row < request.row_count; ++row) {
    for (unsigned x = 0; x < request.output_width; ++x) {
      const auto index = static_cast<std::uint64_t>(row) * request.output_width + x;
      float ray_x = 0.0F;
      float ray_y = 0.0F;
      float ray_z = 0.0F;
      if (request.projection == CpuOutputProjection::equirectangular) {
        const float longitude =
            ((x + 0.5F) / request.output_width - 0.5F) * 2.0F * pi;
        const float latitude =
            (0.5F - (request.row_start + row + 0.5F) /
                        request.output_height) *
            request.latitude_span_degrees * pi / 180.0F;
        const float cosine = std::cos(latitude);
        ray_x = cosine * std::sin(longitude);
        ray_y = std::sin(latitude);
        ray_z = cosine * std::cos(longitude);
      } else {
        ray_x = (static_cast<float>(x) -
                 (request.output_width - 1U) * 0.5F) /
                rectilinear_focal_x;
        ray_y = ((request.output_height - 1U) * 0.5F -
                 (request.row_start + row)) /
                rectilinear_focal_y;
        ray_z = 1.0F;
        const float inverse_length =
            1.0F / std::sqrt(ray_x * ray_x + ray_y * ray_y + 1.0F);
        ray_x *= inverse_length;
        ray_y *= inverse_length;
        ray_z *= inverse_length;
      }
      world_rays[index * 3U] = ray_x;
      world_rays[index * 3U + 1U] = ray_y;
      world_rays[index * 3U + 2U] = ray_z;
    }
  }
  error.clear();
  return true;
}

bool project_cpu_world_rays(
    const CpuProjectionRequest &request, const float *world_rays,
    const std::uint64_t world_ray_bytes, float *coordinates,
    const std::uint64_t coordinate_bytes, std::uint8_t *validity,
    const std::uint64_t validity_bytes, float *edge_distances,
    const std::uint64_t edge_distance_bytes, std::string &error) {
  std::uint64_t ray_bytes = 0;
  std::uint64_t coordinates_bytes = 0;
  std::uint64_t edges_bytes = 0;
  if (request.pixel_count == 0U || request.source_width == 0U ||
      request.source_height == 0U ||
      !checked_multiply(request.pixel_count, 3U * sizeof(float), ray_bytes) ||
      !checked_multiply(request.pixel_count, 2U * sizeof(float), coordinates_bytes) ||
      !checked_multiply(request.pixel_count, sizeof(float), edges_bytes) ||
      !exact_buffer(world_rays, world_ray_bytes, ray_bytes) ||
      !exact_buffer(coordinates, coordinate_bytes, coordinates_bytes) ||
      !exact_buffer(validity, validity_bytes, request.pixel_count) ||
      !exact_buffer(edge_distances, edge_distance_bytes, edges_bytes) ||
      !std::isfinite(request.horizontal_fov_degrees) ||
      !std::isfinite(request.vertical_fov_degrees) ||
      request.horizontal_fov_degrees <= 0.0F ||
      request.horizontal_fov_degrees >= 180.0F ||
      request.vertical_fov_degrees <= 0.0F ||
      request.vertical_fov_degrees >= 180.0F ||
      !std::all_of(request.world_to_camera.begin(),
                   request.world_to_camera.end(),
                   [](const float value) { return std::isfinite(value); })) {
    error = "invalid CPU camera projection request";
    return false;
  }
  constexpr float radians_per_half_degree = 0.00872664625997165F;
  const float focal_x = request.source_width /
                        (2.0F * std::tan(request.horizontal_fov_degrees *
                                        radians_per_half_degree));
  const float focal_y = request.source_height /
                        (2.0F * std::tan(request.vertical_fov_degrees *
                                        radians_per_half_degree));
  for (unsigned index = 0; index < request.pixel_count; ++index) {
    const float x = world_rays[index * 3U];
    const float y = world_rays[index * 3U + 1U];
    const float z = world_rays[index * 3U + 2U];
    const float camera_x = x * request.world_to_camera[0] +
                           y * request.world_to_camera[3] +
                           z * request.world_to_camera[6];
    const float camera_y = x * request.world_to_camera[1] +
                           y * request.world_to_camera[4] +
                           z * request.world_to_camera[7];
    const float camera_z = x * request.world_to_camera[2] +
                           y * request.world_to_camera[5] +
                           z * request.world_to_camera[8];
    const bool finite = std::isfinite(camera_x) && std::isfinite(camera_y) &&
                        std::isfinite(camera_z);
    const float safe_z = finite && std::fabs(camera_z) > 1.0e-8F ? camera_z : 1.0F;
    const float projected_x =
        (request.source_width - 1U) * 0.5F + focal_x * camera_x / safe_z;
    const float projected_y =
        (request.source_height - 1U) * 0.5F - focal_y * camera_y / safe_z;
    const bool projected_finite =
        finite && std::isfinite(projected_x) && std::isfinite(projected_y);
    validity[index] = static_cast<std::uint8_t>(
        projected_finite && camera_z > 0.0F && projected_x >= -0.5F &&
        projected_x <= request.source_width - 0.5F && projected_y >= -0.5F &&
        projected_y <= request.source_height - 0.5F);
    const float coordinate_x = projected_finite
                                   ? std::clamp(projected_x, 0.0F,
                                                request.source_width - 1.0F)
                                   : 0.0F;
    const float coordinate_y = projected_finite
                                   ? std::clamp(projected_y, 0.0F,
                                                request.source_height - 1.0F)
                                   : 0.0F;
    coordinates[index * 2U] = coordinate_x;
    coordinates[index * 2U + 1U] = coordinate_y;
    edge_distances[index] = std::min(
        {coordinate_x, coordinate_y, request.source_width - 1.0F - coordinate_x,
         request.source_height - 1.0F - coordinate_y});
  }
  error.clear();
  return true;
}

bool sample_cpu_bilinear(
    const CpuSampleRequest &request, const void *source,
    const std::uint64_t source_bytes, const float *coordinates,
    const std::uint64_t coordinate_bytes, const std::uint8_t *validity,
    const std::uint64_t validity_bytes, float *candidate_rgb,
    const std::uint64_t candidate_rgb_bytes, std::string &error) {
  const std::uint64_t component_bytes =
      request.sample_type == CpuSampleType::uint8
          ? 1U
          : request.sample_type == CpuSampleType::uint16 ? 2U : 4U;
  std::uint64_t minimum_stride = 0;
  std::uint64_t expected_source = 0;
  std::uint64_t expected_coordinates = 0;
  std::uint64_t expected_candidates = 0;
  if (request.source_width == 0U || request.source_height == 0U ||
      request.pixel_count == 0U ||
      (request.sample_type != CpuSampleType::uint8 &&
       request.sample_type != CpuSampleType::uint16 &&
       request.sample_type != CpuSampleType::float32) ||
      (request.transfer_function != CpuTransferFunction::srgb &&
       request.transfer_function != CpuTransferFunction::pq &&
       request.transfer_function != CpuTransferFunction::linear) ||
      !checked_multiply(request.source_width, 3U * component_bytes,
                        minimum_stride) ||
      request.source_row_stride_bytes < minimum_stride ||
      !checked_multiply(request.source_row_stride_bytes, request.source_height,
                        expected_source) ||
      source == nullptr || source_bytes < expected_source ||
      !checked_multiply(request.pixel_count, 2U * sizeof(float),
                        expected_coordinates) ||
      !checked_multiply(request.pixel_count, 3U * sizeof(float),
                        expected_candidates) ||
      !exact_buffer(coordinates, coordinate_bytes, expected_coordinates) ||
      !exact_buffer(validity, validity_bytes, request.pixel_count) ||
      !exact_buffer(candidate_rgb, candidate_rgb_bytes, expected_candidates)) {
    error = "invalid CPU bilinear sampling request";
    return false;
  }
  for (unsigned index = 0; index < request.pixel_count; ++index) {
    const float coordinate_x = coordinates[index * 2U];
    const float coordinate_y = coordinates[index * 2U + 1U];
    if (validity[index] > 1U || !std::isfinite(coordinate_x) ||
        !std::isfinite(coordinate_y) || coordinate_x < 0.0F ||
        coordinate_x > request.source_width - 1.0F || coordinate_y < 0.0F ||
        coordinate_y > request.source_height - 1.0F) {
      error = "invalid CPU bilinear coordinate or validity value";
      return false;
    }
    const auto lower_x = static_cast<unsigned>(std::floor(coordinate_x));
    const auto lower_y = static_cast<unsigned>(std::floor(coordinate_y));
    const auto upper_x = std::min(lower_x + 1U, request.source_width - 1U);
    const auto upper_y = std::min(lower_y + 1U, request.source_height - 1U);
    const float fraction_x = coordinate_x - lower_x;
    const float fraction_y = coordinate_y - lower_y;
    for (unsigned channel = 0; channel < 3U; ++channel) {
      const float top_left = source_channel(request, source, lower_x, lower_y, channel);
      const float top_right = source_channel(request, source, upper_x, lower_y, channel);
      const float bottom_left = source_channel(request, source, lower_x, upper_y, channel);
      const float bottom_right = source_channel(request, source, upper_x, upper_y, channel);
      const float top = top_left + (top_right - top_left) * fraction_x;
      const float bottom = bottom_left + (bottom_right - bottom_left) * fraction_x;
      candidate_rgb[index * 3U + channel] =
          top + (bottom - top) * fraction_y;
    }
  }
  error.clear();
  return true;
}

bool select_cpu_hard(const unsigned pixel_count, const float *candidate_rgb,
                     const std::uint8_t *candidate_validity,
                     const float *candidate_edge_distance, float *color,
                     float *weight, std::uint8_t *coverage,
                     std::string &error) {
  if (pixel_count == 0U || candidate_rgb == nullptr ||
      candidate_validity == nullptr || candidate_edge_distance == nullptr ||
      color == nullptr || weight == nullptr || coverage == nullptr) {
    error = "invalid CPU hard-selection request";
    return false;
  }
  for (unsigned index = 0; index < pixel_count; ++index) {
    if (candidate_validity[index] > 1U ||
        !std::isfinite(candidate_edge_distance[index]) ||
        !std::isfinite(weight[index]) || candidate_edge_distance[index] < 0.0F ||
        weight[index] < 0.0F) {
      error = "invalid CPU hard-selection input";
      return false;
    }
    const float candidate_weight = candidate_validity[index] != 0U
                                       ? std::max(candidate_edge_distance[index],
                                                  1.0e-6F)
                                       : 0.0F;
    if (candidate_weight > weight[index]) {
      std::copy_n(candidate_rgb + index * 3U, 3U, color + index * 3U);
      weight[index] = candidate_weight;
    }
    coverage[index] = static_cast<std::uint8_t>(weight[index] > 0.0F);
  }
  error.clear();
  return true;
}

bool iterate_cpu_frames(const unsigned frame_count,
                        const CpuFrameCallbacks &callbacks,
                        const CancellationCheck &cancellation,
                        std::string &error) {
  if (frame_count == 0U || callbacks.acquire == nullptr ||
      callbacks.compose == nullptr || callbacks.release == nullptr) {
    error = "invalid CPU frame iteration request";
    return false;
  }
  for (unsigned frame = 0; frame < frame_count; ++frame) {
    if (cancellation.callback != nullptr &&
        cancellation.callback(cancellation.user_data)) {
      error = "CPU frame iteration cancelled";
      return false;
    }
    if (!callbacks.acquire(callbacks.user_data, frame)) {
      error = "CPU frame acquisition failed";
      return false;
    }
    const bool composed = callbacks.compose(callbacks.user_data, frame);
    callbacks.release(callbacks.user_data, frame);
    if (!composed) {
      error = "CPU frame composition failed";
      return false;
    }
  }
  error.clear();
  return true;
}

bool accumulate_cpu_feather(
    const unsigned pixel_count, const unsigned source_width,
    const unsigned source_height, const float *candidate_rgb,
    const std::uint8_t *candidate_validity,
    const float *candidate_edge_distance, float *color, float *weight,
    std::string &error) {
  if (pixel_count == 0U || source_width == 0U || source_height == 0U ||
      candidate_rgb == nullptr || candidate_validity == nullptr ||
      candidate_edge_distance == nullptr || color == nullptr ||
      weight == nullptr) {
    error = "invalid CPU feather-accumulation request";
    return false;
  }
  const float feather_width =
      std::max(1.0F, std::min(source_width, source_height) * 0.08F);
  for (unsigned index = 0; index < pixel_count; ++index) {
    if (candidate_validity[index] > 1U ||
        !std::isfinite(candidate_edge_distance[index]) ||
        candidate_edge_distance[index] < 0.0F || !std::isfinite(weight[index]) ||
        weight[index] < 0.0F) {
      error = "invalid CPU feather-accumulation input";
      return false;
    }
    const float candidate_weight = candidate_validity[index] != 0U
                                       ? std::max(candidate_edge_distance[index] /
                                                      feather_width,
                                                  1.0e-6F)
                                       : 0.0F;
    for (unsigned channel = 0; channel < 3U; ++channel) {
      const auto component = index * 3U + channel;
      if (!std::isfinite(candidate_rgb[component]) ||
          !std::isfinite(color[component])) {
        error = "non-finite CPU feather color";
        return false;
      }
      color[component] += candidate_rgb[component] * candidate_weight;
    }
    weight[index] += candidate_weight;
  }
  error.clear();
  return true;
}

bool normalize_cpu_feather(const unsigned pixel_count, float *color,
                           const float *weight, std::uint8_t *coverage,
                           std::string &error) {
  if (pixel_count == 0U || color == nullptr || weight == nullptr ||
      coverage == nullptr) {
    error = "invalid CPU feather-normalization request";
    return false;
  }
  for (unsigned index = 0; index < pixel_count; ++index) {
    if (!std::isfinite(weight[index]) || weight[index] < 0.0F) {
      error = "invalid CPU feather-normalization weight";
      return false;
    }
    coverage[index] = static_cast<std::uint8_t>(weight[index] > 0.0F);
    for (unsigned channel = 0; channel < 3U; ++channel) {
      const auto component = index * 3U + channel;
      if (!std::isfinite(color[component])) {
        error = "non-finite CPU feather accumulator";
        return false;
      }
      if (weight[index] > 0.0F) color[component] /= weight[index];
    }
  }
  error.clear();
  return true;
}

bool mark_cpu_incomplete(const unsigned pixel_count, float *color,
                         const float *weight, std::string &error) {
  if (pixel_count == 0U || color == nullptr || weight == nullptr) {
    error = "invalid CPU incomplete-marking request";
    return false;
  }
  for (unsigned index = 0; index < pixel_count; ++index) {
    if (!std::isfinite(weight[index]) || weight[index] < 0.0F) {
      error = "invalid CPU incomplete-marking weight";
      return false;
    }
    if (weight[index] == 0.0F) {
      color[index * 3U] = 1.0F;
      color[index * 3U + 1U] = 0.0F;
      color[index * 3U + 2U] = 1.0F;
    }
  }
  error.clear();
  return true;
}

bool apply_cpu_global_gain(const unsigned pixel_count, const float gain,
                           float *color, std::string &error) {
  if (pixel_count == 0U || color == nullptr || !std::isfinite(gain) ||
      gain <= 0.0F) {
    error = "invalid CPU global gain";
    return false;
  }
  for (std::uint64_t component = 0;
       component < static_cast<std::uint64_t>(pixel_count) * 3U; ++component) {
    if (!std::isfinite(color[component])) {
      error = "non-finite CPU gain input";
      return false;
    }
    color[component] *= gain;
  }
  error.clear();
  return true;
}

bool apply_cpu_local_gain(const CpuLocalGainRequest &request,
                          const float *local_log_gain, float *color,
                          std::string &error) {
  if (request.output_width == 0U || request.output_height == 0U ||
      request.row_count == 0U || request.field_width == 0U ||
      request.field_height == 0U || request.row_start > request.output_height ||
      request.row_count > request.output_height - request.row_start ||
      local_log_gain == nullptr || color == nullptr) {
    error = "invalid CPU local-gain request";
    return false;
  }
  for (unsigned row = 0; row < request.row_count; ++row) {
    for (unsigned x = 0; x < request.output_width; ++x) {
      const float field_x = std::clamp(
          (x + 0.5F) * request.field_width / request.output_width - 0.5F,
          0.0F, request.field_width - 1.0F);
      const float field_y = std::clamp(
          (request.row_start + row + 0.5F) * request.field_height /
                  request.output_height -
              0.5F,
          0.0F, request.field_height - 1.0F);
      const auto x0 = static_cast<unsigned>(std::floor(field_x));
      const auto y0 = static_cast<unsigned>(std::floor(field_y));
      const auto x1 = std::min(x0 + 1U, request.field_width - 1U);
      const auto y1 = std::min(y0 + 1U, request.field_height - 1U);
      const float fraction_x = field_x - x0;
      const float fraction_y = field_y - y0;
      const float upper = local_log_gain[y0 * request.field_width + x0] +
                          (local_log_gain[y0 * request.field_width + x1] -
                           local_log_gain[y0 * request.field_width + x0]) *
                              fraction_x;
      const float lower = local_log_gain[y1 * request.field_width + x0] +
                          (local_log_gain[y1 * request.field_width + x1] -
                           local_log_gain[y1 * request.field_width + x0]) *
                              fraction_x;
      const float interpolated = upper + (lower - upper) * fraction_y;
      if (!std::isfinite(interpolated)) {
        error = "non-finite CPU local gain";
        return false;
      }
      const float gain = std::exp(interpolated);
      const auto pixel = static_cast<std::uint64_t>(row) * request.output_width + x;
      for (unsigned channel = 0; channel < 3U; ++channel) {
        if (!std::isfinite(color[pixel * 3U + channel])) {
          error = "non-finite CPU local-gain input";
          return false;
        }
        color[pixel * 3U + channel] *= gain;
      }
    }
  }
  error.clear();
  return true;
}

bool build_cpu_exposure_proxy(const CpuExposureProxyRequest &request,
                              const void *source,
                              const std::uint64_t source_bytes, float *proxy,
                              const std::uint64_t proxy_bytes,
                              std::string &error) {
  const std::uint64_t component_bytes =
      request.source.sample_type == CpuSampleType::uint8
          ? 1U
          : request.source.sample_type == CpuSampleType::uint16 ? 2U : 4U;
  std::uint64_t minimum_stride = 0;
  std::uint64_t minimum_source_bytes = 0;
  std::uint64_t proxy_pixels = 0;
  std::uint64_t expected_proxy_bytes = 0;
  if (request.source.source_width == 0U ||
      request.source.source_height == 0U || request.proxy_width == 0U ||
      request.proxy_height == 0U || source == nullptr || proxy == nullptr ||
      (request.source.sample_type != CpuSampleType::uint8 &&
       request.source.sample_type != CpuSampleType::uint16 &&
       request.source.sample_type != CpuSampleType::float32) ||
      (request.source.transfer_function != CpuTransferFunction::srgb &&
       request.source.transfer_function != CpuTransferFunction::pq &&
       request.source.transfer_function != CpuTransferFunction::linear) ||
      !checked_multiply(request.source.source_width, 3U * component_bytes,
                        minimum_stride) ||
      request.source.source_row_stride_bytes < minimum_stride ||
      !checked_multiply(request.source.source_row_stride_bytes,
                        request.source.source_height, minimum_source_bytes) ||
      source_bytes < minimum_source_bytes ||
      !checked_multiply(request.proxy_width, request.proxy_height,
                        proxy_pixels) ||
      !checked_multiply(proxy_pixels, 3U * sizeof(float),
                        expected_proxy_bytes) ||
      proxy_bytes != expected_proxy_bytes) {
    error = "invalid CPU exposure-proxy request";
    return false;
  }
  for (unsigned y = 0; y < request.proxy_height; ++y) {
    const float top = y * static_cast<float>(request.source.source_height) /
                      request.proxy_height;
    const float bottom =
        (y + 1U) * static_cast<float>(request.source.source_height) /
        request.proxy_height;
    for (unsigned x = 0; x < request.proxy_width; ++x) {
      const float left = x * static_cast<float>(request.source.source_width) /
                         request.proxy_width;
      const float right =
          (x + 1U) * static_cast<float>(request.source.source_width) /
          request.proxy_width;
      std::array<float, 3> sum{};
      float total = 0.0F;
      for (unsigned source_y = static_cast<unsigned>(std::floor(top));
           source_y < std::min(static_cast<unsigned>(std::ceil(bottom)),
                               request.source.source_height);
           ++source_y) {
        const float weight_y = std::max(
            0.0F, std::min(bottom, source_y + 1.0F) -
                      std::max(top, static_cast<float>(source_y)));
        for (unsigned source_x = static_cast<unsigned>(std::floor(left));
             source_x < std::min(static_cast<unsigned>(std::ceil(right)),
                                 request.source.source_width);
             ++source_x) {
          const float weight_x = std::max(
              0.0F, std::min(right, source_x + 1.0F) -
                        std::max(left, static_cast<float>(source_x)));
          const float sample_weight = weight_x * weight_y;
          for (unsigned channel = 0; channel < 3U; ++channel)
            sum[channel] += sample_weight * source_channel(
                                                request.source, source,
                                                source_x, source_y, channel);
          total += sample_weight;
        }
      }
      if (!(total > 0.0F) || !std::isfinite(total)) {
        error = "invalid CPU exposure-proxy area";
        return false;
      }
      const auto pixel = static_cast<std::uint64_t>(y) * request.proxy_width + x;
      for (unsigned channel = 0; channel < 3U; ++channel)
        proxy[pixel * 3U + channel] = sum[channel] / total;
    }
  }
  error.clear();
  return true;
}

bool enumerate_cpu_exposure_pairs(const unsigned frame_count,
                                  CpuFramePair *pairs,
                                  const unsigned pair_capacity,
                                  unsigned &pair_count, std::string &error) {
  const std::uint64_t count = frame_count >= 2U
                                  ? static_cast<std::uint64_t>(frame_count) *
                                        (frame_count - 1U) / 2U
                                  : 0U;
  if (count > std::numeric_limits<unsigned>::max() ||
      pair_capacity != count || (count != 0U && pairs == nullptr)) {
    error = "invalid CPU exposure-pair buffer";
    return false;
  }
  pair_count = static_cast<unsigned>(count);
  unsigned index = 0;
  for (unsigned left = 0; left < frame_count; ++left)
    for (unsigned right = left + 1U; right < frame_count; ++right)
      pairs[index++] = {left, right};
  error.clear();
  return true;
}

bool project_cpu_exposure_pair(const CpuExposurePairRequest &request,
                               float *paired_coordinates,
                               std::uint8_t *overlap, std::string &error) {
  std::uint64_t sample_count = 0;
  if (request.sample_width == 0U || request.sample_height == 0U ||
      request.proxy_width == 0U || request.proxy_height == 0U ||
      paired_coordinates == nullptr || overlap == nullptr ||
      !checked_multiply(request.sample_width, request.sample_height,
                        sample_count)) {
    error = "invalid CPU exposure-pair projection request";
    return false;
  }
  if (!std::isfinite(request.latitude_span_degrees) ||
      request.latitude_span_degrees <= 0.0F ||
      request.latitude_span_degrees > 180.0F ||
      !std::isfinite(request.horizontal_fov_degrees) ||
      request.horizontal_fov_degrees <= 0.0F ||
      request.horizontal_fov_degrees >= 180.0F ||
      !std::isfinite(request.vertical_fov_degrees) ||
      request.vertical_fov_degrees <= 0.0F ||
      request.vertical_fov_degrees >= 180.0F ||
      !std::all_of(request.left_world_to_camera.begin(),
                   request.left_world_to_camera.end(),
                   [](const float value) { return std::isfinite(value); }) ||
      !std::all_of(request.right_world_to_camera.begin(),
                   request.right_world_to_camera.end(),
                   [](const float value) { return std::isfinite(value); })) {
    error = "invalid CPU exposure-pair projection geometry";
    return false;
  }
  constexpr float pi = 3.14159265358979323846F;
  const float focal_x = request.proxy_width /
                        (2.0F * std::tan(request.horizontal_fov_degrees * pi /
                                        360.0F));
  const float focal_y = request.proxy_height /
                        (2.0F * std::tan(request.vertical_fov_degrees * pi /
                                        360.0F));
  const auto project_one = [&](const std::array<float, 9> &rotation,
                               const std::array<float, 3> &world,
                               float *coordinate) {
    const float camera_x = world[0] * rotation[0] + world[1] * rotation[3] +
                           world[2] * rotation[6];
    const float camera_y = world[0] * rotation[1] + world[1] * rotation[4] +
                           world[2] * rotation[7];
    const float camera_z = world[0] * rotation[2] + world[1] * rotation[5] +
                           world[2] * rotation[8];
    const float safe_z = std::fabs(camera_z) > 1.0e-8F ? camera_z : 1.0F;
    const float x = (request.proxy_width - 1U) * 0.5F +
                    focal_x * camera_x / safe_z;
    const float y = (request.proxy_height - 1U) * 0.5F -
                    focal_y * camera_y / safe_z;
    coordinate[0] = std::clamp(x, 0.0F, request.proxy_width - 1.0F);
    coordinate[1] = std::clamp(y, 0.0F, request.proxy_height - 1.0F);
    return camera_z > 0.0F && x >= -0.5F &&
           x <= request.proxy_width - 0.5F && y >= -0.5F &&
           y <= request.proxy_height - 0.5F;
  };
  for (unsigned index = 0; index < sample_count; ++index) {
    const unsigned x = index % request.sample_width;
    const unsigned y = index / request.sample_width;
    const float longitude =
        ((x + 0.5F) / request.sample_width - 0.5F) * 2.0F * pi;
    const float latitude =
        (0.5F - (y + 0.5F) / request.sample_height) *
        request.latitude_span_degrees * pi / 180.0F;
    const float cosine = std::cos(latitude);
    const std::array<float, 3> world{cosine * std::sin(longitude),
                                     std::sin(latitude),
                                     cosine * std::cos(longitude)};
    const bool left_valid = project_one(request.left_world_to_camera, world,
                                        paired_coordinates + index * 4U);
    const bool right_valid = project_one(request.right_world_to_camera, world,
                                         paired_coordinates + index * 4U + 2U);
    overlap[index] = static_cast<std::uint8_t>(left_valid && right_valid);
  }
  error.clear();
  return true;
}

bool sample_cpu_exposure_pair(const CpuExposurePairRequest &request,
                              const float *left_proxy,
                              const float *right_proxy,
                              const float *paired_coordinates,
                              float *sampled_pairs, std::string &error) {
  std::uint64_t sample_count = 0;
  if (request.sample_width == 0U || request.sample_height == 0U ||
      request.proxy_width == 0U || request.proxy_height == 0U ||
      left_proxy == nullptr || right_proxy == nullptr ||
      paired_coordinates == nullptr || sampled_pairs == nullptr ||
      !checked_multiply(request.sample_width, request.sample_height,
                        sample_count)) {
    error = "invalid CPU exposure-pair sampling request";
    return false;
  }
  const auto sample_proxy = [&](const float *proxy, const float x, const float y,
                                float *output) {
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0F || y < 0.0F ||
        x > request.proxy_width - 1.0F || y > request.proxy_height - 1.0F)
      return false;
    const auto x0 = static_cast<unsigned>(std::floor(x));
    const auto y0 = static_cast<unsigned>(std::floor(y));
    const auto x1 = std::min(x0 + 1U, request.proxy_width - 1U);
    const auto y1 = std::min(y0 + 1U, request.proxy_height - 1U);
    const float wx = x - x0;
    const float wy = y - y0;
    for (unsigned channel = 0; channel < 3U; ++channel) {
      const float top = proxy[(y0 * request.proxy_width + x0) * 3U + channel] +
                        (proxy[(y0 * request.proxy_width + x1) * 3U + channel] -
                         proxy[(y0 * request.proxy_width + x0) * 3U + channel]) * wx;
      const float bottom = proxy[(y1 * request.proxy_width + x0) * 3U + channel] +
                           (proxy[(y1 * request.proxy_width + x1) * 3U + channel] -
                            proxy[(y1 * request.proxy_width + x0) * 3U + channel]) * wx;
      output[channel] = top + (bottom - top) * wy;
    }
    return true;
  };
  for (unsigned index = 0; index < sample_count; ++index) {
    if (!sample_proxy(left_proxy, paired_coordinates[index * 4U],
                      paired_coordinates[index * 4U + 1U],
                      sampled_pairs + index * 6U) ||
        !sample_proxy(right_proxy, paired_coordinates[index * 4U + 2U],
                      paired_coordinates[index * 4U + 3U],
                      sampled_pairs + index * 6U + 3U)) {
      error = "invalid CPU exposure-pair coordinate";
      return false;
    }
  }
  error.clear();
  return true;
}

bool classify_cpu_exposure_samples(
    const unsigned sample_count, const CpuTransferFunction transfer_function,
    const float *sampled_pairs, const std::uint8_t *geometric_overlap,
    float *pair_luminance, std::uint8_t *accepted, std::string &error) {
  if (sample_count == 0U || sampled_pairs == nullptr ||
      geometric_overlap == nullptr || pair_luminance == nullptr ||
      accepted == nullptr ||
      (transfer_function != CpuTransferFunction::srgb &&
       transfer_function != CpuTransferFunction::pq &&
       transfer_function != CpuTransferFunction::linear)) {
    error = "invalid CPU exposure classification request";
    return false;
  }
  for (unsigned index = 0; index < sample_count; ++index) {
    if (geometric_overlap[index] > 1U) {
      error = "invalid CPU geometric-overlap value";
      return false;
    }
    const float *const samples = sampled_pairs + index * 6U;
    const float first = samples[0] * 0.2126F + samples[1] * 0.7152F +
                        samples[2] * 0.0722F;
    const float second = samples[3] * 0.2126F + samples[4] * 0.7152F +
                         samples[5] * 0.0722F;
    pair_luminance[index * 2U] = first;
    pair_luminance[index * 2U + 1U] = second;
    const bool clipped = transfer_function != CpuTransferFunction::linear &&
                         std::any_of(samples, samples + 6U, [](const float value) {
                           return value >= 0.995F;
                         });
    accepted[index] = static_cast<std::uint8_t>(
        geometric_overlap[index] != 0U && std::isfinite(first) &&
        std::isfinite(second) && first > 1.0e-5F && second > 1.0e-5F &&
        !clipped);
  }
  error.clear();
  return true;
}

bool calculate_cpu_exposure_gradients(const unsigned sample_width,
                                      const unsigned sample_height,
                                      const float *pair_luminance,
                                      float *gradients, std::string &error) {
  if (sample_width == 0U || sample_height == 0U || pair_luminance == nullptr ||
      gradients == nullptr) {
    error = "invalid CPU exposure-gradient request";
    return false;
  }
  const auto logged = [&](const unsigned x, const unsigned y,
                          const unsigned channel) {
    return std::log(std::max(
        pair_luminance[(static_cast<std::uint64_t>(y) * sample_width + x) * 2U +
                       channel],
        1.0e-5F));
  };
  for (unsigned y = 0; y < sample_height; ++y) {
    const unsigned top = y == 0U ? std::min(1U, sample_height - 1U) : y - 1U;
    const unsigned bottom = y == sample_height - 1U
                                ? std::max(sample_height - 2U, 0U)
                                : y + 1U;
    for (unsigned x = 0; x < sample_width; ++x) {
      const unsigned left = x == 0U ? std::min(1U, sample_width - 1U) : x - 1U;
      const unsigned right = x == sample_width - 1U
                                 ? std::max(sample_width - 2U, 0U)
                                 : x + 1U;
      const auto index = static_cast<std::uint64_t>(y) * sample_width + x;
      for (unsigned channel = 0; channel < 2U; ++channel) {
        const float top_left = logged(left, top, channel);
        const float top_value = logged(x, top, channel);
        const float top_right = logged(right, top, channel);
        const float left_value = logged(left, y, channel);
        const float right_value = logged(right, y, channel);
        const float bottom_left = logged(left, bottom, channel);
        const float bottom_value = logged(x, bottom, channel);
        const float bottom_right = logged(right, bottom, channel);
        const float horizontal = -top_left + top_right - 2.0F * left_value +
                                 2.0F * right_value - bottom_left + bottom_right;
        const float vertical = -top_left - 2.0F * top_value - top_right +
                               bottom_left + 2.0F * bottom_value + bottom_right;
        gradients[index * 2U + channel] =
            std::sqrt(horizontal * horizontal + vertical * vertical);
      }
    }
  }
  error.clear();
  return true;
}

bool filter_cpu_exposure_gradients(const unsigned sample_count,
                                   const float *gradients,
                                   std::uint8_t *accepted,
                                   std::array<float, 2> &limits,
                                   std::string &error) {
  if (sample_count == 0U || gradients == nullptr || accepted == nullptr) {
    error = "invalid CPU exposure-gradient filter request";
    return false;
  }
  for (unsigned channel = 0; channel < 2U; ++channel) {
    std::vector<float> finite;
    finite.reserve(sample_count);
    for (unsigned index = 0; index < sample_count; ++index)
      if (std::isfinite(gradients[index * 2U + channel]))
        finite.push_back(gradients[index * 2U + channel]);
    if (finite.empty()) {
      limits[channel] = std::numeric_limits<float>::quiet_NaN();
      continue;
    }
    std::sort(finite.begin(), finite.end());
    const double position = static_cast<double>(finite.size() - 1U) * 0.9;
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    const float fraction = static_cast<float>(position - lower);
    limits[channel] = finite[lower] * (1.0F - fraction) +
                      finite[upper] * fraction;
  }
  for (unsigned index = 0; index < sample_count; ++index) {
    if (accepted[index] > 1U) {
      error = "invalid CPU exposure acceptance value";
      return false;
    }
    accepted[index] = static_cast<std::uint8_t>(
        accepted[index] != 0U && std::isfinite(gradients[index * 2U]) &&
        std::isfinite(gradients[index * 2U + 1U]) &&
        gradients[index * 2U] <= limits[0] &&
        gradients[index * 2U + 1U] <= limits[1]);
  }
  error.clear();
  return true;
}

bool reduce_cpu_exposure_pair(const unsigned sample_count,
                              const float *pair_luminance,
                              const std::uint8_t *accepted,
                              CpuExposurePairReduction &reduction,
                              std::string &error) {
  if (sample_count == 0U || pair_luminance == nullptr || accepted == nullptr) {
    error = "invalid CPU exposure-pair reduction request";
    return false;
  }
  std::vector<float> ratios;
  ratios.reserve(sample_count);
  for (unsigned index = 0; index < sample_count; ++index) {
    if (accepted[index] > 1U) {
      error = "invalid CPU exposure reduction acceptance value";
      return false;
    }
    const float first = pair_luminance[index * 2U];
    const float second = pair_luminance[index * 2U + 1U];
    if (accepted[index] != 0U && std::isfinite(first) &&
        std::isfinite(second) && first > 1.0e-5F && second > 1.0e-5F)
      ratios.push_back(std::log(first / second));
  }
  std::sort(ratios.begin(), ratios.end());
  reduction = {};
  reduction.valid_count = static_cast<unsigned>(ratios.size());
  reduction.difference = std::numeric_limits<float>::quiet_NaN();
  reduction.mad = std::numeric_limits<float>::quiet_NaN();
  if (!ratios.empty()) {
    const auto quantile = [&](const unsigned numerator) {
      const unsigned span = static_cast<unsigned>(ratios.size() - 1U);
      const unsigned units = span * numerator;
      const unsigned lower = units / 10U;
      const unsigned remainder = units % 10U;
      const unsigned upper = lower + (remainder != 0U ? 1U : 0U);
      const float fraction = remainder / 10.0F;
      return ratios[lower] * (1.0F - fraction) + ratios[upper] * fraction;
    };
    const float lower = quantile(1U);
    const float upper = quantile(9U);
    std::vector<float> inliers;
    inliers.reserve(ratios.size());
    std::copy_if(ratios.begin(), ratios.end(), std::back_inserter(inliers),
                 [&](const float ratio) { return ratio >= lower && ratio <= upper; });
    reduction.inlier_count = static_cast<unsigned>(inliers.size());
    if (!inliers.empty()) {
      const auto median = [](const std::vector<float> &values) {
        const auto lower_index = (values.size() - 1U) / 2U;
        const auto upper_index = values.size() / 2U;
        return 0.5F * (values[lower_index] + values[upper_index]);
      };
      reduction.difference = median(inliers);
      std::vector<float> deviations;
      deviations.reserve(inliers.size());
      std::transform(inliers.begin(), inliers.end(),
                     std::back_inserter(deviations), [&](const float ratio) {
                       return std::fabs(ratio - reduction.difference);
                     });
      std::sort(deviations.begin(), deviations.end());
      reduction.mad = median(deviations);
    }
  }
  if (reduction.valid_count < 24U)
    reduction.rejection = CpuExposurePairRejection::insufficient_valid;
  else if (reduction.inlier_count < 12U)
    reduction.rejection = CpuExposurePairRejection::insufficient_inliers;
  else if (!std::isfinite(reduction.difference) ||
           !std::isfinite(reduction.mad))
    reduction.rejection = CpuExposurePairRejection::non_finite;
  else if (reduction.mad > 0.5F)
    reduction.rejection = CpuExposurePairRejection::excessive_mad;
  else {
    reduction.rejection = CpuExposurePairRejection::accepted;
    reduction.weight = std::sqrt(static_cast<float>(reduction.inlier_count)) /
                       (1.0F + reduction.mad);
  }
  error.clear();
  return true;
}

bool build_cpu_exposure_solve_graph(
    const unsigned frame_count,
    const CpuExposurePairMeasurement *const measurements,
    const unsigned measurement_count,
    std::vector<CpuExposureEquation> &equations, std::string &error) {
  const std::uint64_t expected_count = frame_count >= 2U
                                           ? static_cast<std::uint64_t>(frame_count) *
                                                 (frame_count - 1U) / 2U
                                           : 0U;
  if (frame_count == 0U || measurement_count != expected_count ||
      (measurement_count != 0U && measurements == nullptr)) {
    error = "invalid CPU exposure solve-graph request";
    return false;
  }
  try {
    const std::size_t matrix_size = static_cast<std::size_t>(frame_count) * frame_count;
    std::vector<std::uint8_t> adjacency(matrix_size, 0U);
    std::vector<std::uint8_t> geometric(matrix_size, 0U);
    std::vector<std::uint8_t> bridges(matrix_size, 0U);
    std::vector<CpuExposureEquation> built;
    built.reserve(measurement_count + frame_count - 1U);
    for (unsigned frame = 0; frame < frame_count; ++frame)
      adjacency[static_cast<std::size_t>(frame) * frame_count + frame] = 1U;
    unsigned expected_index = 0;
    for (unsigned left = 0; left < frame_count; ++left) {
      for (unsigned right = left + 1U; right < frame_count; ++right) {
        const auto &measurement = measurements[expected_index++];
        if (measurement.pair.left != left || measurement.pair.right != right) {
          error = "CPU exposure measurements are not in capture-pair order";
          return false;
        }
        const auto matrix_index = static_cast<std::size_t>(left) * frame_count + right;
        if (measurement.geometric_count >= 24U) {
          geometric[matrix_index] = 1U;
          geometric[static_cast<std::size_t>(right) * frame_count + left] = 1U;
        }
        if (measurement.reduction.rejection ==
            CpuExposurePairRejection::accepted) {
          if (!std::isfinite(measurement.reduction.difference) ||
              !std::isfinite(measurement.reduction.weight) ||
              measurement.reduction.weight <= 0.0F) {
            error = "invalid measured CPU exposure equation";
            return false;
          }
          built.push_back({left, right, measurement.reduction.difference,
                           measurement.reduction.weight});
          adjacency[matrix_index] = 1U;
          adjacency[static_cast<std::size_t>(right) * frame_count + left] = 1U;
        }
      }
    }
    for (unsigned iteration = 0; iteration < frame_count; ++iteration) {
      auto reachability = adjacency;
      for (unsigned middle = 0; middle < frame_count; ++middle)
        for (unsigned row = 0; row < frame_count; ++row)
          if (reachability[static_cast<std::size_t>(row) * frame_count + middle] != 0U)
            for (unsigned column = 0; column < frame_count; ++column)
              reachability[static_cast<std::size_t>(row) * frame_count + column] |=
                  reachability[static_cast<std::size_t>(middle) * frame_count + column];
      std::vector<std::uint8_t> additions(matrix_size, 0U);
      for (unsigned row = 0; row < frame_count; ++row) {
        for (unsigned column = 0; column < frame_count; ++column) {
          const auto index = static_cast<std::size_t>(row) * frame_count + column;
          if (geometric[index] != 0U && reachability[index] == 0U) {
            additions[index] = 1U;
            additions[static_cast<std::size_t>(column) * frame_count + row] = 1U;
            break;
          }
        }
      }
      for (std::size_t index = 0; index < matrix_size; ++index) {
        bridges[index] |= additions[index];
        adjacency[index] |= additions[index];
      }
    }
    for (unsigned left = 0; left < frame_count; ++left)
      for (unsigned right = left + 1U; right < frame_count; ++right)
        if (bridges[static_cast<std::size_t>(left) * frame_count + right] != 0U)
          built.push_back({left, right, 0.0, 1.0});
    equations.swap(built);
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate CPU exposure solve graph";
    return false;
  }
}

bool solve_cpu_exposure_graph(
    const unsigned frame_count,
    const std::vector<CpuExposureEquation> &equations,
    CpuExposureSolveResult &result, std::string &error) {
  if (frame_count == 0U) {
    error = "invalid CPU exposure solve request";
    return false;
  }
  try {
    const std::size_t count = frame_count;
    std::vector<double> system(count * count, 0.0);
    std::vector<double> values(count, 0.0);
    for (const auto &edge : equations) {
      if (edge.left >= frame_count || edge.right >= frame_count ||
          edge.left == edge.right || !std::isfinite(edge.difference) ||
          !std::isfinite(edge.weight) || edge.weight <= 0.0) {
        error = "invalid CPU exposure solve equation";
        return false;
      }
      system[static_cast<std::size_t>(edge.left) * count + edge.left] += edge.weight;
      system[static_cast<std::size_t>(edge.right) * count + edge.right] += edge.weight;
      system[static_cast<std::size_t>(edge.left) * count + edge.right] -= edge.weight;
      system[static_cast<std::size_t>(edge.right) * count + edge.left] -= edge.weight;
      values[edge.left] -= edge.weight * edge.difference;
      values[edge.right] += edge.weight * edge.difference;
    }
    system[0] += 1.0;
    for (std::size_t column = 0; column < count; ++column) {
      std::size_t pivot_row = column;
      double pivot_size = std::fabs(system[column * count + column]);
      for (std::size_t row = column + 1U; row < count; ++row) {
        const double candidate = std::fabs(system[row * count + column]);
        if (candidate > pivot_size) {
          pivot_size = candidate;
          pivot_row = row;
        }
      }
      if (pivot_size <= 1.0e-12) continue;
      if (pivot_row != column) {
        for (std::size_t entry = column; entry < count; ++entry)
          std::swap(system[column * count + entry],
                    system[pivot_row * count + entry]);
        std::swap(values[column], values[pivot_row]);
      }
      const double pivot = system[column * count + column];
      for (std::size_t entry = column; entry < count; ++entry)
        system[column * count + entry] /= pivot;
      values[column] /= pivot;
      for (std::size_t row = 0; row < count; ++row) {
        if (row == column) continue;
        const double factor = system[row * count + column];
        if (factor == 0.0) continue;
        for (std::size_t entry = column; entry < count; ++entry)
          system[row * count + entry] -=
              factor * system[column * count + entry];
        values[row] -= factor * values[column];
      }
    }
    std::vector<double> solution(count, 0.0);
    for (std::size_t row = 0; row < count; ++row)
      if (std::fabs(system[row * count + row]) > 1.0e-12)
        solution[row] = values[row];
    auto ordered = solution;
    std::sort(ordered.begin(), ordered.end());
    const auto median_of = [](const std::vector<double> &values_to_measure) {
      const auto size = values_to_measure.size();
      return size % 2U == 0U
                 ? 0.5 * (values_to_measure[size / 2U - 1U] +
                          values_to_measure[size / 2U])
                 : values_to_measure[size / 2U];
    };
    const double median = median_of(ordered);
    for (double &value : solution) value -= median;
    ordered = solution;
    std::sort(ordered.begin(), ordered.end());
    const double centered_median = median_of(ordered);
    unsigned anchor = 0;
    for (unsigned frame = 1; frame < frame_count; ++frame)
      if (std::fabs(solution[frame] - centered_median) <
          std::fabs(solution[anchor] - centered_median))
        anchor = frame;
    constexpr double limit = 0.69314718055994530942;
    CpuExposureSolveResult solved;
    solved.anchor_frame = anchor;
    solved.edge_count = static_cast<unsigned>(equations.size());
    solved.log_gains.reserve(count);
    for (const double value : solution)
      solved.log_gains.push_back(
          static_cast<float>(std::clamp(value, -limit, limit)));
    result = std::move(solved);
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate CPU exposure solve storage";
    return false;
  }
}

bool solve_reference_exposure_gains(
    const unsigned frame_count, const unsigned target,
    const std::vector<CpuExposureEquation> &equations,
    const unsigned measured_equation_count,
    const std::vector<float> &current_gains, std::vector<float> &gains,
    std::string &error) {
  if (frame_count < 2U || target >= frame_count ||
      static_cast<std::size_t>(measured_equation_count) > equations.size() ||
      current_gains.size() != frame_count ||
      std::any_of(current_gains.begin(), current_gains.end(),
                  [](const float gain) {
                    return !std::isfinite(gain) || gain <= 0.0F;
                  })) {
    error = "invalid reference exposure solve request";
    return false;
  }
  for (std::size_t index = 0; index < equations.size(); ++index) {
    const auto &equation = equations[index];
    if (equation.left >= frame_count || equation.right >= frame_count ||
        equation.left == equation.right ||
        !std::isfinite(equation.difference) ||
        !std::isfinite(equation.weight) || equation.weight <= 0.0 ||
        (index >= measured_equation_count && equation.difference != 0.0)) {
      error = "invalid reference exposure equation";
      return false;
    }
  }
  try {
    std::vector<std::optional<double>> corrections(frame_count);
    corrections[target] = 0.0;
    unsigned corrected = 1U;
    while (corrected < frame_count) {
      std::vector<std::vector<double>> proposals(frame_count);
      for (std::size_t index = 0; index < equations.size(); ++index) {
        const auto &equation = equations[index];
        const double adjusted =
            index < measured_equation_count
                ? equation.difference +
                      std::log(current_gains[equation.left]) -
                      std::log(current_gains[equation.right])
                : 0.0;
        if (corrections[equation.left].has_value() &&
            !corrections[equation.right].has_value())
          proposals[equation.right].push_back(
              *corrections[equation.left] + adjusted);
        else if (corrections[equation.right].has_value() &&
                 !corrections[equation.left].has_value())
          proposals[equation.left].push_back(
              *corrections[equation.right] - adjusted);
      }
      unsigned added = 0U;
      for (unsigned frame = 0; frame < frame_count; ++frame) {
        auto &candidates = proposals[frame];
        if (candidates.empty()) continue;
        std::sort(candidates.begin(), candidates.end());
        const auto middle = candidates.size() / 2U;
        corrections[frame] = candidates.size() % 2U == 0U
                                 ? 0.5 * (candidates[middle - 1U] +
                                          candidates[middle])
                                 : candidates[middle];
        ++added;
      }
      if (added == 0U) {
        error = "automatic exposure cannot reach every pose because the "
                "geometric overlap graph is disconnected; correct the "
                "unreachable poses manually";
        return false;
      }
      corrected += added;
    }
    std::vector<float> solved;
    solved.reserve(frame_count);
    for (unsigned frame = 0; frame < frame_count; ++frame) {
      const double gain = current_gains[frame] * std::exp(*corrections[frame]);
      if (!std::isfinite(gain) || gain <= 0.0 ||
          gain > std::numeric_limits<float>::max()) {
        error = "reference exposure gain is invalid";
        return false;
      }
      solved.push_back(static_cast<float>(gain));
    }
    gains.swap(solved);
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate reference exposure solve storage";
    return false;
  }
}

bool make_cpu_exposure_report(const CpuExposureSolveResult &solved,
                              const std::vector<float> &manual_gains,
                              CpuExposureReport &report, std::string &error) {
  if (solved.log_gains.empty() || solved.anchor_frame >= solved.log_gains.size() ||
      (!manual_gains.empty() && manual_gains.size() != solved.log_gains.size())) {
    error = "invalid CPU exposure report inputs";
    return false;
  }
  CpuExposureReport built;
  built.anchor_frame = solved.anchor_frame;
  built.edge_count = solved.edge_count;
  built.warning = solved.log_gains.size() > 1U &&
                  solved.edge_count < solved.log_gains.size() - 1U;
  try {
    built.gains.reserve(solved.log_gains.size());
    for (std::size_t index = 0; index < solved.log_gains.size(); ++index) {
      const float manual = manual_gains.empty() ? 1.0F : manual_gains[index];
      const float gain = std::exp(solved.log_gains[index]) * manual;
      if (!std::isfinite(manual) || manual <= 0.0F || !std::isfinite(gain) ||
          gain <= 0.0F) {
        error = "invalid manual CPU exposure gain";
        return false;
      }
      built.gains.push_back(gain);
    }
  } catch (const std::bad_alloc &) {
    error = "cannot allocate CPU exposure report";
    return false;
  }
  report = std::move(built);
  error.clear();
  return true;
}

bool apply_cpu_exposure_match(CpuExposureEdits &edits, const float gain,
                              std::string &error) {
  if (edits.gains.empty() || !std::isfinite(gain) || gain <= 0.0F ||
      (edits.target.has_value() && *edits.target >= edits.gains.size())) {
    error = "invalid CPU exposure match";
    return false;
  }
  auto updated = edits.gains;
  for (const unsigned selected : edits.selected) {
    if (selected >= updated.size() || !std::isfinite(updated[selected]) ||
        updated[selected] <= 0.0F || !std::isfinite(updated[selected] * gain)) {
      error = "invalid selected CPU exposure frame";
      return false;
    }
    updated[selected] *= gain;
  }
  edits.gains.swap(updated);
  error.clear();
  return true;
}

bool discard_cpu_exposure_edits(CpuExposureEdits &edits,
                                std::string &error) {
  if (edits.gains.empty()) {
    error = "invalid CPU exposure edits";
    return false;
  }
  edits.gains.assign(edits.gains.size(), 1.0F);
  error.clear();
  return true;
}

bool create_cpu_exposure_cache(CpuExposureCache **const cache,
                               std::string &error) {
  if (cache == nullptr || *cache != nullptr) {
    error = "invalid CPU exposure cache output";
    return false;
  }
  try {
    *cache = new CpuExposureCache();
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot allocate CPU exposure cache";
    return false;
  }
}

bool query_cpu_exposure_cache(const CpuExposureCache *const cache,
                              const std::string &identity,
                              CpuExposureReport &report, bool &hit,
                              std::string &error) {
  if (cache == nullptr || identity.empty()) {
    error = "invalid CPU exposure cache query";
    return false;
  }
  std::lock_guard<std::mutex> lock(cache->mutex);
  hit = cache->report.has_value() && cache->identity == identity;
  if (hit) report = *cache->report;
  error.clear();
  return true;
}

bool store_cpu_exposure_cache(CpuExposureCache *const cache,
                              const std::string &identity,
                              const CpuExposureReport &report,
                              std::string &error) {
  if (cache == nullptr || identity.empty() || report.gains.empty() ||
      report.anchor_frame >= report.gains.size() ||
      !std::all_of(report.gains.begin(), report.gains.end(),
                   [](const float gain) {
                     return std::isfinite(gain) && gain > 0.0F;
                   })) {
    error = "invalid CPU exposure cache entry";
    return false;
  }
  try {
    std::lock_guard<std::mutex> lock(cache->mutex);
    cache->identity = identity;
    cache->report = report;
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    error = "cannot store CPU exposure cache entry";
    return false;
  }
}

void invalidate_cpu_exposure_cache(CpuExposureCache *const cache) noexcept {
  if (cache == nullptr) return;
  std::lock_guard<std::mutex> lock(cache->mutex);
  cache->identity.clear();
  cache->report.reset();
}

void destroy_cpu_exposure_cache(CpuExposureCache **const cache) noexcept {
  if (cache == nullptr || *cache == nullptr) return;
  delete *cache;
  *cache = nullptr;
}

bool accumulate_cpu_auto_contrast_histogram(
    const CpuSdrConversionRequest &request, const float *linear_rgb,
    const std::uint8_t *coverage,
    std::array<std::uint64_t, 4096> &histogram, std::string &error) {
  if (request.pixel_count == 0U || linear_rgb == nullptr || coverage == nullptr ||
      !std::isfinite(request.reference_white_nits) ||
      request.reference_white_nits <= 0.0F ||
      !std::isfinite(request.exposure_multiplier) ||
      request.exposure_multiplier <= 0.0F ||
      (request.source_transfer != CpuTransferFunction::srgb &&
       request.source_transfer != CpuTransferFunction::pq &&
       request.source_transfer != CpuTransferFunction::linear) ||
      (request.source_primaries != CpuColorPrimaries::srgb &&
       request.source_primaries != CpuColorPrimaries::rec2020)) {
    error = "invalid CPU auto-contrast histogram request";
    return false;
  }
  for (unsigned index = 0; index < request.pixel_count; ++index) {
    if (coverage[index] > 1U) {
      error = "invalid CPU histogram coverage value";
      return false;
    }
    if (coverage[index] == 0U) continue;
    std::array<float, 3> encoded{};
    if (!convert_sdr_pixel(request, linear_rgb + index * 3U, encoded)) continue;
    const float luminance = std::clamp(encoded[0] * 0.2126F +
                                           encoded[1] * 0.7152F +
                                           encoded[2] * 0.0722F,
                                       0.0F, 1.0F);
    const auto bin = std::min(4095U,
                              static_cast<unsigned>(std::floor(luminance * 4096.0F)));
    ++histogram[bin];
  }
  error.clear();
  return true;
}

bool select_cpu_auto_contrast_levels(
    const std::array<std::uint64_t, 4096> &histogram,
    CpuAutoContrastLevels &levels, std::string &error) {
  std::uint64_t total = 0;
  for (const std::uint64_t count : histogram) {
    if (count > std::numeric_limits<std::uint64_t>::max() - total) {
      error = "CPU auto-contrast histogram count overflows";
      return false;
    }
    total += count;
  }
  CpuAutoContrastLevels selected;
  selected.processed_pixels = total;
  if (total < 2U) {
    levels = selected;
    error.clear();
    return true;
  }
  const double black_rank = 0.005 * static_cast<double>(total - 1U);
  const double white_rank = 0.995 * static_cast<double>(total - 1U);
  std::uint64_t cumulative = 0;
  bool black_found = false;
  for (unsigned bin = 0; bin < histogram.size(); ++bin) {
    const std::uint64_t previous = cumulative;
    cumulative += histogram[bin];
    if (!black_found && static_cast<double>(cumulative) >= black_rank + 1.0) {
      selected.black = static_cast<float>(
          (bin + (black_rank - static_cast<double>(previous)) /
                     static_cast<double>(histogram[bin])) /
          4096.0);
      black_found = true;
    }
    if (static_cast<double>(cumulative) >= white_rank + 1.0) {
      selected.white = static_cast<float>(
          (bin + (white_rank - static_cast<double>(previous)) /
                     static_cast<double>(histogram[bin])) /
          4096.0);
      break;
    }
  }
  selected.valid = std::isfinite(selected.black) &&
                   std::isfinite(selected.white) &&
                   selected.white - selected.black >= 1.0F / 4096.0F;
  if (!selected.valid) {
    selected.black = 0.0F;
    selected.white = 1.0F;
  }
  levels = selected;
  error.clear();
  return true;
}

bool convert_cpu_sdr8_band(const CpuSdrConversionRequest &request,
                           const float *linear_rgb, std::uint8_t *srgb8,
                           std::string &error) {
  if (request.pixel_count == 0U || linear_rgb == nullptr || srgb8 == nullptr ||
      !std::isfinite(request.reference_white_nits) ||
      request.reference_white_nits <= 0.0F ||
      !std::isfinite(request.exposure_multiplier) ||
      request.exposure_multiplier <= 0.0F ||
      (request.apply_auto_contrast && !request.levels.valid)) {
    error = "invalid CPU SDR conversion request";
    return false;
  }
  for (unsigned index = 0; index < request.pixel_count; ++index) {
    std::array<float, 3> encoded{};
    if (!convert_sdr_pixel(request, linear_rgb + index * 3U, encoded)) {
      error = "non-finite CPU SDR input";
      return false;
    }
    for (unsigned channel = 0; channel < 3U; ++channel) {
      const float scaled = encoded[channel] * 255.0F;
      const float lower_float = std::floor(scaled);
      unsigned value = static_cast<unsigned>(lower_float);
      const float fraction = scaled - lower_float;
      if (fraction > 0.5F || (fraction == 0.5F && (value & 1U) != 0U)) ++value;
      srgb8[index * 3U + channel] = static_cast<std::uint8_t>(value);
    }
  }
  error.clear();
  return true;
}

bool copy_cpu_float_band(const unsigned pixel_count, const float *linear_rgb,
                         float *output_rgb, std::string &error) {
  if (pixel_count == 0U || linear_rgb == nullptr || output_rgb == nullptr) {
    error = "invalid CPU float-band request";
    return false;
  }
  const auto component_count = static_cast<std::uint64_t>(pixel_count) * 3U;
  for (std::uint64_t component = 0; component < component_count; ++component) {
    if (!std::isfinite(linear_rgb[component])) {
      error = "non-finite CPU float output";
      return false;
    }
    output_rgb[component] = linear_rgb[component];
  }
  error.clear();
  return true;
}

bool convert_and_write_cpu_band(
    ImageWriter *const writer, const CpuOutputBandSample output_sample,
    const CpuSdrConversionRequest &request, const unsigned width,
    const unsigned row_count, const float *linear_rgb,
    void *const conversion_scratch,
    const std::uint64_t conversion_scratch_bytes,
    const CancellationCheck &cancellation, CodecErrorCategory &category,
    std::string &error) {
  std::uint64_t pixel_count = 0;
  if (writer == nullptr || width == 0U || row_count == 0U ||
      linear_rgb == nullptr || !checked_multiply(width, row_count, pixel_count) ||
      pixel_count != request.pixel_count) {
    category = CodecErrorCategory::invalid_request;
    error = "invalid CPU output band";
    return false;
  }
  if (output_sample == CpuOutputBandSample::srgb8) {
    std::uint64_t expected_bytes = 0;
    if (!checked_multiply(pixel_count, 3U, expected_bytes) ||
        !exact_buffer(conversion_scratch, conversion_scratch_bytes,
                      expected_bytes) ||
        !convert_cpu_sdr8_band(
            request, linear_rgb,
            static_cast<std::uint8_t *>(conversion_scratch), error)) {
      category = CodecErrorCategory::invalid_request;
      return false;
    }
    return write_image_rows(writer, conversion_scratch, row_count,
                            static_cast<std::uint64_t>(width) * 3U,
                            cancellation, category, error);
  }
  if (output_sample != CpuOutputBandSample::linear_float32) {
    category = CodecErrorCategory::invalid_request;
    error = "invalid CPU output sample type";
    return false;
  }
  for (std::uint64_t component = 0; component < pixel_count * 3U; ++component) {
    if (!std::isfinite(linear_rgb[component])) {
      category = CodecErrorCategory::invalid_request;
      error = "non-finite CPU float output";
      return false;
    }
  }
  return write_image_rows(writer, linear_rgb, row_count,
                          static_cast<std::uint64_t>(width) * 3U * sizeof(float),
                          cancellation, category, error);
}

bool create_cpu_render_coordinator(CpuRenderCoordinator **const coordinator,
                                   std::string &error) {
  if (coordinator == nullptr || *coordinator != nullptr) {
    error = "invalid CPU render coordinator output";
    return false;
  }
  auto *created = new (std::nothrow) CpuRenderCoordinator();
  if (created == nullptr) {
    error = "cannot allocate CPU render coordinator";
    return false;
  }
  *coordinator = created;
  error.clear();
  return true;
}

bool run_cpu_render_pipeline(CpuRenderCoordinator *const coordinator,
                             const CpuRenderPipelineCallbacks &callbacks,
                             const CancellationCheck &cancellation,
                             std::string &error) {
  if (coordinator == nullptr || callbacks.run == nullptr ||
      callbacks.cleanup == nullptr) {
    error = "invalid CPU render pipeline request";
    return false;
  }
  bool expected = false;
  if (!coordinator->busy.compare_exchange_strong(expected, true,
                                                  std::memory_order_acq_rel)) {
    error = "CPU render is already active";
    return false;
  }
  struct BusyReset {
    CpuRenderCoordinator *coordinator;
    ~BusyReset() { coordinator->busy.store(false, std::memory_order_release); }
  } reset{coordinator};
  const auto cancelled = [&] {
    return cancellation.callback != nullptr &&
           cancellation.callback(cancellation.user_data);
  };
  constexpr std::array<CpuRenderPhase, 5> phases{
      CpuRenderPhase::allocation, CpuRenderPhase::decode,
      CpuRenderPhase::compose, CpuRenderPhase::encode,
      CpuRenderPhase::publish};
  try {
    for (const auto phase : phases) {
      if (cancelled()) {
        callbacks.cleanup(callbacks.user_data);
        error = "CPU render cancelled";
        return false;
      }
      if (!callbacks.run(callbacks.user_data, phase)) {
        callbacks.cleanup(callbacks.user_data);
        error = "CPU render phase failed";
        return false;
      }
      if (cancelled()) {
        callbacks.cleanup(callbacks.user_data);
        error = "CPU render cancelled";
        return false;
      }
    }
  } catch (...) {
    callbacks.cleanup(callbacks.user_data);
    error = "unexpected CPU render pipeline failure";
    return false;
  }
  error.clear();
  return true;
}

void destroy_cpu_render_coordinator(
    CpuRenderCoordinator **const coordinator) noexcept {
  if (coordinator == nullptr || *coordinator == nullptr) return;
  delete *coordinator;
  *coordinator = nullptr;
}

} // namespace pano::app
