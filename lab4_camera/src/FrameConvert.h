// ---------------------------------------------------------------------------
//  FrameConvert.h -- конвертация пиксельных форматов в BGR24.
//
//  Нужна бэкенду Media Foundation (Source Reader отдаёт RGB32 / YUY2 / NV12)
//  и для сохранения кадров в BMP без участия OpenCV.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <vector>

#include "CameraFrame.h"

namespace cam {

void rgb32_to_bgr(const uint8_t* src, int width, int height, int src_stride,
                  bool bottom_up, std::vector<uint8_t>& dst);

void rgb24_to_bgr(const uint8_t* src, int width, int height, int src_stride,
                  bool bottom_up, std::vector<uint8_t>& dst);

void yuy2_to_bgr(const uint8_t* src, int width, int height, int src_stride,
                 std::vector<uint8_t>& dst);

void nv12_to_bgr(const uint8_t* src, int width, int height, int src_stride,
                 std::vector<uint8_t>& dst);

void bgr_to_gray(const uint8_t* src, int width, int height, int stride,
                 std::vector<uint8_t>& dst);

// Запись 24-битного BMP вручную (без GDI+ и OpenCV) -- требование п.3.3.4
// методички: BITMAPFILEHEADER + BITMAPINFOHEADER + строки BGR снизу вверх.
bool write_bmp(const wchar_t* path, const uint8_t* bgr, int width, int height, int stride);

// Запись PPM (P6) -- простейший формат без сжатия, удобен для отладки.
bool write_ppm(const wchar_t* path, const uint8_t* bgr, int width, int height, int stride);

}  // namespace cam
