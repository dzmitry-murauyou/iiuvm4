#include "DeviceMonitor.h"

#include <dbt.h>
#include <initguid.h>

#include <algorithm>
#include <set>
#include <utility>

namespace cam {

namespace {

// {E5323777-F976-4F5B-9B55-B94699C46E44}
DEFINE_GUID(GUID_DEVINTERFACE_IMAGE_LOCAL, 0xE5323777, 0xF976, 0x4F5B, 0x9B, 0x55,
            0xB9, 0x46, 0x99, 0xC4, 0x6E, 0x44);

}  // namespace

std::wstring describe_device_changes(const std::vector<DeviceInfo>& before,
                                     const std::vector<DeviceInfo>& after) {
    std::set<std::wstring> old_ids;
    std::set<std::wstring> new_ids;
    for (const auto& d : before) old_ids.insert(d.unique_id);
    for (const auto& d : after) new_ids.insert(d.unique_id);

    std::wstring added;
    std::wstring removed;
    for (const auto& d : after) {
        if (old_ids.find(d.unique_id) == old_ids.end()) {
            if (!added.empty()) added += L", ";
            added += d.name;
        }
    }
    for (const auto& d : before) {
        if (new_ids.find(d.unique_id) == new_ids.end()) {
            if (!removed.empty()) removed += L", ";
            removed += d.name;
        }
    }

    std::wstring result;
    if (!added.empty()) result += L"подключена камера: " + added;
    if (!removed.empty()) {
        if (!result.empty()) result += L"; ";
        result += L"отключена камера: " + removed;
    }
    return result;
}

// ---------------------------------------------------------------------------

DeviceMonitor::~DeviceMonitor() { stop(); }

void DeviceMonitor::start(Enumerator enumerator, Callback callback, int interval_ms) {
    stop();

    enumerator_ = std::move(enumerator);
    callback_ = std::move(callback);
    interval_ms_ = interval_ms > 0 ? interval_ms : 3000;

    if (!wake_event_) wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Начальное состояние -- чтобы первое сравнение было корректным.
    {
        std::vector<DeviceInfo> initial = enumerator_ ? enumerator_() : std::vector<DeviceInfo>{};
        std::lock_guard<std::mutex> lock(mutex_);
        devices_ = std::move(initial);
    }

    running_.store(true);
    worker_ = std::thread(&DeviceMonitor::run, this);
}

void DeviceMonitor::stop() {
    if (!running_.exchange(false)) {
        if (worker_.joinable()) worker_.join();
        return;
    }
    if (wake_event_) SetEvent(wake_event_);
    if (worker_.joinable()) worker_.join();
}

void DeviceMonitor::refresh_now() {
    refresh_.store(true);
    if (wake_event_) SetEvent(wake_event_);
}

std::vector<DeviceInfo> DeviceMonitor::devices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_;
}

void DeviceMonitor::run() {
    while (running_.load()) {
        // Ждём либо сигнала WM_DEVICECHANGE, либо истечения интервала опроса.
        WaitForSingleObject(wake_event_, static_cast<DWORD>(interval_ms_));
        if (!running_.load()) break;

        refresh_.store(false);

        std::vector<DeviceInfo> previous;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            previous = devices_;
        }

        std::vector<DeviceInfo> current = enumerator_ ? enumerator_() : std::vector<DeviceInfo>{};
        const std::wstring message = describe_device_changes(previous, current);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            devices_ = current;
        }

        if (!message.empty() && callback_) callback_(current, message);
    }
}

// ---------------------------------------------------------------------------

HDEVNOTIFY DeviceMonitor::register_notifications(HWND hwnd) {
    if (!hwnd) return nullptr;
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = GUID_DEVINTERFACE_IMAGE_LOCAL;
    return RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
}

void DeviceMonitor::unregister_notifications(HDEVNOTIFY handle) {
    if (handle) UnregisterDeviceNotification(handle);
}

}  // namespace cam
