// ===========================================================================
//  Лабораторная работа №4. Система видеонаблюдения на базе веб-камеры
//  Вариант А5. Скрытый режим фотонаблюдения
//
//  Всё приложение — один файл (main.cpp). Оформление повторяет ЛР №1:
//  GDI+, фон bg.jpg с затемнением, полупрозрачные карточки, Segoe UI.
//
//  Что умеет:
//    * предпросмотр с веб-камеры в реальном времени;
//    * снимок JPEG — кнопкой и глобальной клавишей Ctrl+Shift+F12;
//    * запись видео AVI/MJPEG;
//    * скрытый режим: окно в трей, снимок без окна, уведомления;
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
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstring>
#include <deque>
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

// Common Controls v6 — оформление кнопок и списков как в Windows 11.
#pragma comment(linker, "\"/manifestdependency:type='win32' \
 name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
 processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

using namespace Gdiplus;
using cv::Mat;   // чтобы не писать cv::Mat в каждом объявлении

// ===========================================================================
//  1. Оформление — те же цвета и размеры, что в ЛР №1 (theme.h)
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
const Color Bad(255, 235, 87, 87);     // ошибка / запись

const Color Field(235, 12, 18, 30);    // поле ввода, выпадающий список
const Color Btn(255, 30, 42, 64);      // обычная кнопка
const Color BtnHot(255, 42, 58, 86);   // кнопка под курсором / нажата
const Color Preview(255, 8, 10, 16);   // подложка под видео

// Геометрия
const int Pad = 28;
const int Gap = 16;
const int HeaderH = 96;
const int LogH = 168;
const int SaveH = 92;
const int PanelW = 360;
const int RowH = 26;
const REAL Radius = 6.0f;

}  // namespace ui

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

// Имя файла с защитой от совпадения в одну и ту же секунду.
std::wstring UniquePath(const std::wstring& dir, const std::wstring& prefix,
                        const std::wstring& ext) {
    const std::wstring base = JoinPath(dir, prefix + L"_" + FileStamp());
    std::wstring candidate = base + L"." + ext;
    for (int i = 1; FileExists(candidate); ++i) {
        candidate = base + L"_" + std::to_wstring(i) + L"." + ext;
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

Bitmap* g_bg = nullptr;

Bitmap* LoadBg(HINSTANCE inst) {
    HRSRC res = FindResourceW(inst, MAKEINTRESOURCEW(201), RT_RCDATA);
    if (!res) return nullptr;
    const DWORD size = SizeofResource(inst, res);
    if (!size) return nullptr;
    HGLOBAL loaded = LoadResource(inst, res);
    if (!loaded) return nullptr;
    void* data = LockResource(loaded);
    if (!data) return nullptr;

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!memory) return nullptr;
    void* dst = GlobalLock(memory);
    if (!dst) {
        GlobalFree(memory);
        return nullptr;
    }
    memcpy(dst, data, size);
    GlobalUnlock(memory);

    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(memory, TRUE, &stream) != S_OK) {
        GlobalFree(memory);
        return nullptr;
    }
    Bitmap* bitmap = new Bitmap(stream);
    stream->Release();
    if (bitmap->GetLastStatus() != Ok) {
        delete bitmap;
        return nullptr;
    }
    return bitmap;
}

// Фон: картинка bg.jpg на весь экран + затемнение — как в ЛР №1.
void PaintBg(Graphics& g, int w, int h) {
    SolidBrush base(ui::Bg);
    g.FillRectangle(&base, 0, 0, w, h);
    if (g_bg) {
        const REAL iw = (REAL)g_bg->GetWidth(), ih = (REAL)g_bg->GetHeight();
        if (iw > 0 && ih > 0) {
            const REAL s = std::max(w / iw, h / ih);
            g.DrawImage(g_bg, (w - iw * s) / 2, (h - ih * s) / 2, iw * s, ih * s);
        }
    }
    SolidBrush dim(Color(150, 6, 10, 18));
    g.FillRectangle(&dim, 0, 0, w, h);
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

// Карточка: полупрозрачная плашка с рамкой (Card() из ЛР №1).
void Card(Graphics& g, const RECT& rc) {
    Rect(g, rc, ui::Card, ui::Line, ui::Radius);
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

// Строка «подпись : значение» внутри карточки (Row() из ЛР №1).
void Row(Graphics& g, const RECT& card, int y, const std::wstring& label,
         const std::wstring& value, Font* label_font, Font* value_font, const Color& value_color) {
    const int lx = card.left + 18;
    const int vx = card.left + 130;
    RECT l{lx, y, vx - 8, y + ui::RowH};
    RECT v{vx, y, card.right - 18, y + ui::RowH};
    TextG(g, label, l, label_font, ui::Sub);
    TextG(g, value, v, value_font, value_color);
}

void Separator(Graphics& g, const RECT& card, int y, int inset) {
    Pen pen(ui::Line, 1.0f);
    g.DrawLine(&pen, (REAL)(card.left + inset), (REAL)y, (REAL)(card.right - inset), (REAL)y);
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

class Recorder;

enum class CamState { Off, Opening, On, Failed };

// Поток захвата: открывает камеру, постоянно читает кадры, хранит последний.
// GUI только копирует последний кадр — интерфейс не ждёт чтения с камеры.
class Camera {
public:
    Camera() = default;
    ~Camera() { Stop(); }

    void SetNotifyWindow(HWND hwnd) { hwnd_ = hwnd; }
    void SetRecorder(Recorder* recorder) { recorder_.store(recorder); }

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
    double DeclaredFps() const { return declared_fps_.load(); }
    unsigned long long FrameCount() const { return frame_count_.load(); }
    std::wstring FormatName() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return format_;
    }

private:
    void Run(int index, int want_w, int want_h, double want_fps);

    void Notify(CamState state, const std::wstring& text) {
        state_ = state;
        auto* payload = new std::wstring(text);
        if (!PostMessageW(hwnd_, WM_APP + 1, (WPARAM)state, (LPARAM)payload)) delete payload;
    }

    HWND hwnd_ = nullptr;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<CamState> state_{CamState::Off};
    std::atomic<Recorder*> recorder_{nullptr};

    mutable std::mutex mutex_;
    Frame latest_;
    std::wstring format_;

    std::atomic<double> declared_fps_{0.0};
    std::atomic<double> measured_fps_{0.0};
    std::atomic<unsigned long long> frame_count_{0};
};

// ===========================================================================
//  6. Запись видео (AVI / MJPEG) в отдельном потоке
// ===========================================================================

class Recorder {
public:
    ~Recorder() { Stop(); }

    bool Active() const { return active_.load(); }
    unsigned long long Frames() const { return frames_.load(); }
    std::wstring Path() const { return path_; }

    double Seconds() const {
        if (!active_.load()) return seconds_;
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    }

    bool Start(const std::wstring& path, int width, int height, double fps) {
        if (active_.load()) return false;
        path_ = path;
        width_ = std::max(width & ~1, 2);      // MJPG требует чётных размеров
        height_ = std::max(height & ~1, 2);
        fps_ = (fps > 1.0 && fps < 240.0) ? fps : 25.0;
        frames_ = 0;
        seconds_ = 0.0;

        bool opened = false;
        try {
            opened = writer_.open(ToUtf8(path), cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), fps_,
                                  cv::Size(width_, height_));
            if (!opened) {
                opened = writer_.open(ToAnsi(path),
                                      cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), fps_,
                                      cv::Size(width_, height_));
            }
        } catch (const cv::Exception&) {
            opened = false;
        }
        if (!opened) return false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.clear();
        }
        stop_.store(false);
        active_.store(true);
        started_ = std::chrono::steady_clock::now();
        worker_ = std::thread(&Recorder::Run, this);
        return true;
    }

    void Push(const Frame& frame) {
        if (!active_.load() || !frame.ok()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() > 8) queue_.pop_front();   // не копим задержку
        queue_.push_back(frame);
        cond_.notify_one();
    }

    void Stop() {
        if (!active_.exchange(false)) {
            if (worker_.joinable()) worker_.join();
            return;
        }
        stop_.store(true);
        cond_.notify_all();
        if (worker_.joinable()) worker_.join();
        try {
            writer_.release();
        } catch (const cv::Exception&) {
        }
    }

private:
    void Run() {
        while (!stop_.load()) {
            Frame frame;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cond_.wait_for(lock, std::chrono::milliseconds(200),
                               [this] { return stop_.load() || !queue_.empty(); });
                if (queue_.empty()) continue;
                frame = std::move(queue_.front());
                queue_.pop_front();
            }
            if (!frame.ok()) continue;

            try {
                Mat mat(frame.height, frame.width, CV_8UC3, frame.bgr.data());
                if (frame.width != width_ || frame.height != height_) {
                    Mat resized;
                    cv::resize(mat, resized, cv::Size(width_, height_));
                    mat = resized;
                }
                writer_.write(mat);
                frames_.fetch_add(1);
            } catch (const cv::Exception&) {
            }
        }
        seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    }

    cv::VideoWriter writer_;
    std::thread worker_;
    std::atomic<bool> active_{false};
    std::atomic<bool> stop_{false};
    std::atomic<unsigned long long> frames_{0};

    std::mutex mutex_;
    std::condition_variable cond_;
    std::deque<Frame> queue_;

    std::wstring path_;
    int width_ = 640;
    int height_ = 480;
    double fps_ = 25.0;
    double seconds_ = 0.0;
    std::chrono::steady_clock::time_point started_{};
};

// Тело потока захвата вынесено сюда: внутри класса Camera тип Recorder ещё
// неполный, а Push() нужно вызывать по-настоящему.
void Camera::Run(int index, int want_w, int want_h, double want_fps) {
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
        Notify(CamState::Failed, L"Камера не отдаёт кадры (проверьте доступ к камере в Windows)");
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
    declared_fps_.store(capture.get(cv::CAP_PROP_FPS));
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

        if (Recorder* recorder = recorder_.load()) recorder->Push(out);

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

// ===========================================================================
//  7. Главное окно
// ===========================================================================

const wchar_t* kClassName = L"Lab4CameraWindow";
const wchar_t* kTitle = L"Система видеонаблюдения";

const wchar_t* kResTitles[] = {L"1920 × 1080", L"1280 × 720", L"1024 × 768",
                               L"960 × 540",   L"800 × 600",  L"640 × 480"};
const int kResW[] = {1920, 1280, 1024, 960, 800, 640};
const int kResH[] = {1080, 720, 768, 540, 600, 480};

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
        ID_CAMERA = 1001,
        ID_REFRESH,
        ID_RES,
        ID_FPS,
        ID_CONNECT,
        ID_DISCONNECT,
        ID_SNAPSHOT,
        ID_REC_START,
        ID_REC_STOP,
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
                                CW_USEDEFAULT, CW_USEDEFAULT, 1180, 820, nullptr, nullptr, inst_,
                                this);
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
    //  Окно
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
                info->ptMinTrackSize.x = 980;
                info->ptMinTrackSize.y = 720;
                return 0;
            }

            case WM_ERASEBKGND:
                return 1;   // фон рисуем сами, без мерцания

            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd_, &ps);
                RECT rc;
                GetClientRect(hwnd_, &rc);
                if (rc.right > 0 && rc.bottom > 0) {
                    // Двойная буферизация: иначе видео мерцает на 30 кадрах в секунду.
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
                OnHotkey();
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
                OnClose();
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
        camera_.SetRecorder(&recorder_);

        AddTrayIcon();
        hotkey_ok_ = RegisterHotKey(hwnd_, 1, MOD_CONTROL | MOD_SHIFT, VK_F12) != FALSE;
        SetTimer(hwnd_, 1, 500, nullptr);

        AddLog(LogLevel::Info, L"Приложение запущено");
        AddLog(LogLevel::Info, hotkey_ok_
                                   ? L"Ctrl+Shift+F12 — снимок в фоне (работает из любого окна)"
                                   : L"Ctrl+Shift+F12 занята другим приложением");

        RefreshDevices(true);
        Layout();
    }

    void CreateFonts() {
        FontFamily family(L"Segoe UI");
        FontFamily mono(L"Consolas");
        f_title_ = new Font(&family, 26, FontStyleBold, UnitPixel);      // заголовок окна
        f_section_ = new Font(&family, 18, FontStyleBold, UnitPixel);    // заголовок карточки
        f_label_ = new Font(&family, 14, FontStyleRegular, UnitPixel);   // подпись
        f_value_ = new Font(&family, 14, FontStyleBold, UnitPixel);      // значение
        f_button_ = new Font(&family, 14, FontStyleBold, UnitPixel);     // текст кнопки
        f_log_ = new Font(&mono, 13, FontStyleRegular, UnitPixel);       // журнал
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
        h_rec_start_ = Make(L"BUTTON", L"Начать запись", BS_OWNERDRAW | WS_TABSTOP, ID_REC_START);
        h_rec_stop_ = Make(L"BUTTON", L"Остановить запись", BS_OWNERDRAW | WS_TABSTOP, ID_REC_STOP);
        h_report_ = Make(L"BUTTON", L"Сохранить отчёт", BS_OWNERDRAW | WS_TABSTOP, ID_REPORT);
        h_browse_ = Make(L"BUTTON", L"Обзор…", BS_OWNERDRAW | WS_TABSTOP, ID_BROWSE);
        h_tray_ = Make(L"BUTTON", L"Свернуть в трей", BS_OWNERDRAW | WS_TABSTOP, ID_TRAY_BUTTON);

        h_dir_ = Make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_DIR);
        h_log_ = Make(L"LISTBOX", L"",
                      WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                          WS_TABSTOP,
                      ID_LOG);
        SendMessageW(h_log_, LB_SETITEMHEIGHT, 0, 20);

        for (const wchar_t* title : kResTitles) SendMessageW(h_res_, CB_ADDSTRING, 0, (LPARAM)title);
        for (const wchar_t* title : kFpsTitles) SendMessageW(h_fps_, CB_ADDSTRING, 0, (LPARAM)title);

        for (HWND combo : {h_camera_, h_res_, h_fps_}) {
            SendMessageW(combo, CB_SETITEMHEIGHT, (WPARAM)-1, 20);   // высота поля выбора
        }
        SendMessageW(h_res_, CB_SETCURSEL, 1, 0);
        SendMessageW(h_fps_, CB_SETCURSEL, 0, 0);

        save_dir_ = DefaultSaveDir();
        EnsureDir(save_dir_);
        SetWindowTextW(h_dir_, save_dir_.c_str());
    }

    // -----------------------------------------------------------------------
    //  Разметка
    // -----------------------------------------------------------------------
    void Layout() {
        if (!h_log_) return;
        RECT rc;
        GetClientRect(hwnd_, &rc);
        const int w = rc.right, h = rc.bottom;
        const int pad = ui::Pad, gap = ui::Gap;

        log_rc_ = {pad, h - pad - ui::LogH, w - pad, h - pad};
        save_rc_ = {pad, log_rc_.top - gap - ui::SaveH, w - pad, log_rc_.top - gap};
        panel_rc_ = {w - pad - ui::PanelW, ui::HeaderH, w - pad, save_rc_.top - gap};
        preview_rc_ = {pad, ui::HeaderH, panel_rc_.left - gap, save_rc_.top - gap};

        tray_rc_ = {w - pad - 152, 28, w - pad, 28 + 34};

        // --- карточка «Камера» ---
        {
            const int p = 18;
            int y = panel_rc_.top + 46;   // ниже заголовка карточки
            const int combo_h = 24;
            const int refresh_w = 84;

            camera_label_ = {panel_rc_.left + p, y, panel_rc_.right - p, y + 18};
            y += 20;
            SetWindowPos(h_camera_, nullptr, panel_rc_.left + p, y,
                         panel_rc_.right - panel_rc_.left - 2 * p - refresh_w - 8, 240,
                         SWP_NOZORDER);
            SetWindowPos(h_refresh_, nullptr, panel_rc_.right - p - refresh_w, y, refresh_w,
                         combo_h, SWP_NOZORDER);
            y += combo_h + 14;

            const int half = (panel_rc_.right - panel_rc_.left - 2 * p - 10) / 2;
            res_label_ = {panel_rc_.left + p, y, panel_rc_.left + p + half, y + 18};
            fps_label_ = {panel_rc_.left + p + half + 10, y, panel_rc_.right - p, y + 18};
            y += 20;
            SetWindowPos(h_res_, nullptr, panel_rc_.left + p, y, half, 240, SWP_NOZORDER);
            SetWindowPos(h_fps_, nullptr, panel_rc_.left + p + half + 10, y, half, 240,
                         SWP_NOZORDER);
            y += combo_h + 16;

            const int btn_w = (panel_rc_.right - panel_rc_.left - 2 * p - 10) / 2;
            SetWindowPos(h_connect_, nullptr, panel_rc_.left + p, y, btn_w, 32, SWP_NOZORDER);
            SetWindowPos(h_disconnect_, nullptr, panel_rc_.left + p + btn_w + 10, y, btn_w, 32,
                         SWP_NOZORDER);
            y += 32 + 18;

            separator_y_ = y;
            y += 20;
            info_top_ = y;
        }

        // --- карточка «Сохранение» ---
        {
            const int p = 18;
            const int y1 = save_rc_.top + 22;
            dir_label_ = {save_rc_.left + p, y1, save_rc_.left + p + 66, y1 + 24};
            const int browse_w = 84;
            int edit_x = dir_label_.right + 8;
            int edit_w = save_rc_.right - p - browse_w - 8 - edit_x;
            edit_w = std::max(120, edit_w);
            dir_edit_rc_ = {edit_x, y1, edit_x + edit_w, y1 + 24};
            SetWindowPos(h_dir_, nullptr, edit_x, y1, edit_w, 24, SWP_NOZORDER);
            SetWindowPos(h_browse_, nullptr, edit_x + edit_w + 8, y1, browse_w, 24, SWP_NOZORDER);

            const int y2 = save_rc_.top + 54;
            const int gap2 = 10;
            const int avail = save_rc_.right - p - (save_rc_.left + p) - 3 * gap2;
            const int w1 = avail * 16 / 100, w2 = avail * 22 / 100, w3 = avail * 24 / 100;
            int x = save_rc_.left + p;
            SetWindowPos(h_snapshot_, nullptr, x, y2, w1, 30, SWP_NOZORDER);
            x += w1 + gap2;
            SetWindowPos(h_rec_start_, nullptr, x, y2, w2, 30, SWP_NOZORDER);
            x += w2 + gap2;
            SetWindowPos(h_rec_stop_, nullptr, x, y2, w3, 30, SWP_NOZORDER);
            x += w3 + gap2;
            SetWindowPos(h_report_, nullptr, x, y2, save_rc_.right - p - x, 30, SWP_NOZORDER);
        }

        // --- журнал ---
        {
            const int p = 18;
            const int top = log_rc_.top + 44;
            SetWindowPos(h_log_, nullptr, log_rc_.left + p, top, log_rc_.right - log_rc_.left - 2 * p,
                         log_rc_.bottom - p - top, SWP_NOZORDER);
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
        // Заголовок и подзаголовок — как в ЛР №1 (Segoe UI 26 bold).
        RECT title_rc{ui::Pad, 18, client_right() - 200, 52};
        TextG(g, kTitle, title_rc, f_title_, ui::Txt);

        RECT sub_rc{ui::Pad, 52, client_right() - 200, 74};
        TextG(g, L"Лабораторная работа №4 · вариант А5 · скрытый режим фотонаблюдения", sub_rc,
              f_label_, ui::Sub);

        // Индикатор состояния камеры
        Color state_color = ui::Dim;
        if (camera_.State() == CamState::On) state_color = ui::Good;
        else if (camera_.State() == CamState::Opening) state_color = ui::Warn;
        else if (camera_.State() == CamState::Failed) state_color = ui::Bad;

        const int right = tray_rc_.left - 24;
        RECT status_rc{right - 420, 30, right, 50};
        Dot(g, status_rc.left + 5, 40, 5, state_color);
        RECT text_rc{status_rc.left + 18, 30, right, 50};
        TextG(g, status_text_, text_rc, f_value_, state_color);

        // Индикатор записи (мигает дважды в секунду)
        if (recorder_.Active()) {
            RECT rec_rc{right - 220, 54, right, 72};
            if (blink_) Dot(g, rec_rc.left + 4, 63, 4, ui::Bad);
            TextG(g, L"ЗАПИСЬ " + Clock(recorder_.Seconds()), {rec_rc.left + 14, 54, right, 72},
                  f_label_, ui::Bad);
        }
    }

    void DrawPreview(Graphics& g) {
        Card(g, preview_rc_);
        RECT head{preview_rc_.left + 18, preview_rc_.top + 14, preview_rc_.right - 18,
                  preview_rc_.top + 38};
        TextG(g, L"Предпросмотр", head, f_section_, ui::Txt);

        RECT video{preview_rc_.left + 18, preview_rc_.top + 46, preview_rc_.right - 18,
                   preview_rc_.bottom - 18};
        Rect(g, video, ui::Preview, ui::Line, 4.0f);

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
        RECT head{panel_rc_.left + 18, panel_rc_.top + 14, panel_rc_.right - 18,
                  panel_rc_.top + 38};
        TextG(g, L"Камера", head, f_section_, ui::Txt);

        TextG(g, L"Устройство", camera_label_, f_label_, ui::Sub);
        TextG(g, L"Разрешение", res_label_, f_label_, ui::Sub);
        TextG(g, L"Кадров/с", fps_label_, f_label_, ui::Sub);

        Separator(g, panel_rc_, separator_y_, 18);

        int y = info_top_;
        Row(g, panel_rc_, y, L"Режим:", mode_text_.empty() ? L"—" : mode_text_, f_label_,
            f_value_, ui::Txt);
        y += ui::RowH;
        Row(g, panel_rc_, y, L"Формат:", format_text_.empty() ? L"—" : format_text_, f_label_,
            f_value_, ui::Txt);
        y += ui::RowH;
        Row(g, panel_rc_, y, L"Кадров:", frames_text_.empty() ? L"—" : frames_text_, f_label_,
            f_value_, ui::Txt);
        y += ui::RowH;
        Row(g, panel_rc_, y, L"Запись:", recorder_.Active()
                                             ? L"идёт, " + Clock(recorder_.Seconds())
                                             : L"нет",
            f_label_, f_value_, recorder_.Active() ? ui::Bad : ui::Sub);

        // Подсказка про скрытый режим — внизу карточки
        RECT hint{panel_rc_.left + 18, panel_rc_.bottom - 62, panel_rc_.right - 18,
                  panel_rc_.bottom - 44};
        TextG(g, L"Скрытый режим:", hint, f_label_, ui::Sub);
        RECT hint2{panel_rc_.left + 18, panel_rc_.bottom - 44, panel_rc_.right - 18,
                   panel_rc_.bottom - 26};
        TextG(g, L"«Свернуть в трей» + Ctrl+Shift+F12", hint2, f_label_, ui::Sub);
    }

    void DrawSave(Graphics& g) {
        Card(g, save_rc_);
        RECT head{save_rc_.left + 18, save_rc_.top + 2, save_rc_.right - 18, save_rc_.top + 22};
        TextG(g, L"Сохранение материалов", head, f_section_, ui::Txt);
        TextG(g, L"Каталог", dir_label_, f_label_, ui::Sub);

        // Рамка вокруг поля ввода: EDIT не имеет своего оформления.
        RECT field = dir_edit_rc_;
        field.left -= 1;
        field.top -= 1;
        field.right += 1;
        field.bottom += 1;
        Rect(g, field, Color(255, 12, 18, 30), ui::Line, 4.0f);
    }

    void DrawLog(Graphics& g) {
        Card(g, log_rc_);
        RECT head{log_rc_.left + 18, log_rc_.top + 12, log_rc_.right - 18, log_rc_.top + 34};
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
        } else if (dis->CtlID == ID_REC_STOP) {
            fill = pressed ? Color(255, 120, 45, 45) : Color(255, 88, 34, 38);
            border = Color(255, 190, 70, 70);
            text = ui::Bad;
        } else if (pressed) {
            fill = ui::BtnHot;
        }

        RECT rc = dis->rcItem;
        Rect(g, rc, fill, border, 5.0f);

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
            Rect(g, dis->rcItem, ui::Field, ui::Line, 4.0f);
            const int sel = (int)SendMessageW(dis->hwndItem, CB_GETCURSEL, 0, 0);
            std::wstring text;
            if (sel >= 0) {
                wchar_t buffer[256] = {};
                if (SendMessageW(dis->hwndItem, CB_GETLBTEXT, sel, (LPARAM)buffer) != CB_ERR) {
                    text = buffer;
                }
            }
            RECT text_rc = dis->rcItem;
            text_rc.left += 8;
            text_rc.right -= 24;
            TextG(g, text, text_rc, f_label_, text.empty() ? ui::Dim : ui::Txt);

            // «Галочка» выпадающего списка
            const int cx = dis->rcItem.right - 14, cy = (dis->rcItem.top + dis->rcItem.bottom) / 2;
            Pen pen(ui::Sub, 1.4f);
            g.DrawLine(&pen, cx - 4, cy - 2, cx, cy + 2);
            g.DrawLine(&pen, cx, cy + 2, cx + 4, cy - 2);
            return;
        }

        wchar_t buffer[256] = {};
        SendMessageW(dis->hwndItem, CB_GETLBTEXT, dis->itemID, (LPARAM)buffer);
        const bool selected = (dis->itemState & ODS_SELECTED) != 0;
        FillRectG(g, dis->rcItem, selected ? ui::BtnHot : ui::Field);
        RECT text_rc = dis->rcItem;
        text_rc.left += 8;
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
        rc.left += 8;
        RECT time_rc = rc;
        time_rc.right = rc.left + 152;
        TextG(g, line.substr(0, std::min(time_len, line.size())), time_rc, f_log_, ui::Dim);

        RECT text_rc = rc;
        text_rc.left = time_rc.right;
        TextG(g, line.size() > time_len ? line.substr(time_len) : L"", text_rc, f_log_,
              LevelColor(level));
    }

    int client_right() const {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        return rc.right;
    }

    std::wstring Clock(double seconds) const {
        const int total = std::max(0, (int)seconds);
        return Format(L"%02d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
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
        if (recorder_.Active()) StopRecording();
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
    //  Снимок, запись, отчёт
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

        const std::wstring path = UniquePath(save_dir_, L"snapshot", L"jpg");
        Mat mat(frame.height, frame.width, CV_8UC3, frame.bgr.data());
        if (!ImwriteAny(path, mat, 92)) {
            AddLog(LogLevel::Error, L"Не удалось сохранить снимок: " + FileName(path));
            Balloon(L"Ошибка", L"Не удалось сохранить снимок", NIIF_ERROR);
            return;
        }

        AddLog(background ? LogLevel::Background : LogLevel::Info,
               Format(L"Снимок сохранён: %s (%d × %d, %.1f КБ)", FileName(path).c_str(),
                      frame.width, frame.height, (double)FileSize(path) / 1024.0));
        Balloon(background ? L"Снимок (скрытый режим)" : L"Снимок сохранён",
                FileName(path) + L"\n" + save_dir_, NIIF_INFO);
    }

    void StartRecording() {
        if (recorder_.Active() || camera_.State() != CamState::On) return;
        ApplyDirectory();

        Frame frame;
        if (!camera_.Latest(frame)) return;

        double fps = camera_.MeasuredFps();
        if (fps < 1.0) fps = camera_.DeclaredFps();
        if (fps < 1.0) fps = 25.0;

        const std::wstring path = UniquePath(save_dir_, L"video", L"avi");
        if (!recorder_.Start(path, frame.width, frame.height, fps)) {
            AddLog(LogLevel::Error, L"Не удалось открыть файл записи: " + FileName(path));
            return;
        }
        AddLog(LogLevel::Info,
               Format(L"Запись начата: %s (%d × %d, %.0f к/с)", FileName(path).c_str(),
                      frame.width, frame.height, fps));
        if (hidden_) Balloon(L"Запись начата", FileName(path), NIIF_INFO);
        UpdateControlsState();
    }

    void StopRecording() {
        if (!recorder_.Active()) return;
        const std::wstring path = recorder_.Path();
        const unsigned long long frames = recorder_.Frames();
        const double seconds = recorder_.Seconds();
        recorder_.Stop();

        AddLog(LogLevel::Info,
               Format(L"Запись завершена: %s (кадров %llu, %s, %.1f МБ)", FileName(path).c_str(),
                      frames, Clock(seconds).c_str(),
                      (double)FileSize(path) / (1024.0 * 1024.0)));
        if (hidden_) Balloon(L"Запись остановлена", FileName(path), NIIF_INFO);
        UpdateControlsState();
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
        wcscpy_s(data.szTip, L"Видеонаблюдение — ЛР №4, вариант А5");
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
                L"Окно скрыто. Ctrl+Shift+F12 — снимок без открытия окна.\n"
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

    void OnHotkey() {
        if (hidden_) AddLog(LogLevel::Background, L"Горячая клавиша Ctrl+Shift+F12 (скрытый режим)");
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
            case ID_REC_START:
                if (code == BN_CLICKED) StartRecording();
                break;
            case ID_REC_STOP:
                if (code == BN_CLICKED) StopRecording();
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
                OnClose();
                break;
            default:
                break;
        }
    }

    void OnTimer() {
        blink_ = !blink_;   // индикатор записи мигает дважды в секунду

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
        const bool rec = recorder_.Active();
        const bool has_devices = !devices_.empty();

        EnableWindow(h_connect_, (!on && !opening && has_devices) ? TRUE : FALSE);
        EnableWindow(h_disconnect_,
                     (on || opening || camera_.State() == CamState::Failed) ? TRUE : FALSE);
        EnableWindow(h_camera_, (!on && !opening) ? TRUE : FALSE);
        EnableWindow(h_refresh_, (!on && !opening) ? TRUE : FALSE);
        EnableWindow(h_res_, (!on && !opening) ? TRUE : FALSE);
        EnableWindow(h_fps_, (!on && !opening) ? TRUE : FALSE);
        EnableWindow(h_snapshot_, on ? TRUE : FALSE);
        EnableWindow(h_rec_start_, (on && !rec) ? TRUE : FALSE);
        EnableWindow(h_rec_stop_, rec ? TRUE : FALSE);
        EnableWindow(h_dir_, rec ? FALSE : TRUE);
        EnableWindow(h_browse_, rec ? FALSE : TRUE);
    }

    void BrowseDirectory() {
        BROWSEINFOW info{};
        info.hwndOwner = hwnd_;
        info.lpszTitle = L"Каталог для сохранения снимков, видео и журнала";
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

    void OnClose() {
        if (recorder_.Active()) {
            const int answer = MessageBoxW(hwnd_, L"Идёт запись видео. Остановить и выйти?",
                                           L"Выход", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
            if (answer != IDYES) return;
            StopRecording();
        }
        DestroyWindow(hwnd_);
    }

    void Cleanup() {
        if (cleaned_) return;
        cleaned_ = true;

        KillTimer(hwnd_, 1);
        if (hotkey_ok_) {
            UnregisterHotKey(hwnd_, 1);
            hotkey_ok_ = false;
        }
        camera_.SetRecorder(nullptr);
        recorder_.Stop();
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
    HWND h_snapshot_ = nullptr, h_rec_start_ = nullptr, h_rec_stop_ = nullptr;
    HWND h_dir_ = nullptr, h_browse_ = nullptr, h_report_ = nullptr;
    HWND h_tray_ = nullptr, h_log_ = nullptr;

    RECT preview_rc_{}, panel_rc_{}, save_rc_{}, log_rc_{}, tray_rc_{};
    RECT camera_label_{}, res_label_{}, fps_label_{}, dir_label_{}, dir_edit_rc_{};
    int separator_y_ = 0;
    int info_top_ = 0;

    Camera camera_;
    Recorder recorder_;
    std::vector<CameraDevice> devices_;
    std::vector<LogEntry> log_;

    std::wstring save_dir_;
    std::wstring status_text_ = L"Камера не подключена";
    std::wstring mode_text_, format_text_, frames_text_;

    bool tray_added_ = false;
    bool hotkey_ok_ = false;
    bool hidden_ = false;
    bool blink_ = false;
};

}  // namespace

// ===========================================================================
//  8. Точка входа
// ===========================================================================
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
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
        g_bg = LoadBg(inst);
        App app;
        if (!app.Init(inst, show)) {
            MessageBoxW(nullptr, L"Не удалось создать окно приложения.", L"Ошибка",
                        MB_OK | MB_ICONERROR);
            result = 1;
        } else {
            result = app.Run();
        }
        delete g_bg;
        g_bg = nullptr;
    }

    if (SUCCEEDED(com)) CoUninitialize();
    GdiplusShutdown(gdiplus_token);
    return result;
}
