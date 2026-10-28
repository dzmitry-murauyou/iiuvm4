#include "MediaSaver.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>

#include <iterator>
#include <chrono>
#include <memory>

#include "FrameConvert.h"
#include "Logger.h"

namespace cam {

namespace {

// Путь для OpenCV: сначала UTF-8, при неудаче -- системная ANSI-кодировка
// (нужно для каталогов с кириллицей на дисках, где UTF-8 не поддерживается).
std::string path_for_opencv(const std::wstring& path) {
    return to_utf8(path);
}

std::string path_for_opencv_ansi(const std::wstring& path) {
    int size = WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1, out.data(), size, nullptr, nullptr);
    return out;
}

cv::Mat wrap_frame(const CameraFrame& frame) {
    return cv::Mat(frame.height, frame.width, CV_8UC3,
                   const_cast<uint8_t*>(frame.pixels.data()), static_cast<size_t>(frame.stride));
}

// Гарантируем чётные размеры: кодировщик MJPG/AVI требует кратности 2.
cv::Size even_size(int width, int height) {
    int w = width & ~1;
    int h = height & ~1;
    if (w <= 0) w = 2;
    if (h <= 0) h = 2;
    return cv::Size(w, h);
}

}  // namespace

// ---------------------------------------------------------------------------
//  Снимки
// ---------------------------------------------------------------------------

bool save_snapshot(const std::wstring& path, const CameraFrame& frame, int quality,
                   std::wstring* error, std::wstring* actual_path) {
    if (actual_path) *actual_path = path;
    if (!frame.valid()) {
        if (error) *error = L"кадр пуст";
        return false;
    }

    cv::Mat mat = wrap_frame(frame);
    std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, std::clamp(quality, 1, 100)};

    bool ok = false;
    try {
        ok = cv::imwrite(path_for_opencv(path), mat, params);
        if (!ok) ok = cv::imwrite(path_for_opencv_ansi(path), mat, params);
    } catch (const cv::Exception& e) {
        if (error) *error = from_utf8(e.what());
        ok = false;
    }

    if (ok && file_exists(path)) return true;

    // Резервный путь: ручная запись BMP без OpenCV (п.3.3.4 методички).
    std::wstring bmp_path = path;
    size_t dot = bmp_path.find_last_of(L'.');
    if (dot != std::wstring::npos) bmp_path = bmp_path.substr(0, dot);
    bmp_path += L".bmp";

    if (write_bmp(bmp_path.c_str(), frame.pixels.data(), frame.width, frame.height,
                  frame.stride)) {
        if (actual_path) *actual_path = bmp_path;
        if (error) *error = L"JPEG-кодировщик недоступен, сохранён BMP";
        return true;
    }

    if (error && error->empty()) *error = L"не удалось сохранить изображение";
    return false;
}

std::wstring make_snapshot_path(const std::wstring& dir, const SYSTEMTIME& st,
                                const wchar_t* prefix, const wchar_t* ext) {
    std::wstring base = join_path(dir, std::wstring(prefix) + L"_" + file_timestamp(st));
    std::wstring candidate = base + L"." + ext;
    int counter = 1;
    // Два снимка в одну секунду -- добавляем суффикс.
    while (file_exists(candidate)) {
        candidate = base + L"_" + std::to_wstring(counter++) + L"." + ext;
    }
    return candidate;
}

std::wstring make_video_path(const std::wstring& dir, const SYSTEMTIME& st,
                             const wchar_t* prefix) {
    std::wstring base = join_path(dir, std::wstring(prefix) + L"_" + file_timestamp(st));
    std::wstring candidate = base + L".avi";
    int counter = 1;
    while (file_exists(candidate)) {
        candidate = base + L"_" + std::to_wstring(counter++) + L".avi";
    }
    return candidate;
}

// ---------------------------------------------------------------------------
//  VideoRecorder
// ---------------------------------------------------------------------------

struct VideoRecorder::Impl {
    cv::VideoWriter writer;
};

VideoRecorder::VideoRecorder() = default;

VideoRecorder::~VideoRecorder() { stop(); }

std::wstring VideoRecorder::path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
}

void VideoRecorder::set_stop_callback(StopFn cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_cb_ = std::move(cb);
}

bool VideoRecorder::start(FrameQueue* queue, const std::wstring& path, int width, int height,
                          double fps, int jpeg_quality, std::wstring* error) {
    if (recording_.load()) {
        if (error) *error = L"запись уже идёт";
        return false;
    }
    if (!queue) {
        if (error) *error = L"очередь кадров не задана";
        return false;
    }

    if (!impl_) impl_ = std::make_unique<Impl>();

    const cv::Size size = even_size(width, height);
    const double out_fps = (fps > 1.0 && fps < 240.0) ? fps : 25.0;

    bool opened = false;
    try {
        opened = impl_->writer.open(path_for_opencv(path),
                                    cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), out_fps, size);
        if (!opened) {
            opened = impl_->writer.open(path_for_opencv_ansi(path),
                                        cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), out_fps, size);
        }
    } catch (const cv::Exception& e) {
        if (error) *error = from_utf8(e.what());
        opened = false;
    }

    if (!opened) {
        if (error && error->empty()) *error = L"cv::VideoWriter не открыл файл";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        path_ = path;
        result_ = RecordResult{};
        result_.path = path;
        result_.ok = true;
        queue_ = queue;
        width_ = size.width;
        height_ = size.height;
        fps_ = out_fps;
        quality_ = jpeg_quality;
    }

    queue->open();
    queue->clear();
    stop_requested_.store(false);
    recording_.store(true);
    worker_ = std::thread(&VideoRecorder::run, this);
    return true;
}

void VideoRecorder::stop() {
    if (!recording_.load()) {
        if (worker_.joinable()) worker_.join();
        return;
    }
    stop_requested_.store(true);
    if (worker_.joinable()) worker_.join();

    if (impl_ && impl_->writer.isOpened()) impl_->writer.release();

    RecordResult result;
    StopFn cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result = result_;
        cb = stop_cb_;
    }
    recording_.store(false);
    if (impl_) impl_->writer.release();

    if (cb) cb(result);
}

void VideoRecorder::run() {
    cv::VideoWriter* writer = impl_ ? &impl_->writer : nullptr;
    if (!writer) return;

    unsigned long long frames = 0;
    unsigned long long dropped = 0;
    const auto started = std::chrono::steady_clock::now();

    while (!stop_requested_.load()) {
        CameraFrame frame;
        if (!queue_->pop(frame, 300)) {
            if (stop_requested_.load()) break;
            continue;
        }
        if (!frame.valid()) continue;

        try {
            cv::Mat mat = wrap_frame(frame);
            if (mat.cols != width_ || mat.rows != height_) {
                cv::Mat resized;
                cv::resize(mat, resized, cv::Size(width_, height_), 0, 0, cv::INTER_LINEAR);
                mat = resized;
            }
            writer->write(mat);
            ++frames;
        } catch (const cv::Exception& e) {
            ++dropped;
            Logger::instance().error(L"Ошибка записи кадра: " + from_utf8(e.what()));
        }
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    std::lock_guard<std::mutex> lock(mutex_);
    result_.frames = frames;
    result_.dropped = dropped;
    result_.seconds = static_cast<double>(elapsed.count()) / 1000.0;
}

}  // namespace cam
