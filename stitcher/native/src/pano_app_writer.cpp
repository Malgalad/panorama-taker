#include "pano_app.h"

#include <openexr.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
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

CodecErrorCategory exr_category(const exr_result_t result) {
  if (result == EXR_ERR_OUT_OF_MEMORY) return CodecErrorCategory::allocation;
  if (result == EXR_ERR_FILE_ACCESS || result == EXR_ERR_READ_IO ||
      result == EXR_ERR_WRITE_IO)
    return CodecErrorCategory::io;
  if (result == EXR_ERR_INVALID_ARGUMENT ||
      result == EXR_ERR_ARGUMENT_OUT_OF_RANGE)
    return CodecErrorCategory::invalid_request;
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

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

bool utf8_to_wide(const std::string &text, std::wstring &wide) {
  if (text.empty()) return false;
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        text.data(), static_cast<int>(text.size()),
                                        nullptr, 0);
  if (count <= 0) return false;
  wide.resize(static_cast<std::size_t>(count));
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                             static_cast<int>(text.size()), wide.data(), count) ==
         count;
}

CodecErrorCategory wic_category(const HRESULT result) {
  if (result == E_OUTOFMEMORY) return CodecErrorCategory::allocation;
  if (result == WINCODEC_ERR_COMPONENTNOTFOUND ||
      result == WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT)
    return CodecErrorCategory::unsupported;
  if (result == WINCODEC_ERR_BADIMAGE ||
      result == WINCODEC_ERR_UNKNOWNIMAGEFORMAT)
    return CodecErrorCategory::malformed;
  if (result == STG_E_ACCESSDENIED || result == STG_E_FILENOTFOUND ||
      result == STG_E_PATHNOTFOUND || result == STG_E_WRITEFAULT)
    return CodecErrorCategory::io;
  return CodecErrorCategory::codec_failure;
}

bool fail_wic(const HRESULT result, const char *operation,
              CodecErrorCategory &category, std::string &error) {
  return fail(wic_category(result),
              std::string(operation) + " (HRESULT " +
                  std::to_string(static_cast<unsigned long>(result)) + ")",
              category, error);
}

bool write_option(IPropertyBag2 *bag, wchar_t *name, VARIANT &value,
                  CodecErrorCategory &category, std::string &error) {
  PROPBAG2 option{};
  option.pstrName = name;
  const HRESULT result = bag->Write(1, &option, &value);
  if (FAILED(result))
    return fail_wic(result, "cannot set image encoder option", category,
                    error);
  return true;
}
#endif
} // namespace

class ImageWriter {
public:
  explicit ImageWriter(ImageWriterOptions requested)
      : options(std::move(requested)) {}
  ~ImageWriter() { close(false); }

  void close(const bool keep) noexcept {
    if (exr_context != nullptr) exr_finish(&exr_context);
#ifdef _WIN32
    frame.Reset();
    encoder.Reset();
    stream.Reset();
    factory.Reset();
    if (uninitialize_com) {
      CoUninitialize();
      uninitialize_com = false;
    }
#endif
    if (!keep && owns_path) {
      std::error_code ignored;
      fs::remove(fs::u8path(options.path), ignored);
    }
    owns_path = false;
  }

  ImageWriterOptions options;
  unsigned rows_written = 0;
  bool owns_path = false;
  exr_context_t exr_context = nullptr;
  unsigned exr_rows_per_chunk = 0;
  unsigned exr_pending_rows = 0;
  std::vector<float> exr_pending;
#ifdef _WIN32
  bool uninitialize_com = false;
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
#endif
};

namespace {
bool flush_exr_chunk(ImageWriter &writer, CodecErrorCategory &category,
                     std::string &error) {
  if (writer.exr_pending_rows == 0U) return true;
  const unsigned start_y = writer.rows_written - writer.exr_pending_rows;
  exr_chunk_info_t chunk{};
  exr_result_t result = exr_write_scanline_chunk_info(
      writer.exr_context, 0, static_cast<int>(start_y), &chunk);
  if (result != EXR_ERR_SUCCESS)
    return fail_exr(result, "cannot prepare EXR output chunk", category,
                    error);
  if (chunk.height <= 0 ||
      static_cast<unsigned>(chunk.height) != writer.exr_pending_rows)
    return fail(CodecErrorCategory::codec_failure,
                "EXR output chunk row count is inconsistent", category,
                error);
  exr_encode_pipeline_t pipeline{};
  pipeline.pipe_size = sizeof(pipeline);
  result = exr_encoding_initialize(writer.exr_context, 0, &chunk, &pipeline);
  if (result != EXR_ERR_SUCCESS)
    return fail_exr(result, "cannot initialize EXR output chunk", category,
                    error);
  const std::int32_t pixel_stride = 3 * sizeof(float);
  const std::int32_t line_stride =
      static_cast<std::int32_t>(writer.options.width * pixel_stride);
  for (int channel_index = 0; channel_index < pipeline.channel_count;
       ++channel_index) {
    auto &channel = pipeline.channels[channel_index];
    std::size_t input_channel = 0;
    if (std::strcmp(channel.channel_name, "R") == 0)
      input_channel = 0;
    else if (std::strcmp(channel.channel_name, "G") == 0)
      input_channel = 1;
    else if (std::strcmp(channel.channel_name, "B") == 0)
      input_channel = 2;
    else {
      exr_encoding_destroy(writer.exr_context, &pipeline);
      return fail(CodecErrorCategory::codec_failure,
                  "unexpected EXR output channel", category, error);
    }
    channel.encode_from_ptr =
        reinterpret_cast<const std::uint8_t *>(writer.exr_pending.data() +
                                               input_channel);
    channel.user_pixel_stride = pixel_stride;
    channel.user_line_stride = line_stride;
    channel.user_bytes_per_element = sizeof(float);
    channel.user_data_type = EXR_PIXEL_FLOAT;
  }
  result = exr_encoding_choose_default_routines(writer.exr_context, 0,
                                                 &pipeline);
  if (result == EXR_ERR_SUCCESS)
    result = exr_encoding_run(writer.exr_context, 0, &pipeline);
  const exr_result_t destroy_result =
      exr_encoding_destroy(writer.exr_context, &pipeline);
  if (result != EXR_ERR_SUCCESS)
    return fail_exr(result, "cannot encode EXR output chunk", category, error);
  if (destroy_result != EXR_ERR_SUCCESS)
    return fail_exr(destroy_result, "cannot release EXR output chunk",
                    category, error);
  writer.exr_pending_rows = 0;
  return true;
}
} // namespace

bool create_image_writer(const ImageWriterOptions &options,
                         ImageWriter **const writer,
                         CodecErrorCategory &category, std::string &error) {
  if (writer == nullptr)
    return fail(CodecErrorCategory::invalid_request,
                "image writer out-handle is null", category, error);
  *writer = nullptr;
  const bool is_sdr = options.container == ImageContainer::png ||
                      options.container == ImageContainer::jpeg;
  const bool is_exr = options.container == ImageContainer::exr;
  if (options.path.empty() || options.width == 0U || options.height == 0U ||
      (!is_sdr && !is_exr) ||
      (is_sdr && (options.sample_type != "uint8" ||
                  (options.channels != 3U &&
                   (options.container != ImageContainer::png ||
                    options.channels != 1U)) ||
                  options.encoding.color_primaries != "srgb" ||
                  options.encoding.transfer_function != "srgb")) ||
      (is_exr &&
       (options.channels != 3U || options.sample_type != "float32" ||
        options.encoding.sample_type != "float16" ||
        options.encoding.color_primaries != "rec2020" ||
        options.encoding.transfer_function != "linear" ||
        options.width >
            static_cast<unsigned>(std::numeric_limits<std::int32_t>::max() /
                                  (3 * sizeof(float))) ||
        options.height >
            static_cast<unsigned>(std::numeric_limits<std::int32_t>::max()))) ||
      (options.container == ImageContainer::jpeg &&
       (options.jpeg_quality == 0U || options.jpeg_quality > 100U)))
    return fail(CodecErrorCategory::invalid_request,
                "invalid SDR image writer options", category, error);
  std::error_code path_error;
  if (fs::exists(fs::u8path(options.path), path_error) || path_error)
    return fail(CodecErrorCategory::invalid_request,
                "image writer path already exists or cannot be inspected",
                category, error);
  if (is_exr) {
    try {
      auto created = std::make_unique<ImageWriter>(options);
      exr_context_initializer_t initializer = EXR_DEFAULT_CONTEXT_INITIALIZER;
      initializer.flags = EXR_CONTEXT_FLAG_WRITE_LEGACY_HEADER;
      exr_result_t result = exr_start_write(
          &created->exr_context, options.path.c_str(), EXR_WRITE_FILE_DIRECTLY,
          &initializer);
      if (result != EXR_ERR_SUCCESS)
        return fail_exr(result, "cannot create EXR output", category, error);
      created->owns_path = true;
      int part_index = -1;
      result = exr_add_part(created->exr_context, nullptr,
                            EXR_STORAGE_SCANLINE, &part_index);
      if (result == EXR_ERR_SUCCESS)
        result = exr_initialize_required_attr_simple(
            created->exr_context, part_index,
            static_cast<std::int32_t>(options.width),
            static_cast<std::int32_t>(options.height), EXR_COMPRESSION_PIZ);
      for (const char *name : {"R", "G", "B"})
        if (result == EXR_ERR_SUCCESS)
          result = exr_add_channel(created->exr_context, part_index, name,
                                   EXR_PIXEL_HALF,
                                   EXR_PERCEPTUALLY_LOGARITHMIC, 1, 1);
      if (result == EXR_ERR_SUCCESS)
        result = exr_write_header(created->exr_context);
      int rows_per_chunk = 0;
      if (result == EXR_ERR_SUCCESS)
        result = exr_get_scanlines_per_chunk(created->exr_context, part_index,
                                             &rows_per_chunk);
      if (result != EXR_ERR_SUCCESS)
        return fail_exr(result, "cannot initialize EXR output", category,
                        error);
      if (part_index != 0 || rows_per_chunk <= 0)
        return fail(CodecErrorCategory::codec_failure,
                    "invalid EXR output chunk layout", category, error);
      created->exr_rows_per_chunk = static_cast<unsigned>(rows_per_chunk);
      const std::uint64_t samples =
          static_cast<std::uint64_t>(options.width) * 3U *
          created->exr_rows_per_chunk;
      if (samples > std::numeric_limits<std::size_t>::max())
        return fail(CodecErrorCategory::invalid_request,
                    "EXR output chunk is too large", category, error);
      created->exr_pending.resize(static_cast<std::size_t>(samples));
      *writer = created.release();
      category = CodecErrorCategory::none;
      error.clear();
      return true;
    } catch (const std::bad_alloc &) {
      return fail(CodecErrorCategory::allocation,
                  "cannot allocate EXR image writer", category, error);
    } catch (...) {
      return fail(CodecErrorCategory::codec_failure,
                  "unexpected EXR writer creation failure", category, error);
    }
  }
#ifndef _WIN32
  return fail(CodecErrorCategory::unavailable,
              "native PNG/JPEG encoding requires Windows Imaging Component",
              category, error);
#else
  try {
    std::wstring path;
    if (!utf8_to_wide(options.path, path))
      return fail(CodecErrorCategory::invalid_request,
                  "image writer path is not valid UTF-8", category, error);
    auto created = std::make_unique<ImageWriter>(options);
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (com_result == S_OK || com_result == S_FALSE)
      created->uninitialize_com = true;
    else if (com_result != RPC_E_CHANGED_MODE)
      return fail_wic(com_result, "cannot initialize COM for image encoding",
                      category, error);
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&created->factory));
    if (FAILED(result))
      return fail_wic(result, "cannot create image encoder factory", category,
                      error);
    result = created->factory->CreateStream(&created->stream);
    if (FAILED(result))
      return fail_wic(result, "cannot create image output stream", category,
                      error);
    result = created->stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(result))
      return fail_wic(result, "cannot create image output", category, error);
    created->owns_path = true;
    const GUID &container = options.container == ImageContainer::png
                                ? GUID_ContainerFormatPng
                                : GUID_ContainerFormatJpeg;
    result = created->factory->CreateEncoder(container, nullptr,
                                             &created->encoder);
    if (FAILED(result))
      return fail_wic(result, "cannot create image encoder", category, error);
    result = created->encoder->Initialize(created->stream.Get(),
                                          WICBitmapEncoderNoCache);
    if (FAILED(result))
      return fail_wic(result, "cannot initialize image encoder", category,
                      error);
    ComPtr<IPropertyBag2> properties;
    result = created->encoder->CreateNewFrame(&created->frame, &properties);
    if (FAILED(result))
      return fail_wic(result, "cannot create image frame", category, error);
    if (options.container == ImageContainer::jpeg) {
      VARIANT quality;
      VariantInit(&quality);
      quality.vt = VT_R4;
      quality.fltVal = static_cast<float>(options.jpeg_quality) / 100.0F;
      wchar_t quality_name[] = L"ImageQuality";
      if (!write_option(properties.Get(), quality_name, quality, category,
                        error))
        return false;
      VARIANT sampling;
      VariantInit(&sampling);
      sampling.vt = VT_UI1;
      sampling.bVal = WICJpegYCrCbSubsampling444;
      wchar_t sampling_name[] = L"JpegYCrCbSubsampling";
      if (!write_option(properties.Get(), sampling_name, sampling, category,
                        error))
        return false;
    }
    result = created->frame->Initialize(properties.Get());
    if (FAILED(result))
      return fail_wic(result, "cannot initialize image frame", category,
                      error);
    result = created->frame->SetSize(options.width, options.height);
    if (FAILED(result))
      return fail_wic(result, "cannot set image dimensions", category, error);
    WICPixelFormatGUID format = options.channels == 1U
                                    ? GUID_WICPixelFormat8bppGray
                                    : GUID_WICPixelFormat24bppBGR;
    result = created->frame->SetPixelFormat(&format);
    const WICPixelFormatGUID expected_format = options.channels == 1U
                                                   ? GUID_WICPixelFormat8bppGray
                                                   : GUID_WICPixelFormat24bppBGR;
    if (FAILED(result) || format != expected_format)
      return fail(CodecErrorCategory::unsupported,
                  "image encoder does not accept 24-bit BGR", category,
                  error);
    *writer = created.release();
    category = CodecErrorCategory::none;
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    return fail(CodecErrorCategory::allocation,
                "cannot allocate image writer", category, error);
  } catch (...) {
    return fail(CodecErrorCategory::codec_failure,
                "unexpected image writer creation failure", category, error);
  }
#endif
}

bool write_image_rows(ImageWriter *const writer, const void *const rows,
                      const unsigned row_count,
                      const std::uint64_t row_stride_bytes,
                      const CancellationCheck &cancellation,
                      CodecErrorCategory &category, std::string &error) {
  if (writer == nullptr || rows == nullptr || row_count == 0U ||
      writer->rows_written > writer->options.height ||
      row_count > writer->options.height - writer->rows_written)
    return fail(CodecErrorCategory::invalid_request,
                "invalid image output band", category, error);
  const std::uint64_t packed_stride =
      static_cast<std::uint64_t>(writer->options.width) *
      writer->options.channels *
      (writer->options.container == ImageContainer::exr ? sizeof(float) : 1U);
  if (row_stride_bytes < packed_stride ||
      packed_stride > std::numeric_limits<unsigned>::max() ||
      row_count > std::numeric_limits<unsigned>::max() / packed_stride)
    return fail(CodecErrorCategory::invalid_request,
                "invalid image output row stride", category, error);
  if (cancelled(cancellation))
    return fail(CodecErrorCategory::cancelled, "image encode cancelled",
                category, error);
  if (writer->options.container == ImageContainer::exr) {
    try {
      const auto *input = static_cast<const std::uint8_t *>(rows);
      for (unsigned row = 0; row < row_count; ++row) {
        const auto *samples = reinterpret_cast<const float *>(
            input + static_cast<std::size_t>(row) * row_stride_bytes);
        const std::size_t sample_count =
            static_cast<std::size_t>(writer->options.width) * 3U;
        for (std::size_t sample = 0; sample < sample_count; ++sample)
          if (!std::isfinite(samples[sample]))
            return fail(CodecErrorCategory::invalid_request,
                        "EXR output contains a non-finite sample", category,
                        error);
        std::copy_n(samples, sample_count,
                    writer->exr_pending.data() +
                        static_cast<std::size_t>(writer->exr_pending_rows) *
                            sample_count);
        ++writer->exr_pending_rows;
        ++writer->rows_written;
        if (writer->exr_pending_rows == writer->exr_rows_per_chunk ||
            writer->rows_written == writer->options.height)
          if (!flush_exr_chunk(*writer, category, error)) return false;
        if (cancelled(cancellation))
          return fail(CodecErrorCategory::cancelled,
                      "image encode cancelled", category, error);
      }
      category = CodecErrorCategory::none;
      error.clear();
      return true;
    } catch (const std::bad_alloc &) {
      return fail(CodecErrorCategory::allocation,
                  "cannot buffer EXR output chunk", category, error);
    } catch (...) {
      return fail(CodecErrorCategory::codec_failure,
                  "unexpected EXR row encoding failure", category, error);
    }
  }
#ifndef _WIN32
  return fail(CodecErrorCategory::unavailable,
              "native PNG/JPEG encoding requires Windows Imaging Component",
              category, error);
#else
  try {
    std::vector<std::uint8_t> encoded(
        static_cast<std::size_t>(packed_stride) * row_count);
    const auto *input = static_cast<const std::uint8_t *>(rows);
    if (writer->options.channels == 1U) {
      for (unsigned row = 0; row < row_count; ++row)
        std::copy_n(input + static_cast<std::size_t>(row) * row_stride_bytes,
                    static_cast<std::size_t>(packed_stride),
                    encoded.data() + static_cast<std::size_t>(row) *
                                         packed_stride);
    } else {
      for (unsigned row = 0; row < row_count; ++row)
        for (unsigned pixel = 0; pixel < writer->options.width; ++pixel) {
          const std::size_t source =
              static_cast<std::size_t>(row) * row_stride_bytes + pixel * 3U;
          const std::size_t target =
              static_cast<std::size_t>(row) * packed_stride + pixel * 3U;
          encoded[target] = input[source + 2U];
          encoded[target + 1U] = input[source + 1U];
          encoded[target + 2U] = input[source];
        }
    }
    const HRESULT result = writer->frame->WritePixels(
        row_count, static_cast<UINT>(packed_stride),
        static_cast<UINT>(encoded.size()), encoded.data());
    if (FAILED(result))
      return fail_wic(result, "cannot encode image rows", category, error);
    writer->rows_written += row_count;
    if (cancelled(cancellation))
      return fail(CodecErrorCategory::cancelled, "image encode cancelled",
                  category, error);
    category = CodecErrorCategory::none;
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    return fail(CodecErrorCategory::allocation,
                "cannot allocate image output band", category, error);
  } catch (...) {
    return fail(CodecErrorCategory::codec_failure,
                "unexpected image row encoding failure", category, error);
  }
#endif
}

bool finish_image_writer(ImageWriter **const writer,
                         const CancellationCheck &cancellation,
                         CodecErrorCategory &category, std::string &error) {
  if (writer == nullptr || *writer == nullptr)
    return fail(CodecErrorCategory::invalid_request,
                "invalid image writer handle", category, error);
  ImageWriter *const active = *writer;
  if (active->rows_written != active->options.height)
    return fail(CodecErrorCategory::invalid_request,
                "image writer has incomplete rows", category, error);
  if (cancelled(cancellation)) {
    abort_image_writer(writer);
    return fail(CodecErrorCategory::cancelled, "image encode cancelled",
                category, error);
  }
  if (active->options.container == ImageContainer::exr) {
    if (active->exr_pending_rows != 0U &&
        !flush_exr_chunk(*active, category, error)) {
      abort_image_writer(writer);
      return false;
    }
    const exr_result_t result = exr_finish(&active->exr_context);
    if (result != EXR_ERR_SUCCESS) {
      abort_image_writer(writer);
      return fail_exr(result, "cannot finish EXR output", category, error);
    }
    active->close(true);
    delete active;
    *writer = nullptr;
    category = CodecErrorCategory::none;
    error.clear();
    return true;
  }
#ifndef _WIN32
  return fail(CodecErrorCategory::unavailable,
              "native PNG/JPEG encoding requires Windows Imaging Component",
              category, error);
#else
  const HRESULT frame_result = active->frame->Commit();
  const HRESULT encoder_result =
      SUCCEEDED(frame_result) ? active->encoder->Commit() : frame_result;
  if (FAILED(encoder_result)) {
    abort_image_writer(writer);
    return fail_wic(encoder_result, "cannot finish image encoder", category,
                    error);
  }
  active->close(true);
  delete active;
  *writer = nullptr;
  category = CodecErrorCategory::none;
  error.clear();
  return true;
#endif
}

void abort_image_writer(ImageWriter **const writer) noexcept {
  if (writer == nullptr || *writer == nullptr) return;
  delete *writer;
  *writer = nullptr;
}

} // namespace pano::app
