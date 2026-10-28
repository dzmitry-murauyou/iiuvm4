// ---------------------------------------------------------------------------
//  CameraOpenCV.h -- бэкенд видео захвата на OpenCV (cv::VideoCapture).
//
//  Открытие устройства и чтение кадров выполняются в фоновом потоке
//  CaptureController, поэтому долгие операции (инициализация DirectShow
//  бэкенда OpenCV занимает 0.5-1.5 c) не блокируют GUI.
// ---------------------------------------------------------------------------
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Camera.h"

namespace cam {

// Имена камер, полученные перечислением устройств Media Foundation.
// Нужны для "человеческого" названия в панели информации: сам
// cv::VideoCapture имени устройства на Windows не отдаёт.
// Возвращает пустой вектор, если MF недоступен.
std::vector<std::wstring> mf_friendly_names();

class CameraOpenCV final : public ICamera {
public:
    CameraOpenCV();
    ~CameraOpenCV() override;

    std::vector<DeviceInfo> enumerate_devices() override;

    bool open(int device_index, int width, int height, double fps) override;
    void close() override;
    bool is_open() const override;
    bool grab(CameraFrame& out) override;

    DeviceInfo device_info() const override;
    VideoMode current_mode() const override;
    StreamStats stats() const override;
    std::wstring backend_name() const override;

    void set_state_callback(std::function<void(CameraState, const std::wstring&)> cb) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cam
