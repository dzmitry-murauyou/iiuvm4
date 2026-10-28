// ===========================================================================
//  Лабораторная работа №4. Система видеонаблюдения на базе веб-камеры
//  Вариант А5. Скрытый режим фотонаблюдения
//
//  Всё приложение — один файл (main.cpp). Оформление в стиле ЛР №1:
//  GDI+, фон bg.jpg с затемнением, полупрозрачные карточки, Segoe UI.
//
//  Что умеет (только фотонаблюдение, как в задании):
//    * предпросмотр с веб-камеры в реальном времени;
//    * снимок JPEG — кнопкой и глобальной клавишей Ctrl+Shift+F12;
//    * скрытый режим: окно в трей, снимок без окна, всплывающие уведомления;
//    * журнал событий и выгрузка отчёта.
// ===========================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <commctrl.h>
#include <dbt.h>
#include <dshow.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "gdiplus.lib")

// Common Controls v6 — кнопки и списки выглядят как в Windows 11.
#pragma comment(linker, "\"/manifestdependency:type='win32' \
 name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
 processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

using namespace Gdiplus;
using cv::Mat;

// ===========================================================================
//  1. Оформление — цвета и размеры как в ЛР №1 (theme.h), плюс масштаб DPI
// ===========================================================================
namespace ui {

const Color Bg(255, 10, 14, 24);       // базовый фон под картинкой
const Color Card(215, 18, 26, 44);     // карточка
const Color Txt(255, 235, 240, 248);   // основной текст
const Color Sub(255, 165, 178, 198);   // подписи
const Color Dim(255, 120, 130, 150);   // неактивное
const Color Line(255, 70, 95, 135);    // рамки, разделители
const Color Good(255, 46, 204, 113);   // камера подключена
const Color Warn(255, 241, 196, 15);   // подключение / предупреждение
const Color Bad(255, 235, 87, 87);     // ошибка

const Color Field(235, 12, 18, 30);    // поле ввода, выпадающий список
const Color Btn(255, 30, 42, 64);      // обычная кнопка
const Color BtnHot(255, 42, 58, 86);   // кнопка под курсором / нажата
const Color Preview(255, 8, 10, 16);   // подложка под видео

// Геометрия в DIP (независимых от DPI пикселях) — умножается на масштаб.
const int Pad = 28;
const int Gap = 16;
const int HeaderH = 96;
const int LogH = 170;
const int SaveH = 96;
const int PanelW = 360;
const int RowH = 26;
const REAL Radius = 6.0f;

}  // namespace ui

int g_dpi = 96;

// Перевод «дизайнерских» пикселей в реальные: на 125/150 % всё
// (включая размеры шрифтов) увеличивается пропорционально.
int S(int dip) { return MulDiv(dip, g_dpi, 96); }

// ===========================================================================
//  2. Утилиты: строки, время, файлы
// ===========================================================================

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buffer[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buffer, std::size(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    return buffer;
}

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0,
                                         nullptr, nullptr);
    std::string out((size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size, nullptr,
                        nullptr);
    return out;
}

// Системная ANSI-кодировка: нужна, если OpenCV не понял UTF-8 путь
// (например, C:\Users\Дмитрий\Pictures).
std::string ToAnsi(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_ACP, 0, text.c_str(), (int)text.size(), nullptr, 0,
                                         nullptr, nullptr);
    std::string out((size_t)size, '\0');
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), (int)text.size(), out.data(), size, nullptr,
                        nullptr);
    return out;
}

std::wstring NowStamp(bool milliseconds) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return milliseconds ? Format(L"%02u.%02u.%04u %02u:%02u:%02u.%03u", st.wDay, st.wMonth,
                                 st.wYear, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds)
                        : Format(L"%02u.%02u.%04u %02u:%02u:%02u", st.wDay, st.wMonth, st.wYear,
                                 st.wHour, st.wMinute, st.wSecond);
}

std::wstring FileStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return Format(L"%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute, st.wSecond);
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    const wchar_t last = dir.back();
    return (last == L'\\' || last == L'/') ? dir + name : dir + L"\\" + name;
}

std::wstring FileName(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

bool FileExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

unsigned long long FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return ((unsigned long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
}

// Создаёт каталог вместе с родительскими.
bool EnsureDir(const std::wstring& path) {
    if (path.empty()) return false;
    if (DirExists(path)) return true;
    std::wstring partial;
    for (size_t i = 0; i < path.size(); ++i) {
        partial += path[i];
        const bool boundary = (path[i] == L'\\' || path[i] == L'/');
        if (!boundary && i + 1 != path.size()) continue;
        if (partial.size() <= 3 && partial[1] == L':') continue;   // "C:\"
        std::wstring probe = partial;
        while (!probe.empty() && (probe.back() == L'\\' || probe.back() == L'/')) probe.pop_back();
        if (!probe.empty() && !DirExists(probe) && !CreateDirectoryW(probe.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    return DirExists(path);
}

std::wstring DefaultSaveDir() {
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, SHGFP_TYPE_CURRENT, path))) {
        return JoinPath(path, L"CameraLab4");
    }
    return L"snapshots";
}

// Имя файла с защитой от совпадения в одну и ту же секунду:
// snapshot_20261029_183012.jpg, snapshot_20261029_183012_1.jpg ...
std::wstring SnapshotPath(const std::wstring& dir) {
    const std::wstring base = JoinPath(dir, L"snapshot_" + FileStamp());
    std::wstring candidate = base + L".jpg";
    for (int i = 1; FileExists(candidate); ++i) {
        candidate = base + L"_" + std::to_wstring(i) + L".jpg";
    }
    return candidate;
}

bool ImwriteAny(const std::wstring& path, const Mat& image, int quality) {
    const std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, std::clamp(quality, 1, 100)};
    try {
        if (cv::imwrite(ToUtf8(path), image, params)) return true;
        return cv::imwrite(ToAnsi(path), image, params);
    } catch (const cv::Exception&) {
        return false;
    }
}

// ===========================================================================
//  3. Рисование в стиле ЛР №1 (аналог gfx.cpp)
// ===========================================================================

// Фон: просто тёмная заливка с очень мягким градиентом (без картинки).
void PaintBg(Graphics& g, int w, int h) {
    SolidBrush base(ui::Bg);
    g.FillRectangle(&base, 0, 0, w, h);

    LinearGradientBrush sheen(Point(0, 0), Point(0, h), Color(255, 26, 34, 54),
                              Color(255, 8, 11, 20));
    g.FillRectangle(&sheen, 0, 0, w, h);
}

void FillRectG(Graphics& g, const RECT& rc, const Color& color) {
    SolidBrush brush(color);
    g.FillRectangle(&brush, (INT)rc.left, (INT)rc.top, (INT)(rc.right - rc.left),
                    (INT)(rc.bottom - rc.top));
}

void Rect(Graphics& g, const RECT& rc, const Color& fill, const Color& border, REAL radius) {
    GraphicsPath path;
    const REAL x = (REAL)rc.left, y = (REAL)rc.top;
    const REAL w = (REAL)(rc.right - rc.left), h = (REAL)(rc.bottom - rc.top);
    const REAL d = radius * 2;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
    SolidBrush brush(fill);
    g.FillPath(&brush, &path);
    Pen pen(border, 1.0f);
    g.DrawPath(&pen, &path);
}

// Карточка — полупрозрачная плашка с рамкой (Card() из ЛР №1).
void Card(Graphics& g, const RECT& rc) {
    Rect(g, rc, ui::Card, ui::Line, S(ui::Radius * 4) / 4.0f);
}

void TextG(Graphics& g, const std::wstring& text, const RECT& rc, Font* font, const Color& color,
           StringAlignment align = StringAlignmentNear) {
    StringFormat fmt;
    fmt.SetAlignment(align);
    fmt.SetLineAlignment(StringAlignmentCenter);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush brush(color);
    RectF layout((REAL)rc.left, (REAL)rc.top, (REAL)(rc.right - rc.left),
                 (REAL)(rc.bottom - rc.top));
    g.DrawString(text.c_str(), -1, font, layout, &fmt, &brush);
}

// Строка «подпись — значение» внутри карточки (Row() из ЛР №1).
void Row(Graphics& g, const RECT& card, int y, const std::wstring& label,
         const std::wstring& value, Font* label_font, Font* value_font, const Color& value_color) {
    RECT l{card.left + S(18), y, card.left + S(128), y + S(ui::RowH)};
    RECT v{l.right, y, card.right - S(18), y + S(ui::RowH)};
    TextG(g, label, l, label_font, ui::Sub);
    TextG(g, value, v, value_font, value_color);
}

void Separator(Graphics& g, const RECT& card, int y) {
    Pen pen(ui::Line, 1.0f);
    g.DrawLine(&pen, (REAL)(card.left + S(18)), (REAL)y, (REAL)(card.right - S(18)), (REAL)y);
}

void Dot(Graphics& g, int cx, int cy, int r, const Color& color) {
    SolidBrush brush(color);
    g.FillEllipse(&brush, cx - r, cy - r, r * 2, r * 2);
}

// ===========================================================================
//  4. Журнал событий
// ===========================================================================

enum class LogLevel { Info, Warn, Error, Background };

Color LevelColor(LogLevel level) {
    switch (level) {
        case LogLevel::Error:      return ui::Bad;
        case LogLevel::Warn:       return ui::Warn;
        case LogLevel::Background: return ui::Good;
        case LogLevel::Info:       default: return ui::Txt;
    }
}

// ===========================================================================
//  5. Камера: перечисление устройств и поток захвата
// ===========================================================================

struct Frame {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> bgr;   // 24 бита, BGR, построчно
    bool ok() const { return width > 0 && height > 0 && !bgr.empty(); }
};

struct CameraDevice {
    std::wstring name;   // «USB2.0 HD UVC WebCam»
    std::wstring path;   // символьный путь устройства
};

// Перечисление камер через DirectShow (ICreateDevEnum + IPropertyBag).
// Порядок совпадает с тем, который использует OpenCV с бэкендом DSHOW.
std::vector<CameraDevice> EnumerateCameras() {
    std::vector<CameraDevice> result;

    ICreateDevEnum* dev_enum = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dev_enum)))) {
        return result;
    }

    IEnumMoniker* enum_moniker = nullptr;
    if (dev_enum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enum_moniker, 0) == S_OK) {
        IMoniker* moniker = nullptr;
        while (enum_moniker->Next(1, &moniker, nullptr) == S_OK) {
            IPropertyBag* bag = nullptr;
            CameraDevice device;
            if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) {
                VARIANT value;
                VariantInit(&value);
                if (SUCCEEDED(bag->Read(L"FriendlyName", &value, nullptr)) && value.bstrVal) {
                    device.name = value.bstrVal;
                }
                VariantClear(&value);
                VariantInit(&value);
                if (SUCCEEDED(bag->Read(L"DevicePath", &value, nullptr)) && value.bstrVal) {
                    device.path = value.bstrVal;
                }
                VariantClear(&value);
                bag->Release();
            }
            if (device.name.empty()) device.name = L"Камера #" + std::to_wstring(result.size());
            result.push_back(std::move(device));
            moniker->Release();
        }
    }
    if (enum_moniker) enum_moniker->Release();
    dev_enum->Release();
    return result;
}

enum class CamState { Off, Opening, On, Failed };

// Поток захвата: открывает камеру, постоянно читает кадры, хранит последний.
// GUI только копирует последний кадр — интерфейс не ждёт чтения с камеры.
class Camera {
public:
    Camera() = default;
    ~Camera() { Stop(); }

    void SetNotifyWindow(HWND hwnd) { hwnd_ = hwnd; }

    void Start(int index, int width, int height, double fps) {
        Stop();
        running_.store(true);
        state_ = CamState::Opening;
        worker_ = std::thread(&Camera::Run, this, index, width, height, fps);
    }

    void Stop() {
        if (!running_.exchange(false)) {
            if (worker_.joinable()) worker_.join();
            return;
        }
        if (worker_.joinable()) worker_.join();
        state_ = CamState::Off;
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = Frame{};
    }

    bool Latest(Frame& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!latest_.ok()) return false;
        out = latest_;
        return true;
    }

    CamState State() const { return state_.load(); }
    double MeasuredFps() const { return measured_fps_.load(); }
    unsigned long long FrameCount() const { return frame_count_.load(); }
    std::wstring FormatName() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return format_;
    }

private:
    void Run(int index, int want_w, int want_h, double want_fps) {
        cv::VideoCapture capture;
        bool opened = false;
        try {
            opened = capture.open(index, cv::CAP_DSHOW);
            if (!opened) opened = capture.open(index);
        } catch (const cv::Exception&) {
            opened = false;
        }
        if (!opened) {
            Notify(CamState::Failed, L"Не удалось открыть камеру (занята или недоступна)");
            running_.store(false);
            return;
        }

        // MJPG предпочтительнее: при том же USB-трафике доступно большее разрешение.
        capture.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
        if (want_w > 0) capture.set(cv::CAP_PROP_FRAME_WIDTH, want_w);
        if (want_h > 0) capture.set(cv::CAP_PROP_FRAME_HEIGHT, want_h);
        if (want_fps > 0) capture.set(cv::CAP_PROP_FPS, want_fps);

        Mat frame;
        try {
            capture.read(frame);
        } catch (const cv::Exception&) {
            frame = Mat();
        }
        if (frame.empty()) {
            capture.release();
            Notify(CamState::Failed,
                   L"Камера не отдаёт кадры (проверьте доступ к камере в Windows)");
            running_.store(false);
            return;
        }

        {
            const int code = (int)capture.get(cv::CAP_PROP_FOURCC);
            std::string fourcc(4, '\0');
            for (int i = 0; i < 4; ++i) fourcc[i] = (char)((code >> (8 * i)) & 0xFF);
            std::lock_guard<std::mutex> lock(mutex_);
            format_ = std::wstring(fourcc.begin(), fourcc.end());
            if (format_.size() != 4) format_ = L"BGR24";
            for (wchar_t& ch : format_) {
                if (ch < 32) ch = L'?';
            }
        }
        Notify(CamState::On, Format(L"Камера подключена: %d × %d", frame.cols, frame.rows));

        auto fps_start = std::chrono::steady_clock::now();
        unsigned long long fps_frames = 0;
        int failures = 0;

        while (running_.load()) {
            try {
                capture.read(frame);
            } catch (const cv::Exception&) {
                frame = Mat();
            }

            if (frame.empty()) {
                if (++failures > 30) break;   // камеру выдернули
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            failures = 0;

            if (frame.channels() == 1) {
                cv::cvtColor(frame, frame, cv::COLOR_GRAY2BGR);
            } else if (frame.channels() == 4) {
                cv::cvtColor(frame, frame, cv::COLOR_BGRA2BGR);
            }

            Frame out;
            out.width = frame.cols;
            out.height = frame.rows;
            const size_t row = (size_t)frame.cols * 3;
            out.bgr.resize(row * (size_t)frame.rows);
            if (frame.step == row) {
                memcpy(out.bgr.data(), frame.data, row * (size_t)frame.rows);
            } else {
                for (int y = 0; y < frame.rows; ++y) {
                    memcpy(out.bgr.data() + row * (size_t)y, frame.ptr(y), row);
                }
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                latest_ = std::move(out);
            }
            frame_count_.fetch_add(1);

            ++fps_frames;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - fps_start);
            if (elapsed.count() >= 1000) {
                measured_fps_.store((double)fps_frames * 1000.0 / (double)elapsed.count());
                fps_frames = 0;
                fps_start = std::chrono::steady_clock::now();
            }

            if (hwnd_) PostMessageW(hwnd_, WM_APP + 2, 0, 0);   // «новый кадр»
        }

        capture.release();
        measured_fps_.store(0.0);
        if (running_.load()) {
            Notify(CamState::Failed, L"Камера перестала отдавать кадры — подключите её заново");
        }
        running_.store(false);
    }

    void Notify(CamState state, const std::wstring& text) {
        state_ = state;
        auto* payload = new std::wstring(text);
        if (!PostMessageW(hwnd_, WM_APP + 1, (WPARAM)state, (LPARAM)payload)) delete payload;
    }

    HWND hwnd_ = nullptr;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<CamState> state_{CamState::Off};

    mutable std::mutex mutex_;
    Frame latest_;
    std::wstring format_;

    std::atomic<double> measured_fps_{0.0};
    std::atomic<unsigned long long> frame_count_{0};
};

// ===========================================================================
//  6. Главное окно
// ===========================================================================

const wchar_t* kClassName = L"Lab4CameraWindow";
const wchar_t* kTitle = L"Система видеонаблюдения";

const wchar_t* kResTitles[] = {L"1920 × 1080", L"1280 × 720", L"1024 × 768",
                               L"960 × 540",   L"800 × 600",  L"640 × 480"};
const int kResW[] = {1920, 1280, 1024, 960, 800, 640};
const int kResH[] = {1080, 720, 768, 540, 600, 480};

// Снимок можно сделать двумя одиночными клавишами — «=» и «-» (без модификаторов),
// плюс резервная комбинация Ctrl+Shift+F12. Трей живёт только на кнопке.
const UINT kSnapshotKey1 = VK_OEM_PLUS;    // «=»
const UINT kSnapshotKey2 = VK_OEM_MINUS;   // «-»
const UINT kSnapshotMods = 0;

const wchar_t* kFpsTitles[] = {L"30", L"25", L"20", L"15", L"10"};
const double kFpsValues[] = {30.0, 25.0, 20.0, 15.0, 10.0};

struct LogEntry {
    LogLevel level;
    std::wstring text;
};

class App {
public:
    enum : UINT {
        WM_STATE = WM_APP + 1,   // состояние камеры из потока захвата
        WM_FRAME = WM_APP + 2,   // готов новый кадр
        WM_TRAY = WM_APP + 3,    // события значка в трее
    };

    enum : int {
        HK_SNAPSHOT_KEY1 = 1,  // «=»
        HK_SNAPSHOT_KEY2,      // «-»
        HK_SNAPSHOT_ALT,       // Ctrl+Shift+F12
    };

    enum : int {
        ID_CAMERA = 1001,
        ID_REFRESH,
        ID_RES,
        ID_FPS,
        ID_CONNECT,
        ID_DISCONNECT,
        ID_SNAPSHOT,
        ID_DIR,
        ID_BROWSE,
        ID_REPORT,
        ID_TRAY_BUTTON,
        ID_LOG,
        ID_MENU_SHOW = 2001,
        ID_MENU_SNAPSHOT,
        ID_MENU_EXIT,
    };

    App() = default;
    ~App() { Cleanup(); }

    bool Init(HINSTANCE inst, int show) {
        inst_ = inst;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &App::WndProc;
        wc.hInstance = inst_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(101), IMAGE_ICON, 0, 0,
                                     LR_DEFAULTSIZE | LR_SHARED);
        if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = wc.hIcon;
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return false;

        hwnd_ = CreateWindowExW(0, kClassName, kTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, S(1140), S(800), nullptr, nullptr,
                                inst_, this);
        if (!hwnd_) return false;

        ShowWindow(hwnd_, show);
        UpdateWindow(hwnd_);
        return true;
    }

    int Run() {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return (int)msg.wParam;
    }

private:
    // -----------------------------------------------------------------------
    //  Окно и сообщения
    // -----------------------------------------------------------------------
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<App*>(cs->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        return self->Handle(msg, wp, lp);
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE:
                OnCreate();
                return 0;

            case WM_SIZE:
                Layout();
                return 0;

            case WM_GETMINMAXINFO: {
                auto* info = reinterpret_cast<MINMAXINFO*>(lp);
                info->ptMinTrackSize.x = S(980);
                info->ptMinTrackSize.y = S(700);
                return 0;
            }

            case WM_ERASEBKGND:
                return 1;   // фон рисуем сами

            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd_, &ps);
                RECT rc;
                GetClientRect(hwnd_, &rc);
                if (rc.right > 0 && rc.bottom > 0) {
                    // Двойная буферизация: видео не мерцает на 30 кадрах в секунду.
                    HDC mem = CreateCompatibleDC(dc);
                    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
                    HGDIOBJ old = SelectObject(mem, bmp);
                    Paint(mem, rc);
                    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
                    SelectObject(mem, old);
                    DeleteObject(bmp);
                    DeleteDC(mem);
                }
                EndPaint(hwnd_, &ps);
                return 0;
            }

            case WM_TIMER:
                OnTimer();
                return 0;

            case WM_COMMAND:
                OnCommand(LOWORD(wp), HIWORD(wp));
                return 0;

            case WM_HOTKEY:
                OnHotkey((int)wp);
                return 0;

            case WM_DEVICECHANGE:
                if (wp == DBT_DEVNODES_CHANGED || wp == DBT_DEVICEARRIVAL ||
                    wp == DBT_DEVICEREMOVECOMPLETE) {
                    RefreshDevices(false);
                }
                return TRUE;

            case WM_STATE: {
                auto* text = reinterpret_cast<std::wstring*>(lp);
                OnCameraState((CamState)wp, text ? *text : L"");
                delete text;
                return 0;
            }

            case WM_FRAME:
                InvalidateRect(hwnd_, &preview_rc_, FALSE);
                return 0;

            case WM_TRAY:
                OnTray(lp);
                return 0;

            case WM_DPICHANGED: {
                // Сменился монитор или масштаб — пересчитываем шрифты и раскладку.
                g_dpi = HIWORD(wp);
                CreateFonts();
                RECT* rc = reinterpret_cast<RECT*>(lp);
                SetWindowPos(hwnd_, nullptr, rc->left, rc->top, rc->right - rc->left,
                             rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
                ApplyFontsToControls();
                Layout();
                return 0;
            }

            case WM_DRAWITEM: {
                auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
                if (!dis) break;
                switch (dis->CtlType) {
                    case ODT_BUTTON:   DrawButton(dis); return TRUE;
                    case ODT_COMBOBOX: DrawCombo(dis); return TRUE;
                    case ODT_LISTBOX:  DrawLogItem(dis); return TRUE;
                    default: break;
                }
                break;
            }

            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX: {
                HDC dc = (HDC)wp;
                SetTextColor(dc, RGB(235, 240, 248));
                SetBkColor(dc, RGB(12, 18, 30));
                return (LRESULT)field_brush_;
            }

            case WM_CLOSE:
                DestroyWindow(hwnd_);
                return 0;

            case WM_DESTROY:
                Cleanup();
                PostQuitMessage(0);
                return 0;

            default:
                break;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    void OnCreate() {
        field_brush_ = CreateSolidBrush(RGB(12, 18, 30));
        CreateFonts();
        CreateControls();

        camera_.SetNotifyWindow(hwnd_);
        AddTrayIcon();
        // Снимок: «=» и «-» (любая из двух), резерв — Ctrl+Shift+F12.
        key1_ok_ = RegisterHotKey(hwnd_, HK_SNAPSHOT_KEY1, kSnapshotMods, kSnapshotKey1) != FALSE;
        key2_ok_ = RegisterHotKey(hwnd_, HK_SNAPSHOT_KEY2, kSnapshotMods, kSnapshotKey2) != FALSE;
        hotkey_ok_ = RegisterHotKey(hwnd_, HK_SNAPSHOT_ALT, MOD_CONTROL | MOD_SHIFT, VK_F12) != FALSE;
        SetTimer(hwnd_, 1, 500, nullptr);

        AddLog(LogLevel::Info, L"Приложение запущено");
        AddLog(LogLevel::Info, key1_ok_
                                   ? L"Клавиша «=» — снимок в фоне (работает из любого окна)"
                                   : L"Клавиша «=» занята другим приложением");
        AddLog(LogLevel::Info, key2_ok_
                                   ? L"Клавиша «-» — снимок в фоне (то же действие)"
                                   : L"Клавиша «-» занята другим приложением");
        AddLog(LogLevel::Info, hotkey_ok_
                                   ? L"Резерв: Ctrl+Shift+F12 — снимок в фоне"
                                   : L"Ctrl+Shift+F12 занята другим приложением");

        RefreshDevices(true);
        Layout();
    }

    // Шрифты: Segoe UI, размеры в DIP -> пиксели под текущий DPI.
    void CreateFonts() {
        for (Font** font : {&f_title_, &f_section_, &f_label_, &f_value_, &f_button_, &f_log_}) {
            delete *font;
            *font = nullptr;
        }
        // Один шрифт на всё приложение: Segoe UI, размеры кратны шагу 2 px,
        // поэтому строки в карточках и журнале выглядят ровно.
        FontFamily family(L"Segoe UI");
        f_title_ = new Font(&family, (REAL)S(24), FontStyleBold, UnitPixel);     // заголовок окна
        f_section_ = new Font(&family, (REAL)S(16), FontStyleBold, UnitPixel);   // заголовок карточки
        f_label_ = new Font(&family, (REAL)S(14), FontStyleRegular, UnitPixel);  // подпись
        f_value_ = new Font(&family, (REAL)S(14), FontStyleBold, UnitPixel);     // значение
        f_button_ = new Font(&family, (REAL)S(14), FontStyleBold, UnitPixel);    // текст кнопки
        f_log_ = new Font(&family, (REAL)S(14), FontStyleRegular, UnitPixel);    // журнал
    }

    void ApplyFontsToControls() {
        SendMessageW(h_camera_, CB_SETITEMHEIGHT, (WPARAM)-1, S(24));
        SendMessageW(h_res_, CB_SETITEMHEIGHT, (WPARAM)-1, S(24));
        SendMessageW(h_fps_, CB_SETITEMHEIGHT, (WPARAM)-1, S(24));
        SendMessageW(h_log_, LB_SETITEMHEIGHT, 0, S(22));
    }

    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
        return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                               (HMENU)(INT_PTR)id, inst_, nullptr);
    }

    void CreateControls() {
        // Списки камер, разрешений и частот — owner-draw, чтобы выпадали тёмными.
        h_camera_ = Make(L"COMBOBOX", L"",
                         CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL |
                             WS_TABSTOP,
                         ID_CAMERA);
        h_res_ = Make(L"COMBOBOX", L"",
                      CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL |
                          WS_TABSTOP,
                      ID_RES);
        h_fps_ = Make(L"COMBOBOX", L"",
                      CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL |
                          WS_TABSTOP,
                      ID_FPS);

        h_refresh_ = Make(L"BUTTON", L"Обновить", BS_OWNERDRAW | WS_TABSTOP, ID_REFRESH);
        h_connect_ = Make(L"BUTTON", L"Подключить", BS_OWNERDRAW | WS_TABSTOP, ID_CONNECT);
        h_disconnect_ = Make(L"BUTTON", L"Отключить", BS_OWNERDRAW | WS_TABSTOP, ID_DISCONNECT);
        h_snapshot_ = Make(L"BUTTON", L"Снимок", BS_OWNERDRAW | WS_TABSTOP, ID_SNAPSHOT);
        h_report_ = Make(L"BUTTON", L"Сохранить отчёт", BS_OWNERDRAW | WS_TABSTOP, ID_REPORT);
        h_browse_ = Make(L"BUTTON", L"Обзор…", BS_OWNERDRAW | WS_TABSTOP, ID_BROWSE);
        h_tray_ = Make(L"BUTTON", L"Свернуть в трей", BS_OWNERDRAW | WS_TABSTOP, ID_TRAY_BUTTON);

        h_dir_ = Make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_DIR);
        h_log_ = Make(L"LISTBOX", L"",
                      WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                          WS_TABSTOP,
                      ID_LOG);
        SendMessageW(h_log_, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

        for (const wchar_t* title : kResTitles) SendMessageW(h_res_, CB_ADDSTRING, 0, (LPARAM)title);
        for (const wchar_t* title : kFpsTitles) SendMessageW(h_fps_, CB_ADDSTRING, 0, (LPARAM)title);
        SendMessageW(h_res_, CB_SETCURSEL, 1, 0);
        SendMessageW(h_fps_, CB_SETCURSEL, 0, 0);

        save_dir_ = DefaultSaveDir();
        EnsureDir(save_dir_);
        SetWindowTextW(h_dir_, save_dir_.c_str());

        ApplyFontsToControls();
    }

    // -----------------------------------------------------------------------
    //  Разметка (все размеры — через S(), чтобы работало при любом DPI)
    // -----------------------------------------------------------------------
    void Layout() {
        if (!h_log_) return;
        RECT rc;
        GetClientRect(hwnd_, &rc);
        const int w = rc.right, h = rc.bottom;
        const int pad = S(ui::Pad), gap = S(ui::Gap);

        log_rc_ = {pad, h - pad - S(ui::LogH), w - pad, h - pad};
        save_rc_ = {pad, log_rc_.top - gap - S(ui::SaveH), w - pad, log_rc_.top - gap};
        panel_rc_ = {w - pad - S(ui::PanelW), S(ui::HeaderH), w - pad, save_rc_.top - gap};
        preview_rc_ = {pad, S(ui::HeaderH), panel_rc_.left - gap, save_rc_.top - gap};

        tray_rc_ = {w - pad - S(150), S(30), w - pad, S(30) + S(34)};

        // --- карточка «Камера» ---
        {
            const int p = S(18);
            const int field_h = S(24);
            const int refresh_w = S(86);
            int y = panel_rc_.top + S(46);

            camera_label_ = {panel_rc_.left + p, y, panel_rc_.right - p, y + S(18)};
            y += S(20);
            const int combo_w = panel_rc_.right - panel_rc_.left - 2 * p - refresh_w - S(8);
            SetWindowPos(h_camera_, nullptr, panel_rc_.left + p, y, combo_w, S(240), SWP_NOZORDER);
            SetWindowPos(h_refresh_, nullptr, panel_rc_.right - p - refresh_w, y, refresh_w,
                         field_h, SWP_NOZORDER);
            y += field_h + S(14);

            const int half = (panel_rc_.right - panel_rc_.left - 2 * p - S(10)) / 2;
            res_label_ = {panel_rc_.left + p, y, panel_rc_.left + p + half, y + S(18)};
            fps_label_ = {panel_rc_.left + p + half + S(10), y, panel_rc_.right - p, y + S(18)};
            y += S(20);
            SetWindowPos(h_res_, nullptr, panel_rc_.left + p, y, half, S(240), SWP_NOZORDER);
            SetWindowPos(h_fps_, nullptr, panel_rc_.left + p + half + S(10), y, half, S(240),
                         SWP_NOZORDER);
            y += field_h + S(16);

            const int btn_w = (panel_rc_.right - panel_rc_.left - 2 * p - S(10)) / 2;
            SetWindowPos(h_connect_, nullptr, panel_rc_.left + p, y, btn_w, S(32), SWP_NOZORDER);
            SetWindowPos(h_disconnect_, nullptr, panel_rc_.left + p + btn_w + S(10), y, btn_w,
                         S(32), SWP_NOZORDER);
            y += S(32) + S(18);

            separator_y_ = y;
            info_top_ = y + S(20);
        }

        // --- карточка «Фотонаблюдение» ---
        {
            const int p = S(18);
            const int y1 = save_rc_.top + S(24);
            dir_label_ = {save_rc_.left + p, y1, save_rc_.left + p + S(66), y1 + S(24)};
            const int browse_w = S(86);
            const int edit_x = dir_label_.right + S(8);
            int edit_w = save_rc_.right - p - browse_w - S(8) - edit_x;
            edit_w = std::max(S(120), edit_w);
            dir_edit_rc_ = {edit_x, y1, edit_x + edit_w, y1 + S(24)};
            SetWindowPos(h_dir_, nullptr, edit_x, y1, edit_w, S(24), SWP_NOZORDER);
            SetWindowPos(h_browse_, nullptr, edit_x + edit_w + S(8), y1, browse_w, S(24),
                         SWP_NOZORDER);

            const int y2 = save_rc_.top + S(58);
            const int gap2 = S(10);
            const int left = save_rc_.left + p;
            const int avail = save_rc_.right - p - left - 2 * gap2;
            const int w1 = avail * 34 / 100;
            SetWindowPos(h_snapshot_, nullptr, left, y2, w1, S(30), SWP_NOZORDER);
            SetWindowPos(h_report_, nullptr, left + w1 + gap2, y2, save_rc_.right - p - (left + w1 + gap2),
                         S(30), SWP_NOZORDER);
        }

        // --- журнал ---
        {
            const int p = S(18);
            const int top = log_rc_.top + S(46);
            SetWindowPos(h_log_, nullptr, log_rc_.left + p, top,
                         log_rc_.right - log_rc_.left - 2 * p, log_rc_.bottom - p - top,
                         SWP_NOZORDER);
        }

        UpdateControlsState();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // -----------------------------------------------------------------------
    //  Отрисовка
    // -----------------------------------------------------------------------
    void Paint(HDC dc, const RECT& client) {
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        PaintBg(g, client.right, client.bottom);
        DrawHeader(g);
        DrawPreview(g);
        DrawPanel(g);
        DrawSave(g);
        DrawLog(g);
    }

    void DrawHeader(Graphics& g) {
        // Блок состояния стоит справа, у кнопки «Свернуть в трей».
        const int status_right = tray_rc_.left - S(24);
        const int status_left = status_right - S(380);
        const int text_right = status_left - S(16);

        RECT title_rc{S(ui::Pad), S(14), text_right, S(50)};
        TextG(g, kTitle, title_rc, f_title_, ui::Txt);

        RECT sub_rc{S(ui::Pad), S(52), text_right, S(76)};
        TextG(g, L"ЛР №4 · вариант А5 · скрытый режим фотонаблюдения", sub_rc, f_label_, ui::Sub);

        Color state_color = ui::Dim;
        if (camera_.State() == CamState::On) state_color = ui::Good;
        else if (camera_.State() == CamState::Opening) state_color = ui::Warn;
        else if (camera_.State() == CamState::Failed) state_color = ui::Bad;

        RECT status_rc{status_left, S(26), status_right, S(50)};
        Dot(g, status_rc.left + S(5), (status_rc.top + status_rc.bottom) / 2, S(5), state_color);
        RECT text_rc{status_rc.left + S(18), S(26), status_right, S(50)};
        TextG(g, status_text_, text_rc, f_value_, state_color);
    }

    void DrawPreview(Graphics& g) {
        Card(g, preview_rc_);
        RECT head{preview_rc_.left + S(18), preview_rc_.top + S(12), preview_rc_.right - S(18),
                  preview_rc_.top + S(38)};
        TextG(g, L"Предпросмотр", head, f_section_, ui::Txt);

        RECT video{preview_rc_.left + S(18), preview_rc_.top + S(46), preview_rc_.right - S(18),
                   preview_rc_.bottom - S(18)};
        Rect(g, video, ui::Preview, ui::Line, S(ui::Radius * 4) / 4.0f);

        Frame frame;
        if (camera_.Latest(frame)) {
            const int vw = video.right - video.left - 2;
            const int vh = video.bottom - video.top - 2;
            const double scale = std::min((double)vw / frame.width, (double)vh / frame.height);
            const int w = std::max(1, (int)(frame.width * scale));
            const int h = std::max(1, (int)(frame.height * scale));
            const int x = video.left + 1 + (vw - w) / 2;
            const int y = video.top + 1 + (vh - h) / 2;

            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = frame.width;
            bmi.bmiHeader.biHeight = -frame.height;   // строки сверху вниз
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 24;
            bmi.bmiHeader.biCompression = BI_RGB;

            // Видео выводим обычным GDI: GDI+ не умеет BGR24 напрямую.
            HDC vdc = g.GetHDC();
            SetStretchBltMode(vdc, HALFTONE);
            SetBrushOrgEx(vdc, 0, 0, nullptr);
            StretchDIBits(vdc, x, y, w, h, 0, 0, frame.width, frame.height, frame.bgr.data(), &bmi,
                          DIB_RGB_COLORS, SRCCOPY);
            g.ReleaseHDC(vdc);
        } else {
            TextG(g, camera_.State() == CamState::Opening ? L"Подключение камеры…"
                                                          : L"Нет видеосигнала",
                  video, f_section_, ui::Dim, StringAlignmentCenter);
        }
    }

    void DrawPanel(Graphics& g) {
        Card(g, panel_rc_);
        RECT head{panel_rc_.left + S(18), panel_rc_.top + S(12), panel_rc_.right - S(18),
                  panel_rc_.top + S(38)};
        TextG(g, L"Камера", head, f_section_, ui::Txt);

        TextG(g, L"Устройство", camera_label_, f_label_, ui::Sub);
        TextG(g, L"Разрешение", res_label_, f_label_, ui::Sub);
        TextG(g, L"Кадров/с", fps_label_, f_label_, ui::Sub);

        Separator(g, panel_rc_, separator_y_);

        int y = info_top_;
        Row(g, panel_rc_, y, L"Режим:", mode_text_.empty() ? L"—" : mode_text_, f_label_,
            f_value_, ui::Txt);
        y += S(ui::RowH);
        Row(g, panel_rc_, y, L"Формат:", format_text_.empty() ? L"—" : format_text_, f_label_,
            f_value_, ui::Txt);
        y += S(ui::RowH);
        Row(g, panel_rc_, y, L"Кадров:", frames_text_.empty() ? L"—" : frames_text_, f_label_,
            f_value_, ui::Txt);
        y += S(ui::RowH);
        Row(g, panel_rc_, y, L"Снимков:", std::to_wstring(snapshot_count_), f_label_, f_value_,
            ui::Txt);

        // Последний снимок — внизу карточки (имя файла)
        RECT last_title{panel_rc_.left + S(18), panel_rc_.bottom - S(66), panel_rc_.right - S(18),
                        panel_rc_.bottom - S(48)};
        TextG(g, L"Последний снимок:", last_title, f_label_, ui::Sub);
        RECT last_value{panel_rc_.left + S(18), panel_rc_.bottom - S(48),
                        panel_rc_.right - S(18), panel_rc_.bottom - S(26)};
        TextG(g, last_snapshot_.empty() ? L"—" : last_snapshot_, last_value, f_log_, ui::Txt);

        RECT hint{panel_rc_.left + S(18), panel_rc_.bottom - S(24), panel_rc_.right - S(18),
                  panel_rc_.bottom - S(8)};
        TextG(g, L"Снимок: «=» или «-» · трей — кнопкой ниже", hint, f_label_, ui::Dim);
    }

    void DrawSave(Graphics& g) {
        Card(g, save_rc_);
        RECT head{save_rc_.left + S(18), save_rc_.top + S(2), save_rc_.right - S(18),
                  save_rc_.top + S(22)};
        TextG(g, L"Фотонаблюдение", head, f_section_, ui::Txt);
        TextG(g, L"Каталог", dir_label_, f_label_, ui::Sub);

        // Рамка вокруг поля ввода: у EDIT нет своего оформления.
        RECT field = dir_edit_rc_;
        field.left -= 1;
        field.top -= 1;
        field.right += 1;
        field.bottom += 1;
        Rect(g, field, Color(255, 12, 18, 30), ui::Line, S(ui::Radius * 4) / 4.0f);
    }

    void DrawLog(Graphics& g) {
        Card(g, log_rc_);
        RECT head{log_rc_.left + S(18), log_rc_.top + S(10), log_rc_.right - S(18),
                  log_rc_.top + S(36)};
        TextG(g, L"Журнал событий", head, f_section_, ui::Txt);
    }

    // Кнопка: скруглённая плашка в стиле карточек.
    void DrawButton(const DRAWITEMSTRUCT* dis) {
        Graphics g(dis->hDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
        const bool disabled = (dis->itemState & ODS_DISABLED) != 0;

        Color fill = ui::Btn, border = ui::Line, text = ui::Txt;
        if (disabled) {
            fill = Color(120, 24, 32, 48);
            border = Color(90, 60, 80, 110);
            text = ui::Dim;
        } else if (dis->CtlID == ID_CONNECT || dis->CtlID == ID_SNAPSHOT) {
            fill = pressed ? Color(255, 30, 130, 90) : Color(255, 24, 110, 74);
            border = Color(255, 60, 200, 140);
            text = Color(255, 235, 255, 245);
        } else if (pressed) {
            fill = ui::BtnHot;
        }

        RECT rc = dis->rcItem;
        Rect(g, rc, fill, border, S(ui::Radius * 4) / 4.0f);

        wchar_t text_buffer[128] = {};
        GetWindowTextW(dis->hwndItem, text_buffer, 128);
        TextG(g, text_buffer, rc, f_button_, text, StringAlignmentCenter);
    }

    // Выпадающий список: поле выбора (itemID == -1) и пункты списка.
    void DrawCombo(const DRAWITEMSTRUCT* dis) {
        Graphics g(dis->hDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        if (dis->itemID == (UINT)-1) {
            Rect(g, dis->rcItem, ui::Field, ui::Line, S(ui::Radius * 4) / 4.0f);
            const int sel = (int)SendMessageW(dis->hwndItem, CB_GETCURSEL, 0, 0);
            std::wstring text;
            if (sel >= 0) {
                wchar_t buffer[256] = {};
                if (SendMessageW(dis->hwndItem, CB_GETLBTEXT, sel, (LPARAM)buffer) != CB_ERR) {
                    text = buffer;
                }
            }
            RECT text_rc = dis->rcItem;
            text_rc.left += S(8);
            text_rc.right -= S(24);
            TextG(g, text, text_rc, f_label_, text.empty() ? ui::Dim : ui::Txt);

            const int cx = dis->rcItem.right - S(14);
            const int cy = (dis->rcItem.top + dis->rcItem.bottom) / 2;
            Pen pen(ui::Sub, 1.4f);
            g.DrawLine(&pen, cx - S(4), cy - S(2), cx, cy + S(2));
            g.DrawLine(&pen, cx, cy + S(2), cx + S(4), cy - S(2));
            return;
        }

        wchar_t buffer[256] = {};
        SendMessageW(dis->hwndItem, CB_GETLBTEXT, dis->itemID, (LPARAM)buffer);
        const bool selected = (dis->itemState & ODS_SELECTED) != 0;
        FillRectG(g, dis->rcItem, selected ? ui::BtnHot : ui::Field);
        RECT text_rc = dis->rcItem;
        text_rc.left += S(8);
        TextG(g, buffer, text_rc, f_label_, selected ? ui::Txt : ui::Sub);
    }

    void DrawLogItem(const DRAWITEMSTRUCT* dis) {
        Graphics g(dis->hDC);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        wchar_t buffer[1024] = {};
        SendMessageW(dis->hwndItem, LB_GETTEXT, dis->itemID, (LPARAM)buffer);
        const std::wstring line(buffer);

        FillRectG(g, dis->rcItem, Color(255, 14, 20, 34));

        const LogLevel level = (LogLevel)SendMessageW(dis->hwndItem, LB_GETITEMDATA, dis->itemID, 0);
        const size_t time_len = 23;   // «29.10.2026 22:15:03.120»

        RECT rc = dis->rcItem;
        rc.left += S(8);
        RECT time_rc = rc;
        time_rc.right = rc.left + S(152);
        TextG(g, line.substr(0, std::min(time_len, line.size())), time_rc, f_log_, ui::Dim);

        RECT text_rc = rc;
        text_rc.left = time_rc.right;
        TextG(g, line.size() > time_len ? line.substr(time_len) : L"", text_rc, f_log_,
              LevelColor(level));
    }

    // -----------------------------------------------------------------------
    //  Журнал
    // -----------------------------------------------------------------------
    void AddLog(LogLevel level, const std::wstring& text) {
        const std::wstring line = NowStamp(true) + L"  " + text;
        log_.push_back({level, line});
        if (log_.size() > 2000) log_.erase(log_.begin());

        if (h_log_) {
            const int index = (int)SendMessageW(h_log_, LB_ADDSTRING, 0, (LPARAM)line.c_str());
            if (index >= 0) {
                SendMessageW(h_log_, LB_SETITEMDATA, index, (LPARAM)level);
                SendMessageW(h_log_, LB_SETTOPINDEX, index, 0);
            }
            while (SendMessageW(h_log_, LB_GETCOUNT, 0, 0) > 500) {
                SendMessageW(h_log_, LB_DELETESTRING, 0, 0);
            }
        }

        // Дублируем в файл: журнал сохраняется даже при аварийном закрытии.
        if (!save_dir_.empty()) {
            HANDLE h = CreateFileW(JoinPath(save_dir_, L"camera_log.txt").c_str(), FILE_APPEND_DATA,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                const std::string utf8 = ToUtf8(line + L"\r\n");
                DWORD written = 0;
                LARGE_INTEGER size{};
                if (GetFileSizeEx(h, &size) && size.QuadPart == 0) {
                    static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
                    WriteFile(h, bom, 3, &written, nullptr);
                }
                WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
                CloseHandle(h);
            }
        }
    }

    // -----------------------------------------------------------------------
    //  Камера
    // -----------------------------------------------------------------------
    void RefreshDevices(bool initial) {
        devices_ = EnumerateCameras();
        SendMessageW(h_camera_, CB_RESETCONTENT, 0, 0);
        for (const auto& device : devices_) {
            SendMessageW(h_camera_, CB_ADDSTRING, 0, (LPARAM)device.name.c_str());
        }

        if (!devices_.empty()) {
            SendMessageW(h_camera_, CB_SETCURSEL, 0, 0);
            if (!initial) {
                AddLog(LogLevel::Info,
                       Format(L"Список камер обновлён: найдено %d", (int)devices_.size()));
            }
        } else {
            SendMessageW(h_camera_, CB_SETCURSEL, (WPARAM)-1, 0);
            AddLog(LogLevel::Warn,
                   L"Камеры не найдены: проверьте подключение и доступ к камере в Windows");
        }

        UpdateControlsState();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Connect() {
        if (camera_.State() == CamState::On || camera_.State() == CamState::Opening) return;
        if (devices_.empty()) return;

        int index = (int)SendMessageW(h_camera_, CB_GETCURSEL, 0, 0);
        index = std::clamp(index, 0, (int)devices_.size() - 1);
        int res = (int)SendMessageW(h_res_, CB_GETCURSEL, 0, 0);
        res = std::clamp(res, 0, (int)std::size(kResTitles) - 1);
        int fps = (int)SendMessageW(h_fps_, CB_GETCURSEL, 0, 0);
        fps = std::clamp(fps, 0, (int)std::size(kFpsValues) - 1);

        status_text_ = L"Подключение камеры…";
        AddLog(LogLevel::Info,
               Format(L"Подключение «%s» (%s, %.0f к/с)", devices_[index].name.c_str(),
                      kResTitles[res], kFpsValues[fps]));
        camera_.Start(index, kResW[res], kResH[res], kFpsValues[fps]);
        UpdateControlsState();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Disconnect() {
        camera_.Stop();
        status_text_ = L"Камера не подключена";
        AddLog(LogLevel::Info, L"Захват остановлен");
        UpdateControlsState();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void OnCameraState(CamState state, const std::wstring& text) {
        status_text_ = text;
        switch (state) {
            case CamState::On:      AddLog(LogLevel::Info, text); break;
            case CamState::Failed:  AddLog(LogLevel::Error, text); break;
            case CamState::Opening: AddLog(LogLevel::Info, text); break;
            case CamState::Off:     AddLog(LogLevel::Warn, text); break;
        }
        UpdateControlsState();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    // -----------------------------------------------------------------------
    //  Снимок и отчёт
    // -----------------------------------------------------------------------
    void ApplyDirectory() {
        wchar_t buffer[MAX_PATH * 2] = {};
        GetWindowTextW(h_dir_, buffer, (int)std::size(buffer));
        std::wstring dir = buffer;
        while (!dir.empty() && (dir.back() == L' ' || dir.back() == L'\r')) dir.pop_back();
        if (dir.empty()) dir = DefaultSaveDir();

        if (!EnsureDir(dir)) {
            dir = DefaultSaveDir();
            EnsureDir(dir);
            SetWindowTextW(h_dir_, dir.c_str());
            AddLog(LogLevel::Error, L"Не удалось создать каталог, выбран каталог по умолчанию");
        }
        save_dir_ = dir;
    }

    void TakeSnapshot(bool background) {
        ApplyDirectory();

        Frame frame;
        if (!camera_.Latest(frame)) {
            AddLog(LogLevel::Warn, L"Снимок не создан: нет видеосигнала");
            if (background) Balloon(L"Снимок не создан", L"Нет видеосигнала", NIIF_WARNING);
            return;
        }

        const std::wstring path = SnapshotPath(save_dir_);
        Mat mat(frame.height, frame.width, CV_8UC3, frame.bgr.data());
        if (!ImwriteAny(path, mat, 92)) {
            AddLog(LogLevel::Error, L"Не удалось сохранить снимок: " + FileName(path));
            Balloon(L"Ошибка", L"Не удалось сохранить снимок", NIIF_ERROR);
            return;
        }

        ++snapshot_count_;
        last_snapshot_ = FileName(path);
        AddLog(background ? LogLevel::Background : LogLevel::Info,
               Format(L"Снимок сохранён: %s (%d × %d, %.1f КБ)", FileName(path).c_str(),
                      frame.width, frame.height, (double)FileSize(path) / 1024.0));
        Balloon(background ? L"Снимок (скрытый режим)" : L"Снимок сохранён",
                FileName(path) + L"\n" + save_dir_, NIIF_INFO);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void SaveReport() {
        ApplyDirectory();
        const std::wstring path = JoinPath(save_dir_, L"report_" + FileStamp() + L".txt");

        std::wstring text;
        text += L"ЛР №4. Система видеонаблюдения. Вариант А5 (скрытый режим фотонаблюдения)\r\n";
        text += L"Отчёт сформирован: " + NowStamp(false) + L"\r\n";
        text += std::wstring(78, L'=') + L"\r\n\r\n";
        for (const auto& entry : log_) text += entry.text + L"\r\n";
        if (log_.empty()) text += L"(журнал пуст)\r\n";

        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            AddLog(LogLevel::Error, L"Не удалось сохранить отчёт: " + FileName(path));
            return;
        }
        const std::string utf8 = ToUtf8(text);
        DWORD written = 0;
        static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        WriteFile(h, bom, 3, &written, nullptr);
        WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        CloseHandle(h);

        AddLog(LogLevel::Info, L"Отчёт сохранён: " + FileName(path));
        Balloon(L"Отчёт сохранён", FileName(path) + L"\n" + save_dir_, NIIF_INFO);
    }

    // -----------------------------------------------------------------------
    //  Скрытый режим: трей и горячая клавиша
    // -----------------------------------------------------------------------
    void AddTrayIcon() {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd_;
        data.uID = 1;
        data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        data.uCallbackMessage = WM_TRAY;
        data.hIcon = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(101), IMAGE_ICON, 0, 0,
                                       LR_DEFAULTSIZE | LR_SHARED);
        if (!data.hIcon) data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(data.szTip, L"Видеонаблюдение — ЛР №4 (снимок: «=» или «-»)");
        tray_added_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    }

    void HideToTray() {
        // Без открытой камеры фоновый снимок невозможен — подключаем автоматически.
        if (camera_.State() != CamState::On && !devices_.empty()) {
            AddLog(LogLevel::Background, L"Фоновый режим: подключаю камеру автоматически");
            Connect();
        }
        ShowWindow(hwnd_, SW_HIDE);
        hidden_ = true;
        AddLog(LogLevel::Background,
               L"Окно свёрнуто в трей — работает фоновое фотонаблюдение (Ctrl+Shift+F12)");
        Balloon(L"Фотонаблюдение включено",
                L"Окно скрыто. «=» или «-» — снимок без открытия окна.\n"
                L"Двойной щелчок по значку вернёт окно.",
                NIIF_INFO);
    }

    void Restore() {
        ShowWindow(hwnd_, SW_SHOW);
        ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        if (hidden_) {
            hidden_ = false;
            AddLog(LogLevel::Background, L"Окно восстановлено из трея");
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void OnHotkey(int id) {
        // Все зарегистрированные клавиши делают одно и то же — снимок,
        // в том числе при скрытом в трее окне.
        if (hidden_) {
            const wchar_t* name = id == HK_SNAPSHOT_KEY1 ? L"«=»"
                                : id == HK_SNAPSHOT_KEY2 ? L"«-»"
                                                         : L"Ctrl+Shift+F12";
            AddLog(LogLevel::Background, std::wstring(L"Горячая клавиша ") + name + L" (скрытый режим)");
        }
        TakeSnapshot(true);
    }

    void OnTray(LPARAM event) {
        switch (LOWORD(event)) {
            case WM_LBUTTONDBLCLK:
                Restore();
                break;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU: {
                POINT pt;
                GetCursorPos(&pt);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, ID_MENU_SHOW, L"Показать окно");
                AppendMenuW(menu, MF_STRING, ID_MENU_SNAPSHOT, L"Сделать снимок");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, ID_MENU_EXIT, L"Выход");
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

    void Balloon(const std::wstring& title, const std::wstring& text, DWORD flags) {
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

    // -----------------------------------------------------------------------
    //  Команды, таймер, завершение
    // -----------------------------------------------------------------------
    void OnCommand(int id, int code) {
        switch (id) {
            case ID_CONNECT:
                if (code == BN_CLICKED) Connect();
                break;
            case ID_DISCONNECT:
                if (code == BN_CLICKED) Disconnect();
                break;
            case ID_REFRESH:
                if (code == BN_CLICKED) RefreshDevices(false);
                break;
            case ID_SNAPSHOT:
                if (code == BN_CLICKED) TakeSnapshot(false);
                break;
            case ID_BROWSE:
                if (code == BN_CLICKED) BrowseDirectory();
                break;
            case ID_REPORT:
                if (code == BN_CLICKED) SaveReport();
                break;
            case ID_TRAY_BUTTON:
                if (code == BN_CLICKED) HideToTray();
                break;
            case ID_MENU_SHOW:
                Restore();
                break;
            case ID_MENU_SNAPSHOT:
                TakeSnapshot(true);
                break;
            case ID_MENU_EXIT:
                DestroyWindow(hwnd_);
                break;
            default:
                break;
        }
    }

    void OnTimer() {
        Frame frame;
        if (camera_.Latest(frame)) {
            const double fps = camera_.MeasuredFps();
            mode_text_ = Format(L"%d × %d", frame.width, frame.height);
            if (fps > 0.5) mode_text_ += Format(L", %.0f к/с", fps);
            format_text_ = camera_.FormatName();
            frames_text_ = Format(L"%llu", camera_.FrameCount());
        } else {
            mode_text_.clear();
            format_text_.clear();
            frames_text_.clear();
        }

        UpdateControlsState();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void UpdateControlsState() {
        const bool on = camera_.State() == CamState::On;
        const bool opening = camera_.State() == CamState::Opening;
        const bool has_devices = !devices_.empty();

        EnableWindow(h_connect_, (!on && !opening && has_devices) ? TRUE : FALSE);
        EnableWindow(h_disconnect_,
                     (on || opening || camera_.State() == CamState::Failed) ? TRUE : FALSE);
        EnableWindow(h_camera_, (!on && !opening) ? TRUE : FALSE);
        EnableWindow(h_refresh_, (!on && !opening) ? TRUE : FALSE);
        EnableWindow(h_res_, (!on && !opening) ? TRUE : FALSE);
        EnableWindow(h_fps_, (!on && !opening) ? TRUE : FALSE);
        EnableWindow(h_snapshot_, on ? TRUE : FALSE);
        EnableWindow(h_dir_, TRUE);
        EnableWindow(h_browse_, TRUE);
    }

    void BrowseDirectory() {
        BROWSEINFOW info{};
        info.hwndOwner = hwnd_;
        info.lpszTitle = L"Каталог для сохранения снимков и журнала";
        info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE idl = SHBrowseForFolderW(&info);
        if (!idl) return;
        wchar_t path[MAX_PATH] = {};
        if (SHGetPathFromIDListW(idl, path)) {
            SetWindowTextW(h_dir_, path);
            ApplyDirectory();
            AddLog(LogLevel::Info, std::wstring(L"Каталог сохранения: ") + path);
        }
        CoTaskMemFree(idl);
    }

    void Cleanup() {
        if (cleaned_) return;
        cleaned_ = true;

        KillTimer(hwnd_, 1);
        for (int id : {HK_SNAPSHOT_KEY1, HK_SNAPSHOT_KEY2, HK_SNAPSHOT_ALT}) {
            UnregisterHotKey(hwnd_, id);
        }
        key1_ok_ = key2_ok_ = hotkey_ok_ = false;
        camera_.Stop();

        if (tray_added_) {
            NOTIFYICONDATAW data{};
            data.cbSize = sizeof(data);
            data.hWnd = hwnd_;
            data.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &data);
            tray_added_ = false;
        }

        for (Font** font : {&f_title_, &f_section_, &f_label_, &f_value_, &f_button_, &f_log_}) {
            delete *font;
            *font = nullptr;
        }
        if (field_brush_) {
            DeleteObject(field_brush_);
            field_brush_ = nullptr;
        }
    }

    // --- поля --------------------------------------------------------------
    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    bool cleaned_ = false;

    Font* f_title_ = nullptr;
    Font* f_section_ = nullptr;
    Font* f_label_ = nullptr;
    Font* f_value_ = nullptr;
    Font* f_button_ = nullptr;
    Font* f_log_ = nullptr;
    HBRUSH field_brush_ = nullptr;

    HWND h_camera_ = nullptr, h_refresh_ = nullptr, h_res_ = nullptr, h_fps_ = nullptr;
    HWND h_connect_ = nullptr, h_disconnect_ = nullptr;
    HWND h_snapshot_ = nullptr, h_browse_ = nullptr, h_report_ = nullptr;
    HWND h_tray_ = nullptr, h_dir_ = nullptr, h_log_ = nullptr;

    RECT preview_rc_{}, panel_rc_{}, save_rc_{}, log_rc_{}, tray_rc_{};
    RECT camera_label_{}, res_label_{}, fps_label_{}, dir_label_{}, dir_edit_rc_{};
    int separator_y_ = 0;
    int info_top_ = 0;

    Camera camera_;
    std::vector<CameraDevice> devices_;
    std::vector<LogEntry> log_;

    std::wstring save_dir_;
    std::wstring status_text_ = L"Камера не подключена";
    std::wstring mode_text_, format_text_, frames_text_;
    std::wstring last_snapshot_;
    int snapshot_count_ = 0;

    bool tray_added_ = false;
    bool key1_ok_ = false;    // «=»
    bool key2_ok_ = false;    // «-»
    bool hotkey_ok_ = false;  // Ctrl+Shift+F12
    bool hidden_ = false;
};

}  // namespace

// ===========================================================================
//  7. Точка входа
// ===========================================================================
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    // DPI: без этого шрифты выглядят мелкими и размытыми на масштабе 125–150 %.
    if (HMODULE user32 = GetModuleHandleW(L"user32")) {
        using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        if (auto set_ctx = reinterpret_cast<SetCtxFn>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
            set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        } else {
            SetProcessDPIAware();
        }
    }
    {
        HDC screen = GetDC(nullptr);
        if (screen) {
            g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
            ReleaseDC(nullptr, screen);
        }
        if (g_dpi < 96 || g_dpi > 480) g_dpi = 96;
    }

    // GDI+ — основа оформления (как в ЛР №1).
    GdiplusStartupInput gdiplus_input;
    ULONG_PTR gdiplus_token = 0;
    if (GdiplusStartup(&gdiplus_token, &gdiplus_input, nullptr) != Ok) return 1;

    // COM нужен для DirectShow (перечисление камер) и диалога выбора каталога.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&controls);

    int result = 0;
    {
        App app;
        if (!app.Init(inst, show)) {
            MessageBoxW(nullptr, L"Не удалось создать окно приложения.", L"Ошибка",
                        MB_OK | MB_ICONERROR);
            result = 1;
        } else {
            result = app.Run();
        }
    }

    if (SUCCEEDED(com)) CoUninitialize();
    GdiplusShutdown(gdiplus_token);
    return result;
}
