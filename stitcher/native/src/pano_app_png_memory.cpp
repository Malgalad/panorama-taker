#include "pano_app.h"

#include <algorithm>
#include <limits>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace pano::app {
bool query_host_memory(HostMemoryStatus &memory) noexcept {
  memory = {};
#ifdef _WIN32
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status))
    return false;
  memory = {status.ullTotalPhys, status.ullAvailPhys, status.ullAvailPageFile};
  return true;
#else
  return false;
#endif
}

bool default_fast_png(const std::uint64_t total_bytes) noexcept {
  return total_bytes > 16ULL * 1024U * 1024U * 1024U;
}

bool fast_png_memory_admitted(const unsigned width, const unsigned height,
                              const HostMemoryStatus &memory,
                              const bool input_buffered) noexcept {
  constexpr std::uint64_t mib = 1024U * 1024U;
  const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
  // Budget six RGB-sized buffers, including worst-case compressed output,
  // plus workspace. Do not count the already resident input twice at finish.
  const std::uint64_t factor = input_buffered ? 15U : 18U;
  const std::uint64_t reserve = std::max(1024U * mib, memory.total_bytes / 10U);
  if (pixels == 0 || pixels > (std::numeric_limits<std::uint64_t>::max() -
                               64U * mib - reserve) /
                                  factor)
    return false;
  const std::uint64_t required = pixels * factor + 64U * mib;
  return memory.total_bytes != 0 &&
         memory.available_bytes >= required + reserve &&
         memory.available_commit_bytes >= required + 512U * mib;
}

const char *png_encoder_message(const PngEncoderStatus status) noexcept {
  switch (status) {
  case PngEncoderStatus::oxipng:
    return "PNG: OxiPNG (higher RAM)";
  case PngEncoderStatus::low_memory:
    return "PNG: WIC fallback (low available memory)";
  case PngEncoderStatus::unavailable:
    return "PNG: WIC fallback (OxiPNG unavailable)";
  case PngEncoderStatus::allocation:
    return "PNG: WIC fallback (buffer allocation failed)";
  default:
    return "PNG: WIC";
  }
}
} // namespace pano::app
