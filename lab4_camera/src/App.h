// ---------------------------------------------------------------------------
//  App.h -- главное окно приложения (вариант А5: скрытый режим фотонаблюдения).
//
//  Состав:
//    * панель предпросмотра (кадры из CaptureController);
//    * панель информации об устройстве и элементы управления;
//    * панель управления (каталог, снимок, запись, отчёт);
//    * журнал событий с цветовой индикацией уровней;
//    * значок в трее, глобальная горячая клавиша Ctrl+Shift+F12,
//      уведомления (balloon tip), восстановление окна по двойному клику.
// ---------------------------------------------------------------------------
#pragma once

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include "Camera.h"
#include "CaptureController.h"
#include "DeviceMonitor.h"
#include "Logger.h"
#include "MediaSaver.h"
#include "Theme.h"

namespace cam {

// Настройки, сохраняемые в HKEY_CURRENT_USER\Software\CameraLab4.
struct Settings {
    std::wstring save_dir;
    int device_index = 0;
    int resolution_index = 1;   // 1280x720
    int fps_index = 1;          // 30
    int backend_index = 0;      // OpenCV
    int quality = 90;
    bool dark_theme = true;
    bool auto_reconnect = true;
    bool minimize_to_tray = false;

    void load();
    void save() const;
};

class App {
public:
    App();
    ~App();

    bool init(HINSTANCE hinst, int show_cmd);
    int run();

private:
    // --- оконная процедура и обработчики сообщений
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    void on_create();
    void on_destroy();
    void on_size(int cx, int cy);
    void on_timer();
    void on_command(int id, int code);
    void on_hotkey(int id);
    void on_tray(LPARAM event);
    void on_device_change(WPARAM wp, LPARAM lp);
    void on_state_message(CameraState state, const std::wstring& text);
    void on_new_frame();
    void on_log_record(LogRecord* record);
    void on_devices_changed(std::vector<DeviceInfo>* devices, std::wstring* message);

    // --- построение интерфейса и отрисовка
    void create_controls();
    void layout();
    void apply_theme();
    void update_enabled_state();
    void update_info_labels();
    void paint(HDC dc, const RECT& client);
    void draw_header(HDC dc);
    void draw_preview(HDC dc);
    void draw_devices_panel(HDC dc);
    void draw_log_panel(HDC dc);
    int S(int value) const;   // масштабирование под DPI

    // --- действия
    void refresh_device_list(bool initial);
    void connect_camera();
    void disconnect_camera();
    void take_snapshot(bool from_hotkey);
    void start_recording();
    void stop_recording();
    void choose_save_dir();
    void open_save_dir();
    void save_report();
    void hide_to_tray();
    void restore_window();
    void request_exit();
    void apply_save_dir();

    // --- журнал
    void append_log(const LogRecord& record);
    void add_log(LogLevel level, const std::wstring& text);

    // --- трей
    void create_tray_icon();
    void remove_tray_icon();
    void update_tray_tooltip();
    void show_balloon(const std::wstring& title, const std::wstring& text, DWORD flags);

    // Сообщение из потока захвата -> GUI-поток.
    void post_state(CameraState state, const std::wstring& text);

    // --- данные
    HINSTANCE hinst_ = nullptr;
    HWND hwnd_ = nullptr;
    HDEVNOTIFY device_notify_ = nullptr;
    bool tray_added_ = false;
    bool hotkey_registered_ = false;
    bool minimized_to_tray_ = false;

    UiFonts fonts_;
    int dpi_ = 96;
    bool dark_ = true;
    const Palette* palette_ = nullptr;

    CaptureController controller_;
    VideoRecorder recorder_;
    FrameQueue record_queue_{8};
    DeviceMonitor monitor_;

    Settings settings_;
    std::vector<DeviceInfo> devices_;
    CameraState state_ = CameraState::Disconnected;
    std::wstring state_text_ = L"Камера не подключена";
    bool blink_on_ = false;
    int blink_counter_ = 0;

    HWND h_device_ = nullptr;
    HWND h_refresh_ = nullptr;
    HWND h_resolution_ = nullptr;
    HWND h_fps_ = nullptr;
    HWND h_backend_ = nullptr;
    HWND h_quality_ = nullptr;
    HWND h_connect_ = nullptr;
    HWND h_disconnect_ = nullptr;
    HWND h_auto_reconnect_ = nullptr;
    HWND h_snapshot_ = nullptr;
    HWND h_record_start_ = nullptr;
    HWND h_record_stop_ = nullptr;
    HWND h_dir_ = nullptr;
    HWND h_browse_ = nullptr;
    HWND h_open_dir_ = nullptr;
    HWND h_report_ = nullptr;
    HWND h_theme_ = nullptr;
    HWND h_tray_button_ = nullptr;
    HWND h_log_ = nullptr;
    HWND h_clear_log_ = nullptr;
    HWND h_minimize_tray_ = nullptr;

    HBRUSH panel_brush_ = nullptr;
    HBRUSH window_brush_ = nullptr;
    HBRUSH log_brush_ = nullptr;

    // Текстовые значения для панели информации (рисуются в paint()).
    std::wstring info_name_;
    std::wstring info_mode_;
    std::wstring info_format_;
    std::wstring info_backend_;
    std::wstring info_stats_;
    std::wstring info_record_;
    std::wstring status_text_;
    std::chrono::steady_clock::time_point record_started_{};

    RECT header_rc_{};
    RECT save_rc_{};
    RECT device_label_rc_{};
    RECT res_label_rc_{};
    RECT fps_label_rc_{};
    RECT backend_label_rc_{};
    RECT quality_label_rc_{};
    RECT info_area_rc_{};
    RECT dir_label_rc_{};
    RECT preview_rc_{};
    RECT right_rc_{};
    RECT log_rc_{};
    RECT devices_group_rc_{};
    RECT info_group_rc_{};

    std::atomic<bool> frame_posted_{false};

    // Идентификаторы команд
    enum : int {
        IDC_DEVICE = 1001,
        IDC_REFRESH,
        IDC_RESOLUTION,
        IDC_FPS,
        IDC_BACKEND,
        IDC_QUALITY,
        IDC_CONNECT,
        IDC_DISCONNECT,
        IDC_AUTO_RECONNECT,
        IDC_SNAPSHOT,
        IDC_RECORD_START,
        IDC_RECORD_STOP,
        IDC_DIR_EDIT,
        IDC_BROWSE,
        IDC_OPEN_DIR,
        IDC_REPORT,
        IDC_THEME,
        IDC_TRAY_BUTTON,
        IDC_LOG,
        IDC_CLEAR_LOG,
        IDC_MINIMIZE_TRAY,
        IDC_TRAY_MENU_SHOW = 2001,
        IDC_TRAY_MENU_SNAPSHOT,
        IDC_TRAY_MENU_RECORD,
        IDC_TRAY_MENU_EXIT,
    };

    enum : UINT {
        WM_APP_STATE = WM_APP + 1,
        WM_APP_FRAME,
        WM_APP_TRAY,
        WM_APP_RECORD_STOPPED,
        WM_APP_LOG,
        WM_APP_DEVICES,
    };

    enum : int {
        HOTKEY_SNAPSHOT = 1,
        TIMER_UI = 1,
    };
};

}  // namespace cam
