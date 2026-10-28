// ---------------------------------------------------------------------------
//  Camera.h -- абстракция веб-камеры и описание её возможностей.
//
//  Реализации:
//    CameraOpenCV  -- универсальный бэкенд на cv::VideoCapture (OpenCV);
//    CameraMF      -- бэкенд на Media Foundation (Source Reader),
//                     оставлен как "низкоуровневый" вариант из методички.
//
//  Приложение работает только через этот интерфейс, поэтому бэкенд можно
//  переключить без изменения GUI и логики сохранения файлов.
// ---------------------------------------------------------------------------
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "CameraFrame.h"

namespace cam {

// Формат пикселей, который отдаёт камера (для информационной панели).
enum class PixelFormat { Unknown, MJPG, YUY2, NV12, RGB24, BGR24 };

const wchar_t* pixel_format_name(PixelFormat fmt);

struct VideoMode {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    PixelFormat format = PixelFormat::Unknown;
    std::wstring fourcc;      // для бэкенда OpenCV: "MJPG", "YUY2" ...
};

struct DeviceInfo {
    int index = -1;
    std::wstring name;           // понятное имя устройства
    std::wstring unique_id;      // стабильный идентификатор (символьный/путь)
    std::wstring vendor;         // производитель (если удалось узнать)
    std::wstring bus_info;       // шина / путь устройства
    std::vector<VideoMode> modes;
    bool reachable = false;      // удалось открыть прямо сейчас
};

struct StreamStats {
    double measured_fps = 0.0;
    unsigned long long frames = 0;
    unsigned long long dropped = 0;
};

// Состояние камеры -- для индикатора в GUI и для журнала.
enum class CameraState { Disconnected, Connecting, Connected, Error };

const wchar_t* camera_state_name(CameraState state);

class ICamera {
public:
    virtual ~ICamera() = default;

    // --- перечисление устройств (обнаружение) ------------------------------
    virtual std::vector<DeviceInfo> enumerate_devices() = 0;

    // --- управление --------------------------------------------------------
    virtual bool open(int device_index, int width, int height, double fps) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;
    virtual bool grab(CameraFrame& out) = 0;

    // --- информация --------------------------------------------------------
    virtual DeviceInfo device_info() const = 0;
    virtual VideoMode current_mode() const = 0;
    virtual StreamStats stats() const = 0;
    virtual std::wstring backend_name() const = 0;

    // Подписка на изменение состояния (вызывается из потока захвата!).
    virtual void set_state_callback(std::function<void(CameraState, const std::wstring&)> cb) = 0;
};

// Фабрики бэкендов.
ICamera* create_opencv_camera();
ICamera* create_mf_camera();

}  // namespace cam
