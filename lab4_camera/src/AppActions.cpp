// ---------------------------------------------------------------------------
//  AppActions.cpp -- логика приложения: подключение камеры, снимки, запись,
//  работа с треем, горячей клавишей, журналом и настройками.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <dbt.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>

#include <iterator>
#include <memory>

#include "App.h"
#include "Logger.h"

namespace cam {

namespace {

const struct ResolutionOption {
    int width;
    int height;
} kResolutions[] = {
    {1920, 1080}, {1280, 720}, {1024, 768}, {960, 540},
    {800, 600},   {640, 480},  {640, 360},  {320, 240},
};

const double kFpsOptions[] = {60.0, 30.0, 25.0, 20.0, 15.0, 10.0};

constexpr int kResolutionCount = static_cast<int>(std::size(kResolutions));
constexpr int kFpsCount = static_cast<int>(std::size(kFpsOptions));

const wchar_t* kRegistryKey = L"Software\\CameraLab4";

std::wstring default_save_dir() {
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, SHGFP_TYPE_CURRENT,
                                   path))) {
        return join_path(path, L"CameraLab4");
    }
    return join_path(get_module_dir(), L"snapshots");
}

std::wstring trim(const std::wstring& text) {
    const size_t begin = text.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos) return L"";
    const size_t end = text.find_last_not_of(L" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

}  // namespace

// ---------------------------------------------------------------------------
//  Настройки (реестр HKCU)
// ---------------------------------------------------------------------------

void Settings::load() {
    save_dir = default_save_dir();

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return;
    }

    wchar_t buffer[MAX_PATH * 2] = {};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    if (RegQueryValueExW(key, L"SaveDir", nullptr, &type, reinterpret_cast<LPBYTE>(buffer),
                         &size) == ERROR_SUCCESS &&
        type == REG_SZ && buffer[0] != L'\0') {
        save_dir = buffer;
    }

    auto read_dword = [&](const wchar_t* name, int& value) {
        DWORD data = 0;
        DWORD data_size = sizeof(data);
        DWORD data_type = 0;
        if (RegQueryValueExW(key, name, nullptr, &data_type,
                             reinterpret_cast<LPBYTE>(&data), &data_size) == ERROR_SUCCESS &&
            data_type == REG_DWORD) {
            value = static_cast<int>(data);
        }
    };

    read_dword(L"Device", device_index);
    read_dword(L"Resolution", resolution_index);
    read_dword(L"Fps", fps_index);
    read_dword(L"Backend", backend_index);
    read_dword(L"Quality", quality);

    int flag = dark_theme ? 1 : 0;
    read_dword(L"DarkTheme", flag);
    dark_theme = flag != 0;

    flag = auto_reconnect ? 1 : 0;
    read_dword(L"AutoReconnect", flag);
    auto_reconnect = flag != 0;

    flag = minimize_to_tray ? 1 : 0;
    read_dword(L"MinimizeToTray", flag);
    minimize_to_tray = flag != 0;

    RegCloseKey(key);
}

void Settings::save() const {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }

    RegSetValueExW(key, L"SaveDir", 0, REG_SZ, reinterpret_cast<const BYTE*>(save_dir.c_str()),
                   static_cast<DWORD>((save_dir.size() + 1) * sizeof(wchar_t)));

    auto write_dword = [&](const wchar_t* name, int value) {
        DWORD data = static_cast<DWORD>(value);
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&data),
                       sizeof(data));
    };

    write_dword(L"Device", device_index);
    write_dword(L"Resolution", resolution_index);
    write_dword(L"Fps", fps_index);
    write_dword(L"Backend", backend_index);
    write_dword(L"Quality", quality);
    write_dword(L"DarkTheme", dark_theme ? 1 : 0);
    write_dword(L"AutoReconnect", auto_reconnect ? 1 : 0);
    write_dword(L"MinimizeToTray", minimize_to_tray ? 1 : 0);

    RegCloseKey(key);
}

// ---------------------------------------------------------------------------
//  Команды
// ---------------------------------------------------------------------------

void App::on_command(int id, int code) {
    switch (id) {
        case IDC_REFRESH:
            if (code == BN_CLICKED) {
                refresh_device_list(false);
            }
            break;

        case IDC_CONNECT:
            if (code == BN_CLICKED) connect_camera();
            break;

        case IDC_DISCONNECT:
            if (code == BN_CLICKED) disconnect_camera();
            break;

        case IDC_SNAPSHOT:
            if (code == BN_CLICKED) take_snapshot(false);
            break;

        case IDC_RECORD_START:
            if (code == BN_CLICKED) start_recording();
            break;

        case IDC_RECORD_STOP:
            if (code == BN_CLICKED) stop_recording();
            break;

        case IDC_BROWSE:
            if (code == BN_CLICKED) choose_save_dir();
            break;

        case IDC_OPEN_DIR:
            if (code == BN_CLICKED) open_save_dir();
            break;

        case IDC_REPORT:
            if (code == BN_CLICKED) save_report();
            break;

        case IDC_THEME:
            if (code == BN_CLICKED) {
                dark_ = !dark_;
                settings_.dark_theme = dark_;
                palette_ = dark_ ? &dark_palette() : &light_palette();
                apply_theme();
                settings_.save();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            break;

        case IDC_TRAY_BUTTON:
            if (code == BN_CLICKED) hide_to_tray();
            break;

        case IDC_MINIMIZE_TRAY:
            if (code == BN_CLICKED) {
                settings_.minimize_to_tray =
                    SendMessageW(h_minimize_tray_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                settings_.save();
            }
            break;

        case IDC_CLEAR_LOG:
            if (code == BN_CLICKED) {
                SendMessageW(h_log_, LB_RESETCONTENT, 0, 0);
                Logger::instance().clear();
                add_log(LogLevel::Info, L"Журнал очищен");
            }
            break;

        case IDC_DEVICE:
            if (code == CBN_SELCHANGE) {
                settings_.device_index =
                    std::max(0, static_cast<int>(SendMessageW(h_device_, CB_GETCURSEL, 0, 0)));
                settings_.save();
            }
            break;

        case IDC_RESOLUTION:
            if (code == CBN_SELCHANGE) {
                settings_.resolution_index =
                    std::max(0, static_cast<int>(SendMessageW(h_resolution_, CB_GETCURSEL, 0, 0)));
                settings_.save();
            }
            break;

        case IDC_FPS:
            if (code == CBN_SELCHANGE) {
                settings_.fps_index =
                    std::max(0, static_cast<int>(SendMessageW(h_fps_, CB_GETCURSEL, 0, 0)));
                settings_.save();
            }
            break;

        case IDC_BACKEND:
            if (code == CBN_SELCHANGE) {
                settings_.backend_index =
                    std::max(0, static_cast<int>(SendMessageW(h_backend_, CB_GETCURSEL, 0, 0)));
                settings_.save();
                refresh_device_list(false);
            }
            break;

        case IDC_QUALITY:
            if (code == EN_CHANGE) {
                wchar_t buffer[8] = {};
                GetWindowTextW(h_quality_, buffer, 8);
                const int value = _wtoi(buffer);
                if (value >= 1 && value <= 100) {
                    settings_.quality = value;
                }
            }
            break;

        case IDC_AUTO_RECONNECT:
            if (code == BN_CLICKED) {
                settings_.auto_reconnect =
                    SendMessageW(h_auto_reconnect_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                settings_.save();
            }
            break;

        case IDC_TRAY_MENU_SHOW:
            restore_window();
            break;

        case IDC_TRAY_MENU_SNAPSHOT:
            take_snapshot(true);
            break;

        case IDC_TRAY_MENU_RECORD:
            if (recorder_.is_recording()) {
                stop_recording();
            } else {
                start_recording();
            }
            break;

        case IDC_TRAY_MENU_EXIT:
            request_exit();
            break;

        default:
            break;
    }
}

void App::on_hotkey(int id) {
    if (id != HOTKEY_SNAPSHOT) return;
    if (minimized_to_tray_) {
        add_log(LogLevel::Background, L"Горячая клавиша Ctrl+Shift+F12 (скрытый режим)");
    }
    take_snapshot(true);
}

void App::on_tray(LPARAM event) {
    switch (static_cast<UINT>(LOWORD(event))) {
        case WM_LBUTTONDBLCLK:
            restore_window();
            break;

        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, IDC_TRAY_MENU_SHOW, L"Показать окно");
            AppendMenuW(menu, MF_STRING, IDC_TRAY_MENU_SNAPSHOT, L"Сделать снимок");
            AppendMenuW(menu, MF_STRING, IDC_TRAY_MENU_RECORD,
                        recorder_.is_recording() ? L"Остановить запись" : L"Начать запись");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, IDC_TRAY_MENU_EXIT, L"Выход");

            POINT pt{};
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd_);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd_,
                           nullptr);
            DestroyMenu(menu);
            PostMessageW(hwnd_, WM_NULL, 0, 0);
            break;
        }

        default:
            break;
    }
}

void App::on_device_change(WPARAM wp, LPARAM lp) {
    (void)lp;
    if (wp == DBT_DEVICEARRIVAL) {
        add_log(LogLevel::Info, L"WM_DEVICECHANGE: подключено устройство");
        monitor_.refresh_now();
    } else if (wp == DBT_DEVICEREMOVECOMPLETE) {
        add_log(LogLevel::Warn, L"WM_DEVICECHANGE: устройство отключено");
        monitor_.refresh_now();
    } else if (wp == DBT_DEVNODES_CHANGED) {
        // Приходит несколько раз подряд -- не засоряем журнал.
        monitor_.refresh_now();
    }
}

void App::on_devices_changed(std::vector<DeviceInfo>* devices, std::wstring* message) {
    if (!devices || !message) return;

    devices_ = *devices;
    add_log(LogLevel::Warn, *message);

    SendMessageW(h_device_, CB_RESETCONTENT, 0, 0);
    for (const auto& device : devices_) {
        SendMessageW(h_device_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(device.name.c_str()));
    }
    const int index =
        std::min<int>(settings_.device_index, static_cast<int>(devices_.size()) - 1);
    SendMessageW(h_device_, CB_SETCURSEL, index >= 0 ? index : -1, 0);

    update_enabled_state();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::on_log_record(LogRecord* record) {
    std::unique_ptr<LogRecord> holder(record);
    if (!holder) return;
    append_log(*holder);
}

// ---------------------------------------------------------------------------
//  Камера
// ---------------------------------------------------------------------------

void App::refresh_device_list(bool initial) {
    std::unique_ptr<ICamera> camera(settings_.backend_index == 0 ? create_opencv_camera()
                                                                 : create_mf_camera());
    devices_ = camera->enumerate_devices();

    SendMessageW(h_device_, CB_RESETCONTENT, 0, 0);
    for (const auto& device : devices_) {
        SendMessageW(h_device_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(device.name.c_str()));
    }

    int index = -1;
    if (!devices_.empty()) {
        index = std::clamp(settings_.device_index, 0, static_cast<int>(devices_.size()) - 1);
        SendMessageW(h_device_, CB_SETCURSEL, index, 0);
        settings_.device_index = index;
    }

    if (devices_.empty()) {
        add_log(LogLevel::Warn,
                L"Камеры не обнаружены. Проверьте подключение USB-камеры и разрешение "
                L"приложения на доступ к камере (Параметры → Конфиденциальность → Камера).");
    } else if (!initial) {
        add_log(LogLevel::Info,
                format(L"Список камер обновлён: найдено %zu устройств", devices_.size()));
    }

    update_enabled_state();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::connect_camera() {
    if (state_ == CameraState::Connected || state_ == CameraState::Connecting) return;
    if (devices_.empty()) {
        add_log(LogLevel::Warn, L"Подключение невозможно: камера не выбрана");
        return;
    }

    const int selected = static_cast<int>(SendMessageW(h_device_, CB_GETCURSEL, 0, 0));
    const int device_index = std::clamp(selected, 0, static_cast<int>(devices_.size()) - 1);
    settings_.device_index = device_index;

    CaptureOptions options;
    options.backend = settings_.backend_index == 0 ? Backend::OpenCV : Backend::MediaFoundation;
    options.device_index = device_index;
    const auto& resolution = kResolutions[std::clamp(settings_.resolution_index, 0,
                                                     kResolutionCount - 1)];
    options.width = resolution.width;
    options.height = resolution.height;
    options.fps = kFpsOptions[std::clamp(settings_.fps_index, 0, kFpsCount - 1)];
    options.auto_reconnect = settings_.auto_reconnect;

    state_ = CameraState::Connecting;
    status_text_ = L"Подключение камеры…";
    add_log(LogLevel::Info,
            format(L"Подключение камеры «%s» (%d×%d, %.0f к/с, бэкенд %s)",
                   devices_[static_cast<size_t>(device_index)].name.c_str(), options.width,
                   options.height,
                   options.fps, backend_name(options.backend)));

    controller_.connect(options);
    settings_.save();
    update_enabled_state();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::disconnect_camera() {
    if (recorder_.is_recording()) stop_recording();
    controller_.disconnect();
    state_ = CameraState::Disconnected;
    status_text_ = L"Камера отключена";
    add_log(LogLevel::Info, L"Захват остановлен пользователем");
    update_info_labels();
    update_enabled_state();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
//  Снимок кадра
// ---------------------------------------------------------------------------

void App::take_snapshot(bool from_hotkey) {
    apply_save_dir();

    CameraFrame frame;
    if (!controller_.get_latest(frame)) {
        add_log(LogLevel::Warn, L"Снимок не создан: нет видеосигнала");
        if (from_hotkey) {
            show_balloon(L"Снимок не создан", L"Камера не подключена или нет видеосигнала",
                         NIIF_WARNING);
        }
        return;
    }

    SYSTEMTIME now;
    GetLocalTime(&now);
    const std::wstring path = make_snapshot_path(settings_.save_dir, now);

    std::wstring error;
    std::wstring actual_path;
    if (!save_snapshot(path, frame, settings_.quality, &error, &actual_path)) {
        add_log(LogLevel::Error, L"Не удалось сохранить снимок: " + error);
        show_balloon(L"Ошибка сохранения снимка", error, NIIF_ERROR);
        return;
    }

    const std::wstring message =
        format(L"Снимок сохранён: %s (%d×%d, %.1f КБ)",
               file_name_of(actual_path).c_str(), frame.width, frame.height,
               static_cast<double>(file_size(actual_path)) / 1024.0);
    add_log(from_hotkey ? LogLevel::Background : LogLevel::Info, message);

    if (!error.empty()) add_log(LogLevel::Warn, error);

    show_balloon(from_hotkey ? L"Снимок (скрытый режим)" : L"Снимок сохранён",
                 file_name_of(actual_path) + L"\n" + settings_.save_dir, NIIF_INFO);
    update_tray_tooltip();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
//  Запись видео
// ---------------------------------------------------------------------------

void App::start_recording() {
    if (recorder_.is_recording()) return;
    if (state_ != CameraState::Connected) {
        add_log(LogLevel::Warn, L"Запись невозможна: камера не подключена");
        show_balloon(L"Запись не начата", L"Сначала подключите камеру", NIIF_WARNING);
        return;
    }

    apply_save_dir();

    int width = controller_.mode().width;
    int height = controller_.mode().height;
    CameraFrame latest;
    if (controller_.get_latest(latest)) {
        width = latest.width;
        height = latest.height;
    }
    if (width <= 0 || height <= 0) {
        add_log(LogLevel::Error, L"Запись невозможна: неизвестный размер кадра");
        return;
    }

    double fps = controller_.measured_fps();
    if (fps < 1.0) fps = controller_.mode().fps;
    if (fps < 1.0) fps = 25.0;

    SYSTEMTIME now;
    GetLocalTime(&now);
    const std::wstring path = make_video_path(settings_.save_dir, now);

    std::wstring error;
    if (!recorder_.start(&record_queue_, path, width, height, fps, settings_.quality, &error)) {
        add_log(LogLevel::Error, L"Не удалось начать запись видео: " + error);
        show_balloon(L"Ошибка записи", error, NIIF_ERROR);
        return;
    }

    // Кадры для рекордера теперь копируются из потока захвата в кольцевой буфер.
    controller_.set_record_queue(&record_queue_);
    record_started_ = std::chrono::steady_clock::now();
    blink_counter_ = 0;

    add_log(LogLevel::Info,
            format(L"Запись видео начата: %s (%d×%d, %.0f к/с, MJPEG/AVI)",
                   file_name_of(path).c_str(), width, height, fps));
    if (minimized_to_tray_) {
        show_balloon(L"Запись начата", file_name_of(path), NIIF_INFO);
    }

    update_info_labels();
    update_enabled_state();
    update_tray_tooltip();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::stop_recording() {
    if (!recorder_.is_recording()) return;

    // Сначала отключаем питание рекордера кадрами, затем останавливаем поток.
    controller_.set_record_queue(nullptr);
    recorder_.stop();   // внутри вызывается колбэк с итогами записи

    update_info_labels();
    update_enabled_state();
    update_tray_tooltip();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
//  Каталог, отчёт
// ---------------------------------------------------------------------------

void App::apply_save_dir() {
    wchar_t buffer[MAX_PATH * 2] = {};
    GetWindowTextW(h_dir_, buffer, static_cast<int>(std::size(buffer)));
    std::wstring dir = trim(buffer);
    if (dir.empty()) dir = default_save_dir();

    if (!ensure_dir(dir)) {
        add_log(LogLevel::Error, L"Не удалось создать каталог: " + dir);
        dir = get_module_dir();
        if (!ensure_dir(join_path(dir, L"snapshots"))) return;
        dir = join_path(dir, L"snapshots");
        SetWindowTextW(h_dir_, dir.c_str());
    }

    if (settings_.save_dir != dir) {
        settings_.save_dir = dir;
        settings_.save();
    }
    Logger::instance().set_log_file(join_path(dir, L"camera_log.txt"));
}

void App::choose_save_dir() {
    BROWSEINFOW browse{};
    browse.hwndOwner = hwnd_;
    browse.lpszTitle = L"Каталог для сохранения снимков, видео и журнала";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST root = nullptr;
    if (SUCCEEDED(SHParseDisplayName(settings_.save_dir.c_str(), nullptr, &root, 0, nullptr))) {
        browse.pidlRoot = root;
    }

    LPITEMIDLIST result = SHBrowseForFolderW(&browse);
    if (root) CoTaskMemFree(root);

    if (result) {
        wchar_t path[MAX_PATH] = {};
        if (SHGetPathFromIDListW(result, path)) {
            SetWindowTextW(h_dir_, path);
            apply_save_dir();
            add_log(LogLevel::Info, std::wstring(L"Каталог сохранения изменён: ") + path);
        }
        CoTaskMemFree(result);
    }
}

void App::open_save_dir() {
    apply_save_dir();
    if (!ensure_dir(settings_.save_dir)) {
        add_log(LogLevel::Error, L"Каталог не найден: " + settings_.save_dir);
        return;
    }
    HINSTANCE hinst = ShellExecuteW(hwnd_, L"open", settings_.save_dir.c_str(), nullptr, nullptr,
                                    SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(hinst) <= 32) {
        add_log(LogLevel::Error, L"Не удалось открыть каталог в Проводнике");
    }
}

void App::save_report() {
    apply_save_dir();

    SYSTEMTIME now;
    GetLocalTime(&now);
    const std::wstring path =
        join_path(settings_.save_dir, L"report_" + file_timestamp(now) + L".txt");

    if (Logger::instance().save_report(path)) {
        add_log(LogLevel::Info, L"Отчёт сохранён: " + file_name_of(path));
        show_balloon(L"Отчёт сохранён", file_name_of(path) + L"\n" + settings_.save_dir,
                     NIIF_INFO);
    } else {
        add_log(LogLevel::Error, L"Не удалось сохранить отчёт: " + path);
        MessageBoxW(hwnd_, (L"Не удалось сохранить отчёт:\n" + path).c_str(), L"Ошибка",
                    MB_OK | MB_ICONERROR);
    }
}

// ---------------------------------------------------------------------------
//  Скрытый режим и трей
// ---------------------------------------------------------------------------

void App::hide_to_tray() {
    // Для фотонаблюдения в фоне камера должна быть уже открыта: если её нет,
    // подключаем автоматически (это и есть "фоновый режим работы").
    if (state_ != CameraState::Connected && !devices_.empty()) {
        add_log(LogLevel::Background, L"Фоновый режим: автоматическое подключение камеры");
        connect_camera();
    }

    ShowWindow(hwnd_, SW_HIDE);
    minimized_to_tray_ = true;
    add_log(LogLevel::Background,
            L"Окно свёрнуто в трей — работает фоновое фотонаблюдение (Ctrl+Shift+F12)");
    show_balloon(L"Фотонаблюдение включено",
                 L"Окно скрыто. Ctrl+Shift+F12 — снимок без открытия окна.\n"
                 L"Двойной щелчок по значку — вернуть окно.",
                 NIIF_INFO);
    update_tray_tooltip();
}

void App::restore_window() {
    ShowWindow(hwnd_, SW_SHOW);
    ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    minimized_to_tray_ = false;
    if (state_ != CameraState::Connected) {
        add_log(LogLevel::Background, L"Окно восстановлено из трея");
    } else {
        add_log(LogLevel::Background, L"Окно восстановлено из трея (камера остаётся подключена)");
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::request_exit() {
    if (recorder_.is_recording()) {
        const int answer = MessageBoxW(hwnd_,
                                       L"Идёт запись видео. Остановить запись и выйти?",
                                       L"Выход из приложения",
                                       MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (answer != IDYES) return;
        stop_recording();
    }
    settings_.save();
    DestroyWindow(hwnd_);
}

// ---------------------------------------------------------------------------
//  Значок в трее
// ---------------------------------------------------------------------------

void App::create_tray_icon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = WM_APP_TRAY;
    data.hIcon = LoadIconW(hinst_, MAKEINTRESOURCEW(101));
    if (!data.hIcon) data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(data.szTip, L"Видеонаблюдение — ЛР №4 (вариант А5)");

    tray_added_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if (tray_added_) {
        data.uVersion = NOTIFYICON_VERSION;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
        add_log(LogLevel::Info, L"Значок добавлен в системный трей");
    } else {
        add_log(LogLevel::Warn, L"Не удалось добавить значок в системный трей");
    }
}

void App::remove_tray_icon() {
    if (!tray_added_) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
    tray_added_ = false;
}

void App::update_tray_tooltip() {
    if (!tray_added_) return;

    std::wstring tip = L"Видеонаблюдение (А5)";
    if (state_ == CameraState::Connected) {
        tip += L"\nКамера подключена";
    } else if (state_ == CameraState::Connecting) {
        tip += L"\nПодключение…";
    } else {
        tip += L"\nКамера отключена";
    }
    if (recorder_.is_recording()) tip += L"\n● ИДЁТ ЗАПИСЬ";
    tip += L"\nCtrl+Shift+F12 — снимок";

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;
    data.uFlags = NIF_TIP;
    wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void App::show_balloon(const std::wstring& title, const std::wstring& text, DWORD flags) {
    if (!tray_added_) return;

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = flags;
    wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

// ---------------------------------------------------------------------------
//  Журнал
// ---------------------------------------------------------------------------

void App::append_log(const LogRecord& record) {
    if (!h_log_) return;

    const std::wstring line = log_timestamp(record.time) + L"  " + record.text;
    const int index = static_cast<int>(
        SendMessageW(h_log_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str())));
    if (index >= 0) {
        SendMessageW(h_log_, LB_SETITEMDATA, index, static_cast<LPARAM>(record.level));
    }

    // Ограничиваем размер списка, чтобы интерфейс не замедлялся.
    int count = static_cast<int>(SendMessageW(h_log_, LB_GETCOUNT, 0, 0));
    while (count > 500) {
        SendMessageW(h_log_, LB_DELETESTRING, 0, 0);
        --count;
    }
    if (count > 0) {
        SendMessageW(h_log_, LB_SETTOPINDEX, count - 1, 0);
    }
}

void App::add_log(LogLevel level, const std::wstring& text) {
    switch (level) {
        case LogLevel::Warn:       Logger::instance().warn(text); break;
        case LogLevel::Error:      Logger::instance().error(text); break;
        case LogLevel::Background: Logger::instance().background(text); break;
        case LogLevel::Info:
        default:                   Logger::instance().info(text); break;
    }
}

}  // namespace cam
