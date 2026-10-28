#include "FrameConvert.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace cam {

namespace {

inline void store_bgr(uint8_t* dst, int r, int g, int b) {
    dst[0] = static_cast<uint8_t>(b);
    dst[1] = static_cast<uint8_t>(g);
    dst[2] = static_cast<uint8_t>(r);
}

inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

}  // namespace

void rgb32_to_bgr(const uint8_t* src, int width, int height, int src_stride,
                  bool bottom_up, std::vector<uint8_t>& dst) {
    const int stride = width * 3;
    dst.resize(static_cast<size_t>(stride) * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
        const int src_y = bottom_up ? (height - 1 - y) : y;
        const uint8_t* row = src + static_cast<size_t>(src_y) * static_cast<size_t>(src_stride);
        uint8_t* out = dst.data() + static_cast<size_t>(y) * static_cast<size_t>(stride);
        for (int x = 0; x < width; ++x) {
            // Память RGB32: B, G, R, X
            out[x * 3 + 0] = row[x * 4 + 0];
            out[x * 3 + 1] = row[x * 4 + 1];
            out[x * 3 + 2] = row[x * 4 + 2];
        }
    }
}

void rgb24_to_bgr(const uint8_t* src, int width, int height, int src_stride,
                  bool bottom_up, std::vector<uint8_t>& dst) {
    const int stride = width * 3;
    dst.resize(static_cast<size_t>(stride) * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
        const int src_y = bottom_up ? (height - 1 - y) : y;
        const uint8_t* row = src + static_cast<size_t>(src_y) * static_cast<size_t>(src_stride);
        uint8_t* out = dst.data() + static_cast<size_t>(y) * static_cast<size_t>(stride);
        for (int x = 0; x < width; ++x) {
            out[x * 3 + 0] = row[x * 3 + 2];  // память RGB24: R,G,B -> BGR
            out[x * 3 + 1] = row[x * 3 + 1];
            out[x * 3 + 2] = row[x * 3 + 0];
        }
    }
}

void yuy2_to_bgr(const uint8_t* src, int width, int height, int src_stride,
                 std::vector<uint8_t>& dst) {
    const int stride = width * 3;
    dst.resize(static_cast<size_t>(stride) * static_cast<size_t>(height));

    for (int y = 0; y < height; ++y) {
        const uint8_t* row = src + static_cast<size_t>(y) * static_cast<size_t>(src_stride);
        uint8_t* out = dst.data() + static_cast<size_t>(y) * static_cast<size_t>(stride);
        for (int x = 0; x + 1 < width; x += 2) {
            const int y0 = row[0];
            const int u = row[1] - 128;
            const int y1 = row[2];
            const int v = row[3] - 128;
            row += 4;

            // ITU-R BT.601, ограниченный диапазон (как в UVC/драйверах)
            const int c0 = y0 - 16;
            const int c1 = y1 - 16;
            const int u1 = 74 * u;   // 0.5 * 255/128 * 256 ~= 74
            const int v1 = 102 * v;  // 0.718 * 255/128 * 256 ~= 102

            store_bgr(out + x * 3, clamp255((298 * c0 + v1) >> 8),
                      clamp255((298 * c0 - u1 - v1) >> 8),
                      clamp255((298 * c0 + u1) >> 8));
            store_bgr(out + (x + 1) * 3, clamp255((298 * c1 + v1) >> 8),
                      clamp255((298 * c1 - u1 - v1) >> 8),
                      clamp255((298 * c1 + u1) >> 8));
        }
    }
}

void nv12_to_bgr(const uint8_t* src, int width, int height, int src_stride,
                 std::vector<uint8_t>& dst) {
    const int stride = width * 3;
    dst.resize(static_cast<size_t>(stride) * static_cast<size_t>(height));
    const uint8_t* y_plane = src;
    const uint8_t* uv_plane = src + static_cast<size_t>(src_stride) * static_cast<size_t>(height);

    for (int y = 0; y < height; ++y) {
        const uint8_t* yrow = y_plane + static_cast<size_t>(y) * static_cast<size_t>(src_stride);
        const uint8_t* uvrow = uv_plane + static_cast<size_t>(y / 2) * static_cast<size_t>(src_stride);
        uint8_t* out = dst.data() + static_cast<size_t>(y) * static_cast<size_t>(stride);
        for (int x = 0; x + 1 < width; x += 2) {
            const int u = uvrow[x] - 128;
            const int v = uvrow[x + 1] - 128;
            const int u1 = 74 * u;
            const int v1 = 102 * v;
            const int c0 = yrow[x] - 16;
            const int c1 = yrow[x + 1] - 16;
            store_bgr(out + x * 3, clamp255((298 * c0 + v1) >> 8),
                      clamp255((298 * c0 - u1 - v1) >> 8),
                      clamp255((298 * c0 + u1) >> 8));
            store_bgr(out + (x + 1) * 3, clamp255((298 * c1 + v1) >> 8),
                      clamp255((298 * c1 - u1 - v1) >> 8),
                      clamp255((298 * c1 + u1) >> 8));
        }
    }
}

void bgr_to_gray(const uint8_t* src, int width, int height, int stride,
                 std::vector<uint8_t>& dst) {
    dst.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = src + static_cast<size_t>(y) * static_cast<size_t>(stride);
        uint8_t* out = dst.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
        for (int x = 0; x < width; ++x) {
            const int b = row[x * 3 + 0];
            const int g = row[x * 3 + 1];
            const int r = row[x * 3 + 2];
            out[x] = static_cast<uint8_t>((29 * b + 150 * g + 77 * r) >> 8);
        }
    }
}

// ---------------------------------------------------------------------------
//  BMP / PPM
// ---------------------------------------------------------------------------

bool write_bmp(const wchar_t* path, const uint8_t* bgr, int width, int height, int stride) {
    if (!path || !bgr || width <= 0 || height <= 0) return false;

    // Строка BMP выравнивается по границе 4 байт.
    const int row_bytes = width * 3;
    const int padding = (4 - (row_bytes % 4)) % 4;
    const int padded_row = row_bytes + padding;
    const DWORD pixel_bytes = static_cast<DWORD>(padded_row) * static_cast<DWORD>(height);

    BITMAPFILEHEADER file_header{};
    BITMAPINFOHEADER info_header{};

    info_header.biSize = sizeof(BITMAPINFOHEADER);
    info_header.biWidth = width;
    info_header.biHeight = height;          // положительная высота -> bottom-up
    info_header.biPlanes = 1;
    info_header.biBitCount = 24;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = pixel_bytes;

    file_header.bfType = 0x4D42;            // "BM"
    file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file_header.bfSize = file_header.bfOffBits + pixel_bytes;

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    bool ok = WriteFile(h, &file_header, sizeof(file_header), &written, nullptr) != FALSE &&
              WriteFile(h, &info_header, sizeof(info_header), &written, nullptr) != FALSE;

    std::vector<uint8_t> pad(static_cast<size_t>(padding), 0);
    for (int y = height - 1; ok && y >= 0; --y) {
        const uint8_t* row = bgr + static_cast<size_t>(y) * static_cast<size_t>(stride);
        ok = WriteFile(h, row, static_cast<DWORD>(row_bytes), &written, nullptr) != FALSE;
        if (ok && padding > 0) {
            ok = WriteFile(h, pad.data(), static_cast<DWORD>(padding), &written, nullptr) != FALSE;
        }
    }
    CloseHandle(h);
    return ok;
}

bool write_ppm(const wchar_t* path, const uint8_t* bgr, int width, int height, int stride) {
    if (!path || !bgr || width <= 0 || height <= 0) return false;

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    char header[64];
    const int n = wsprintfA(header, "P6\n%d %d\n255\n", width, height);

    DWORD written = 0;
    bool ok = WriteFile(h, header, static_cast<DWORD>(n), &written, nullptr) != FALSE;

    std::vector<uint8_t> rgb(static_cast<size_t>(width) * 3);
    for (int y = 0; ok && y < height; ++y) {
        const uint8_t* row = bgr + static_cast<size_t>(y) * static_cast<size_t>(stride);
        for (int x = 0; x < width; ++x) {
            rgb[x * 3 + 0] = row[x * 3 + 2];
            rgb[x * 3 + 1] = row[x * 3 + 1];
            rgb[x * 3 + 2] = row[x * 3 + 0];
        }
        ok = WriteFile(h, rgb.data(), static_cast<DWORD>(rgb.size()), &written, nullptr) != FALSE;
    }
    CloseHandle(h);
    return ok;
}

}  // namespace cam
