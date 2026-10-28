// ---------------------------------------------------------------------------
//  CameraFrame.h -- нейтральный к платформе контейнер кадра.
//
//  * pixels -- 24-битный BGR, построчно (BGRBGR...), как в DIB/GDI и OpenCV;
//  * jpeg   -- сжатый кадр, если он есть (Source Reader может отдавать MJPEG).
//
//  Формат BGR выбран специально: он одинаково понимается GDI (BitBlt по
//  DIB-секции с 24 битами на пиксель) и OpenCV (cv::imwrite / cv::VideoWriter
//  с параметром isColor=true).
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cam {

struct CameraFrame {
    int width = 0;
    int height = 0;
    int stride = 0;                    // байт в строке (width * 3)
    std::vector<uint8_t> pixels;       // BGR, top-down
    std::vector<uint8_t> jpeg;         // MJPEG-кадр как есть (может быть пустым)
    long long time_ns = 0;             // время получения, 100 нс от эпохи
    bool valid() const { return width > 0 && height > 0 && !pixels.empty(); }
    size_t size_bytes() const { return pixels.size(); }
};

// Источник кадров (реальная камера, файл, "нет сигнала").
class ICameraSource {
public:
    virtual ~ICameraSource() = default;
    // Возвращает true, если кадр получен. Реализация должна быть блокирующей
    // не дольше ~1 c и корректно реагировать на request_stop().
    virtual bool grab(CameraFrame& out) = 0;
    virtual void request_stop() = 0;
};

}  // namespace cam
