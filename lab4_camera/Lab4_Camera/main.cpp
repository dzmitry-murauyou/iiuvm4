// ===========================================================================
//  Лабораторная работа №4. Система видеонаблюдения на базе веб-камеры
//  Вариант А5. Скрытый режим фотонаблюдения
//
//  Всё приложение — один файл: журнал, захват, запись, интерфейс, трей,
//  глобальная горячая клавиша. Сборка: Visual Studio 2026, проект
//  Lab4_Camera.vcxproj (см. README.md).
//
//  Что делает:
//    * предпросмотр с веб-камеры в реальном времени (OpenCV/DirectShow);
//    * снимок JPEG по кнопке и по глобальной клавише Ctrl+Shift+F12;
//    * запись видео AVI/MJPEG;
//    * свёртывание в трей и фоновая работа без окна;
//    * журнал событий с метками времени и выгрузка отчёта.
// ===========================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <commctrl.h>
#include <dbt.h>
#include <dshow.h>
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
#include <cstdio>
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
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "strmiids.lib")

// Common Controls v6 — из-за этого кнопки и список выглядят как в Windows 11.
#pragma comment(linker, "\"/manifestdependency:type='win32' \
 name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
 processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

// ===========================================================================
//  1. Оформление интерфейса
//     Все цвета собраны здесь: чтобы подогнать окно под стиль ЛР №1,
//     достаточно поменять значения в этом блоке.
// ===========================================================================
namespace ui {

const COLORREF kWindow    = RGB(245, 246, 248);   // фон окна
const COLORREF kHeader    = RGB(255, 255, 255);   // шапка
const COLORREF kPanel     = RGB(255, 255, 255);   // панели
const COLORREF kBorder    = RGB(208, 213, 221);   // рамки
const COLORREF kText      = RGB(32, 37, 46);      // основной текст
const COLORREF kTextDim   = RGB(108, 117, 132);   // подписи
const COLORREF kAccent    = RGB(0, 108, 200);     // акцент
const COLORREF kOk        = RGB(22, 152, 84);     // камера подключена
const COLORREF kWarn      = RGB(206, 132, 0);     // подключение / предупреждение
const COLORREF kError     = RGB(198, 40, 40);     // ошибка / запись
const COLORREF kPreview   = RGB(28, 30, 34);      // фон предпросмотра
const COLORREF kLogBg     = RGB(252, 252, 253);   // фон журнала

const int kMargin = 12;

}  // namespace ui

// ===========================================================================
//  2. Мелкие утилиты: строки, время, файлы
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

// Путь в системной ANSI-кодировке: нужен, если OpenCV не понимает UTF-8
// (каталог с кириллицей, например C:\Users\Дмитрий\...).
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

// "20261029_183012" — для имён файлов.
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
        if (partial.size() <= 3 && partial[1] == L':') continue;  // "C:\"
        std::wstring probe = partial;
        while (!probe.empty() && (probe.back() == L'\\' || probe.back() == L'/')) probe.pop_back();
        if (!probe.empty() && !DirExists(probe) &&
            !CreateDirectoryW(probe.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    return DirExists(path);
}

std::wstring ModuleDir() {
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, (DWORD)std::size(path));
    if (n == 0) return L".";
    std::wstring s(path, n);
    const size_t pos = s.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : s.substr(0, pos);
}

std::wstring DefaultSaveDir() {
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, SHGFP_TYPE_CURRENT, path))) {
        return JoinPath(path, L"CameraLab4");
    }
    return JoinPath(ModuleDir(), L"snapshots");
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

// OpenCV принимает путь как байты: пробуем UTF-8, затем системную ANSI.
bool ImwriteAny(const std::wstring& path, const cv::Mat& image, int quality) {
    const std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, std::clamp(quality, 1, 100)};
    try {
        if (cv::imwrite(ToUtf8(path), image, params)) return true;
        return cv::imwrite(ToAnsi(path), image, params);
    } catch (const cv::Exception&) {
        return false;
    }
}

// ===========================================================================
//  3. Журнал событий
// ===========================================================================

enum class LogLevel { Info, Warn, Error, Background };

COLORREF LevelColor(LogLevel level) {
    switch (level) {
        case LogLevel::Error:      return ui::kError;
        case LogLevel::Warn:       return ui::kWarn;
        case LogLevel::Background: return ui::kAccent;
        case LogLevel::Info:       default: return ui::kText;
    }
}

// ===========================================================================
//  4. Камера: перечисление устройств и поток захвата
// ===========================================================================

struct Frame {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> bgr;   // 24 бита, BGR, построчно
    bool ok() const { return width > 0 && height > 0 && !bgr.empty(); }
};

struct CameraDevice {
    std::wstring name;   // "USB2.0 HD UVC WebCam"
    std::wstring path;   // символьный путь устройства
};

// Перечисление камер через DirectShow (ICreateDevEnum + IPropertyBag).
// Индексы совпадают с порядком, который использует OpenCV (бэкенд DSHOW).
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

// Класс записи видео объявлен заранее: поток захвата отдаёт ему кадры.
class Recorder;

enum class CamState { Off, Opening, On, Failed };

// Поток захвата: открывает камеру, постоянно читает кадры и хранит последний.
// GUI берёт копию последнего кадра — блокирующих вызовов в интерфейсе нет.
class Camera {
public:
    Camera() = default;
    ~Camera() { Stop(); }

    void SetNotifyWindow(HWND hwnd) { hwnd_ = hwnd; }
    void SetRecorder(Recorder* recorder) { recorder_.store(recorder); }

    bool Start(int index, int width, int height, double fps) {
        Stop();
        running_.store(true);
        state_ = CamState::Opening;
        worker_ = std::thread(&Camera::Run, this, index, width, height, fps);
        return true;
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
    int Width() const { return width_.load(); }
    int Height() const { return height_.load(); }
    double DeclaredFps() const { return declared_fps_.load(); }
    double MeasuredFps() const { return measured_fps_.load(); }
    std::wstring FormatName() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return format_;
    }
    unsigned long long FrameCount() const { return frame_count_.load(); }

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

    std::atomic<int> width_{0};
    std::atomic<int> height_{0};
    std::atomic<double> declared_fps_{0.0};
    std::atomic<double> measured_fps_{0.0};
    std::atomic<unsigned long long> frame_count_{0};
};

// ===========================================================================
//  5. Запись видео (AVI / MJPEG) в отдельном потоке
// ===========================================================================

class Recorder {
public:
    ~Recorder() { Stop(); }

    bool Active() const { return active_.load(); }

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

    unsigned long long Frames() const { return frames_.load(); }
    double Seconds() const {
        if (!active_.load()) return seconds_;
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    }
    std::wstring Path() const { return path_; }

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
                cv::Mat mat(frame.height, frame.width, CV_8UC3, frame.bgr.data());
                if (frame.width != width_ || frame.height != height_) {
                    cv::Mat resized;
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

// Определение потока захвата вынесено сюда: внутри класса Camera тип Recorder
// ещё неполный, а Push() нужно вызывать по-настоящему.
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

    cv::Mat frame;
    try {
        capture.read(frame);
    } catch (const cv::Exception&) {
        frame = cv::Mat();
    }
    if (frame.empty()) {
        capture.release();
        Notify(CamState::Failed, L"Камера не отдаёт кадры (проверьте доступ к камере в Windows)");
        running_.store(false);
        return;
    }

    const std::string fourcc_code = [&] {
        const int code = (int)capture.get(cv::CAP_PROP_FOURCC);
        std::string s(4, '\0');
        for (int i = 0; i < 4; ++i) s[i] = (char)((code >> (8 * i)) & 0xFF);
        return s;
    }();

    width_.store(frame.cols);
    height_.store(frame.rows);
    declared_fps_.store(capture.get(cv::CAP_PROP_FPS));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        format_ = std::wstring(fourcc_code.begin(), fourcc_code.end());
        if (format_.empty() || format_.size() != 4) format_ = L"BGR24";
    }
    Notify(CamState::On, Format(L"Камера подключена: %dx%d", frame.cols, frame.rows));

    auto fps_start = std::chrono::steady_clock::now();
    unsigned long long fps_frames = 0;
    int failures = 0;

    while (running_.load()) {
        try {
            capture.read(frame);
        } catch (const cv::Exception&) {
            frame = cv::Mat();
        }

        if (frame.empty()) {
            // Камеру могли выдернуть: несколько попыток, затем выходим.
            if (++failures > 30) break;
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
            std::memcpy(out.bgr.data(), frame.data, row * (size_t)frame.rows);
        } else {
            for (int y = 0; y < frame.rows; ++y) {
                std::memcpy(out.bgr.data() + row * (size_t)y, frame.ptr(y), row);
            }
        }
        width_.store(out.width);
        height_.store(out.height);

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

        if (HWND hwnd = hwnd_) PostMessageW(hwnd, WM_APP + 2, 0, 0);   // "новый кадр"
    }

    capture.release();
    measured_fps_.store(0.0);
    if (running_.load()) {
        Notify(CamState::Failed, L"Камера перестала отдавать кадры — подключите её заново");
    }
    running_.store(false);
}


// ===========================================================================
//  6. Главное окно приложения
// ===========================================================================

const wchar_t* kClassName = L"Lab4CameraWindow";
const wchar_t* kAppTitle = L"ЛР №4 — видеонаблюдение (вариант А5: скрытый режим фотонаблюдения)";

struct LogEntry {
    LogLevel level;
    std::wstring text;
};

const wchar_t* kResolutions[] = {L"1920 × 1080", L"1280 × 720", L"1024 × 768",
                                 L"960 × 540",   L"800 × 600",  L"640 × 480"};
const int kResW[] = {1920, 1280, 1024, 960, 800, 640};
const int kResH[] = {1080, 720, 768, 540, 600, 480};

const wchar_t* kFpsTitles[] = {L"30", L"25", L"20", L"15", L"10"};
const double kFpsValues[] = {30.0, 25.0, 20.0, 15.0, 10.0};

class App {
public:
    enum : UINT {
        WM_STATE = WM_APP + 1,   // состояние камеры (из потока захвата)
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
        ID_OPEN_DIR,
        ID_REPORT,
        ID_CLEAR_LOG,
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
                                     LR_DEFAULTSIZE);
        if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = wc.hIcon;
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return false;

        hwnd_ = CreateWindowExW(0, kClassName, kAppTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1180, 780, nullptr, nullptr, inst_,
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
    //  Создание окна
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
                info->ptMinTrackSize.y = S(660);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd_, &ps);
                RECT rc;
                GetClientRect(hwnd_, &rc);
                if (rc.right > 0 && rc.bottom > 0) {
                    // Двойная буферизация: окно не мерцает на 30 кадрах в секунду.
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
                if (dis && dis->CtlID == ID_LOG) {
                    DrawLogItem(dis);
                    return TRUE;
                }
                break;
            }
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORBTN: {
                HDC dc = (HDC)wp;
                SetTextColor(dc, ui::kText);
                SetBkColor(dc, ui::kPanel);
                return (LRESULT)panel_brush_;
            }
            case WM_CTLCOLOREDIT: {
                HDC dc = (HDC)wp;
                SetTextColor(dc, ui::kText);
                SetBkColor(dc, ui::kPanel);
                return (LRESULT)panel_brush_;
            }
            case WM_CTLCOLORLISTBOX: {
                HDC dc = (HDC)wp;
                SetTextColor(dc, ui::kText);
                SetBkColor(dc, ui::kLogBg);
                return (LRESULT)log_brush_;
            }
            case WM_DPICHANGED: {
                dpi_ = HIWORD(wp);
                CreateFonts();
                RECT* rc = reinterpret_cast<RECT*>(lp);
                SetWindowPos(hwnd_, nullptr, rc->left, rc->top, rc->right - rc->left,
                             rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
                Layout();
                return 0;
            }
            case WM_SYSCOMMAND:
                if ((wp & 0xFFF0) == SC_MINIMIZE && minimize_to_tray_) {
                    HideToTray();
                    return 0;
                }
                break;
            case WM_CLOSE:
                OnClose();
                return 0;
            case WM_DESTROY:
                Cleanup();
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    void OnCreate() {
        dpi_ = 96;
        if (HMODULE user32 = GetModuleHandleW(L"user32")) {
            using GetDpiFn = UINT(WINAPI*)(HWND);
            auto get_dpi = reinterpret_cast<GetDpiFn>(GetProcAddress(user32, "GetDpiForWindow"));
            if (get_dpi) {
                const UINT dpi = get_dpi(hwnd_);
                if (dpi >= 96 && dpi <= 480) dpi_ = (int)dpi;
            }
        }

        CreateFonts();
        panel_brush_ = CreateSolidBrush(ui::kPanel);
        log_brush_ = CreateSolidBrush(ui::kLogBg);
        window_brush_ = CreateSolidBrush(ui::kWindow);
        header_brush_ = CreateSolidBrush(ui::kHeader);
        border_brush_ = CreateSolidBrush(ui::kBorder);
        preview_brush_ = CreateSolidBrush(ui::kPreview);

        LoadSettings();
        if (settings_.save_dir.empty()) settings_.save_dir = DefaultSaveDir();
        EnsureDir(settings_.save_dir);
        CreateControls();

        camera_.SetNotifyWindow(hwnd_);
        camera_.SetRecorder(&recorder_);

        // Значок в трее: работает всё время, не только в скрытом режиме.
        tray_icon_ = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(101), IMAGE_ICON, 0, 0,
                                       LR_DEFAULTSIZE | LR_SHARED);
        if (!tray_icon_) tray_icon_ = LoadIconW(nullptr, IDI_APPLICATION);
        AddTrayIcon();

        // Глобальная горячая клавиша создания снимка.
        hotkey_ok_ = RegisterHotKey(hwnd_, 1, MOD_CONTROL | MOD_SHIFT, VK_F12) != FALSE;

        SetTimer(hwnd_, 1, 1000, nullptr);

        AddLog(LogLevel::Info, L"Приложение запущено");
        if (hotkey_ok_) {
            AddLog(LogLevel::Info, L"Глобальная клавиша Ctrl+Shift+F12 — снимок в фоне");
        } else {
            AddLog(LogLevel::Warn,
                   L"Ctrl+Shift+F12 уже занята другим приложением — глобальный снимок недоступен");
        }

        RefreshDevices(true);
        Layout();
    }

    void CreateFonts() {
        if (font_) DeleteObject(font_);
        if (font_bold_) DeleteObject(font_bold_);
        if (font_title_) DeleteObject(font_title_);
        if (font_mono_) DeleteObject(font_mono_);
        const int h = -MulDiv(9, dpi_, 72);
        font_ = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH, L"Segoe UI");
        font_bold_ = CreateFontW(h, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH, L"Segoe UI");
        font_title_ = CreateFontW(-MulDiv(12, dpi_, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        font_mono_ = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 FIXED_PITCH | FF_MODERN, L"Consolas");
    }

    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
        HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                                 (HMENU)(INT_PTR)id, inst_, nullptr);
        if (h) SendMessageW(h, WM_SETFONT, (WPARAM)font_, TRUE);
        return h;
    }

    void CreateControls() {
        h_camera_ = Make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_CAMERA,
                         WS_EX_CLIENTEDGE);
        h_refresh_ = Make(L"BUTTON", L"Обновить", BS_PUSHBUTTON | WS_TABSTOP, ID_REFRESH);
        h_res_ = Make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_RES,
                      WS_EX_CLIENTEDGE);
        h_fps_ = Make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_FPS,
                      WS_EX_CLIENTEDGE);
        h_connect_ = Make(L"BUTTON", L"Подключить камеру", BS_PUSHBUTTON | WS_TABSTOP, ID_CONNECT,
                          WS_EX_CLIENTEDGE);
        h_disconnect_ = Make(L"BUTTON", L"Отключить", BS_PUSHBUTTON | WS_TABSTOP, ID_DISCONNECT);
        h_snapshot_ = Make(L"BUTTON", L"Снимок", BS_PUSHBUTTON | WS_TABSTOP, ID_SNAPSHOT);
        h_rec_start_ = Make(L"BUTTON", L"Начать запись", BS_PUSHBUTTON | WS_TABSTOP, ID_REC_START);
        h_rec_stop_ = Make(L"BUTTON", L"Остановить запись", BS_PUSHBUTTON | WS_TABSTOP, ID_REC_STOP);
        h_dir_ = Make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_DIR, WS_EX_CLIENTEDGE);
        h_browse_ = Make(L"BUTTON", L"Обзор…", BS_PUSHBUTTON | WS_TABSTOP, ID_BROWSE);
        h_open_dir_ = Make(L"BUTTON", L"Открыть каталог", BS_PUSHBUTTON | WS_TABSTOP, ID_OPEN_DIR);
        h_report_ = Make(L"BUTTON", L"Сохранить отчёт", BS_PUSHBUTTON | WS_TABSTOP, ID_REPORT);
        h_clear_log_ = Make(L"BUTTON", L"Очистить", BS_PUSHBUTTON | WS_TABSTOP, ID_CLEAR_LOG);
        h_tray_ = Make(L"BUTTON", L"Свернуть в трей", BS_PUSHBUTTON | WS_TABSTOP, ID_TRAY_BUTTON);
        h_log_ = Make(L"LISTBOX", L"",
                      WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                          WS_TABSTOP,
                      ID_LOG, WS_EX_CLIENTEDGE);
        if (h_log_) {
            SendMessageW(h_log_, WM_SETFONT, (WPARAM)font_mono_, TRUE);
            SendMessageW(h_log_, LB_SETITEMHEIGHT, 0, S(16));
        }

        for (const wchar_t* title : kResolutions) {
            SendMessageW(h_res_, CB_ADDSTRING, 0, (LPARAM)title);
        }
        for (const wchar_t* title : kFpsTitles) {
            SendMessageW(h_fps_, CB_ADDSTRING, 0, (LPARAM)title);
        }
        SendMessageW(h_res_, CB_SETCURSEL, std::clamp(settings_.resolution, 0,
                                                      (int)std::size(kResolutions) - 1), 0);
        SendMessageW(h_fps_, CB_SETCURSEL, std::clamp(settings_.fps, 0,
                                                      (int)std::size(kFpsValues) - 1), 0);
        SetWindowTextW(h_dir_, settings_.save_dir.c_str());
    }

    // -----------------------------------------------------------------------
    //  Разметка и отрисовка
    // -----------------------------------------------------------------------
    int S(int value) const { return MulDiv(value, dpi_, 96); }

    void Layout() {
        if (!h_log_) return;
        RECT rc;
        GetClientRect(hwnd_, &rc);
        const int m = S(ui::kMargin);
        const int header_h = S(58);
        header_rc_ = {0, 0, rc.right, header_h};

        const int log_h = std::max<int>(S(140), rc.bottom / 5);
        log_rc_ = {m, rc.bottom - log_h - m, rc.right - m, rc.bottom - m};

        const int panel_w = std::min<int>(S(360), std::max<int>(S(320), rc.right / 3));
        panel_rc_ = {rc.right - m - panel_w, header_h + m, rc.right - m, log_rc_.top - m};

        const int left = m;
        const int left_r = panel_rc_.left - m;
        const int strip_h = S(92);
        strip_rc_ = {left, panel_rc_.bottom - strip_h, left_r, panel_rc_.bottom};
        preview_rc_ = {left, header_h + m, left_r, strip_rc_.top - m};

        // Панель устройства
        {
            const int p = S(12);
            const int row = S(24);
            int y = panel_rc_.top + S(38);
            h_camera_label_rc_ = {panel_rc_.left + p, y, panel_rc_.right - p, y + S(16)};
            y += S(18);
            const int refresh_w = S(90);
            SetWindowPos(h_camera_, nullptr, panel_rc_.left + p, y,
                         panel_rc_.right - panel_rc_.left - 2 * p - refresh_w - S(8), S(200),
                         SWP_NOZORDER);
            SetWindowPos(h_refresh_, nullptr, panel_rc_.right - p - refresh_w, y, refresh_w, row,
                         SWP_NOZORDER);
            y += row + S(12);

            h_res_label_rc_ = {panel_rc_.left + p, y, panel_rc_.right - p, y + S(16)};
            h_fps_label_rc_ = h_res_label_rc_;
            y += S(18);
            const int half = (panel_rc_.right - panel_rc_.left - 2 * p - S(8)) / 2;
            SetWindowPos(h_res_, nullptr, panel_rc_.left + p, y, half, S(200), SWP_NOZORDER);
            SetWindowPos(h_fps_, nullptr, panel_rc_.left + p + half + S(8), y, half, S(200),
                         SWP_NOZORDER);
            h_res_label_rc_.right = panel_rc_.left + p + half;
            h_fps_label_rc_.left = panel_rc_.left + p + half + S(8);
            y += row + S(12);

            const int btn_w = (panel_rc_.right - panel_rc_.left - 2 * p - S(8)) / 2;
            SetWindowPos(h_connect_, nullptr, panel_rc_.left + p, y, btn_w, S(30), SWP_NOZORDER);
            SetWindowPos(h_disconnect_, nullptr, panel_rc_.left + p + btn_w + S(8), y, btn_w,
                         S(30), SWP_NOZORDER);
            y += S(30) + S(16);

            info_rc_ = {panel_rc_.left + p, y + S(20), panel_rc_.right - p,
                        panel_rc_.bottom - S(12)};
        }

        // Панель сохранения под предпросмотром
        {
            const int p = S(12);
            const int row = S(24);
            const int y = strip_rc_.top + S(10);
            h_dir_label_rc_ = {strip_rc_.left + p, y, strip_rc_.left + p + S(64), y + row};
            const int browse_w = S(84);
            const int edit_x = h_dir_label_rc_.right + S(6);
            const int edit_w =
                std::max<int>(S(120), (int)(strip_rc_.right - p - browse_w - S(8) - edit_x));
            SetWindowPos(h_dir_, nullptr, edit_x, y, edit_w, row, SWP_NOZORDER);
            SetWindowPos(h_browse_, nullptr, edit_x + edit_w + S(8), y, browse_w, row, SWP_NOZORDER);

            const int by = y + row + S(10);
            const int bh = S(30);
            int x = strip_rc_.left + p;
            const int gap = S(8);
            const int btn_w = (strip_rc_.right - p - x - 3 * gap) / 4;
            HWND buttons[4] = {h_snapshot_, h_rec_start_, h_rec_stop_, h_open_dir_};
            for (int i = 0; i < 4; ++i) {
                const int w = (i == 3) ? std::max<int>(S(110), (int)(strip_rc_.right - p - x))
                                       : btn_w;
                SetWindowPos(buttons[i], nullptr, x, by, w, bh, SWP_NOZORDER);
                x += w + gap;
            }
        }

        // Журнал
        {
            const int p = S(10);
            const int bh = S(26);
            SetWindowPos(h_report_, nullptr, log_rc_.right - p - S(140), log_rc_.top + p, S(140),
                         bh, SWP_NOZORDER);
            SetWindowPos(h_clear_log_, nullptr, log_rc_.right - p - S(140) - S(8) - S(90),
                         log_rc_.top + p, S(90), bh, SWP_NOZORDER);
            h_log_title_rc_ = {log_rc_.left + p, log_rc_.top + p, log_rc_.right - S(260),
                               log_rc_.top + p + bh};
            const int top = log_rc_.top + p + bh + S(6);
            SetWindowPos(h_log_, nullptr, log_rc_.left + p, top, log_rc_.right - log_rc_.left - 2 * p,
                         log_rc_.bottom - p - top, SWP_NOZORDER);
        }

        // Шапка
        {
            const int bh = S(28);
            SetWindowPos(h_tray_, nullptr, rc.right - m - S(150), (header_h - bh) / 2, S(150), bh,
                         SWP_NOZORDER);
        }

        UpdateControlsState();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Paint(HDC dc, const RECT& client) {
        FillRect(dc, &client, window_brush_);

        // ---- шапка ----
        FillRect(dc, &header_rc_, header_brush_);
        RECT line = {0, header_rc_.bottom - 1, header_rc_.right, header_rc_.bottom};
        FillRect(dc, &line, border_brush_);
        Text(dc, L"Система видеонаблюдения на базе веб-камеры",
             {S(16), S(8), header_rc_.right / 2, S(32)}, font_title_, ui::kText);
        Text(dc, L"Лабораторная работа №4 · вариант А5 · скрытый режим фотонаблюдения",
             {S(16), S(32), header_rc_.right / 2, S(50)}, font_, ui::kTextDim);

        // индикатор состояния
        COLORREF dot = ui::kTextDim;
        if (camera_.State() == CamState::On) dot = ui::kOk;
        else if (camera_.State() == CamState::Opening) dot = ui::kWarn;
        else if (camera_.State() == CamState::Failed) dot = ui::kError;

        const int status_right = header_rc_.right - S(150) - S(24);
        const int status_left = std::max<int>(header_rc_.right / 2, status_right - S(330));
        RECT status_rc = {status_left, S(14), status_right, S(44)};
        const int cy = (status_rc.top + status_rc.bottom) / 2;
        Dot(dc, status_rc.left + S(5), cy, S(5), dot);
        RECT text_rc = status_rc;
        text_rc.left += S(18);
        Text(dc, status_text_, text_rc, font_bold_, dot);

        if (recorder_.Active()) {
            RECT rec = {status_left, S(42), status_right, S(56)};
            if (blink_) Dot(dc, rec.left + S(4), (rec.top + rec.bottom) / 2, S(4), ui::kError);
            rec.left += S(14);
            Text(dc, L"ИДЁТ ЗАПИСЬ  " + Clock(recorder_.Seconds()), rec, font_, ui::kError);
        }

        // ---- предпросмотр ----
        FillRect(dc, &preview_rc_, preview_brush_);
        Frame frame;
        if (camera_.Latest(frame)) {
            const int pw = preview_rc_.right - preview_rc_.left;
            const int ph = preview_rc_.bottom - preview_rc_.top;
            const double scale =
                std::min((double)pw / frame.width, (double)ph / frame.height);
            const int w = std::max(1, (int)(frame.width * scale));
            const int h = std::max(1, (int)(frame.height * scale));
            const int x = preview_rc_.left + (pw - w) / 2;
            const int y = preview_rc_.top + (ph - h) / 2;

            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = frame.width;
            bmi.bmiHeader.biHeight = -frame.height;   // строки сверху вниз
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 24;
            bmi.bmiHeader.biCompression = BI_RGB;

            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, x, y, w, h, 0, 0, frame.width, frame.height, frame.bgr.data(), &bmi,
                          DIB_RGB_COLORS, SRCCOPY);

            // дата и время в кадре
            const std::wstring stamp = NowStamp(false);
            RECT box = {x + S(10), y + h - S(30), x + S(240), y + h - S(8)};
            HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(dc, &box, black);
            DeleteObject(black);
            Text(dc, stamp, box, font_, RGB(235, 235, 235));
        } else {
            Text(dc, camera_.State() == CamState::Opening ? L"Подключение камеры…"
                                                          : L"Нет видеосигнала",
                 preview_rc_, font_title_, ui::kTextDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        FrameRectBorder(dc, preview_rc_);

        // ---- панель устройства ----
        FillRect(dc, &panel_rc_, panel_brush_);
        FrameRectBorder(dc, panel_rc_);
        Text(dc, L"Камера", {panel_rc_.left + S(12), panel_rc_.top + S(10), panel_rc_.right,
                             panel_rc_.top + S(32)}, font_bold_, ui::kText);
        Text(dc, L"Устройство", h_camera_label_rc_, font_, ui::kTextDim);
        Text(dc, L"Разрешение", h_res_label_rc_, font_, ui::kTextDim);
        Text(dc, L"Кадров/с", h_fps_label_rc_, font_, ui::kTextDim);

        RECT info_title = {info_rc_.left, info_rc_.top - S(20), info_rc_.right, info_rc_.top};
        Text(dc, L"Информация", info_title, font_bold_, ui::kText);
        const std::wstring lines[][2] = {
            {L"Устройство", camera_name_},
            {L"Разрешение", resolution_text_},
            {L"Формат", format_text_},
            {L"Кадров", frames_text_},
            {L"Запись", record_text_},
        };
        int y = info_rc_.top + S(4);
        for (const auto& line : lines) {
            RECT name_rc = {info_rc_.left, y, info_rc_.left + S(96), y + S(18)};
            RECT value_rc = {name_rc.right, y, info_rc_.right, y + S(18)};
            Text(dc, line[0], name_rc, font_, ui::kTextDim);
            Text(dc, line[1].empty() ? L"—" : line[1], value_rc, font_, ui::kText);
            y += S(20);
        }

        // ---- панель сохранения ----
        FillRect(dc, &strip_rc_, panel_brush_);
        FrameRectBorder(dc, strip_rc_);
        Text(dc, L"Сохранение", {strip_rc_.left + S(12), strip_rc_.top + S(6), strip_rc_.right,
                                 strip_rc_.top + S(24)}, font_bold_, ui::kText);
        Text(dc, L"Каталог", h_dir_label_rc_, font_, ui::kTextDim);

        // ---- журнал ----
        FillRect(dc, &log_rc_, panel_brush_);
        FrameRectBorder(dc, log_rc_);
        Text(dc, L"Журнал событий", h_log_title_rc_, font_bold_, ui::kText);
    }

    void Dot(HDC dc, int cx, int cy, int radius, COLORREF color) {
        HBRUSH brush = CreateSolidBrush(color);
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        HGDIOBJ old_brush = SelectObject(dc, brush);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        Ellipse(dc, cx - radius, cy - radius, cx + radius, cy + radius);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
        DeleteObject(pen);
        DeleteObject(brush);
    }

    void FrameRectBorder(HDC dc, const RECT& rc) {
        HPEN pen = CreatePen(PS_SOLID, 1, ui::kBorder);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
    }

    void Text(HDC dc, const std::wstring& text, RECT rc, HFONT font, COLORREF color,
              UINT format = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) {
        HGDIOBJ old_font = SelectObject(dc, font);
        const int old_mode = SetBkMode(dc, TRANSPARENT);
        const COLORREF old_color = SetTextColor(dc, color);
        DrawTextW(dc, text.c_str(), (int)text.size(), &rc, format | DT_NOPREFIX);
        SetTextColor(dc, old_color);
        SetBkMode(dc, old_mode);
        SelectObject(dc, old_font);
    }

    void DrawLogItem(DRAWITEMSTRUCT* dis) {
        wchar_t buffer[1024] = {};
        SendMessageW(dis->hwndItem, LB_GETTEXT, dis->itemID, (LPARAM)buffer);
        const std::wstring line(buffer);
        const bool selected = (dis->itemState & ODS_SELECTED) != 0;
        HBRUSH bg = CreateSolidBrush(selected ? RGB(232, 240, 250) : ui::kLogBg);
        FillRect(dis->hDC, &dis->rcItem, bg);
        DeleteObject(bg);

        const LogLevel level = (LogLevel)SendMessageW(dis->hwndItem, LB_GETITEMDATA, dis->itemID, 0);
        const size_t time_len = 23;   // "29.10.2026 18:30:12.123"
        RECT rc = dis->rcItem;
        rc.left += S(6);
        RECT time_rc = rc;
        time_rc.right = rc.left + S(150);
        Text(dis->hDC, line.substr(0, std::min(time_len, line.size())), time_rc, font_mono_,
             ui::kTextDim);
        RECT text_rc = rc;
        text_rc.left = time_rc.right;
        Text(dis->hDC, line.size() > time_len ? line.substr(time_len) : L"", text_rc, font_mono_,
             LevelColor(level));
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
            const int index =
                (int)SendMessageW(h_log_, LB_ADDSTRING, 0, (LPARAM)line.c_str());
            if (index >= 0) {
                SendMessageW(h_log_, LB_SETITEMDATA, index, (LPARAM)level);
                SendMessageW(h_log_, LB_SETTOPINDEX, index, 0);
            }
            while (SendMessageW(h_log_, LB_GETCOUNT, 0, 0) > 500) {
                SendMessageW(h_log_, LB_DELETESTRING, 0, 0);
            }
        }

        // Дублируем в файл, чтобы журнал сохранялся даже при аварийном закрытии.
        if (!settings_.save_dir.empty()) {
            const std::wstring path = JoinPath(settings_.save_dir, L"camera_log.txt");
            HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA,
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
            const int index = std::clamp(settings_.device, 0, (int)devices_.size() - 1);
            SendMessageW(h_camera_, CB_SETCURSEL, index, 0);
            if (!initial) {
                AddLog(LogLevel::Info,
                       Format(L"Список камер обновлён: найдено %d", (int)devices_.size()));
            }
        } else {
            SendMessageW(h_camera_, CB_SETCURSEL, (WPARAM)-1, 0);
            AddLog(LogLevel::Warn,
                   L"Камеры не найдены. Проверьте подключение и разрешение доступа к камере "
                   L"(Параметры → Конфиденциальность → Камера)");
        }
        UpdateControlsState();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Connect() {
        if (camera_.State() == CamState::On || camera_.State() == CamState::Opening) return;
        if (devices_.empty()) return;

        const int selected = (int)SendMessageW(h_camera_, CB_GETCURSEL, 0, 0);
        const int index = std::clamp(selected, 0, (int)devices_.size() - 1);
        const int res = std::clamp((int)SendMessageW(h_res_, CB_GETCURSEL, 0, 0), 0,
                                   (int)std::size(kResolutions) - 1);
        const int fps = std::clamp((int)SendMessageW(h_fps_, CB_GETCURSEL, 0, 0), 0,
                                   (int)std::size(kFpsValues) - 1);

        settings_.device = index;
        settings_.resolution = res;
        settings_.fps = fps;
        SaveSettings();

        status_text_ = L"Подключение камеры…";
        AddLog(LogLevel::Info,
               Format(L"Подключение «%s» (%dx%d, %.0f к/с)", devices_[index].name.c_str(),
                      kResW[res], kResH[res], kFpsValues[fps]));
        camera_.Start(index, kResW[res], kResH[res], kFpsValues[fps]);
        UpdateControlsState();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Disconnect() {
        if (recorder_.Active()) StopRecording();
        const bool was_active = camera_.State() == CamState::On ||
                                camera_.State() == CamState::Opening;
        camera_.Stop();
        status_text_ = L"Камера не подключена";
        if (was_active) AddLog(LogLevel::Info, L"Захват остановлен");
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
            AddLog(LogLevel::Error, L"Не удалось создать каталог: " + dir);
            dir = JoinPath(ModuleDir(), L"snapshots");
            EnsureDir(dir);
            SetWindowTextW(h_dir_, dir.c_str());
        }
        if (dir != settings_.save_dir) {
            settings_.save_dir = dir;
            SaveSettings();
        }
    }

    void TakeSnapshot(bool background) {
        ApplyDirectory();

        Frame frame;
        if (!camera_.Latest(frame)) {
            AddLog(LogLevel::Warn, L"Снимок не создан: нет видеосигнала");
            if (background) Balloon(L"Снимок не создан", L"Нет видеосигнала", NIIF_WARNING);
            return;
        }

        const std::wstring path = UniquePath(settings_.save_dir, L"snapshot", L"jpg");
        cv::Mat mat(frame.height, frame.width, CV_8UC3, frame.bgr.data());
        if (!ImwriteAny(path, mat, 92)) {
            AddLog(LogLevel::Error, L"Не удалось сохранить снимок: " + FileName(path));
            Balloon(L"Ошибка", L"Не удалось сохранить снимок", NIIF_ERROR);
            return;
        }

        const std::wstring size = Format(L"%.1f КБ", (double)FileSize(path) / 1024.0);
        AddLog(background ? LogLevel::Background : LogLevel::Info,
               Format(L"Снимок сохранён: %s (%dx%d, %s)", FileName(path).c_str(), frame.width,
                      frame.height, size.c_str()));
        Balloon(background ? L"Снимок (скрытый режим)" : L"Снимок сохранён",
                FileName(path) + L"\n" + settings_.save_dir, NIIF_INFO);
    }

    void StartRecording() {
        if (recorder_.Active()) return;
        if (camera_.State() != CamState::On) {
            AddLog(LogLevel::Warn, L"Запись невозможна: камера не подключена");
            return;
        }
        ApplyDirectory();

        Frame frame;
        if (!camera_.Latest(frame)) {
            AddLog(LogLevel::Warn, L"Запись невозможна: нет кадра");
            return;
        }

        double fps = camera_.MeasuredFps();
        if (fps < 1.0) fps = camera_.DeclaredFps();
        if (fps < 1.0) fps = 25.0;

        const std::wstring path = UniquePath(settings_.save_dir, L"video", L"avi");
        if (!recorder_.Start(path, frame.width, frame.height, fps)) {
            AddLog(LogLevel::Error, L"Не удалось открыть файл записи: " + FileName(path));
            Balloon(L"Ошибка записи", FileName(path), NIIF_ERROR);
            return;
        }

        AddLog(LogLevel::Info,
               Format(L"Запись начата: %s (%dx%d, %.0f к/с, MJPEG/AVI)", FileName(path).c_str(),
                      frame.width, frame.height, fps));
        if (Hidden()) Balloon(L"Запись начата", FileName(path), NIIF_INFO);
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
        if (Hidden()) Balloon(L"Запись остановлена", FileName(path), NIIF_INFO);
        UpdateControlsState();
    }

    void SaveReport() {
        ApplyDirectory();
        const std::wstring path =
            JoinPath(settings_.save_dir, L"report_" + FileStamp() + L".txt");

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
        Balloon(L"Отчёт сохранён", FileName(path) + L"\n" + settings_.save_dir, NIIF_INFO);
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
        data.hIcon = tray_icon_;
        wcscpy_s(data.szTip, L"Видеонаблюдение — ЛР №4, вариант А5");
        tray_added_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    }

    void HideToTray() {
        // Если камера не подключена — подключаем: иначе фоновый снимок невозможен.
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

    bool Hidden() const { return hidden_; }

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
    //  Прочее: команды, таймер, настройки, завершение
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
            case ID_OPEN_DIR:
                if (code == BN_CLICKED) {
                    ApplyDirectory();
                    ShellExecuteW(hwnd_, L"open", settings_.save_dir.c_str(), nullptr, nullptr,
                                  SW_SHOWNORMAL);
                }
                break;
            case ID_REPORT:
                if (code == BN_CLICKED) SaveReport();
                break;
            case ID_CLEAR_LOG:
                if (code == BN_CLICKED) {
                    log_.clear();
                    SendMessageW(h_log_, LB_RESETCONTENT, 0, 0);
                    AddLog(LogLevel::Info, L"Журнал очищен");
                }
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
            case ID_DIR:
                if (code == EN_CHANGE) {
                    wchar_t buffer[MAX_PATH * 2] = {};
                    GetWindowTextW(h_dir_, buffer, (int)std::size(buffer));
                    settings_.save_dir = buffer;
                }
                break;
            default:
                break;
        }
    }

    void OnTimer() {
        blink_ = !blink_;

        Frame frame;
        if (devices_.empty()) {
            camera_name_ = L"не найдена";
        } else {
            int selected = (int)SendMessageW(h_camera_, CB_GETCURSEL, 0, 0);
            selected = std::clamp(selected, 0, (int)devices_.size() - 1);
            camera_name_ = devices_[selected].name;
        }

        if (camera_.Latest(frame)) {
            const double fps = camera_.MeasuredFps();
            resolution_text_ = Format(L"%d × %d%s", frame.width, frame.height,
                                      fps > 0.5 ? Format(L", %.0f к/с", fps).c_str() : L"");
            format_text_ = camera_.FormatName();
            frames_text_ = Format(L"%llu", camera_.FrameCount());
        } else {
            resolution_text_.clear();
            format_text_.clear();
            frames_text_.clear();
        }
        record_text_ = recorder_.Active()
                           ? Format(L"идёт, %s", Clock(recorder_.Seconds()).c_str())
                           : L"нет";

        UpdateControlsState();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void UpdateControlsState() {
        const bool on = camera_.State() == CamState::On;
        const bool opening = camera_.State() == CamState::Opening;
        const bool rec = recorder_.Active();
        const bool has_devices = !devices_.empty();

        EnableWindow(h_connect_, (!on && !opening && has_devices) ? TRUE : FALSE);
        EnableWindow(h_disconnect_, (on || opening || camera_.State() == CamState::Failed) ? TRUE
                                                                                          : FALSE);
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
        SaveSettings();
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

        for (HBRUSH* brush : {&panel_brush_, &log_brush_, &window_brush_, &header_brush_,
                              &border_brush_, &preview_brush_}) {
            if (*brush) DeleteObject(*brush);
            *brush = nullptr;
        }
        if (font_) DeleteObject(font_);
        if (font_bold_) DeleteObject(font_bold_);
        if (font_title_) DeleteObject(font_title_);
        if (font_mono_) DeleteObject(font_mono_);
        font_ = font_bold_ = font_title_ = font_mono_ = nullptr;
    }

    struct Settings {
        std::wstring save_dir;   // заполняется в OnCreate (после CoInitialize)
        int device = 0;
        int resolution = 1;
        int fps = 0;
        bool minimize_to_tray = false;
    };

    void LoadSettings() {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Lab4Camera", 0, KEY_READ, &key) !=
            ERROR_SUCCESS) {
            return;
        }
        wchar_t buffer[MAX_PATH * 2] = {};
        DWORD size = sizeof(buffer);
        DWORD type = 0;
        if (RegQueryValueExW(key, L"SaveDir", nullptr, &type, (LPBYTE)buffer, &size) ==
                ERROR_SUCCESS &&
            type == REG_SZ && buffer[0]) {
            settings_.save_dir = buffer;
        }
        auto read_dword = [&](const wchar_t* name, int* value, bool* flag) {
            DWORD data = 0;
            DWORD data_size = sizeof(data);
            DWORD data_type = 0;
            if (RegQueryValueExW(key, name, nullptr, &data_type, (LPBYTE)&data, &data_size) ==
                    ERROR_SUCCESS &&
                data_type == REG_DWORD) {
                if (value) *value = (int)data;
                if (flag) *flag = data != 0;
            }
        };
        read_dword(L"Device", &settings_.device, nullptr);
        read_dword(L"Resolution", &settings_.resolution, nullptr);
        read_dword(L"Fps", &settings_.fps, nullptr);
        read_dword(L"MinimizeToTray", nullptr, &settings_.minimize_to_tray);
        RegCloseKey(key);
        minimize_to_tray_ = settings_.minimize_to_tray;
    }

    void SaveSettings() {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Lab4Camera", 0, nullptr, 0, KEY_WRITE,
                            nullptr, &key, nullptr) != ERROR_SUCCESS) {
            return;
        }
        RegSetValueExW(key, L"SaveDir", 0, REG_SZ, (const BYTE*)settings_.save_dir.c_str(),
                       (DWORD)((settings_.save_dir.size() + 1) * sizeof(wchar_t)));
        auto write_dword = [&](const wchar_t* name, int value) {
            DWORD data = (DWORD)value;
            RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&data, sizeof(data));
        };
        write_dword(L"Device", settings_.device);
        write_dword(L"Resolution", settings_.resolution);
        write_dword(L"Fps", settings_.fps);
        write_dword(L"MinimizeToTray", minimize_to_tray_ ? 1 : 0);
        RegCloseKey(key);
    }

    // --- поля --------------------------------------------------------------
    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    int dpi_ = 96;
    bool cleaned_ = false;

    HFONT font_ = nullptr, font_bold_ = nullptr, font_title_ = nullptr, font_mono_ = nullptr;
    HBRUSH panel_brush_ = nullptr, log_brush_ = nullptr, window_brush_ = nullptr,
           header_brush_ = nullptr, border_brush_ = nullptr, preview_brush_ = nullptr;

    HWND h_camera_ = nullptr, h_refresh_ = nullptr, h_res_ = nullptr, h_fps_ = nullptr;
    HWND h_connect_ = nullptr, h_disconnect_ = nullptr;
    HWND h_snapshot_ = nullptr, h_rec_start_ = nullptr, h_rec_stop_ = nullptr;
    HWND h_dir_ = nullptr, h_browse_ = nullptr, h_open_dir_ = nullptr;
    HWND h_report_ = nullptr, h_clear_log_ = nullptr, h_tray_ = nullptr, h_log_ = nullptr;

    RECT header_rc_{}, preview_rc_{}, panel_rc_{}, strip_rc_{}, log_rc_{}, info_rc_{};
    RECT h_camera_label_rc_{}, h_res_label_rc_{}, h_fps_label_rc_{}, h_dir_label_rc_{},
        h_log_title_rc_{};

    Camera camera_;
    Recorder recorder_;
    std::vector<CameraDevice> devices_;
    std::vector<LogEntry> log_;

    Settings settings_;
    std::wstring status_text_ = L"Камера не подключена";
    std::wstring camera_name_ = L"не найдена";
    std::wstring resolution_text_;
    std::wstring format_text_;
    std::wstring frames_text_;
    std::wstring record_text_ = L"нет";

    bool tray_added_ = false;
    bool hotkey_ok_ = false;
    bool hidden_ = false;
    bool minimize_to_tray_ = false;
    bool blink_ = false;

    HICON tray_icon_ = nullptr;
};

}  // namespace

// ===========================================================================
//  Точка входа
// ===========================================================================
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    // COM нужен для DirectShow (перечисление камер) и диалога выбора каталога.
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

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

    if (SUCCEEDED(hr)) CoUninitialize();
    return result;
}
