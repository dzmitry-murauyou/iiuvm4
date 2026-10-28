// ---------------------------------------------------------------------------
//  DeviceMonitor.h -- контроль состояния камеры: подключена / отключена.
//
//  Два независимых механизма (как в п.3.4 методички):
//   1) окно регистрируется на WM_DEVICECHANGE через RegisterDeviceNotification
//      (GUID_DEVINTERFACE_IMAGE) и при событии просит монитор о внеочередном
//      опросе: refresh_now();
//   2) фоновый поток раз в N секунд перечисляет устройства, сравнивает список
//      с предыдущим и сообщает о добавлении/удалении камеры.
//  Перечисление выполняется через Media Foundation и НЕ открывает поток,
//  поэтому не мешает активному захвату.
// ---------------------------------------------------------------------------
#pragma once

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Camera.h"

namespace cam {

class DeviceMonitor {
public:
    // devices -- актуальный список, message -- что изменилось (для журнала)
    using Callback = std::function<void(const std::vector<DeviceInfo>& devices,
                                        const std::wstring& message)>;
    using Enumerator = std::function<std::vector<DeviceInfo>()>;

    DeviceMonitor() = default;
    ~DeviceMonitor();

    void start(Enumerator enumerator, Callback callback, int interval_ms = 3000);
    void stop();

    // Внеочередной опрос (вызывается из обработчика WM_DEVICECHANGE).
    void refresh_now();

    std::vector<DeviceInfo> devices() const;

    // Регистрация окна на уведомления об изменении устройств.
    static HDEVNOTIFY register_notifications(HWND hwnd);
    static void unregister_notifications(HDEVNOTIFY handle);

private:
    void run();

    Enumerator enumerator_;
    Callback callback_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> refresh_{false};
    int interval_ms_ = 3000;
    HANDLE wake_event_ = nullptr;

    mutable std::mutex mutex_;
    std::vector<DeviceInfo> devices_;
};

// Сравнение списков устройств по уникальным идентификаторам.
std::wstring describe_device_changes(const std::vector<DeviceInfo>& before,
                                     const std::vector<DeviceInfo>& after);

}  // namespace cam
