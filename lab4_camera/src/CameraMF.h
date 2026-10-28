// ---------------------------------------------------------------------------
//  CameraMF.h -- бэкенд видео захвата на Media Foundation (Source Reader).
//
//  Это "низкоуровневый" вариант, который прямо соответствует разделу 3.3
//  методички: MFEnumDeviceSources -> IMFSourceReader -> ReadSample.
//  Приложение использует его, если в настройках выбран бэкенд Media Foundation.
// ---------------------------------------------------------------------------
#pragma once

#include <memory>
#include <vector>

#include "Camera.h"

namespace cam {

class CameraMF final : public ICamera {
public:
    CameraMF();
    ~CameraMF() override;

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

ICamera* create_mf_camera();

}  // namespace cam
