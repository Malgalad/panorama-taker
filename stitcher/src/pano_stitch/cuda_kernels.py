"""CUDA kernel sources for the resident panorama compositor."""

from __future__ import annotations

COMPOSITE_FRAME = r"""
extern "C" __global__ void composite_frame(
    const float* source, const float* map_x, const float* map_y,
    const unsigned char* valid, const float* candidate, const float* correction,
    float* color, float* weight, int source_width, int source_height,
    int pixels, int hard_blend) {
  int index = blockDim.x * blockIdx.x + threadIdx.x;
  if (index >= pixels || !valid[index]) return;
  float x = map_x[index], y = map_y[index];
  int x0 = (int)floorf(x), y0 = (int)floorf(y);
  int x1 = min(x0 + 1, source_width - 1), y1 = min(y0 + 1, source_height - 1);
  float wx = x - (float)x0, wy = y - (float)y0;
  int p00 = (y0 * source_width + x0) * 3;
  int p10 = (y0 * source_width + x1) * 3;
  int p01 = (y1 * source_width + x0) * 3;
  int p11 = (y1 * source_width + x1) * 3;
  float w00 = (1.0f - wx) * (1.0f - wy), w10 = wx * (1.0f - wy);
  float w01 = (1.0f - wx) * wy, w11 = wx * wy;
  float c = candidate[index];
  if (hard_blend) {
    if (c <= weight[index]) return;
    for (int channel = 0; channel < 3; ++channel)
      color[index * 3 + channel] = (source[p00 + channel] * w00 + source[p10 + channel] * w10 +
        source[p01 + channel] * w01 + source[p11 + channel] * w11) * correction[index];
    weight[index] = c;
  } else {
    for (int channel = 0; channel < 3; ++channel)
      color[index * 3 + channel] += (source[p00 + channel] * w00 + source[p10 + channel] * w10 +
        source[p01 + channel] * w01 + source[p11 + channel] * w11) * correction[index] * c;
    weight[index] += c;
  }
}
"""

COMPOSITE_PROJECTED = r"""
extern "C" __global__ void composite_projected(
    const float* source, const float* exposure, float* color, float* weight,
    int source_width, int source_height, int output_width, int output_height,
    float latitude_span, float horizontal_fov, float vertical_fov,
    const float* rotation, int hard_blend, int exposure_width, int exposure_height,
    float log_gain) {
  int index = blockDim.x * blockIdx.x + threadIdx.x;
  int pixels = output_width * output_height;
  if (index >= pixels) return;
  int row = index / output_width, column = index - row * output_width;
  float longitude = (((float)column + 0.5f) / (float)output_width - 0.5f) * 6.28318530718f;
  float latitude = (0.5f - ((float)row + 0.5f) / (float)output_height)
      * latitude_span * 0.01745329252f;
  float cl = cosf(latitude), world_x = cl * sinf(longitude);
  float world_y = sinf(latitude), world_z = cl * cosf(longitude);
  float local_x = world_x * rotation[0] + world_y * rotation[3] + world_z * rotation[6];
  float local_y = world_x * rotation[1] + world_y * rotation[4] + world_z * rotation[7];
  float local_z = world_x * rotation[2] + world_y * rotation[5] + world_z * rotation[8];
  float safe_z = fabsf(local_z) > 1e-8f ? local_z : 1.0f;
  float focal_x = (float)source_width / (2.0f * tanf(horizontal_fov * 0.00872664626f));
  float focal_y = (float)source_height / (2.0f * tanf(vertical_fov * 0.00872664626f));
  float x = (float)(source_width - 1) * 0.5f + focal_x * local_x / safe_z;
  float y = (float)(source_height - 1) * 0.5f - focal_y * local_y / safe_z;
  if (local_z <= 0.0f || x < -0.5f || x > (float)source_width - 0.5f ||
      y < -0.5f || y > (float)source_height - 0.5f) return;
  x = fminf(fmaxf(x, 0.0f), (float)source_width - 1.0f);
  y = fminf(fmaxf(y, 0.0f), (float)source_height - 1.0f);
  float edge = fminf(fminf(x, y), fminf((float)source_width - 1.0f - x,
                                        (float)source_height - 1.0f - y));
  float feather_width = fmaxf(1.0f, fminf(source_width, source_height) * 0.08f);
  float candidate = hard_blend ? fmaxf(edge, 1e-6f) : fmaxf(edge / feather_width, 1e-6f);
  int x0 = (int)floorf(x), y0 = (int)floorf(y);
  int x1 = min(x0 + 1, source_width - 1), y1 = min(y0 + 1, source_height - 1);
  float wx = x - (float)x0, wy = y - (float)y0;
  int p00 = (y0 * source_width + x0) * 3, p10 = (y0 * source_width + x1) * 3;
  int p01 = (y1 * source_width + x0) * 3, p11 = (y1 * source_width + x1) * 3;
  int out = index * 3;
  float exposure_x = ((float)column + 0.5f) * (float)exposure_width /
      (float)output_width - 0.5f;
  float exposure_y = ((float)row + 0.5f) * (float)exposure_height /
      (float)output_height - 0.5f;
  exposure_x = fminf(fmaxf(exposure_x, 0.0f), (float)exposure_width - 1.0f);
  exposure_y = fminf(fmaxf(exposure_y, 0.0f), (float)exposure_height - 1.0f);
  int exposure_x0 = (int)floorf(exposure_x), exposure_y0 = (int)floorf(exposure_y);
  int exposure_x1 = min(exposure_x0 + 1, exposure_width - 1);
  int exposure_y1 = min(exposure_y0 + 1, exposure_height - 1);
  float exposure_wx = exposure_x - (float)exposure_x0;
  float exposure_wy = exposure_y - (float)exposure_y0;
  float exposure_value =
      (1.0f - exposure_wx) * (1.0f - exposure_wy) *
          exposure[exposure_y0 * exposure_width + exposure_x0] +
      exposure_wx * (1.0f - exposure_wy) *
          exposure[exposure_y0 * exposure_width + exposure_x1] +
      (1.0f - exposure_wx) * exposure_wy *
          exposure[exposure_y1 * exposure_width + exposure_x0] +
      exposure_wx * exposure_wy * exposure[exposure_y1 * exposure_width + exposure_x1];
  float gain = expf(log_gain - exposure_value);
  float sampled[3];
  for (int channel = 0; channel < 3; ++channel) {
    sampled[channel] = ((1.0f-wx)*(1.0f-wy)*source[p00+channel] + wx*(1.0f-wy)*source[p10+channel] +
      (1.0f-wx)*wy*source[p01+channel] + wx*wy*source[p11+channel]) * gain;
  }
  if (hard_blend) {
    if (candidate > weight[index]) {
      color[out] = sampled[0]; color[out+1] = sampled[1];
      color[out+2] = sampled[2]; weight[index] = candidate;
    }
  } else {
    color[out] += sampled[0] * candidate; color[out+1] += sampled[1] * candidate;
    color[out+2] += sampled[2] * candidate; weight[index] += candidate;
  }
}
"""

NORMALIZE_EXPOSURE = r"""
extern "C" __global__ void normalize_exposure(
    float* exposure_sum, const float* exposure_weight, int pixels) {
  int index = blockDim.x * blockIdx.x + threadIdx.x;
  if (index >= pixels) return;
  float weight = exposure_weight[index];
  if (weight > 0.0f) exposure_sum[index] /= weight;
}
"""

ACCUMULATE_EXPOSURE = r"""
extern "C" __global__ void accumulate_exposure(
    float* exposure_sum, float* exposure_weight, int width, int height,
    float latitude_span, const float* rotation, int source_width, int source_height,
    float horizontal_fov, float vertical_fov, float log_gain) {
  int index = blockDim.x * blockIdx.x + threadIdx.x;
  int pixels = width * height;
  if (index >= pixels) return;
  int x = index % width, y = index / width;
  float lon = (((float)x + 0.5f) / (float)width - 0.5f) * 6.283185307179586f;
  float lat = (0.5f - ((float)y + 0.5f) / (float)height) * latitude_span * 0.0174532925199433f;
  float cl = cosf(lat), sx = cl * sinf(lon), sy = sinf(lat), sz = cl * cosf(lon);
  float lx = sx * rotation[0] + sy * rotation[3] + sz * rotation[6];
  float ly = sx * rotation[1] + sy * rotation[4] + sz * rotation[7];
  float lz = sx * rotation[2] + sy * rotation[5] + sz * rotation[8];
  float safe_z = fabsf(lz) > 1e-8f ? lz : 1.0f;
  float fx = (float)source_width / (2.0f * tanf(horizontal_fov * 0.00872664625997165f));
  float fy = (float)source_height / (2.0f * tanf(vertical_fov * 0.00872664625997165f));
  float map_x = (float)(source_width - 1) * 0.5f + fx * lx / safe_z;
  float map_y = (float)(source_height - 1) * 0.5f - fy * ly / safe_z;
  if (fabsf(map_x + 0.5f) < 1e-4f) map_x = -0.5f;
  if (fabsf(map_y + 0.5f) < 1e-4f) map_y = -0.5f;
  bool valid = lz > 0.0f && map_x >= -0.5f && map_x <= source_width - 0.5f &&
               map_y >= -0.5f && map_y <= source_height - 0.5f;
  map_x = fminf(fmaxf(map_x, 0.0f), source_width - 1.0f);
  map_y = fminf(fmaxf(map_y, 0.0f), source_height - 1.0f);
  float edge = fminf(
      fminf(map_x, map_y),
      fminf(source_width - 1.0f - map_x, source_height - 1.0f - map_y));
  float feather = fmaxf(1.0f, fminf(source_width, source_height) * 0.08f);
  float weight = valid ? fmaxf(edge / feather, 1e-6f) : 0.0f;
  exposure_sum[index] += weight * log_gain;
  exposure_weight[index] += weight;
}
"""
