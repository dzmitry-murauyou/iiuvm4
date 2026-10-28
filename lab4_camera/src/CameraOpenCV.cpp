#include "CameraOpenCV.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#  include <windows.h>
#  include <mfapi.h>
#  include <mfidl.h>
#endif

namespace cam {

namespace {

struct CvFourcc {
    PixelFormat format;
    const char* code;
};

// Порядок важен: MJPG даёт большее разрешение при том же USB-трафике,
// поэтому пробуем его первым.
const CvFourcc kFourccCandidates[] = {
    {PixelFormat::MJPG, "MJPG"},
    {PixelFormat::YUY2, "YUY2"},
    {PixelFormat::NV12, "NV12"},
};

std::string fourcc_to_string(double value) {
    int v = static_cast<int>(value);
    std::string s(4, '\0');
    s[0] = static_cast<char>(v & 0xFF);
    s[1] = static_cast<char>((v >> 8) & 0xFF);
    s[2] = static_cast<char>((v >> 16) & 0xFF);
    s[3] = static_cast<char>((v >> 24) & 0xFF);
    if (s[0] == '\0' || s[0] < 32) return "";
    return s;
}

int make_fourcc(const char* code) {
    return static_cast<int>(code[0]) | (static_cast<int>(code[1]) << 8) |
           (static_cast<int>(code[2]) << 16) | (static_cast<int>(code[3]) << 24);
}

std::wstring wide_from_ascii(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

}  // namespace

// ---------------------------------------------------------------------------

struct CameraOpenCV::Impl {
    cv::VideoCapture capture;
    mutable std::mutex mutex;
    DeviceInfo info;
    VideoMode mode;
    unsigned long long frames = 0;
    std::function<void(CameraState, const std::wstring&)> state_cb;
    std::wstring backend;

    void notify(CameraState state, const std::wstring& text) {
        std::function<void(CameraState, const std::wstring&)> cb;
        {
            std::lock_guard<std::mutex> lock(mutex);
            cb = state_cb;
        }
        if (cb) cb(state, text);
    }
};

CameraOpenCV::CameraOpenCV() : impl_(std::make_unique<Impl>()) {}

CameraOpenCV::~CameraOpenCV() { close(); }

std::wstring CameraOpenCV::backend_name() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->backend.empty() ? L"OpenCV" : impl_->backend;
}

void CameraOpenCV::set_state_callback(
    std::function<void(CameraState, const std::wstring&)> cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->state_cb = std::move(cb);
}

// ---------------------------------------------------------------------------
//  Перечисление устройств
// ---------------------------------------------------------------------------

std::vector<DeviceInfo> CameraOpenCV::enumerate_devices() {
    std::vector<DeviceInfo> devices;

    // Основной путь: перечисление через Media Foundation (IMFActivate).
    // Оно не открывает поток и потому безопасно при работающем захвате.
    std::vector<std::wstring> names = mf_friendly_names();
    for (size_t i = 0; i < names.size(); ++i) {
        DeviceInfo info;
        info.index = static_cast<int>(i);
        info.name = names[i];
        info.unique_id = L"cvdev:" + std::to_wstring(i);
        info.vendor = L"UVC / DirectShow";
        info.bus_info = L"индекс устройства " + std::to_wstring(i);
        info.reachable = true;
        devices.push_back(std::move(info));
    }
    if (!devices.empty()) return devices;

    // Резервный путь: перебор индексов OpenCV. Используется, если MF не
    // инициализировался; требует свободного устройства.
    try {
        for (int index = 0; index < 8; ++index) {
            cv::VideoCapture probe;
            bool ok = false;
            try {
                ok = probe.open(index, cv::CAP_DSHOW);
            } catch (const cv::Exception&) {
                ok = false;
            }
            if (!ok) {
                try {
                    ok = probe.open(index);
                } catch (const cv::Exception&) {
                    ok = false;
                }
            }
            if (!ok) {
                probe.release();
                continue;
            }

            DeviceInfo info;
            info.index = static_cast<int>(devices.size());
            info.name = L"Камера #" + std::to_wstring(index);
            info.unique_id = L"cvdev:" + std::to_wstring(index);
            info.vendor = L"неизвестен";
            info.bus_info = L"индекс OpenCV " + std::to_wstring(index);
            info.reachable = true;
            devices.push_back(std::move(info));
            probe.release();
        }
    } catch (const cv::Exception&) {
        // перечисление не удалось -- вернём то, что уже собрали
    }
    return devices;
}

// ---------------------------------------------------------------------------
//  Открытие устройства
// ---------------------------------------------------------------------------

bool CameraOpenCV::open(int device_index, int width, int height, double fps) {
    close();
    impl_->notify(CameraState::Connecting, L"Открытие устройства " +
                                               std::to_wstring(device_index));

    cv::VideoCapture cap;
    bool opened = false;
    std::string used_backend = "OpenCV/unknown";

#ifdef _WIN32
    // DirectShow в OpenCV работает стабильнее для UVC-камер и позволяет
    // менять формат (MJPG/YUY2) через CAP_PROP_FOURCC.
    try {
        opened = cap.open(device_index, cv::CAP_DSHOW);
        if (opened) used_backend = "OpenCV/DirectShow";
    } catch (const cv::Exception&) {
        opened = false;
    }
#endif
    if (!opened) {
        try {
            opened = cap.open(device_index);
            if (opened) used_backend = "OpenCV/" + cap.getBackendName();
        } catch (const cv::Exception&) {
            opened = false;
        }
    }

    if (!opened || !cap.isOpened()) {
        impl_->notify(CameraState::Error,
                      L"Не удалось открыть камеру #" + std::to_wstring(device_index) +
                          L" (устройство занято или отсутствует)");
        return false;
    }

    // 1) Формат пикселей: MJPG предпочтительнее -- камера отдаёт сжатый поток,
    //    а разрешение ограничено только USB-каналом, а не пропускной способностью
    //    несжатого YUY2.
    for (const auto& cand : kFourccCandidates) {
        if (!cap.set(cv::CAP_PROP_FOURCC, make_fourcc(cand.code))) continue;
        // Проверяем, что драйвер действительно применил формат.
        std::string applied = fourcc_to_string(cap.get(cv::CAP_PROP_FOURCC));
        if (applied == cand.code) {
            impl_->mode.format = cand.format;
            impl_->mode.fourcc = wide_from_ascii(cand.code);
            break;
        }
    }

    // 2) Разрешение и частота кадров.
    if (width > 0 && height > 0) {
        cap.set(cv::CAP_PROP_FRAME_WIDTH, width);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, height);
    }
    if (fps > 0.0) cap.set(cv::CAP_PROP_FPS, fps);

    double applied_w = cap.get(cv::CAP_PROP_FRAME_WIDTH);
    double applied_h = cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    double applied_fps = cap.get(cv::CAP_PROP_FPS);

    // 3) Некоторые драйверы игнорируют S_FMT до первого кадра -- читаем один
    //    кадр, чтобы формат применился и мы узнали истинное разрешение.
    cv::Mat first;
    cap.read(first);
    if (!first.empty()) {
        applied_w = first.cols;
        applied_h = first.rows;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->capture = std::move(cap);
        impl_->backend = wide_from_ascii(used_backend);
        impl_->frames = 0;

        impl_->mode.width = static_cast<int>(applied_w);
        impl_->mode.height = static_cast<int>(applied_h);
        impl_->mode.fps = (applied_fps > 0.0 && applied_fps < 1000.0) ? applied_fps : 0.0;
        if (impl_->mode.format == PixelFormat::Unknown) {
            impl_->mode.format = PixelFormat::BGR24;
        }
        impl_->info.index = device_index;
        if (impl_->info.name.empty()) {
            impl_->info.name = L"Камера #" + std::to_wstring(device_index);
        }
        if (impl_->info.vendor.empty()) impl_->info.vendor = L"UVC / DirectShow";
        impl_->info.reachable = true;
    }

    impl_->notify(CameraState::Connected,
                  L"Камера открыта: " + impl_->info.name + L", " +
                      std::to_wstring(impl_->mode.width) + L"x" +
                      std::to_wstring(impl_->mode.height) + L", " +
                      impl_->mode.fourcc);
    return true;
}

void CameraOpenCV::close() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->capture.isOpened()) impl_->capture.release();
}

bool CameraOpenCV::is_open() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->capture.isOpened();
}

bool CameraOpenCV::grab(CameraFrame& out) {
    cv::Mat frame;
    {
        // Блокирующее чтение кадра -- должно происходить без удержания мьютекса,
        // иначе GUI не сможет получить текущий режим во время ожидания.
        cv::VideoCapture* cap;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (!impl_->capture.isOpened()) return false;
            cap = &impl_->capture;
        }
        try {
            *cap >> frame;
        } catch (const cv::Exception&) {
            return false;
        }
    }
    if (frame.empty()) return false;

    if (frame.type() != CV_8UC3) {
        if (frame.channels() == 1) {
            cv::cvtColor(frame, frame, cv::COLOR_GRAY2BGR);
        } else if (frame.channels() == 4) {
            cv::cvtColor(frame, frame, cv::COLOR_BGRA2BGR);
        } else {
            return false;
        }
    }

    out.width = frame.cols;
    out.height = frame.rows;
    out.stride = frame.cols * 3;
    out.pixels.resize(static_cast<size_t>(out.stride) * static_cast<size_t>(out.height));

    // OpenCV отдаёт BGR, строки могут быть выровнены (step >= width*3),
    // поэтому копируем построчно, а не одним memcpy.
    for (int y = 0; y < frame.rows; ++y) {
        std::memcpy(out.pixels.data() + static_cast<size_t>(y) * out.stride,
                    frame.ptr(y), static_cast<size_t>(out.stride));
    }
    out.jpeg.clear();

    SYSTEMTIME st;
    GetLocalTime(&st);
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER ticks;
    ticks.LowPart = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;
    out.time_ns = static_cast<long long>(ticks.QuadPart);

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        ++impl_->frames;
        impl_->mode.width = out.width;
        impl_->mode.height = out.height;
    }
    return true;
}

DeviceInfo CameraOpenCV::device_info() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->info;
}

VideoMode CameraOpenCV::current_mode() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->mode;
}

StreamStats CameraOpenCV::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    StreamStats s;
    s.frames = impl_->frames;
    return s;
}

ICamera* create_opencv_camera() { return new CameraOpenCV(); }

}  // namespace cam
