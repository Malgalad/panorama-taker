#include "pano_app.h"
#include "pano_gpu.h"

#include <openexr.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>
#endif

namespace pano::app {
namespace {
namespace fs = std::filesystem;

bool cancelled(const CancellationCheck &check) {
  return check.callback != nullptr && check.callback(check.user_data);
}

bool fail(const CodecErrorCategory value, const std::string &message,
          CodecErrorCategory &category, std::string &error) {
  category = value;
  error = message;
  return false;
}

std::uint16_t read_be16(const std::uint8_t *data) {
  return static_cast<std::uint16_t>((static_cast<unsigned>(data[0]) << 8U) |
                                    static_cast<unsigned>(data[1]));
}

std::uint32_t read_be32(const std::uint8_t *data) {
  return (static_cast<std::uint32_t>(data[0]) << 24U) |
         (static_cast<std::uint32_t>(data[1]) << 16U) |
         (static_cast<std::uint32_t>(data[2]) << 8U) |
         static_cast<std::uint32_t>(data[3]);
}

bool read_exact(std::istream &stream, void *data, const std::size_t size) {
  stream.read(static_cast<char *>(data), static_cast<std::streamsize>(size));
  return stream.good() ||
         (stream.eof() &&
          stream.gcount() == static_cast<std::streamsize>(size));
}

std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t *data,
                           const std::size_t size) {
  for (std::size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1U) ^
            (0xedb88320U &
             static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & 1U)));
  }
  return crc;
}

bool inspect_png(std::ifstream &stream, const std::uint64_t file_size,
                 ImageInfo &info, CodecErrorCategory &category,
                 std::string &error) {
  std::array<std::uint8_t, 8> signature{};
  if (!read_exact(stream, signature.data(), signature.size()) ||
      signature != std::array<std::uint8_t, 8>{0x89, 'P', 'N', 'G', '\r', '\n',
                                               0x1a, '\n'})
    return fail(CodecErrorCategory::malformed, "invalid PNG signature",
                category, error);

  bool have_header = false;
  bool have_cicp = false;
  for (;;) {
    const auto position = stream.tellg();
    if (position < 0 || static_cast<std::uint64_t>(position) + 12U > file_size)
      return fail(CodecErrorCategory::malformed, "truncated PNG chunk header",
                  category, error);
    std::array<std::uint8_t, 8> chunk_header{};
    if (!read_exact(stream, chunk_header.data(), chunk_header.size()))
      return fail(CodecErrorCategory::malformed, "truncated PNG chunk header",
                  category, error);
    const std::uint32_t length = read_be32(chunk_header.data());
    const std::array<std::uint8_t, 4> type = {chunk_header[4], chunk_header[5],
                                              chunk_header[6], chunk_header[7]};
    const auto payload_position = stream.tellg();
    if (payload_position < 0 ||
        static_cast<std::uint64_t>(payload_position) + length + 4U > file_size)
      return fail(CodecErrorCategory::malformed, "truncated PNG chunk payload",
                  category, error);
    if (!have_header && type != std::array<std::uint8_t, 4>{'I', 'H', 'D', 'R'})
      return fail(CodecErrorCategory::malformed, "PNG IHDR must be first",
                  category, error);
    if (type == std::array<std::uint8_t, 4>{'I', 'D', 'A', 'T'}) {
      if (!have_header)
        return fail(CodecErrorCategory::malformed, "PNG has no IHDR", category,
                    error);
      break;
    }
    if (type == std::array<std::uint8_t, 4>{'I', 'E', 'N', 'D'})
      return fail(CodecErrorCategory::malformed, "PNG has no image data",
                  category, error);

    std::uint32_t crc = crc32_update(0xffffffffU, type.data(), type.size());
    std::array<std::uint8_t, 4096> block{};
    std::array<std::uint8_t, 13> header{};
    std::array<std::uint8_t, 4> cicp{};
    std::uint32_t remaining = length;
    std::size_t copied = 0;
    while (remaining != 0U) {
      const std::size_t count = std::min<std::size_t>(remaining, block.size());
      if (!read_exact(stream, block.data(), count))
        return fail(CodecErrorCategory::malformed,
                    "truncated PNG chunk payload", category, error);
      crc = crc32_update(crc, block.data(), count);
      if (type == std::array<std::uint8_t, 4>{'I', 'H', 'D', 'R'} &&
          copied + count <= header.size())
        std::copy_n(block.data(), count, header.data() + copied);
      if (type == std::array<std::uint8_t, 4>{'c', 'I', 'C', 'P'} &&
          copied + count <= cicp.size())
        std::copy_n(block.data(), count, cicp.data() + copied);
      copied += count;
      remaining -= static_cast<std::uint32_t>(count);
    }
    std::array<std::uint8_t, 4> expected_crc{};
    if (!read_exact(stream, expected_crc.data(), expected_crc.size()) ||
        (crc ^ 0xffffffffU) != read_be32(expected_crc.data()))
      return fail(CodecErrorCategory::malformed, "invalid PNG chunk CRC",
                  category, error);

    if (type == std::array<std::uint8_t, 4>{'I', 'H', 'D', 'R'}) {
      if (have_header || length != header.size())
        return fail(CodecErrorCategory::malformed, "invalid PNG IHDR", category,
                    error);
      const auto width = read_be32(header.data());
      const auto height = read_be32(header.data() + 4);
      if (width == 0U || height == 0U)
        return fail(CodecErrorCategory::malformed, "invalid PNG dimensions",
                    category, error);
      const bool rgb = header[9] == 2U;
      const bool grayscale = header[9] == 0U && header[8] == 8U;
      if ((header[8] != 8U && header[8] != 16U) || (!rgb && !grayscale) ||
          header[10] != 0U || header[11] != 0U || header[12] > 1U)
        return fail(CodecErrorCategory::unsupported,
                    "unsupported PNG sample layout", category, error);
      info.container = ImageContainer::png;
      info.width = width;
      info.height = height;
      info.channels = rgb ? 3U : 1U;
      info.encoding.sample_type = header[8] == 16U ? "uint16" : "uint8";
      info.encoding.color_primaries = "srgb";
      info.encoding.transfer_function = "srgb";
      info.encoding.reference_white_nits = 100.0;
      have_header = true;
    } else if (type == std::array<std::uint8_t, 4>{'c', 'I', 'C', 'P'}) {
      if (have_cicp || length != cicp.size())
        return fail(CodecErrorCategory::malformed, "invalid PNG cICP chunk",
                    category, error);
      if (cicp != std::array<std::uint8_t, 4>{9, 16, 0, 1})
        return fail(CodecErrorCategory::unsupported,
                    "unsupported PNG cICP signaling", category, error);
      info.png_cicp = cicp;
      info.encoding.color_primaries = "rec2020";
      info.encoding.transfer_function = "pq";
      info.encoding.reference_white_nits = 203.0;
      have_cicp = true;
    }
  }
  category = CodecErrorCategory::none;
  error.clear();
  return true;
}

bool is_start_of_frame(const std::uint8_t marker) {
  switch (marker) {
  case 0xc0:
  case 0xc1:
  case 0xc2:
  case 0xc3:
  case 0xc5:
  case 0xc6:
  case 0xc7:
  case 0xc9:
  case 0xca:
  case 0xcb:
  case 0xcd:
  case 0xce:
  case 0xcf:
    return true;
  default:
    return false;
  }
}

std::string jpeg_sampling(const std::array<std::uint8_t, 3> &factors) {
  if (factors[0] == factors[1] && factors[1] == factors[2])
    return "4:4:4";
  if (factors[1] == 0x11U && factors[2] == 0x11U) {
    if (factors[0] == 0x21U)
      return "4:2:2";
    if (factors[0] == 0x22U)
      return "4:2:0";
    if (factors[0] == 0x12U)
      return "4:4:0";
  }
  return "other";
}

bool inspect_jpeg(std::ifstream &stream, ImageInfo &info,
                  CodecErrorCategory &category, std::string &error) {
  std::array<std::uint8_t, 2> soi{};
  if (!read_exact(stream, soi.data(), soi.size()) ||
      soi != std::array<std::uint8_t, 2>{0xff, 0xd8})
    return fail(CodecErrorCategory::malformed, "invalid JPEG signature",
                category, error);
  for (;;) {
    std::uint8_t prefix = 0;
    if (!read_exact(stream, &prefix, 1))
      return fail(CodecErrorCategory::malformed, "truncated JPEG header",
                  category, error);
    if (prefix != 0xffU)
      return fail(CodecErrorCategory::malformed, "invalid JPEG marker",
                  category, error);
    std::uint8_t marker = 0xffU;
    while (marker == 0xffU)
      if (!read_exact(stream, &marker, 1))
        return fail(CodecErrorCategory::malformed, "truncated JPEG marker",
                    category, error);
    if (marker == 0xd9U || marker == 0xdaU)
      return fail(CodecErrorCategory::malformed, "JPEG has no frame header",
                  category, error);
    if (marker == 0x01U || (marker >= 0xd0U && marker <= 0xd8U))
      continue;
    std::array<std::uint8_t, 2> length_bytes{};
    if (!read_exact(stream, length_bytes.data(), length_bytes.size()))
      return fail(CodecErrorCategory::malformed, "truncated JPEG segment",
                  category, error);
    const auto length = read_be16(length_bytes.data());
    if (length < 2U)
      return fail(CodecErrorCategory::malformed, "invalid JPEG segment length",
                  category, error);
    const auto payload = static_cast<std::uint16_t>(length - 2U);
    if (!is_start_of_frame(marker)) {
      stream.seekg(payload, std::ios::cur);
      if (!stream)
        return fail(CodecErrorCategory::malformed, "truncated JPEG segment",
                    category, error);
      continue;
    }
    std::array<std::uint8_t, 15> frame{};
    if (payload < 15U || !read_exact(stream, frame.data(), frame.size()))
      return fail(CodecErrorCategory::malformed, "truncated JPEG frame header",
                  category, error);
    if (frame[0] != 8U || frame[5] != 3U)
      return fail(CodecErrorCategory::unsupported,
                  "unsupported JPEG sample layout", category, error);
    const auto height = read_be16(frame.data() + 1);
    const auto width = read_be16(frame.data() + 3);
    if (width == 0U || height == 0U)
      return fail(CodecErrorCategory::malformed, "invalid JPEG dimensions",
                  category, error);
    const std::array<std::uint8_t, 3> factors{frame[7], frame[10], frame[13]};
    info.container = ImageContainer::jpeg;
    info.width = width;
    info.height = height;
    info.channels = 3;
    info.encoding = ImageEncoding{};
    info.jpeg_subsampling = jpeg_sampling(factors);
    category = CodecErrorCategory::none;
    error.clear();
    return true;
  }
}

CodecErrorCategory exr_category(const exr_result_t result) {
  if (result == EXR_ERR_OUT_OF_MEMORY)
    return CodecErrorCategory::allocation;
  if (result == EXR_ERR_FILE_ACCESS || result == EXR_ERR_READ_IO)
    return CodecErrorCategory::io;
  if (result == EXR_ERR_FILE_BAD_HEADER || result == EXR_ERR_MISSING_REQ_ATTR ||
      result == EXR_ERR_INVALID_ATTR || result == EXR_ERR_BAD_CHUNK_LEADER ||
      result == EXR_ERR_CORRUPT_CHUNK ||
      result == EXR_ERR_INCOMPLETE_CHUNK_TABLE ||
      result == EXR_ERR_INVALID_SAMPLE_DATA)
    return CodecErrorCategory::malformed;
  return CodecErrorCategory::codec_failure;
}

bool fail_exr(const exr_result_t result, const char *operation,
              CodecErrorCategory &category, std::string &error) {
  const char *detail = exr_get_default_error_message(result);
  return fail(exr_category(result),
              std::string(operation) + (detail == nullptr ? "" : ": ") +
                  (detail == nullptr ? "" : detail),
              category, error);
}

bool fail_exr_header(const exr_result_t result, const char *operation,
                     CodecErrorCategory &category, std::string &error) {
  CodecErrorCategory value = exr_category(result);
  if (value == CodecErrorCategory::codec_failure)
    value = CodecErrorCategory::malformed;
  const char *detail = exr_get_default_error_message(result);
  return fail(value,
              std::string(operation) + (detail == nullptr ? "" : ": ") +
                  (detail == nullptr ? "" : detail),
              category, error);
}

class ExrContext {
public:
  ~ExrContext() {
    if (value_ != nullptr)
      exr_finish(&value_);
  }
  exr_context_t *output() { return &value_; }
  exr_context_t get() const { return value_; }

private:
  exr_context_t value_ = nullptr;
};

bool inspect_exr_context(const std::string &path, ExrContext &owner,
                         ImageInfo &info, CodecErrorCategory &category,
                         std::string &error) {
  exr_context_initializer_t initializer = EXR_DEFAULT_CONTEXT_INITIALIZER;
  initializer.flags = EXR_CONTEXT_FLAG_STRICT_HEADER |
                      EXR_CONTEXT_FLAG_SILENT_HEADER_PARSE |
                      EXR_CONTEXT_FLAG_DISABLE_CHUNK_RECONSTRUCTION;
  const exr_result_t open_result =
      exr_start_read(owner.output(), path.c_str(), &initializer);
  if (open_result != EXR_ERR_SUCCESS)
    return fail_exr_header(open_result, "cannot open EXR", category, error);

  int part_count = 0;
  exr_storage_t storage{};
  exr_attr_box2i_t data_window{};
  exr_attr_box2i_t display_window{};
  exr_compression_t compression{};
  const exr_attr_chlist_t *channels = nullptr;
  exr_result_t result = exr_get_count(owner.get(), &part_count);
  if (result == EXR_ERR_SUCCESS)
    result = exr_get_storage(owner.get(), 0, &storage);
  if (result == EXR_ERR_SUCCESS)
    result = exr_get_data_window(owner.get(), 0, &data_window);
  if (result == EXR_ERR_SUCCESS)
    result = exr_get_display_window(owner.get(), 0, &display_window);
  if (result == EXR_ERR_SUCCESS)
    result = exr_get_compression(owner.get(), 0, &compression);
  if (result == EXR_ERR_SUCCESS)
    result = exr_get_channels(owner.get(), 0, &channels);
  if (result != EXR_ERR_SUCCESS)
    return fail_exr_header(result, "cannot inspect EXR header", category,
                           error);
  if (part_count != 1 || storage != EXR_STORAGE_SCANLINE ||
      compression != EXR_COMPRESSION_PIZ || channels == nullptr ||
      channels->num_channels != 3)
    return fail(CodecErrorCategory::unsupported,
                "unsupported EXR parts, storage, compression, or channels",
                category, error);
  const std::array<std::string, 3> required{"B", "G", "R"};
  for (int index = 0; index < channels->num_channels; ++index) {
    const auto &channel = channels->entries[index];
    const std::string name(channel.name.str,
                           static_cast<std::size_t>(channel.name.length));
    if (name != required[static_cast<std::size_t>(index)] ||
        channel.pixel_type != EXR_PIXEL_FLOAT || channel.x_sampling != 1 ||
        channel.y_sampling != 1)
      return fail(CodecErrorCategory::unsupported,
                  "EXR must contain full-resolution float B, G, R channels",
                  category, error);
  }
  const std::int64_t width =
      static_cast<std::int64_t>(data_window.max.x) - data_window.min.x + 1;
  const std::int64_t height =
      static_cast<std::int64_t>(data_window.max.y) - data_window.min.y + 1;
  if (width <= 0 || height <= 0 ||
      width > std::numeric_limits<unsigned>::max() ||
      height > std::numeric_limits<unsigned>::max())
    return fail(CodecErrorCategory::malformed, "invalid EXR data window",
                category, error);
  info.container = ImageContainer::exr;
  info.width = static_cast<unsigned>(width);
  info.height = static_cast<unsigned>(height);
  info.channels = 3;
  info.encoding.sample_type = "float32";
  info.encoding.color_primaries = "rec2020";
  info.encoding.transfer_function = "linear";
  info.encoding.reference_white_nits = 203.0;
  info.exr_data_window =
      std::array<std::int32_t, 4>{data_window.min.x, data_window.min.y,
                                  data_window.max.x, data_window.max.y};
  info.exr_display_window =
      std::array<std::int32_t, 4>{display_window.min.x, display_window.min.y,
                                  display_window.max.x, display_window.max.y};
  info.exr_compression = "PIZ";
  category = CodecErrorCategory::none;
  error.clear();
  return true;
}

bool decode_exr(const std::string &path, const ImageInfo &expected,
                void *output, const std::uint64_t row_stride_bytes,
                const CancellationCheck &cancellation,
                CodecErrorCategory &category, std::string &error) {
  ExrContext owner;
  ImageInfo actual;
  if (!inspect_exr_context(path, owner, actual, category, error))
    return false;
  if (actual.width != expected.width || actual.height != expected.height ||
      actual.encoding.sample_type != expected.encoding.sample_type ||
      actual.exr_data_window != expected.exr_data_window ||
      actual.exr_display_window != expected.exr_display_window ||
      actual.exr_compression != expected.exr_compression)
    return fail(CodecErrorCategory::invalid_request,
                "decoded EXR metadata does not match header", category, error);
  if (row_stride_bytes >
      static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
    return fail(CodecErrorCategory::invalid_request,
                "EXR row stride is too large", category, error);
  const std::int32_t first_y = (*actual.exr_data_window)[1];
  const std::int32_t final_y = (*actual.exr_data_window)[3];
  auto *bytes = static_cast<std::uint8_t *>(output);
  for (std::int32_t y = first_y; y <= final_y;) {
    if (cancelled(cancellation))
      return fail(CodecErrorCategory::cancelled, "image decode cancelled",
                  category, error);
    exr_chunk_info_t chunk{};
    exr_result_t result =
        exr_read_scanline_chunk_info(owner.get(), 0, y, &chunk);
    if (result != EXR_ERR_SUCCESS)
      return fail_exr(result, "cannot read EXR chunk", category, error);
    if (chunk.height <= 0 || chunk.start_y != y ||
        chunk.start_y > final_y - chunk.height + 1)
      return fail(CodecErrorCategory::malformed, "invalid EXR scanline chunk",
                  category, error);
    exr_decode_pipeline_t pipeline{};
    pipeline.pipe_size = sizeof(pipeline);
    result = exr_decoding_initialize(owner.get(), 0, &chunk, &pipeline);
    if (result != EXR_ERR_SUCCESS)
      return fail_exr(result, "cannot initialize EXR decode", category, error);
    const std::size_t row = static_cast<std::size_t>(y - first_y);
    for (int channel_index = 0; channel_index < pipeline.channel_count;
         ++channel_index) {
      auto &channel = pipeline.channels[channel_index];
      std::size_t output_channel = 0;
      if (std::strcmp(channel.channel_name, "R") == 0)
        output_channel = 0;
      else if (std::strcmp(channel.channel_name, "G") == 0)
        output_channel = 1;
      else if (std::strcmp(channel.channel_name, "B") == 0)
        output_channel = 2;
      else {
        exr_decoding_destroy(owner.get(), &pipeline);
        return fail(CodecErrorCategory::unsupported,
                    "unexpected EXR decode channel", category, error);
      }
      channel.decode_to_ptr =
          bytes + row * row_stride_bytes + output_channel * sizeof(float);
      channel.user_pixel_stride = 3 * sizeof(float);
      channel.user_line_stride = static_cast<std::int32_t>(row_stride_bytes);
      channel.user_bytes_per_element = sizeof(float);
      channel.user_data_type = EXR_PIXEL_FLOAT;
    }
    result = exr_decoding_choose_default_routines(owner.get(), 0, &pipeline);
    if (result == EXR_ERR_SUCCESS)
      result = exr_decoding_run(owner.get(), 0, &pipeline);
    const exr_result_t destroy_result =
        exr_decoding_destroy(owner.get(), &pipeline);
    if (result != EXR_ERR_SUCCESS)
      return fail_exr(result, "cannot decode EXR chunk", category, error);
    if (destroy_result != EXR_ERR_SUCCESS)
      return fail_exr(destroy_result, "cannot release EXR decode", category,
                      error);
    for (std::int32_t chunk_row = 0; chunk_row < chunk.height; ++chunk_row) {
      const auto *samples = reinterpret_cast<const float *>(
          bytes +
          (row + static_cast<std::size_t>(chunk_row)) * row_stride_bytes);
      for (std::size_t sample = 0;
           sample < static_cast<std::size_t>(actual.width) * 3U; ++sample)
        if (!std::isfinite(samples[sample]))
          return fail(CodecErrorCategory::malformed,
                      "EXR contains a non-finite sample", category, error);
    }
    y += chunk.height;
  }
  if (cancelled(cancellation))
    return fail(CodecErrorCategory::cancelled, "image decode cancelled",
                category, error);
  category = CodecErrorCategory::none;
  error.clear();
  return true;
}

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

class ComApartment {
public:
  ComApartment() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComApartment() {
    if (result_ == S_OK || result_ == S_FALSE)
      CoUninitialize();
  }
  bool available() const {
    return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
  }

private:
  HRESULT result_;
};

bool utf8_to_wide(const std::string &text, std::wstring &wide) {
  if (text.empty())
    return false;
  const int count =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (count <= 0)
    return false;
  wide.resize(static_cast<std::size_t>(count));
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                             static_cast<int>(text.size()), wide.data(),
                             count) == count;
}

CodecErrorCategory wic_category(const HRESULT result) {
  if (result == WINCODEC_ERR_UNKNOWNIMAGEFORMAT ||
      result == WINCODEC_ERR_BADIMAGE || result == WINCODEC_ERR_FRAMEMISSING)
    return CodecErrorCategory::malformed;
  if (result == WINCODEC_ERR_COMPONENTNOTFOUND ||
      result == WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT)
    return CodecErrorCategory::unsupported;
  if (result == E_OUTOFMEMORY)
    return CodecErrorCategory::allocation;
  return CodecErrorCategory::codec_failure;
}

bool decode_wic(const std::string &path, const ImageInfo &info, void *output,
                const std::uint64_t row_stride_bytes,
                const std::uint64_t output_bytes,
                const CancellationCheck &cancellation,
                CodecErrorCategory &category, std::string &error) {
  if (cancelled(cancellation))
    return fail(CodecErrorCategory::cancelled, "image decode cancelled",
                category, error);
  std::wstring path_wide;
  if (!utf8_to_wide(path, path_wide))
    return fail(CodecErrorCategory::invalid_request,
                "image path is not valid UTF-8", category, error);
  ComApartment apartment;
  if (!apartment.available())
    return fail(CodecErrorCategory::unavailable,
                "cannot initialize COM for image decoding", category, error);
  ComPtr<IWICImagingFactory> factory;
  HRESULT result =
      CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                       IID_PPV_ARGS(&factory));
  if (FAILED(result))
    return fail(CodecErrorCategory::unavailable,
                "Windows Imaging Component is unavailable", category, error);
  ComPtr<IWICBitmapDecoder> decoder;
  result = factory->CreateDecoderFromFilename(
      path_wide.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
      &decoder);
  if (FAILED(result))
    return fail(wic_category(result), "cannot open image decoder", category,
                error);
  if (cancelled(cancellation))
    return fail(CodecErrorCategory::cancelled, "image decode cancelled",
                category, error);
  ComPtr<IWICBitmapFrameDecode> frame;
  result = decoder->GetFrame(0, &frame);
  if (FAILED(result))
    return fail(wic_category(result), "cannot read image frame", category,
                error);
  UINT width = 0;
  UINT height = 0;
  result = frame->GetSize(&width, &height);
  if (FAILED(result) || width != info.width || height != info.height)
    return fail(CodecErrorCategory::malformed,
                "decoded image dimensions do not match header", category,
                error);
  ComPtr<IWICFormatConverter> converter;
  result = factory->CreateFormatConverter(&converter);
  if (FAILED(result))
    return fail(wic_category(result), "cannot create image converter", category,
                error);
  const WICPixelFormatGUID &pixel_format = info.encoding.sample_type == "uint16"
                                               ? GUID_WICPixelFormat48bppRGB
                                               : GUID_WICPixelFormat24bppRGB;
  result =
      converter->Initialize(frame.Get(), pixel_format, WICBitmapDitherTypeNone,
                            nullptr, 0.0, WICBitmapPaletteTypeCustom);
  if (FAILED(result))
    return fail(wic_category(result), "cannot convert image to native RGB",
                category, error);
  if (cancelled(cancellation))
    return fail(CodecErrorCategory::cancelled, "image decode cancelled",
                category, error);
  result = converter->CopyPixels(nullptr, static_cast<UINT>(row_stride_bytes),
                                 static_cast<UINT>(output_bytes),
                                 static_cast<BYTE *>(output));
  if (FAILED(result))
    return fail(wic_category(result), "cannot decode image pixels", category,
                error);
  if (cancelled(cancellation))
    return fail(CodecErrorCategory::cancelled, "image decode cancelled",
                category, error);
  category = CodecErrorCategory::none;
  error.clear();
  return true;
}
#endif
} // namespace

bool inspect_image(const std::string &path, ImageInfo &info,
                   CodecErrorCategory &category, std::string &error) {
  try {
    std::ifstream stream(fs::u8path(path), std::ios::binary);
    if (!stream)
      return fail(CodecErrorCategory::io, "cannot open image", category, error);
    std::error_code size_error;
    const auto file_size = fs::file_size(fs::u8path(path), size_error);
    if (size_error)
      return fail(CodecErrorCategory::io, "cannot query image size", category,
                  error);
    std::array<std::uint8_t, 8> signature{};
    if (!read_exact(stream, signature.data(), signature.size()))
      return fail(CodecErrorCategory::malformed, "truncated image signature",
                  category, error);
    stream.seekg(0);
    ImageInfo parsed;
    if (signature == std::array<std::uint8_t, 8>{0x89, 'P', 'N', 'G', '\r',
                                                 '\n', 0x1a, '\n'}) {
      if (!inspect_png(stream, file_size, parsed, category, error))
        return false;
    } else if (signature[0] == 0xffU && signature[1] == 0xd8U) {
      if (!inspect_jpeg(stream, parsed, category, error))
        return false;
    } else if (signature[0] == 0x76U && signature[1] == 0x2fU &&
               signature[2] == 0x31U && signature[3] == 0x01U) {
      ExrContext owner;
      if (!inspect_exr_context(path, owner, parsed, category, error))
        return false;
    } else {
      return fail(CodecErrorCategory::unsupported,
                  "unsupported image container", category, error);
    }
    info = std::move(parsed);
    return true;
  } catch (const std::bad_alloc &) {
    return fail(CodecErrorCategory::allocation,
                "cannot allocate image metadata", category, error);
  } catch (...) {
    return fail(CodecErrorCategory::codec_failure,
                "unexpected image inspection failure", category, error);
  }
}

bool decode_image(const std::string &path, const ImageInfo &info, void *output,
                  const std::uint64_t row_stride_bytes,
                  const std::uint64_t output_bytes,
                  const CancellationCheck &cancellation,
                  CodecErrorCategory &category, std::string &error) {
  const bool is_uint8 = info.encoding.sample_type == "uint8";
  const bool is_uint16 = info.encoding.sample_type == "uint16";
  const bool is_float32 = info.encoding.sample_type == "float32";
  if (output == nullptr || info.width == 0U || info.height == 0U ||
      info.channels != 3U || (!is_uint8 && !is_uint16 && !is_float32) ||
      (info.container != ImageContainer::jpeg &&
       info.container != ImageContainer::png &&
       info.container != ImageContainer::exr) ||
      (info.container == ImageContainer::jpeg && !is_uint8))
    return fail(CodecErrorCategory::invalid_request,
                "invalid native RGB decode request", category, error);
  const std::uint64_t sample_bytes = is_float32 ? 4U : (is_uint16 ? 2U : 1U);
  const std::uint64_t minimum_stride =
      static_cast<std::uint64_t>(info.width) * 3U * sample_bytes;
  if (row_stride_bytes < minimum_stride ||
      row_stride_bytes > std::numeric_limits<unsigned>::max() ||
      info.height > output_bytes / row_stride_bytes ||
      output_bytes > std::numeric_limits<unsigned>::max())
    return fail(CodecErrorCategory::invalid_request,
                "native RGB decode buffer is too small", category, error);
  if (cancelled(cancellation))
    return fail(CodecErrorCategory::cancelled, "image decode cancelled",
                category, error);
  if (info.container == ImageContainer::exr)
    return decode_exr(path, info, output, row_stride_bytes, cancellation,
                      category, error);
#ifdef _WIN32
  try {
    return decode_wic(path, info, output, row_stride_bytes, output_bytes,
                      cancellation, category, error);
  } catch (const std::bad_alloc &) {
    return fail(CodecErrorCategory::allocation, "cannot allocate image decoder",
                category, error);
  } catch (...) {
    return fail(CodecErrorCategory::codec_failure,
                "unexpected image decode failure", category, error);
  }
#else
  (void)path;
  return fail(CodecErrorCategory::unavailable,
              "native JPEG/PNG decoding requires Windows Imaging Component",
              category, error);
#endif
}

bool decode_and_upload_images(
    pano_gpu_session *const session, const std::vector<std::string> &paths,
    const ImageInfo &expected_info, const CancellationCheck &cancellation,
    const pano_gpu_cancellation_token *const gpu_cancellation,
    CodecErrorCategory &category, std::string &error,
    const NativeProgressCallback progress, void *const progress_user_data,
    const unsigned progress_begin, const unsigned progress_end) {
  if (session == nullptr || paths.empty() || expected_info.width == 0U ||
      expected_info.height == 0U || expected_info.channels != 3U ||
      (expected_info.encoding.sample_type != "uint8" &&
       expected_info.encoding.sample_type != "uint16" &&
       expected_info.encoding.sample_type != "float32"))
    return fail(CodecErrorCategory::invalid_request,
                "invalid native RGB upload request", category, error);
  const bool is_uint16 = expected_info.encoding.sample_type == "uint16";
  const bool is_float32 = expected_info.encoding.sample_type == "float32";
  const std::uint64_t sample_bytes = is_float32 ? 4U : (is_uint16 ? 2U : 1U);
  const std::uint64_t row_stride =
      static_cast<std::uint64_t>(expected_info.width) * 3U * sample_bytes;
  if (expected_info.height >
          std::numeric_limits<std::uint64_t>::max() / row_stride ||
      row_stride > std::numeric_limits<std::uint32_t>::max())
    return fail(CodecErrorCategory::invalid_request,
                "native RGB upload buffer size overflows", category, error);
  const std::uint64_t frame_bytes =
      row_stride * static_cast<std::uint64_t>(expected_info.height);
  if (frame_bytes > std::numeric_limits<std::size_t>::max())
    return fail(CodecErrorCategory::invalid_request,
                "native RGB upload buffer is too large", category, error);
  if (cancelled(cancellation) ||
      (gpu_cancellation != nullptr &&
       pano_gpu_cancellation_token_is_cancelled(gpu_cancellation) != 0))
    return fail(CodecErrorCategory::cancelled, "image upload cancelled",
                category, error);

  const auto fail_gpu = [&](const pano_gpu_result result,
                            const std::array<char, 512> &gpu_error) {
    CodecErrorCategory gpu_category = CodecErrorCategory::codec_failure;
    if (result == PANO_GPU_CANCELLED)
      gpu_category = CodecErrorCategory::cancelled;
    else if (result == PANO_GPU_INVALID_ARGUMENT)
      gpu_category = CodecErrorCategory::invalid_request;
    else if (result == PANO_GPU_OUT_OF_MEMORY)
      gpu_category = CodecErrorCategory::allocation;
    else if (result == PANO_GPU_UNAVAILABLE)
      gpu_category = CodecErrorCategory::unavailable;
    return fail(gpu_category,
                gpu_error[0] == '\0' ? "D3D12 source upload failed"
                                     : std::string(gpu_error.data()),
                category, error);
  };

  try {
    const auto report_progress = [&](const std::size_t completed) {
      if (progress == nullptr)
        return;
      const std::size_t total = paths.size() + 1U;
      const unsigned span =
          progress_end >= progress_begin ? progress_end - progress_begin : 0U;
      const unsigned value =
          progress_begin + static_cast<unsigned>(span * completed / total);
      progress(progress_user_data, value, 100U, "Loading source images");
    };
    report_progress(0U);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(frame_bytes));
    std::array<char, 512> gpu_error{};
    pano_gpu_result result = pano_gpu_session_allocate_source(
        session, gpu_error.data(),
        static_cast<std::uint32_t>(gpu_error.size()));
    if (result != PANO_GPU_SUCCESS)
      return fail_gpu(result, gpu_error);
    result = pano_gpu_session_allocate_upload_slot(
        session, gpu_error.data(),
        static_cast<std::uint32_t>(gpu_error.size()));
    if (result != PANO_GPU_SUCCESS)
      return fail_gpu(result, gpu_error);
    result = pano_gpu_session_allocate_second_upload_slot(
        session, gpu_error.data(),
        static_cast<std::uint32_t>(gpu_error.size()));
    if (result != PANO_GPU_SUCCESS)
      return fail_gpu(result, gpu_error);

    for (std::size_t index = 0; index < paths.size(); ++index) {
      if (index > std::numeric_limits<std::uint32_t>::max())
        return fail(CodecErrorCategory::invalid_request,
                    "too many source images", category, error);
      ImageInfo actual_info;
      if (!inspect_image(paths[index], actual_info, category, error))
        return false;
      if (actual_info.width != expected_info.width ||
          actual_info.height != expected_info.height ||
          actual_info.channels != expected_info.channels ||
          actual_info.encoding.sample_type !=
              expected_info.encoding.sample_type ||
          actual_info.encoding.color_primaries !=
              expected_info.encoding.color_primaries ||
          actual_info.encoding.transfer_function !=
              expected_info.encoding.transfer_function ||
          actual_info.encoding.reference_white_nits !=
              expected_info.encoding.reference_white_nits)
        return fail(CodecErrorCategory::invalid_request,
                    "source image metadata does not match the session",
                    category, error);
      if (!decode_image(paths[index], actual_info, pixels.data(), row_stride,
                        frame_bytes, cancellation, category, error))
        return false;
      pano_gpu_source_upload upload{};
      upload.size = sizeof(upload);
      upload.abi_version = PANO_GPU_ABI_VERSION;
      upload.frame_index = static_cast<std::uint32_t>(index);
      upload.source_sample_type =
          is_float32
              ? PANO_GPU_SAMPLE_FLOAT32
              : (is_uint16 ? PANO_GPU_SAMPLE_UINT16 : PANO_GPU_SAMPLE_UINT8);
      upload.source_row_stride_bytes = static_cast<std::uint32_t>(row_stride);
      upload.data = pixels.data();
      upload.data_bytes = frame_bytes;
      gpu_error.fill('\0');
      result = pano_gpu_session_upload_frame_cancellable(
          session, &upload, gpu_cancellation, gpu_error.data(),
          static_cast<std::uint32_t>(gpu_error.size()));
      if (result != PANO_GPU_SUCCESS)
        return fail_gpu(result, gpu_error);
      report_progress(index + 1U);
    }
    gpu_error.fill('\0');
    result = pano_gpu_session_finish_uploads_cancellable(
        session, gpu_cancellation, gpu_error.data(),
        static_cast<std::uint32_t>(gpu_error.size()));
    if (result != PANO_GPU_SUCCESS)
      return fail_gpu(result, gpu_error);
    report_progress(paths.size() + 1U);
    category = CodecErrorCategory::none;
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    return fail(CodecErrorCategory::allocation,
                "cannot allocate native RGB upload buffer", category, error);
  } catch (...) {
    return fail(CodecErrorCategory::codec_failure,
                "unexpected native RGB upload failure", category, error);
  }
}

} // namespace pano::app
