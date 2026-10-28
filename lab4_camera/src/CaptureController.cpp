#include "CaptureController.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <utility>

#include "Logger.h"

namespace cam {

const wchar_t* backend_name(Backend backend) {
    return backend == Backend::MediaFoundation ? L"Media Foundation" : L"OpenCV";
}

// ---------------------------------------------------------------------------
//  FrameQueue
// ---------------------------------------------------------------------------

void FrameQueue::push(const CameraFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    if (queue_.size() >= capacity_) {
        queue_.pop_front();
        ++dropped_;
    }
    queue_.push_back(frame);
    cv_.notify_one();
}

bool FrameQueue::pop(CameraFrame& out, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                      [this] { return !queue_.empty() || closed_; })) {
        return false;
    }
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void FrameQueue::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
}

void FrameQueue::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    cv_.notify_all();
}

void FrameQueue::open() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = false;
}

size_t FrameQueue::dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

// ---------------------------------------------------------------------------
//  CaptureController
// ---------------------------------------------------------------------------

CaptureController::CaptureController() = default;

CaptureController::~CaptureController() { disconnect(); }

bool CaptureController::is_connected() const {
    return state_ == CameraState::Connected;
}

CameraState CaptureController::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void CaptureController::set_state_callback(StateFn cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_cb_ = std::move(cb);
}

void CaptureController::set_frame_callback(FrameFn cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_cb_ = std::move(cb);
}

void CaptureController::set_record_queue(FrameQueue* queue) {
    record_queue_.store(queue);
}

DeviceInfo CaptureController::info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return info_;
}

VideoMode CaptureController::mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mode_;
}

double CaptureController::measured_fps() const { return measured_fps_.load(); }

unsigned long long CaptureController::frame_count() const { return frames_.load(); }

std::wstring CaptureController::backend() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return backend_;
}

bool CaptureController::get_latest(CameraFrame& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!latest_.valid()) return false;
    out = latest_;
    return true;
}

void CaptureController::set_state(CameraState state, const std::wstring& text) {
    StateFn cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = state;
        cb = state_cb_;
    }
    if (cb) cb(state, text);
}

void CaptureController::connect(const CaptureOptions& options) {
    disconnect();

    options_ = options;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = CameraState::Connecting;
        latest_ = CameraFrame{};
        info_ = DeviceInfo{};
        mode_ = VideoMode{};
        backend_ = backend_name(options.backend);
    }
    frames_.store(0);
    measured_fps_.store(0.0);
    fps_count_ = 0;
    fps_start_ = std::chrono::steady_clock::now();

    if (options.backend == Backend::MediaFoundation) {
        camera_.reset(create_mf_camera());
    } else {
        camera_.reset(create_opencv_camera());
    }

    // Камера сообщает об ошибках открытия через этот колбэк (вызывается из
    // потока захвата).
    camera_->set_state_callback([this](CameraState state, const std::wstring& text) {
        set_state(state, text);
    });

    running_.store(true);
    worker_ = std::thread(&CaptureController::run, this);
}

void CaptureController::disconnect() {
    if (!running_.exchange(false)) {
        if (worker_.joinable()) worker_.join();
        return;
    }
    // Захват блокируется на чтении кадра не дольше одного кадра, поэтому
    // join выполняется быстро. Если камера "зависла", поток завершится по
    // таймауту чтения драйвера.
    if (worker_.joinable()) worker_.join();

    camera_.reset();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = CameraFrame{};
        info_ = DeviceInfo{};
        mode_ = VideoMode{};
    }
    measured_fps_.store(0.0);
    set_state(CameraState::Disconnected, L"Захват остановлен");
}

void CaptureController::update_latest(CameraFrame&& frame,
                                      const std::chrono::steady_clock::time_point& now) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = std::move(frame);
    }

    ++fps_count_;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - fps_start_);
    if (elapsed.count() >= 1000) {
        measured_fps_.store(static_cast<double>(fps_count_) * 1000.0 /
                            static_cast<double>(elapsed.count()));
        fps_count_ = 0;
        fps_start_ = now;
    }
}

void CaptureController::run() {
    auto last_report = std::chrono::steady_clock::now();
    int consecutive_failures = 0;
    bool opened = false;

    while (running_.load()) {
        if (!opened) {
            const auto t0 = std::chrono::steady_clock::now();
            opened = camera_->open(options_.device_index, options_.width, options_.height,
                                   options_.fps);
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0);
            if (!opened) {
                set_state(CameraState::Error, L"Не удалось открыть камеру");
                Logger::instance().error(
                    format(L"Не удалось открыть камеру (индекс %d, бэкенд %s) за %lld мс",
                           options_.device_index, backend_name(options_.backend),
                           static_cast<long long>(spent.count())));
                if (!options_.auto_reconnect) break;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    state_ = CameraState::Error;
                }
                for (int i = 0; i < 20 && running_.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                info_ = camera_->device_info();
                mode_ = camera_->current_mode();
            }
            Logger::instance().info(
                format(L"Камера подключена: %s (%dx%d @ %.1f fps, формат %s, бэкенд %s)",
                       camera_->device_info().name.c_str(), camera_->current_mode().width,
                       camera_->current_mode().height, camera_->current_mode().fps,
                       pixel_format_name(camera_->current_mode().format),
                       camera_->backend_name().c_str()));
            set_state(CameraState::Connected, L"Камера подключена");
            consecutive_failures = 0;
        }

        CameraFrame frame;
        const auto now = std::chrono::steady_clock::now();
        if (camera_->grab(frame) && frame.valid()) {
            consecutive_failures = 0;
            ++frames_;
            update_latest(std::move(frame), now);

            if (auto* queue = record_queue_.load()) {
                CameraFrame copy;
                if (get_latest(copy)) queue->push(copy);
            }

            FrameFn frame_cb;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                frame_cb = frame_cb_;
            }
            if (frame_cb) frame_cb();

            // Обновляем сведения о режиме раз в секунду (драйвер может
            // изменить разрешение "на лету").
            if (now - last_report > std::chrono::seconds(1)) {
                last_report = now;
                std::lock_guard<std::mutex> lock(mutex_);
                mode_ = camera_->current_mode();
            }
        } else {
            ++consecutive_failures;
            if (consecutive_failures >= 20) {
                Logger::instance().warn(L"Камера перестала отдавать кадры -- переподключение");
                camera_->close();
                opened = false;
                set_state(CameraState::Disconnected, L"Камера отключена");
                if (!options_.auto_reconnect) break;
                for (int i = 0; i < 20 && running_.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
    }

    if (camera_) camera_->close();
}

}  // namespace cam
