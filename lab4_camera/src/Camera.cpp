// ---------------------------------------------------------------------------
//  Camera.cpp -- общие для всех бэкендов функции (текстовые обозначения).
// ---------------------------------------------------------------------------
#include "Camera.h"

namespace cam {

const wchar_t* pixel_format_name(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::MJPG:  return L"MJPG";
        case PixelFormat::YUY2:  return L"YUY2";
        case PixelFormat::NV12:  return L"NV12";
        case PixelFormat::RGB24: return L"RGB24";
        case PixelFormat::BGR24: return L"BGR24";
        case PixelFormat::Unknown:
        default:                 return L"неизвестен";
    }
}

const wchar_t* camera_state_name(CameraState state) {
    switch (state) {
        case CameraState::Disconnected: return L"отключена";
        case CameraState::Connecting:   return L"подключение";
        case CameraState::Connected:    return L"подключена";
        case CameraState::Error:        return L"ошибка";
    }
    return L"неизвестно";
}

}  // namespace cam
