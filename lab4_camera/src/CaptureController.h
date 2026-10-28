// ---------------------------------------------------------------------------
//  CaptureController.h -- поток захвата, кольцевой буфер кадров и статистика.
//
//  Архитектура (см. рис. 3.9 методички):
//      поток захвата (V4L2/DirectShow/OpenCV) -> последний кадр (для GUI)
//                                             -> кольцевой буфер -> рекордер
//  GUI никогда не вызывает функции камеры напрямую: он забирает копию
//  последнего кадра и получает уведомления о состоянии.
// ---------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "Camera.h"

namespace cam {

enum class Backend { OpenCV, MediaFoundation };

const wchar_t* backend_name(Backend backend);

struct CaptureOptions {
    Backend backend = Backend::OpenCV;
    int device_index = 0;
    int width = 1280;
    int height = 720;
    double fps = 30.0;
    bool auto_reconnect = true;
};

// Ограниченная очередь кадров: при переполнении старый кадр отбрасывается,
// чтобы задержка записи не росла неограниченно.
class FrameQueue {
public:
    explicit FrameQueue(size_t capacity = 16) : capacity_(capacity) {}

    void push(const CameraFrame& frame);
    bool pop(CameraFrame& out, int timeout_ms);
    void clear();
    void close();
    void open();
    size_t dropped() const;

private:
    size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<CameraFrame> queue_;
    size_t dropped_ = 0;
    bool closed_ = false;
};

class CaptureController {
public:
    using StateFn = std::function<void(CameraState, const std::wstring&)>;
    using FrameFn = std::function<void()>;

    CaptureController();
    ~CaptureController();

    void connect(const CaptureOptions& options);
    void disconnect();
    bool is_connected() const;
    CameraState state() const;

    bool get_latest(CameraFrame& out) const;
    DeviceInfo info() const;
    VideoMode mode() const;
    double measured_fps() const;
    unsigned long long frame_count() const;
    std::wstring backend() const;

    void set_state_callback(StateFn cb);
    void set_frame_callback(FrameFn cb);
    // Куда складывать кадры для записи видео (nullptr -- не записываем).
    void set_record_queue(FrameQueue* queue);

private:
    void run();
    void set_state(CameraState state, const std::wstring& text);
    void update_latest(CameraFrame&& frame, const std::chrono::steady_clock::time_point& now);

    std::unique_ptr<ICamera> camera_;
    std::thread worker_;
    std::atomic<bool> running_{false};

    mutable std::mutex mutex_;
    CameraFrame latest_;
    CameraState state_ = CameraState::Disconnected;
    DeviceInfo info_;
    VideoMode mode_;
    std::wstring backend_;
    CaptureOptions options_;

    std::atomic<double> measured_fps_{0.0};
    std::atomic<unsigned long long> frames_{0};
    std::atomic<FrameQueue*> record_queue_{nullptr};

    StateFn state_cb_;
    FrameFn frame_cb_;

    // Для расчёта FPS
    unsigned long long fps_count_ = 0;
    std::chrono::steady_clock::time_point fps_start_;
};

}  // namespace cam
