#include "CameraMF.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <mutex>
#include <string>

#include "FrameConvert.h"

namespace cam {

namespace {

// ---------------------------------------------------------------------------
//  Инициализация COM / Media Foundation (один раз на процесс)
// ---------------------------------------------------------------------------
struct MfRuntime {
    bool available = false;
    MfRuntime() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool com_ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
            available = false;
            return;
        }
        available = com_ok;
    }
};

MfRuntime& mf_runtime() {
    static MfRuntime runtime;
    return runtime;
}

// Освобождение массива IMFActivate**, полученного MFEnumDeviceSources.
void release_activates(IMFActivate** devices, UINT32 count) {
    if (!devices) return;
    for (UINT32 i = 0; i < count; ++i) {
        if (devices[i]) devices[i]->Release();
    }
    CoTaskMemFree(devices);
}

struct DeviceList {
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    ~DeviceList() { release_activates(devices, count); }
};

HRESULT enumerate_sources(DeviceList& list) {
    if (!mf_runtime().available) return E_FAIL;

    // MFEnumDeviceSources принимает набор атрибутов, в котором задан тип
    // источника (устройство видеозахвата).
    IMFAttributes* attributes = nullptr;
    HRESULT hr = MFCreateAttributes(&attributes, 1);
    if (FAILED(hr)) return hr;
    attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    hr = MFEnumDeviceSources(attributes, &list.devices, &list.count);
    attributes->Release();
    return hr;
}

std::wstring activate_string(IMFActivate* activate, const GUID& key) {
    WCHAR* value = nullptr;
    UINT32 length = 0;
    if (SUCCEEDED(activate->GetAllocatedString(key, &value, &length)) && value) {
        std::wstring result(value, length);
        CoTaskMemFree(value);
        return result;
    }
    return L"";
}

PixelFormat pixel_format_from_subtype(const GUID& subtype) {
    if (subtype == MFVideoFormat_RGB32 || subtype == MFVideoFormat_ARGB32) return PixelFormat::RGB24;
    if (subtype == MFVideoFormat_RGB24) return PixelFormat::RGB24;
    if (subtype == MFVideoFormat_YUY2) return PixelFormat::YUY2;
    if (subtype == MFVideoFormat_NV12) return PixelFormat::NV12;
    if (subtype == MFVideoFormat_MJPG) return PixelFormat::MJPG;
    return PixelFormat::Unknown;
}

std::wstring subtype_name(const GUID& subtype) {
    if (subtype == MFVideoFormat_RGB32) return L"RGB32";
    if (subtype == MFVideoFormat_ARGB32) return L"ARGB32";
    if (subtype == MFVideoFormat_RGB24) return L"RGB24";
    if (subtype == MFVideoFormat_YUY2) return L"YUY2";
    if (subtype == MFVideoFormat_NV12) return L"NV12";
    if (subtype == MFVideoFormat_MJPG) return L"MJPG";
    return L"UNKNOWN";
}

}  // namespace

// Безымянные камеры Media Foundation описываются только символической ссылкой,
// поэтому для "человеческого" имени пользуемся MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME.
std::vector<std::wstring> mf_friendly_names() {
    std::vector<std::wstring> names;
    DeviceList list;
    if (FAILED(enumerate_sources(list))) return names;
    for (UINT32 i = 0; i < list.count; ++i) {
        std::wstring name = activate_string(list.devices[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        if (name.empty()) name = L"Камера #" + std::to_wstring(i);
        names.push_back(std::move(name));
    }
    return names;
}

// ---------------------------------------------------------------------------

struct CameraMF::Impl {
    IMFMediaSource* source = nullptr;
    IMFSourceReader* reader = nullptr;

    mutable std::mutex mutex;
    DeviceInfo info;
    VideoMode mode;
    StreamStats stats;

    PixelFormat output_format = PixelFormat::Unknown;
    int stride = 0;
    bool bottom_up = false;

    std::function<void(CameraState, const std::wstring&)> state_cb;

    void notify(CameraState state, const std::wstring& text) {
        std::function<void(CameraState, const std::wstring&)> cb;
        {
            std::lock_guard<std::mutex> lock(mutex);
            cb = state_cb;
        }
        if (cb) cb(state, text);
    }

    void release() {
        if (reader) {
            reader->Release();
            reader = nullptr;
        }
        if (source) {
            source->Release();
            source = nullptr;
        }
        output_format = PixelFormat::Unknown;
        stride = 0;
        bottom_up = false;
    }
};

CameraMF::CameraMF() : impl_(std::make_unique<Impl>()) {}

CameraMF::~CameraMF() {
    close();
    mf_runtime();  // гарантируем, что MF был инициализирован до Shutdown
}

std::wstring CameraMF::backend_name() const { return L"Media Foundation (Source Reader)"; }

void CameraMF::set_state_callback(
    std::function<void(CameraState, const std::wstring&)> cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->state_cb = std::move(cb);
}

std::vector<DeviceInfo> CameraMF::enumerate_devices() {
    std::vector<DeviceInfo> devices;
    DeviceList list;
    if (FAILED(enumerate_sources(list))) return devices;

    for (UINT32 i = 0; i < list.count; ++i) {
        DeviceInfo info;
        info.index = static_cast<int>(i);
        info.name = activate_string(list.devices[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        if (info.name.empty()) info.name = L"Камера #" + std::to_wstring(i);
        info.unique_id = activate_string(list.devices[i],
                                         MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
        if (info.unique_id.empty()) info.unique_id = L"mfdev:" + std::to_wstring(i);
        info.vendor = L"UVC-камера (Media Foundation)";
        info.bus_info = info.unique_id;
        info.reachable = true;
        devices.push_back(std::move(info));
    }
    return devices;
}

bool CameraMF::open(int device_index, int width, int height, double fps) {
    close();
    impl_->notify(CameraState::Connecting, L"Media Foundation: открытие устройства " +
                                               std::to_wstring(device_index));

    DeviceList list;
    HRESULT hr = enumerate_sources(list);
    if (FAILED(hr) || device_index < 0 || static_cast<UINT32>(device_index) >= list.count) {
        impl_->notify(CameraState::Error, L"Media Foundation: устройство не найдено");
        return false;
    }

    IMFActivate* activate = list.devices[device_index];
    hr = activate->ActivateObject(IID_PPV_ARGS(&impl_->source));
    if (FAILED(hr)) {
        impl_->notify(CameraState::Error, L"Media Foundation: ActivateObject не удался");
        return false;
    }

    // Запрашиваем у Source Reader конвертацию в RGB32 (п.3.3.3 методички).
    IMFAttributes* attributes = nullptr;
    MFCreateAttributes(&attributes, 2);
    if (attributes) {
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }
    hr = MFCreateSourceReaderFromMediaSource(impl_->source, attributes, &impl_->reader);
    if (attributes) attributes->Release();
    if (FAILED(hr)) {
        impl_->notify(CameraState::Error, L"Media Foundation: MFCreateSourceReaderFromMediaSource не удался");
        close();
        return false;
    }

    // --- перечисление нативных форматов (разрешение / FPS / пиксельный формат)
    DeviceInfo info;
    info.index = device_index;
    info.name = activate_string(activate, MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
    if (info.name.empty()) info.name = L"Камера #" + std::to_wstring(device_index);
    info.unique_id = activate_string(activate, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
    info.vendor = L"UVC-камера";
    info.bus_info = info.unique_id;
    info.reachable = true;

    VideoMode best;
    int best_score = INT_MAX;
    for (DWORD i = 0;; ++i) {
        IMFMediaType* type = nullptr;
        hr = impl_->reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &type);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) break;

        UINT32 w = 0, h = 0;
        MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
        UINT32 num = 0, den = 1;
        MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den);
        GUID subtype = GUID_NULL;
        type->GetGUID(MF_MT_SUBTYPE, &subtype);

        VideoMode mode;
        mode.width = static_cast<int>(w);
        mode.height = static_cast<int>(h);
        mode.fps = (den != 0) ? static_cast<double>(num) / static_cast<double>(den) : 0.0;
        mode.format = pixel_format_from_subtype(subtype);
        mode.fourcc = subtype_name(subtype);

        bool duplicate = false;
        for (const auto& existing : info.modes) {
            if (existing.width == mode.width && existing.height == mode.height &&
                existing.fourcc == mode.fourcc &&
                std::abs(existing.fps - mode.fps) < 0.5) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) info.modes.push_back(mode);

        // Выбираем формат, ближайший к запрошенному пользователем.
        int score = std::abs(mode.width - width) + std::abs(mode.height - height);
        if (score < best_score) {
            best_score = score;
            best = mode;
        }
        type->Release();
    }

    if (info.modes.empty()) {
        best.width = width > 0 ? width : 640;
        best.height = height > 0 ? height : 480;
        best.fps = fps > 0 ? fps : 30.0;
        best.format = PixelFormat::RGB24;
        best.fourcc = L"RGB24";
    }

    // --- установка выходного типа RGB32 нужного размера
    IMFMediaType* output = nullptr;
    MFCreateMediaType(&output);
    output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    MFSetAttributeSize(output, MF_MT_FRAME_SIZE, static_cast<UINT32>(std::max(best.width, 1)),
                       static_cast<UINT32>(std::max(best.height, 1)));
    const UINT32 best_fps_num = static_cast<UINT32>(best.fps > 0 ? best.fps * 1000 : 30000);
    MFSetAttributeRatio(output, MF_MT_FRAME_RATE, best_fps_num, 1000);
    output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    hr = impl_->reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, output);
    if (SUCCEEDED(hr)) {
        impl_->output_format = PixelFormat::RGB24;  // RGB32 (BGRX в памяти)
    } else {
        // Не удалось получить RGB32 -- остаёмся на нативном формате и
        // конвертируем вручную (YUY2/NV12/RGB24).
        output->Release();
        output = nullptr;
        for (DWORD i = 0;; ++i) {
            IMFMediaType* native = nullptr;
            if (FAILED(impl_->reader->GetNativeMediaType(
                    MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &native))) {
                break;
            }
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(native, MF_MT_FRAME_SIZE, &w, &h);
            if (static_cast<int>(w) == best.width && static_cast<int>(h) == best.height) {
                hr = impl_->reader->SetCurrentMediaType(
                    MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, native);
                if (SUCCEEDED(hr)) {
                    GUID subtype = GUID_NULL;
                    native->GetGUID(MF_MT_SUBTYPE, &subtype);
                    impl_->output_format = pixel_format_from_subtype(subtype);
                    native->Release();
                    break;
                }
            }
            native->Release();
        }
    }

    // --- фактический тип и шаг строки (stride)
    IMFMediaType* current = nullptr;
    if (SUCCEEDED(impl_->reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current))) {
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &w, &h);
        UINT32 stride = 0;
        if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) {
            impl_->stride = static_cast<int>(stride);
        }
        impl_->bottom_up = impl_->stride < 0;
        GUID subtype = GUID_NULL;
        current->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype == MFVideoFormat_RGB32 || subtype == MFVideoFormat_ARGB32) {
            impl_->output_format = PixelFormat::RGB24;
        }

        VideoMode actual;
        actual.width = static_cast<int>(w);
        actual.height = static_cast<int>(h);
        UINT32 num = 0, den = 1;
        MFGetAttributeRatio(current, MF_MT_FRAME_RATE, &num, &den);
        actual.fps = (den != 0) ? static_cast<double>(num) / static_cast<double>(den) : best.fps;
        actual.format = impl_->output_format;
        actual.fourcc = subtype_name(subtype);
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->mode = actual;
            impl_->info = info;
            impl_->stats = StreamStats{};
        }
        current->Release();
    } else {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->mode = best;
        impl_->info = info;
    }

    if (impl_->stride == 0) {
        switch (impl_->output_format) {
            case PixelFormat::RGB24: impl_->stride = impl_->mode.width * 4; break;
            case PixelFormat::YUY2:  impl_->stride = impl_->mode.width * 2; break;
            case PixelFormat::NV12:  impl_->stride = impl_->mode.width; break;
            default:                 impl_->stride = impl_->mode.width * 3; break;
        }
    }
    if (output) output->Release();

    impl_->notify(CameraState::Connected,
                  L"Media Foundation: камера открыта (" +
                      std::to_wstring(impl_->mode.width) + L"x" +
                      std::to_wstring(impl_->mode.height) + L", " + impl_->mode.fourcc + L")");
    return true;
}

void CameraMF::close() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->release();
    impl_->mode = VideoMode{};
    impl_->stats = StreamStats{};
}

bool CameraMF::is_open() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->reader != nullptr;
}

bool CameraMF::grab(CameraFrame& out) {
    IMFSourceReader* reader = nullptr;
    int stride = 0;
    bool bottom_up = false;
    PixelFormat format = PixelFormat::Unknown;
    int width = 0, height = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->reader) return false;
        reader = impl_->reader;
        reader->AddRef();
        stride = impl_->stride;
        bottom_up = impl_->bottom_up;
        format = impl_->output_format;
        width = impl_->mode.width;
        height = impl_->mode.height;
    }

    IMFSample* sample = nullptr;
    DWORD flags = 0;
    DWORD actual_stream = 0;
    LONGLONG timestamp = 0;
    HRESULT hr = S_OK;
    // STREAMTICK / отсутствие сэмпла -- не ошибка: просто читаем следующий.
    for (int attempt = 0; attempt < 4; ++attempt) {
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &actual_stream,
                                &flags, &timestamp, &sample);
        if (FAILED(hr)) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            hr = E_FAIL;
            break;
        }
        if (sample) break;
    }

    if (FAILED(hr) || !sample) {
        if (sample) sample->Release();
        reader->Release();
        return false;
    }

    IMFMediaBuffer* buffer = nullptr;
    hr = sample->ConvertToContiguousBuffer(&buffer);
    BYTE* data = nullptr;
    DWORD max_len = 0, cur_len = 0;
    if (SUCCEEDED(hr) && SUCCEEDED(buffer->Lock(&data, &max_len, &cur_len)) && data) {
        if (width <= 0) width = 640;
        if (height <= 0) height = 480;
        out.width = width;
        out.height = height;
        out.stride = width * 3;

        switch (format) {
            case PixelFormat::YUY2:
                yuy2_to_bgr(data, width, height, stride, out.pixels);
                break;
            case PixelFormat::NV12:
                nv12_to_bgr(data, width, height, stride, out.pixels);
                break;
            case PixelFormat::RGB24:
            default:
                // RGB32 в памяти -- B,G,R,X, тогда как RGB24 -- R,G,B.
                if (stride >= width * 4) {
                    rgb32_to_bgr(data, width, height, stride, bottom_up, out.pixels);
                } else {
                    rgb24_to_bgr(data, width, height, stride, bottom_up, out.pixels);
                }
                break;
        }
        out.jpeg.clear();
        out.time_ns = static_cast<long long>(timestamp);

        buffer->Unlock();
        std::lock_guard<std::mutex> lock(impl_->mutex);
        ++impl_->stats.frames;
    } else {
        hr = E_FAIL;
    }

    if (buffer) buffer->Release();
    sample->Release();
    reader->Release();
    return SUCCEEDED(hr);
}

DeviceInfo CameraMF::device_info() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->info;
}

VideoMode CameraMF::current_mode() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->mode;
}

StreamStats CameraMF::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->stats;
}

ICamera* create_mf_camera() { return new CameraMF(); }

}  // namespace cam
