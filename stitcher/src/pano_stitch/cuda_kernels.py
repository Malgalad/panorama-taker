"""CUDA kernel sources for the resident panorama compositor."""

from __future__ import annotations

FULL_GPU_PIPELINE = r"""
__device__ inline float decode_source_value(
    const void* sources, long long index, int sample_type, int transfer_function) {
  float value;
  if (sample_type == 0) value = ((const unsigned char*)sources)[index] * (1.0f / 255.0f);
  else if (sample_type == 1) value = ((const unsigned short*)sources)[index] * (1.0f / 65535.0f);
  else value = ((const float*)sources)[index];
  if (transfer_function == 0) {
    return value <= 0.04045f ? value * (1.0f / 12.92f) : powf((value + 0.055f) / 1.055f, 2.4f);
  }
  if (transfer_function == 1) {
    const float powered = powf(fmaxf(value, 0.0f), 32.0f / 2523.0f);
    const float numerator = fmaxf(powered - (3424.0f / 4096.0f), 0.0f);
    const float denominator = fmaxf(
        (2413.0f / 128.0f) - (2392.0f / 128.0f) * powered, 1.17549435e-38f);
    return powf(numerator / denominator, 16384.0f / 2610.0f);
  }
  return value;
}

__device__ inline bool project_output_ray(
    float output_x, float output_y, int output_width, int output_height, float latitude_span,
    const float* rotation, int source_width, int source_height, float horizontal_fov,
    float vertical_fov, int rectilinear_output, float output_vertical_fov, float* map_x,
    float* map_y, float* edge_distance) {
  float world_x, world_y, world_z;
  if (rectilinear_output) {
    const float focal_x = output_width * 0.5f;
    const float focal_y = output_height
        / (2.0f * tanf(output_vertical_fov * 0.00872664625997165f));
    world_x = (output_x - (output_width - 1) * 0.5f) / focal_x;
    world_y = ((output_height - 1) * 0.5f - output_y) / focal_y;
    world_z = 1.0f;
    const float inverse_length = rsqrtf(world_x * world_x + world_y * world_y + world_z * world_z);
    world_x *= inverse_length;
    world_y *= inverse_length;
    world_z *= inverse_length;
  } else {
    const float longitude = ((output_x + 0.5f) / output_width - 0.5f) * 6.283185307179586f;
    const float latitude = (0.5f - (output_y + 0.5f) / output_height) * latitude_span
        * 0.0174532925199433f;
    const float cosine = cosf(latitude);
    world_x = cosine * sinf(longitude);
    world_y = sinf(latitude);
    world_z = cosine * cosf(longitude);
  }
  const float local_x = world_x * rotation[0] + world_y * rotation[3] + world_z * rotation[6];
  const float local_y = world_x * rotation[1] + world_y * rotation[4] + world_z * rotation[7];
  const float local_z = world_x * rotation[2] + world_y * rotation[5] + world_z * rotation[8];
  const float safe_z = fabsf(local_z) > 1e-8f ? local_z : 1.0f;
  const float focal_x = source_width / (2.0f * tanf(horizontal_fov * 0.00872664625997165f));
  const float focal_y = source_height / (2.0f * tanf(vertical_fov * 0.00872664625997165f));
  const float projected_x = (source_width - 1) * 0.5f + focal_x * local_x / safe_z;
  const float projected_y = (source_height - 1) * 0.5f - focal_y * local_y / safe_z;
  const bool valid = local_z > 0.0f && projected_x >= -0.5f && projected_x <= source_width - 0.5f
      && projected_y >= -0.5f && projected_y <= source_height - 0.5f;
  *map_x = fminf(fmaxf(projected_x, 0.0f), source_width - 1.0f);
  *map_y = fminf(fmaxf(projected_y, 0.0f), source_height - 1.0f);
  *edge_distance = fminf(
      fminf(*map_x, *map_y), fminf(source_width - 1.0f - *map_x, source_height - 1.0f - *map_y));
  return valid;
}

__device__ inline float sample_local_exposure(
    const float* field, int field_width, int field_height, int output_width, int output_height,
    int x, int y) {
  float field_x = ((x + 0.5f) * field_width / output_width) - 0.5f;
  float field_y = ((y + 0.5f) * field_height / output_height) - 0.5f;
  field_x = fminf(fmaxf(field_x, 0.0f), field_width - 1.0f);
  field_y = fminf(fmaxf(field_y, 0.0f), field_height - 1.0f);
  const int x0 = (int)floorf(field_x), y0 = (int)floorf(field_y);
  const int x1 = min(x0 + 1, field_width - 1), y1 = min(y0 + 1, field_height - 1);
  const float wx = field_x - x0, wy = field_y - y0;
  const float upper = field[y0 * field_width + x0] * (1.0f - wx)
      + field[y0 * field_width + x1] * wx;
  const float lower = field[y1 * field_width + x0] * (1.0f - wx)
      + field[y1 * field_width + x1] * wx;
  return upper * (1.0f - wy) + lower * wy;
}

extern "C" __global__ void build_exposure_proxies(
    const void* sources, int sample_type, int transfer_function, float* proxies, int frame_count,
    int source_width, int source_height, int proxy_width, int proxy_height) {
  const int index = blockIdx.x * blockDim.x + threadIdx.x;
  const int pixels = frame_count * proxy_width * proxy_height;
  if (index >= pixels) return;
  const int frame = index / (proxy_width * proxy_height);
  const int pixel = index - frame * proxy_width * proxy_height;
  const int x = pixel % proxy_width, y = pixel / proxy_width;
  const float left = x * (float)source_width / proxy_width;
  const float right = (x + 1) * (float)source_width / proxy_width;
  const float top = y * (float)source_height / proxy_height;
  const float bottom = (y + 1) * (float)source_height / proxy_height;
  const int x0 = (int)floorf(left), x1 = min((int)ceilf(right), source_width);
  const int y0 = (int)floorf(top), y1 = min((int)ceilf(bottom), source_height);
  const long long source_samples = (long long)source_width * source_height * 3;
  const long long source_base = (long long)frame * source_samples;
  float red = 0.0f, green = 0.0f, blue = 0.0f, total = 0.0f;
  for (int sample_y = y0; sample_y < y1; ++sample_y) {
    const float weight_y = fmaxf(
        0.0f, fminf(bottom, sample_y + 1.0f) - fmaxf(top, (float)sample_y));
    for (int sample_x = x0; sample_x < x1; ++sample_x) {
      const float weight_x = fmaxf(
          0.0f, fminf(right, sample_x + 1.0f) - fmaxf(left, (float)sample_x));
      const float weight = weight_x * weight_y;
      const long long offset = source_base + ((long long)sample_y * source_width + sample_x) * 3;
      red += weight * decode_source_value(sources, offset, sample_type, transfer_function);
      green += weight * decode_source_value(sources, offset + 1, sample_type, transfer_function);
      blue += weight * decode_source_value(sources, offset + 2, sample_type, transfer_function);
      total += weight;
    }
  }
  const long long output = ((long long)frame * proxy_width * proxy_height + pixel) * 3;
  proxies[output] = red / total;
  proxies[output + 1] = green / total;
  proxies[output + 2] = blue / total;
}

extern "C" __global__ void sample_exposure_grid(
    const float* proxies, float* luminance, unsigned char* coverage, unsigned char* clipped,
    const float* rotations, int frame_count, int proxy_width, int proxy_height, int sample_width,
    int sample_height, float latitude_span, float horizontal_fov, float vertical_fov) {
  const int index = blockIdx.x * blockDim.x + threadIdx.x;
  const int pixels_per_frame = sample_width * sample_height;
  const int pixels = frame_count * pixels_per_frame;
  if (index >= pixels) return;
  const int frame = index / pixels_per_frame;
  const int pixel = index - frame * pixels_per_frame;
  const int x = pixel % sample_width, y = pixel / sample_width;
  float map_x, map_y, edge_distance;
  const bool valid = project_output_ray(x, y, sample_width, sample_height, latitude_span,
      rotations + frame * 9, proxy_width, proxy_height, horizontal_fov, vertical_fov, 0, 0.0f,
      &map_x, &map_y, &edge_distance);
  if (!valid) {
    luminance[index] = 0.0f;
    coverage[index] = 0;
    clipped[index] = 0;
    return;
  }
  const int x0 = (int)floorf(map_x), y0 = (int)floorf(map_y);
  const int x1 = min(x0 + 1, proxy_width - 1), y1 = min(y0 + 1, proxy_height - 1);
  const float wx = map_x - x0, wy = map_y - y0;
  const float w00 = (1.0f - wx) * (1.0f - wy), w10 = wx * (1.0f - wy);
  const float w01 = (1.0f - wx) * wy, w11 = wx * wy;
  const long long base = ((long long)frame * proxy_width * proxy_height) * 3;
  const long long p00 = base + ((long long)y0 * proxy_width + x0) * 3;
  const long long p10 = base + ((long long)y0 * proxy_width + x1) * 3;
  const long long p01 = base + ((long long)y1 * proxy_width + x0) * 3;
  const long long p11 = base + ((long long)y1 * proxy_width + x1) * 3;
  const float red = w00 * proxies[p00] + w10 * proxies[p10]
      + w01 * proxies[p01] + w11 * proxies[p11];
  const float green = w00 * proxies[p00 + 1] + w10 * proxies[p10 + 1]
      + w01 * proxies[p01 + 1] + w11 * proxies[p11 + 1];
  const float blue = w00 * proxies[p00 + 2] + w10 * proxies[p10 + 2]
      + w01 * proxies[p01 + 2] + w11 * proxies[p11 + 2];
  luminance[index] = red * 0.2126f + green * 0.7152f + blue * 0.0722f;
  coverage[index] = 1;
  clipped[index] = red >= 0.995f || green >= 0.995f || blue >= 0.995f;
}

extern "C" __global__ void classify_exposure_samples(
    const float* luminance, const unsigned char* coverage, const unsigned char* clipped,
    const float* gradient_limits, float* gradients, unsigned char* valid, int frame_count,
    int sample_width, int sample_height, int apply_gradient_limit) {
  const int index = blockIdx.x * blockDim.x + threadIdx.x;
  const int pixels_per_frame = sample_width * sample_height;
  const int pixels = frame_count * pixels_per_frame;
  if (index >= pixels) return;
  const int frame = index / pixels_per_frame;
  const int pixel = index - frame * pixels_per_frame;
  const int x = pixel % sample_width, y = pixel / sample_width;
  const int left_x = x == 0 ? min(1, sample_width - 1) : x - 1;
  const int right_x = x == sample_width - 1 ? max(sample_width - 2, 0) : x + 1;
  const int top_y = y == 0 ? min(1, sample_height - 1) : y - 1;
  const int bottom_y = y == sample_height - 1 ? max(sample_height - 2, 0) : y + 1;
  const int base = frame * pixels_per_frame;
  const int top_left = base + top_y * sample_width + left_x;
  const int top = base + top_y * sample_width + x;
  const int top_right = base + top_y * sample_width + right_x;
  const int left = base + y * sample_width + left_x;
  const int right = base + y * sample_width + right_x;
  const int bottom_left = base + bottom_y * sample_width + left_x;
  const int bottom = base + bottom_y * sample_width + x;
  const int bottom_right = base + bottom_y * sample_width + right_x;
  const float center = logf(fmaxf(luminance[index], 1e-5f));
  const float log_top_left = logf(fmaxf(luminance[top_left], 1e-5f));
  const float log_top = logf(fmaxf(luminance[top], 1e-5f));
  const float log_top_right = logf(fmaxf(luminance[top_right], 1e-5f));
  const float log_left = logf(fmaxf(luminance[left], 1e-5f));
  const float log_right = logf(fmaxf(luminance[right], 1e-5f));
  const float log_bottom_left = logf(fmaxf(luminance[bottom_left], 1e-5f));
  const float log_bottom = logf(fmaxf(luminance[bottom], 1e-5f));
  const float log_bottom_right = logf(fmaxf(luminance[bottom_right], 1e-5f));
  const float horizontal = -log_top_left + log_top_right - 2.0f * log_left
      + 2.0f * log_right - log_bottom_left + log_bottom_right;
  const float vertical = -log_top_left - 2.0f * log_top - log_top_right
      + log_bottom_left + 2.0f * log_bottom + log_bottom_right;
  const float gradient = sqrtf(horizontal * horizontal + vertical * vertical);
  gradients[index] = gradient;
  const bool finite = isfinite(center) && luminance[index] > 1e-5f;
  const bool accepted = coverage[index] && !clipped[index] && finite
      && (!apply_gradient_limit || gradient <= gradient_limits[frame]);
  valid[index] = accepted ? 1 : 0;
}

extern "C" __global__ void build_local_exposure(
    float* field, int field_width, int field_height, int output_width, int output_height,
    float latitude_span, const float* rotations, const float* log_gains, int frame_count,
    int source_width, int source_height, float horizontal_fov, float vertical_fov,
    int rectilinear_output, float output_vertical_fov) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= field_width || y >= field_height) return;
  const int index = y * field_width + x;
  const float output_x = (x + 0.5f) * output_width / field_width - 0.5f;
  const float output_y = (y + 0.5f) * output_height / field_height - 0.5f;
  float weighted_gain = 0.0f;
  float total_weight = 0.0f;
  const float feather = fmaxf(1.0f, fminf(source_width, source_height) * 0.08f);
  for (int frame = 0; frame < frame_count; ++frame) {
    float map_x, map_y, edge_distance;
    if (!project_output_ray(output_x, output_y, output_width, output_height, latitude_span,
          rotations + frame * 9, source_width, source_height, horizontal_fov, vertical_fov,
          rectilinear_output, output_vertical_fov,
          &map_x, &map_y, &edge_distance)) continue;
    const float weight = fmaxf(edge_distance / feather, 1e-6f);
    weighted_gain += weight * log_gains[frame];
    total_weight += weight;
  }
  field[index] = total_weight > 0.0f ? weighted_gain / total_weight : 0.0f;
}

extern "C" __global__ void compose_output(
    const void* sources, int source_sample_type, int transfer_function, const float* rotations,
    const float* log_gains, const float* local_exposure, int local_width, int local_height,
    float* color, unsigned char* coverage, int source_width, int source_height, int output_width,
    int output_height, int row_start, int rows, float latitude_span, float horizontal_fov,
    float vertical_fov, int frame_count, int hard_blend, int incomplete_magenta,
    int rectilinear_output, float output_vertical_fov) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int local_row = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= output_width || local_row >= rows) return;
  const int index = local_row * output_width + x;
  const int y = row_start + local_row;
  const float exposure = sample_local_exposure(
      local_exposure, local_width, local_height, output_width, output_height, x, y);
  float output_r = 0.0f, output_g = 0.0f, output_b = 0.0f;
  float best_weight = 0.0f, total_weight = 0.0f;
  const float feather = fmaxf(1.0f, fminf(source_width, source_height) * 0.08f);
  const long long source_samples = (long long)source_width * source_height * 3;
  for (int frame = 0; frame < frame_count; ++frame) {
    float map_x, map_y, edge_distance;
    if (!project_output_ray(x, y, output_width, output_height, latitude_span, rotations + frame * 9,
          source_width, source_height, horizontal_fov, vertical_fov, rectilinear_output,
          output_vertical_fov, &map_x, &map_y,
          &edge_distance)) continue;
    const int x0 = (int)floorf(map_x), y0 = (int)floorf(map_y);
    const int x1 = min(x0 + 1, source_width - 1), y1 = min(y0 + 1, source_height - 1);
    const float wx = map_x - x0, wy = map_y - y0;
    const float w00 = (1.0f - wx) * (1.0f - wy), w10 = wx * (1.0f - wy);
    const float w01 = (1.0f - wx) * wy, w11 = wx * wy;
    const long long base = (long long)frame * source_samples;
    const long long p00 = base + ((long long)y0 * source_width + x0) * 3;
    const long long p10 = base + ((long long)y0 * source_width + x1) * 3;
    const long long p01 = base + ((long long)y1 * source_width + x0) * 3;
    const long long p11 = base + ((long long)y1 * source_width + x1) * 3;
    const float gain = expf(log_gains[frame] - exposure);
    const float red = gain * (w00 * decode_source_value(
        sources, p00, source_sample_type, transfer_function)
        + w10 * decode_source_value(sources, p10, source_sample_type, transfer_function)
        + w01 * decode_source_value(sources, p01, source_sample_type, transfer_function)
        + w11 * decode_source_value(sources, p11, source_sample_type, transfer_function));
    const float green = gain * (w00 * decode_source_value(
        sources, p00 + 1, source_sample_type, transfer_function)
        + w10 * decode_source_value(sources, p10 + 1, source_sample_type, transfer_function)
        + w01 * decode_source_value(sources, p01 + 1, source_sample_type, transfer_function)
        + w11 * decode_source_value(sources, p11 + 1, source_sample_type, transfer_function));
    const float blue = gain * (w00 * decode_source_value(
        sources, p00 + 2, source_sample_type, transfer_function)
        + w10 * decode_source_value(sources, p10 + 2, source_sample_type, transfer_function)
        + w01 * decode_source_value(sources, p01 + 2, source_sample_type, transfer_function)
        + w11 * decode_source_value(sources, p11 + 2, source_sample_type, transfer_function));
    const float candidate = hard_blend ? fmaxf(edge_distance, 1e-6f)
        : fmaxf(edge_distance / feather, 1e-6f);
    if (hard_blend) {
      if (candidate > best_weight) {
        output_r = red; output_g = green; output_b = blue; best_weight = candidate;
      }
    } else {
      output_r += red * candidate; output_g += green * candidate; output_b += blue * candidate;
      total_weight += candidate;
    }
  }
  const bool covered = hard_blend ? best_weight > 0.0f : total_weight > 0.0f;
  if (!hard_blend && covered) {
    output_r /= total_weight; output_g /= total_weight; output_b /= total_weight;
  }
  if (!covered && incomplete_magenta) {
    output_r = 1.0f; output_g = 0.0f; output_b = 1.0f;
  }
  color[index * 3] = output_r;
  color[index * 3 + 1] = output_g;
  color[index * 3 + 2] = output_b;
  coverage[index] = covered ? 255 : 0;
}

__device__ inline void linear_to_sdr(
    float red, float green, float blue, int source_transfer, float reference_white,
    float* output_red, float* output_green, float* output_blue) {
  if (source_transfer == 1) {
    red = fmaxf(red, 0.0f) * (10000.0f / reference_white);
    green = fmaxf(green, 0.0f) * (10000.0f / reference_white);
    blue = fmaxf(blue, 0.0f) * (10000.0f / reference_white);
    const float luminance = red * 0.2627f + green * 0.6780f + blue * 0.0593f;
    const float mapped = luminance / (1.0f + luminance);
    const float scale = luminance > 0.0f ? mapped / luminance : 0.0f;
    const float rec2020_red = red * scale;
    const float rec2020_green = green * scale;
    const float rec2020_blue = blue * scale;
    red = rec2020_red * 1.660491f - rec2020_green * 0.587641f - rec2020_blue * 0.072850f;
    green = -rec2020_red * 0.124550f + rec2020_green * 1.132900f - rec2020_blue * 0.008349f;
    blue = -rec2020_red * 0.018151f - rec2020_green * 0.100579f + rec2020_blue * 1.118730f;
  }
  red = fmaxf(red, 0.0f);
  green = fmaxf(green, 0.0f);
  blue = fmaxf(blue, 0.0f);
  *output_red = red <= 0.0031308f ? red * 12.92f : 1.055f * powf(red, 1.0f / 2.4f) - 0.055f;
  *output_green = green <= 0.0031308f ? green * 12.92f : 1.055f * powf(green, 1.0f / 2.4f) - 0.055f;
  *output_blue = blue <= 0.0031308f ? blue * 12.92f : 1.055f * powf(blue, 1.0f / 2.4f) - 0.055f;
}

__device__ inline unsigned char quantize_sdr(float value) {
  return (unsigned char)__float2uint_rn(fminf(fmaxf(value, 0.0f), 1.0f) * 255.0f);
}

extern "C" __global__ void build_auto_contrast_histogram(
    const float* color, const unsigned char* coverage, unsigned long long* histogram, int pixels,
    int source_transfer, float reference_white) {
  const int index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixels || !coverage[index]) return;
  float red, green, blue;
  linear_to_sdr(color[index * 3], color[index * 3 + 1], color[index * 3 + 2], source_transfer,
      reference_white, &red, &green, &blue);
  const float luminance = fminf(
      fmaxf(red * 0.2126f + green * 0.7152f + blue * 0.0722f, 0.0f), 1.0f);
  const int bin = min(4095, (int)floorf(luminance * 4096.0f));
  atomicAdd(histogram + bin, (unsigned long long)1);
}

extern "C" __global__ void select_auto_contrast_levels(
    const unsigned long long* histogram, float* levels) {
  if (blockIdx.x != 0 || threadIdx.x != 0) return;
  unsigned long long total = 0;
  for (int bin = 0; bin < 4096; ++bin) total += histogram[bin];
  if (total == 0) {
    levels[0] = 0.0f;
    levels[1] = 1.0f;
    return;
  }
  const float black_rank = 0.005f * (total - 1);
  const float white_rank = 0.995f * (total - 1);
  unsigned long long cumulative = 0;
  float black = 0.0f, white = 1.0f;
  bool black_found = false;
  for (int bin = 0; bin < 4096; ++bin) {
    const unsigned long long previous = cumulative;
    cumulative += histogram[bin];
    if (!black_found && cumulative >= black_rank + 1.0f) {
      const float position = (black_rank - previous) / histogram[bin];
      black = (bin + position) * (1.0f / 4096.0f);
      black_found = true;
    }
    if (cumulative >= white_rank + 1.0f) {
      const float position = (white_rank - previous) / histogram[bin];
      white = (bin + position) * (1.0f / 4096.0f);
      break;
    }
  }
  if (white - black < (1.0f / 4096.0f)) {
    levels[0] = 0.0f;
    levels[1] = 0.0f;
    return;
  }
  levels[0] = black;
  levels[1] = white;
}

extern "C" __global__ void convert_output(
    const float* color, unsigned char* converted, int pixels, int source_transfer,
    float reference_white, const float* levels, int apply_levels) {
  const int index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixels) return;
  float red, green, blue;
  linear_to_sdr(color[index * 3], color[index * 3 + 1], color[index * 3 + 2], source_transfer,
      reference_white, &red, &green, &blue);
  if (apply_levels) {
    const float black = levels[0], white = levels[1];
    if (white > black) {
      red = (red - black) / (white - black);
      green = (green - black) / (white - black);
      blue = (blue - black) / (white - black);
    }
  }
  converted[index * 3] = quantize_sdr(red);
  converted[index * 3 + 1] = quantize_sdr(green);
  converted[index * 3 + 2] = quantize_sdr(blue);
}
"""


CUDA_KERNEL_NAMES = (
    "build_local_exposure",
    "compose_output",
    "build_exposure_proxies",
    "sample_exposure_grid",
    "classify_exposure_samples",
    "build_auto_contrast_histogram",
    "select_auto_contrast_levels",
    "convert_output",
)

# One translation unit keeps shared helpers and compiler settings identical for
# every kernel. The module is compiled before any device allocation or source
# upload so an NVRTC failure can still select the CPU renderer safely.
CUDA_MODULE_SOURCE = "\n".join((FULL_GPU_PIPELINE,))
