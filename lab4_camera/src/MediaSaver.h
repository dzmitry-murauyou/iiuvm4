// ---------------------------------------------------------------------------
//  MediaSaver.h -- сохранение материалов наблюдения.
//
//  * снимок кадра  -> JPEG (OpenCV imwrite, качество настраивается);
//                     резервный путь -- ручная запись BMP (BITMAPFILEHEADER);
//  * видеозапись   -> AVI/MJPEG через cv::VideoWriter в отдельном потоке,
//                     кадры поступают из кольцевого буфера CaptureController.
// ---------------------------------------------------------------------------
#pragma once

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "CameraFrame.h"
#include "CaptureController.h"

namespace cam {

// Сохранение одного кадра в JPEG. При ошибке OpenCV выполняется резервная
// запись BMP (в этом случае actual_path получит расширение .bmp).
bool save_snapshot(const std::wstring& path, const CameraFrame& frame, int quality,
                   std::wstring* error, std::wstring* actual_path = nullptr);

// Имя файла вида snapshot_YYYYMMDD_HHMMSS.jpg с защитой от совпадений
// (если снимок сделан дважды в одну секунду -- добавляется _1, _2 ...).
std::wstring make_snapshot_path(const std::wstring& dir, const SYSTEMTIME& st,
                                const wchar_t* prefix = L"snapshot",
                                const wchar_t* ext = L"jpg");

std::wstring make_video_path(const std::wstring& dir, const SYSTEMTIME& st,
                             const wchar_t* prefix = L"video");

// ---------------------------------------------------------------------------
//  Запись видео
// ---------------------------------------------------------------------------
struct RecordResult {
    std::wstring path;
    unsigned long long frames = 0;
    unsigned long long dropped = 0;
    double seconds = 0.0;
    bool ok = false;
    std::wstring error;
};

class VideoRecorder {
public:
    using StopFn = std::function<void(const RecordResult&)>;

    VideoRecorder();
    ~VideoRecorder();

    // Запуск записи: кадры берутся из queue (владелец -- приложение).
    bool start(FrameQueue* queue, const std::wstring& path, int width, int height,
               double fps, int jpeg_quality, std::wstring* error);

    void stop();                       // асинхронная остановка
    bool is_recording() const { return recording_.load(); }
    std::wstring path() const;

    // Колбэк вызывается из потока записи после остановки.
    void set_stop_callback(StopFn cb);

private:
    void run();

    std::thread worker_;
    std::atomic<bool> recording_{false};
    std::atomic<bool> stop_requested_{false};

    mutable std::mutex mutex_;
    std::wstring path_;
    RecordResult result_;
    StopFn stop_cb_;

    struct Impl;
    std::unique_ptr<Impl> impl_;

    FrameQueue* queue_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    double fps_ = 25.0;
    int quality_ = 90;
};

}  // namespace cam
