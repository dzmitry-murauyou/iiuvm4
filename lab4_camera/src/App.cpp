#include "App.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>

#include <iterator>
#include <memory>

#include "Logger.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace cam {

namespace {

const wchar_t* kWindowClass = L"CameraLab4Window";
const wchar_t* kWindowTitle =
    L"ЛР №4. Система видеонаблюдения — вариант А5 (скрытый режим фотонаблюдения)";

const struct ResolutionOption {
    int width;
    int height;
    const wchar_t* title;
} kResolutions[] = {
    {1920, 1080, L"1920 × 1080"}, {1280, 720, L"1280 × 720"},
    {1024, 768, L"1024 × 768"},   {960, 540, L"960 × 540"},
    {800, 600, L"800 × 600"},     {640, 480, L"640 × 480"},
    {640, 360, L"640 × 360"},     {320, 240, L"320 × 240"},
};

const struct FpsOption {
    double fps;
    const wchar_t* title;
} kFpsOptions[] = {
    {60.0, L"60"}, {30.0, L"30"}, {25.0, L"25"}, {20.0, L"20"},
    {15.0, L"15"}, {10.0, L"10"},
};

const wchar_t* kBackendTitles[] = {L"OpenCV (cv::VideoCapture)",
                                   L"Media Foundation (Source Reader)"};

constexpr int kResolutionCount = static_cast<int>(std::size(kResolutions));
constexpr int kFpsCount = static_cast<int>(std::size(kFpsOptions));
constexpr int kBackendCount = static_cast<int>(std::size(kBackendTitles));

COLORREF level_color(LogLevel level, const Palette& p) {
    switch (level) {
        case LogLevel::Error:      return p.error;
        case LogLevel::Warn:       return p.warn;
        case LogLevel::Background: return p.accent;
        case LogLevel::Info:
        default:                   return p.text;
    }
}

std::wstring seconds_to_hms(double seconds) {
    if (seconds < 0) seconds = 0;
    const int total = static_cast<int>(seconds);
    wchar_t buffer[32];
    swprintf_s(buffer, L"%02d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
    return buffer;
}

}  // namespace

App::App() : palette_(&light_palette()) {}

App::~App() {
    recorder_.set_stop_callback(nullptr);
    if (panel_brush_) DeleteObject(panel_brush_);
    if (window_brush_) DeleteObject(window_brush_);
    if (log_brush_) DeleteObject(log_brush_);
    fonts_.destroy();
}

int App::S(int value) const { return MulDiv(value, dpi_, 96); }

// ---------------------------------------------------------------------------
//  Инициализация окна
// ---------------------------------------------------------------------------

bool App::init(HINSTANCE hinst, int show_cmd) {
    hinst_ = hinst;

    // DPI-awareness: иначе интерфейс "мылится" на мониторах с масштабом.
    using SetDpiCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE user32 = GetModuleHandleW(L"user32")) {
        auto set_ctx = reinterpret_cast<SetDpiCtxFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (set_ctx) {
            set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        } else {
            SetProcessDPIAware();
        }
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &App::wnd_proc;
    wc.hInstance = hinst_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(hinst_, MAKEINTRESOURCEW(101));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) return false;

    hwnd_ = CreateWindowExW(0, kWindowClass, kWindowTitle,
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800, nullptr, nullptr,
                            hinst_, this);
    if (!hwnd_) return false;

    ShowWindow(hwnd_, show_cmd);
    UpdateWindow(hwnd_);
    return true;
}

int App::run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK App::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->handle(msg, wp, lp);
}

LRESULT App::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            on_create();
            return 0;

        case WM_SIZE:
            on_size(LOWORD(lp), HIWORD(lp));
            return 0;

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize.x = S(1040);
            info->ptMinTrackSize.y = S(700);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT rc;
            GetClientRect(hwnd_, &rc);
            if (rc.right <= 0 || rc.bottom <= 0) {
                EndPaint(hwnd_, &ps);
                return 0;
            }

            // Двойная буферизация: без неё окно мерцает при 25-30 кадрах/с.
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ old_bmp = SelectObject(mem, bmp);
            paint(mem, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old_bmp);
            DeleteObject(bmp);
            DeleteDC(mem);

            EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_TIMER:
            on_timer();
            return 0;

        case WM_COMMAND:
            on_command(LOWORD(wp), HIWORD(wp));
            return 0;

        case WM_HOTKEY:
            on_hotkey(static_cast<int>(wp));
            return 0;

        case WM_APP_STATE: {
            std::unique_ptr<std::pair<CameraState, std::wstring>> payload(
                reinterpret_cast<std::pair<CameraState, std::wstring>*>(lp));
            if (payload) on_state_message(payload->first, payload->second);
            return 0;
        }

        case WM_APP_FRAME:
            on_new_frame();
            return 0;

        case WM_APP_LOG:
            on_log_record(reinterpret_cast<LogRecord*>(lp));
            return 0;

        case WM_APP_DEVICES: {
            std::unique_ptr<std::pair<std::vector<DeviceInfo>, std::wstring>> payload(
                reinterpret_cast<std::pair<std::vector<DeviceInfo>, std::wstring>*>(lp));
            if (payload) on_devices_changed(&payload->first, &payload->second);
            return 0;
        }

        case WM_APP_TRAY:
            on_tray(lp);
            return 0;

        case WM_DEVICECHANGE:
            on_device_change(wp, lp);
            return 0;

        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (!dis || dis->CtlID != IDC_LOG) break;

            wchar_t text[1024] = {};
            SendMessageW(dis->hwndItem, LB_GETTEXT, dis->itemID,
                         reinterpret_cast<LPARAM>(text));
            const std::wstring line(text);

            const bool selected = (dis->itemState & ODS_SELECTED) != 0;
            const COLORREF bg = selected ? palette_->panel_alt : palette_->log_bg;
            fill_rect(dis->hDC, dis->rcItem, bg);

            const LogLevel level = static_cast<LogLevel>(
                SendMessageW(dis->hwndItem, LB_GETITEMDATA, dis->itemID, 0));
            RECT rc = dis->rcItem;
            rc.left += S(6);

            // Метка времени: фиксированная ширина "дд.мм.гггг чч:мм:сс.ммм"
            const size_t time_len = 23;
            std::wstring time_part = line.substr(0, std::min(time_len, line.size()));
            std::wstring rest = (line.size() > time_len) ? line.substr(time_len) : L"";

            RECT time_rc = rc;
            time_rc.right = time_rc.left + S(150);
            draw_text(dis->hDC, time_part, time_rc, fonts_.mono, palette_->text_dim,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER);

            RECT text_rc = rc;
            text_rc.left = time_rc.right;
            draw_text(dis->hDC, rest, text_rc, fonts_.mono, level_color(level, *palette_),
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            return TRUE;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, palette_->text);
            SetBkColor(dc, palette_->panel);
            return reinterpret_cast<LRESULT>(panel_brush_);
        }

        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, palette_->text);
            SetBkColor(dc, palette_->log_bg);
            return reinterpret_cast<LRESULT>(log_brush_);
        }

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            fonts_.create(dpi_);
            for (HWND child : {h_device_, h_refresh_, h_resolution_, h_fps_, h_backend_,
                               h_quality_, h_connect_, h_disconnect_, h_auto_reconnect_,
                               h_snapshot_, h_record_start_, h_record_stop_, h_dir_,
                               h_browse_, h_open_dir_, h_report_, h_theme_, h_tray_button_,
                               h_log_, h_clear_log_, h_minimize_tray_}) {
                if (child) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.ui), TRUE);
            }
            if (h_log_) {
                SendMessageW(h_log_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.mono), TRUE);
                SendMessageW(h_log_, LB_SETITEMHEIGHT, 0, S(16));
            }
            RECT* suggested = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_MINIMIZE && settings_.minimize_to_tray) {
                hide_to_tray();
                return 0;
            }
            break;

        case WM_CLOSE:
            request_exit();
            return 0;

        case WM_DESTROY:
            on_destroy();
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---------------------------------------------------------------------------
//  Создание интерфейса
// ---------------------------------------------------------------------------

void App::on_create() {
    dpi_ = 96;
    if (HMODULE user32 = GetModuleHandleW(L"user32")) {
        using GetDpiFn = UINT(WINAPI*)(HWND);
        auto get_dpi = reinterpret_cast<GetDpiFn>(GetProcAddress(user32, "GetDpiForWindow"));
        if (get_dpi) {
            UINT dpi = get_dpi(hwnd_);
            if (dpi >= 96 && dpi <= 480) dpi_ = static_cast<int>(dpi);
        }
    }

    settings_.load();
    dark_ = settings_.dark_theme;
    palette_ = dark_ ? &dark_palette() : &light_palette();
    fonts_.create(dpi_);

    apply_theme();
    create_controls();

    // Журнал: все записи дублируются в файл и в список окна.
    Logger::instance().set_callback([this](const LogRecord& record) {
        auto* copy = new LogRecord(record);
        if (!PostMessageW(hwnd_, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(copy))) {
            delete copy;
        }
    });

    // Поток захвата не трогает GUI напрямую -- только посылает сообщения.
    controller_.set_state_callback([this](CameraState state, const std::wstring& text) {
        post_state(state, text);
    });
    controller_.set_frame_callback([this]() {
        if (!frame_posted_.exchange(true)) PostMessageW(hwnd_, WM_APP_FRAME, 0, 0);
    });

    recorder_.set_stop_callback([this](const RecordResult& result) {
        const std::wstring size_text =
            format(L"%.1f МБ", static_cast<double>(file_size(result.path)) / (1024.0 * 1024.0));
        add_log(LogLevel::Info, format(L"Запись видео завершена: %s (кадров %llu, %s, %s)",
                                       file_name_of(result.path).c_str(), result.frames,
                                       seconds_to_hms(result.seconds).c_str(), size_text.c_str()));
        if (minimized_to_tray_) {
            show_balloon(L"Запись остановлена", file_name_of(result.path), NIIF_INFO);
        }
        update_info_labels();
        update_enabled_state();
        InvalidateRect(hwnd_, nullptr, FALSE);
    });

    create_tray_icon();

    // Глобальная горячая клавиша создания снимка (работает и в скрытом режиме).
    hotkey_registered_ = RegisterHotKey(hwnd_, HOTKEY_SNAPSHOT, MOD_CONTROL | MOD_SHIFT,
                                        VK_F12) != FALSE;
    if (!hotkey_registered_) {
        add_log(LogLevel::Warn,
                L"Не удалось зарегистрировать горячую клавишу Ctrl+Shift+F12 "
                L"(возможно, её заняло другое приложение)");
    }

    device_notify_ = DeviceMonitor::register_notifications(hwnd_);

    apply_save_dir();
    refresh_device_list(true);

    // Монитор списка камер: перечисление не открывает поток, поэтому не мешает
    // активному захвату.
    monitor_.start(
        [this]() {
            std::unique_ptr<ICamera> camera(
                settings_.backend_index == 0 ? create_opencv_camera() : create_mf_camera());
            return camera->enumerate_devices();
        },
        [this](const std::vector<DeviceInfo>& devices, const std::wstring& message) {
            // Колбэк вызывается из потока монитора -- передаём данные в GUI
            // через сообщение, чтобы не трогать оконные элементы из чужого потока.
            auto* payload = new std::pair<std::vector<DeviceInfo>, std::wstring>(devices, message);
            if (!PostMessageW(hwnd_, WM_APP_DEVICES, 0, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        },
        3000);

    SetTimer(hwnd_, TIMER_UI, 1000, nullptr);

    add_log(LogLevel::Info, L"Приложение запущено");
    add_log(LogLevel::Info,
            L"Горячая клавиша Ctrl+Shift+F12 — снимок в фоне (работает из любого приложения)");
    if (!dir_exists(settings_.save_dir)) {
        add_log(LogLevel::Warn, L"Каталог сохранения будет создан автоматически");
    }

    layout();
    update_info_labels();
    update_enabled_state();
}

void App::on_destroy() {
    KillTimer(hwnd_, TIMER_UI);
    if (hotkey_registered_) {
        UnregisterHotKey(hwnd_, HOTKEY_SNAPSHOT);
        hotkey_registered_ = false;
    }
    monitor_.stop();
    if (device_notify_) {
        DeviceMonitor::unregister_notifications(device_notify_);
        device_notify_ = nullptr;
    }
    recorder_.set_stop_callback(nullptr);
    recorder_.stop();
    controller_.set_record_queue(nullptr);
    controller_.disconnect();
    remove_tray_icon();
    settings_.save();
    Logger::instance().set_callback(nullptr);
    Logger::instance().set_log_file(L"");
    PostQuitMessage(0);
}

void App::create_controls() {
    auto make = [this](const wchar_t* cls, const wchar_t* text, DWORD style, int id,
                       DWORD ex_style = 0) {
        HWND h = CreateWindowExW(ex_style, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10,
                                 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 hinst_, nullptr);
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.ui), TRUE);
        return h;
    };

    h_device_ = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                     IDC_DEVICE, WS_EX_CLIENTEDGE);
    h_refresh_ = make(L"BUTTON", L"Обновить", BS_PUSHBUTTON | WS_TABSTOP, IDC_REFRESH);

    h_resolution_ = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                         IDC_RESOLUTION, WS_EX_CLIENTEDGE);
    h_fps_ = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_FPS,
                  WS_EX_CLIENTEDGE);
    h_backend_ = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                      IDC_BACKEND, WS_EX_CLIENTEDGE);
    h_quality_ = make(L"EDIT", L"90", ES_NUMBER | WS_TABSTOP, IDC_QUALITY,
                      WS_EX_CLIENTEDGE);
    SendMessageW(h_quality_, EM_SETLIMITTEXT, 3, 0);

    h_auto_reconnect_ = make(L"BUTTON", L"Автопереподключение камеры",
                             BS_AUTOCHECKBOX | WS_TABSTOP, IDC_AUTO_RECONNECT);
    h_connect_ = make(L"BUTTON", L"Подключить камеру", BS_PUSHBUTTON | WS_TABSTOP, IDC_CONNECT);
    h_disconnect_ = make(L"BUTTON", L"Отключить", BS_PUSHBUTTON | WS_TABSTOP, IDC_DISCONNECT);

    h_snapshot_ = make(L"BUTTON", L"Снимок  (Ctrl+Shift+F12)", BS_PUSHBUTTON | WS_TABSTOP,
                       IDC_SNAPSHOT);
    h_record_start_ = make(L"BUTTON", L"Начать запись", BS_PUSHBUTTON | WS_TABSTOP,
                           IDC_RECORD_START);
    h_record_stop_ = make(L"BUTTON", L"Остановить запись", BS_PUSHBUTTON | WS_TABSTOP,
                          IDC_RECORD_STOP);
    h_open_dir_ = make(L"BUTTON", L"Открыть каталог", BS_PUSHBUTTON | WS_TABSTOP, IDC_OPEN_DIR);
    h_report_ = make(L"BUTTON", L"Сохранить отчёт", BS_PUSHBUTTON | WS_TABSTOP, IDC_REPORT);

    h_dir_ = make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_DIR_EDIT, WS_EX_CLIENTEDGE);
    h_browse_ = make(L"BUTTON", L"Обзор…", BS_PUSHBUTTON | WS_TABSTOP, IDC_BROWSE);

    h_theme_ = make(L"BUTTON", L"Тема: тёмная", BS_PUSHBUTTON | WS_TABSTOP, IDC_THEME);
    h_tray_button_ = make(L"BUTTON", L"Свернуть в трей", BS_PUSHBUTTON | WS_TABSTOP,
                          IDC_TRAY_BUTTON);

    h_log_ = make(L"LISTBOX", L"",
                  WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                      WS_TABSTOP,
                  IDC_LOG, WS_EX_CLIENTEDGE);
    if (h_log_) {
        SendMessageW(h_log_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.mono), TRUE);
        SendMessageW(h_log_, LB_SETITEMHEIGHT, 0, S(16));
    }
    h_clear_log_ = make(L"BUTTON", L"Очистить журнал", BS_PUSHBUTTON | WS_TABSTOP,
                        IDC_CLEAR_LOG);
    h_minimize_tray_ = make(L"BUTTON", L"Сворачивать в трей при минимизации",
                            BS_AUTOCHECKBOX | WS_TABSTOP, IDC_MINIMIZE_TRAY);

    // Заполнение списков
    for (const auto& option : kResolutions) {
        SendMessageW(h_resolution_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(option.title));
    }
    for (const auto& option : kFpsOptions) {
        SendMessageW(h_fps_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option.title));
    }
    for (const auto& title : kBackendTitles) {
        SendMessageW(h_backend_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(title));
    }

    SendMessageW(h_resolution_, CB_SETCURSEL,
                 std::clamp(settings_.resolution_index, 0, kResolutionCount - 1), 0);
    SendMessageW(h_fps_, CB_SETCURSEL, std::clamp(settings_.fps_index, 0, kFpsCount - 1), 0);
    SendMessageW(h_backend_, CB_SETCURSEL, std::clamp(settings_.backend_index, 0, kBackendCount - 1),
                 0);
    SetWindowTextW(h_quality_, std::to_wstring(std::clamp(settings_.quality, 1, 100)).c_str());
    SendMessageW(h_auto_reconnect_, BM_SETCHECK,
                 settings_.auto_reconnect ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(h_minimize_tray_, BM_SETCHECK,
                 settings_.minimize_to_tray ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(h_dir_, settings_.save_dir.c_str());
}

void App::apply_theme() {
    if (panel_brush_) DeleteObject(panel_brush_);
    if (window_brush_) DeleteObject(window_brush_);
    if (log_brush_) DeleteObject(log_brush_);
    panel_brush_ = CreateSolidBrush(palette_->panel);
    window_brush_ = CreateSolidBrush(palette_->window);
    log_brush_ = CreateSolidBrush(palette_->log_bg);

    if (h_theme_) {
        SetWindowTextW(h_theme_, dark_ ? L"Тема: тёмная" : L"Тема: светлая");
    }
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

// ---------------------------------------------------------------------------
//  Разметка
// ---------------------------------------------------------------------------

void App::layout() {
    if (!hwnd_ || !h_log_) return;

    RECT client{};
    GetClientRect(hwnd_, &client);
    const int cx = client.right;
    const int cy = client.bottom;

    const int m = S(12);
    const int header_h = S(60);

    header_rc_ = {0, 0, cx, header_h};

    const int log_h = std::max<int>(S(150), cy / 4);
    log_rc_ = {m, cy - log_h - m, cx - m, cy - m};

    const int main_top = header_h + m;
    const int main_bottom = log_rc_.top - m;

    const int right_w = std::min(S(420), std::max(S(330), cx / 3));
    right_rc_ = {cx - m - right_w, main_top, cx - m, main_bottom};

    const int left_l = m;
    const int left_r = right_rc_.left - m;
    const int left_w = left_r - left_l;

    const int save_h = S(104);
    save_rc_ = {left_l, main_bottom - save_h, left_r, main_bottom};
    preview_rc_ = {left_l, main_top, left_r, save_rc_.top - m};

    // --- панель сохранения --------------------------------------------------
    {
        const int p = S(10);
        const int row_h = S(26);
        const int btn_h = S(30);
        const int gap = S(8);

        dir_label_rc_ = {save_rc_.left + p, save_rc_.top + p, save_rc_.left + p + S(72),
                         save_rc_.top + p + row_h};

        const int browse_w = S(88);
        const int edit_x = dir_label_rc_.right + S(6);
        const int edit_w =
            std::max<int>(S(120), static_cast<int>(save_rc_.right - p - browse_w - gap - edit_x));
        SetWindowPos(h_dir_, nullptr, edit_x, save_rc_.top + p, edit_w, row_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(h_browse_, nullptr, edit_x + edit_w + gap, save_rc_.top + p, browse_w,
                     row_h, SWP_NOZORDER | SWP_NOACTIVATE);

        const int buttons_y = save_rc_.top + p + row_h + S(8);
        const int avail = left_w - 2 * p - 4 * gap;
        const double weights[5] = {0.215, 0.225, 0.205, 0.2, 0.155};
        HWND buttons[5] = {h_snapshot_, h_record_start_, h_record_stop_, h_open_dir_, h_report_};
        int x = save_rc_.left + p;
        for (int i = 0; i < 5; ++i) {
            const int w = (i == 4) ? static_cast<int>(save_rc_.right - p - x)
                                   : static_cast<int>(avail * weights[i]);
            if (buttons[i]) {
                SetWindowPos(buttons[i], nullptr, x, buttons_y, std::max(w, S(40)), btn_h,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            x += w + gap;
        }
    }

    // --- правая панель ------------------------------------------------------
    {
        const int p = S(12);
        const int row_h = S(26);
        const int gap = S(8);
        const int label_h = S(17);
        int y = right_rc_.top + S(30);   // заголовок группы "Устройство"

        // Камера
        device_label_rc_ = {right_rc_.left + p, y, right_rc_.right - p, y + label_h};
        y += label_h;
        const int refresh_w = S(96);
        const int combo_w = right_rc_.right - p - refresh_w - gap - (right_rc_.left + p);
        SetWindowPos(h_device_, nullptr, right_rc_.left + p, y, std::max(combo_w, S(120)),
                     S(220), SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(h_refresh_, nullptr, right_rc_.left + p + std::max(combo_w, S(120)) + gap, y,
                     refresh_w, row_h, SWP_NOZORDER | SWP_NOACTIVATE);
        y += row_h + S(10);

        // Разрешение и FPS в одной строке
        const int half = (right_w - 2 * p - gap) / 2;
        res_label_rc_ = {right_rc_.left + p, y, right_rc_.left + p + half, y + label_h};
        fps_label_rc_ = {right_rc_.left + p + half + gap, y, right_rc_.right - p, y + label_h};
        y += label_h;
        SetWindowPos(h_resolution_, nullptr, right_rc_.left + p, y, half, S(220),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(h_fps_, nullptr, right_rc_.left + p + half + gap, y, half, S(220),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        y += row_h + S(10);

        // Бэкенд
        backend_label_rc_ = {right_rc_.left + p, y, right_rc_.right - p, y + label_h};
        y += label_h;
        SetWindowPos(h_backend_, nullptr, right_rc_.left + p, y,
                     right_rc_.right - right_rc_.left - 2 * p, S(220),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        y += row_h + S(10);

        // Качество JPEG + автопереподключение
        quality_label_rc_ = {right_rc_.left + p, y, right_rc_.left + p + S(120), y + label_h};
        y += label_h;
        SetWindowPos(h_quality_, nullptr, right_rc_.left + p, y, S(64), row_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(h_auto_reconnect_, nullptr, right_rc_.left + p + S(80), y,
                     right_rc_.right - p - (right_rc_.left + p + S(80)), S(24),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        y += row_h + S(10);

        // Кнопки подключения
        const int btn_w = (right_w - 2 * p - gap) / 2;
        SetWindowPos(h_connect_, nullptr, right_rc_.left + p, y, btn_w, S(30),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(h_disconnect_, nullptr, right_rc_.left + p + btn_w + gap, y, btn_w, S(30),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        y += S(30) + S(12);

        info_area_rc_ = {right_rc_.left + p, y + S(22), right_rc_.right - p,
                         right_rc_.bottom - S(10)};
    }

    // --- журнал -------------------------------------------------------------
    {
        const int p = S(10);
        const int btn_h = S(28);
        SetWindowPos(h_clear_log_, nullptr, log_rc_.right - p - S(140), log_rc_.top + p, S(140),
                     btn_h, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(h_minimize_tray_, nullptr, log_rc_.left + S(290), log_rc_.top + p,
                     S(330), btn_h, SWP_NOZORDER | SWP_NOACTIVATE);
        const int log_top = static_cast<int>(log_rc_.top) + p + btn_h + S(6);
        SetWindowPos(h_log_, nullptr, static_cast<int>(log_rc_.left + p), log_top,
                     static_cast<int>(log_rc_.right - log_rc_.left - 2 * p),
                     static_cast<int>(log_rc_.bottom - p - log_top), SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // --- кнопки в заголовке -------------------------------------------------
    {
        const int btn_h = S(30);
        const int tray_w = S(150);
        const int theme_w = S(120);
        const int y = (header_h - btn_h) / 2;
        SetWindowPos(h_tray_button_, nullptr, cx - m - tray_w, y, tray_w, btn_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(h_theme_, nullptr, cx - m - tray_w - S(8) - theme_w, y, theme_w, btn_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::on_size(int cx, int cy) {
    (void)cx;
    (void)cy;
    layout();
}

void App::paint(HDC dc, const RECT& client) {
    fill_rect(dc, client, palette_->window);
    draw_header(dc);
    draw_preview(dc);
    draw_devices_panel(dc);
    draw_log_panel(dc);
}

void App::draw_header(HDC dc) {
    fill_rect(dc, header_rc_, palette_->header);
    RECT line = {header_rc_.left, header_rc_.bottom - 1, header_rc_.right, header_rc_.bottom};
    fill_rect(dc, line, palette_->border);

    const int m = S(16);
    // Кнопки «Тема» и «Свернуть в трей» стоят в заголовке справа: под текст
    // оставляем всё, что левее индикатора состояния.
    const int btns_left = header_rc_.right - S(12) - S(150) - S(8) - S(120);
    const int status_left = std::max<int>(header_rc_.right / 2, btns_left - S(330));

    RECT title_rc = {m, S(8), status_left - S(12), S(34)};
    draw_text(dc, L"Система видеонаблюдения на базе веб-камеры", title_rc, fonts_.title,
              palette_->text);

    RECT subtitle_rc = {m, S(32), status_left - S(12), S(54)};
    draw_text(dc,
              L"Лабораторная работа №4 · вариант А5 · скрытый режим фотонаблюдения",
              subtitle_rc, fonts_.ui, palette_->text_dim);

    // Индикатор состояния камеры
    COLORREF color = palette_->text_dim;
    switch (state_) {
        case CameraState::Connected:    color = palette_->ok; break;
        case CameraState::Connecting:   color = palette_->warn; break;
        case CameraState::Error:        color = palette_->error; break;
        case CameraState::Disconnected: color = palette_->text_dim; break;
    }

    RECT status_rc = {status_left, S(14), btns_left - S(12), S(44)};
    draw_status_dot(dc, status_rc.left + S(8), (status_rc.top + status_rc.bottom) / 2, S(6),
                    color);
    RECT status_text_rc = status_rc;
    status_text_rc.left += S(22);
    std::wstring state_line = status_text_.empty() ? L"Камера не подключена" : status_text_;
    if (state_ == CameraState::Connected) {
        const VideoMode mode = controller_.mode();
        if (mode.width > 0) {
            state_line += format(L"  ·  %d×%d", mode.width, mode.height);
        }
        const double fps = controller_.measured_fps();
        if (fps > 0.5) state_line += format(L"  ·  %.0f к/с", fps);
    }
    draw_text(dc, state_line, status_text_rc, fonts_.ui_bold, color);

    // Индикатор записи (мигает раз в секунду)
    if (recorder_.is_recording()) {
        RECT rec_rc = {btns_left - S(330), S(44), btns_left - S(12), S(58)};
        if (blink_on_) {
            RECT dot_rc = {rec_rc.left, rec_rc.top, rec_rc.left + S(12), rec_rc.bottom};
            const int cy = (rec_rc.top + rec_rc.bottom) / 2;
            draw_status_dot(dc, dot_rc.left + S(6), cy, S(5), palette_->error);
        }
        rec_rc.left += S(20);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - record_started_);
        draw_text(dc, L"ИДЁТ ЗАПИСЬ  " + seconds_to_hms(elapsed.count() / 1000.0), rec_rc,
                  fonts_.ui, palette_->error);
    }
}

void App::draw_preview(HDC dc) {
    fill_rect(dc, preview_rc_, palette_->preview_bg);

    CameraFrame frame;
    const bool has_frame = controller_.get_latest(frame) && frame.valid();

    if (has_frame) {
        const int pw = preview_rc_.right - preview_rc_.left;
        const int ph = preview_rc_.bottom - preview_rc_.top;
        const double scale = std::min(static_cast<double>(pw) / frame.width,
                                      static_cast<double>(ph) / frame.height);
        const int w = std::max(1, static_cast<int>(frame.width * scale));
        const int h = std::max(1, static_cast<int>(frame.height * scale));
        const int x = preview_rc_.left + (pw - w) / 2;
        const int y = preview_rc_.top + (ph - h) / 2;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = frame.width;
        bmi.bmiHeader.biHeight = -frame.height;   // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 24;
        bmi.bmiHeader.biCompression = BI_RGB;

        const int old_mode = SetStretchBltMode(dc, HALFTONE);
        POINT old_org{};
        SetBrushOrgEx(dc, 0, 0, &old_org);
        StretchDIBits(dc, x, y, w, h, 0, 0, frame.width, frame.height, frame.pixels.data(), &bmi,
                      DIB_RGB_COLORS, SRCCOPY);
        SetStretchBltMode(dc, old_mode);
        SetBrushOrgEx(dc, old_org.x, old_org.y, nullptr);

        // Отметка даты и времени, как в кадре видеорегистратора.
        SYSTEMTIME now;
        GetLocalTime(&now);
        const std::wstring stamp = human_timestamp(now);
        RECT stamp_rc = {x + S(10), y + h - S(28), x + S(230), y + h - S(8)};
        HBRUSH shade = CreateSolidBrush(RGB(0, 0, 0));
        RECT shadow = {stamp_rc.left - S(6), stamp_rc.top - S(2), stamp_rc.right + S(6),
                       stamp_rc.bottom + S(2)};
        FillRect(dc, &shadow, shade);
        DeleteObject(shade);
        draw_text(dc, stamp, stamp_rc, fonts_.ui, RGB(235, 235, 235));
    } else {
        RECT text_rc = preview_rc_;
        draw_text(dc, state_ == CameraState::Connecting ? L"Идёт подключение камеры…"
                                                       : L"Нет видеосигнала",
                  text_rc, fonts_.title, palette_->text_dim, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        RECT hint_rc = preview_rc_;
        hint_rc.top += S(40);
        draw_text(dc, L"Выберите камеру и нажмите «Подключить камеру»", hint_rc, fonts_.ui,
                  palette_->text_dim, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    frame_rect(dc, preview_rc_, palette_->border);
}

void App::draw_devices_panel(HDC dc) {
    fill_rect(dc, right_rc_, palette_->panel);
    frame_rect(dc, right_rc_, palette_->border);

    // Заголовки групп и подписи к элементам управления
    RECT group_title = {right_rc_.left + S(12), right_rc_.top + S(8), right_rc_.right - S(12),
                        right_rc_.top + S(28)};
    draw_text(dc, L"Устройство видеозахвата", group_title, fonts_.ui_bold, palette_->text);

    auto label = [&](const RECT& rc, const std::wstring& text) {
        draw_text(dc, text, rc, fonts_.ui, palette_->text_dim,
                  DT_LEFT | DT_SINGLELINE | DT_BOTTOM);
    };

    label(device_label_rc_, L"Камера");
    label(res_label_rc_, L"Разрешение");
    label(fps_label_rc_, L"Частота кадров, к/с");
    label(backend_label_rc_, L"Бэкенд захвата");
    label(quality_label_rc_, L"Качество JPEG (1–100)");

    // Информационный блок
    RECT info_title = {info_area_rc_.left, info_area_rc_.top - S(20), info_area_rc_.right,
                       info_area_rc_.top};
    draw_text(dc, L"Информация об устройстве", info_title, fonts_.ui_bold, palette_->text);

    const struct {
        const wchar_t* name;
        const std::wstring* value;
    } lines[] = {
        {L"Имя", &info_name_},
        {L"Режим", &info_mode_},
        {L"Формат", &info_format_},
        {L"Бэкенд", &info_backend_},
        {L"Статистика", &info_stats_},
        {L"Запись", &info_record_},
    };

    int y = info_area_rc_.top + S(4);
    const int label_w = S(96);
    for (const auto& line : lines) {
        RECT name_rc = {info_area_rc_.left, y, info_area_rc_.left + label_w, y + S(18)};
        RECT value_rc = {name_rc.right, y, info_area_rc_.right, y + S(18)};
        draw_text(dc, line.name, name_rc, fonts_.ui, palette_->text_dim);
        draw_text(dc, line.value->empty() ? L"—" : *line.value, value_rc, fonts_.ui_bold,
                  palette_->text);
        y += S(20);
    }

    RECT hint_rc = {info_area_rc_.left, info_area_rc_.bottom - S(36), info_area_rc_.right,
                    info_area_rc_.bottom};
    draw_text(dc, L"Скрытый режим: закройте окно кнопкой «Свернуть в трей» —",
              hint_rc, fonts_.ui, palette_->text_dim);
    RECT hint2 = hint_rc;
    hint2.top += S(16);
    hint2.bottom += S(16);
    draw_text(dc, L"снимки по Ctrl+Shift+F12 сохраняются без открытия окна.", hint2, fonts_.ui,
              palette_->text_dim);
}

void App::draw_log_panel(HDC dc) {
    fill_rect(dc, log_rc_, palette_->panel);
    frame_rect(dc, log_rc_, palette_->border);

    RECT title = {log_rc_.left + S(10), log_rc_.top + S(8), log_rc_.right - S(170),
                  log_rc_.top + S(30)};
    draw_text(dc, L"Журнал событий наблюдения", title, fonts_.ui_bold, palette_->text);

    // Панель сохранения
    fill_rect(dc, save_rc_, palette_->panel);
    frame_rect(dc, save_rc_, palette_->border);
    RECT save_title = {save_rc_.left + S(10), save_rc_.top - S(2), save_rc_.right - S(10),
                       save_rc_.top + S(14)};
    draw_text(dc, L"Сохранение материалов", save_title, fonts_.ui_bold, palette_->text_dim);
    draw_text(dc, L"Каталог", dir_label_rc_, fonts_.ui, palette_->text_dim,
              DT_LEFT | DT_SINGLELINE | DT_BOTTOM);
}

// ---------------------------------------------------------------------------
//  Таймер и обновление состояния
// ---------------------------------------------------------------------------

void App::on_timer() {
    ++blink_counter_;
    blink_on_ = (blink_counter_ % 2) == 0;

    if (recorder_.is_recording()) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - record_started_);
        info_record_ = L"идёт, " + seconds_to_hms(elapsed.count() / 1000.0);
        update_tray_tooltip();
    }

    update_info_labels();
    update_enabled_state();
    InvalidateRect(hwnd_, &header_rc_, FALSE);
}

void App::on_new_frame() {
    frame_posted_.store(false);
    InvalidateRect(hwnd_, &preview_rc_, FALSE);
}

void App::on_state_message(CameraState state, const std::wstring& text) {
    state_ = state;
    status_text_ = text;

    switch (state) {
        case CameraState::Connected:
            add_log(LogLevel::Info, text);
            break;
        case CameraState::Error:
            add_log(LogLevel::Error, text);
            break;
        case CameraState::Disconnected:
            add_log(LogLevel::Warn, text);
            break;
        case CameraState::Connecting:
            add_log(LogLevel::Info, text);
            break;
    }

    update_info_labels();
    update_enabled_state();
    update_tray_tooltip();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::post_state(CameraState state, const std::wstring& text) {
    auto* payload = new std::pair<CameraState, std::wstring>(state, text);
    if (!PostMessageW(hwnd_, WM_APP_STATE, 0, reinterpret_cast<LPARAM>(payload))) {
        delete payload;
    }
}

void App::update_info_labels() {
    const DeviceInfo info = controller_.info();
    const VideoMode mode = controller_.mode();

    info_name_ = info.name.empty() ? L"не подключена" : info.name;

    if (mode.width > 0 && mode.height > 0) {
        info_mode_ = format(L"%d × %d", mode.width, mode.height);
        const double fps = controller_.measured_fps();
        if (fps > 0.5) info_mode_ += format(L", %.1f к/с", fps);
        else if (mode.fps > 0.5) info_mode_ += format(L", %.0f к/с", mode.fps);
    } else {
        info_mode_ = L"—";
    }

    std::wstring format_text = pixel_format_name(mode.format);
    if (!mode.fourcc.empty()) format_text += L" (" + mode.fourcc + L")";
    info_format_ = (mode.format == PixelFormat::Unknown && mode.fourcc.empty()) ? L"—" : format_text;

    info_backend_ = state_ == CameraState::Connected ? controller_.backend() : L"—";

    if (controller_.frame_count() > 0) {
        info_stats_ = format(L"кадров %llu, в очереди записи потеряно %zu",
                             controller_.frame_count(), record_queue_.dropped());
    } else {
        info_stats_ = L"—";
    }

    if (recorder_.is_recording()) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - record_started_);
        info_record_ = L"идёт, " + seconds_to_hms(elapsed.count() / 1000.0);
    } else {
        info_record_ = L"нет";
    }
}

void App::update_enabled_state() {
    const bool connected = state_ == CameraState::Connected;
    const bool busy = state_ == CameraState::Connecting;
    const bool recording = recorder_.is_recording();
    const bool has_devices = !devices_.empty();

    auto enable = [](HWND h, bool state) {
        if (h) EnableWindow(h, state ? TRUE : FALSE);
    };

    enable(h_connect_, !connected && !busy && has_devices);
    enable(h_disconnect_, connected || busy || state_ == CameraState::Error);
    enable(h_device_, !connected && !busy);
    enable(h_refresh_, !busy);
    enable(h_resolution_, !connected && !busy);
    enable(h_fps_, !connected && !busy);
    enable(h_backend_, !connected && !busy);
    enable(h_snapshot_, connected);
    enable(h_record_start_, connected && !recording);
    enable(h_record_stop_, recording);
    enable(h_open_dir_, true);
    enable(h_report_, true);
    enable(h_clear_log_, true);
    enable(h_dir_, !recording);
    enable(h_browse_, !recording);
    enable(h_quality_, !recording);
    enable(h_auto_reconnect_, !connected && !busy);
}

}  // namespace cam
