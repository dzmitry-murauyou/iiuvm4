cmake_minimum_required(VERSION 3.16)

project(CameraLab4 LANGUAGES CXX)

# ---------------------------------------------------------------------------
#  ЛР №4, вариант А5: «Скрытый режим фотонаблюдения»
#
#  GUI   : чистый Win32 API (без MFC/Qt), Shell_NotifyIcon + RegisterHotKey
#  Захват: OpenCV cv::VideoCapture (DirectShow) или Media Foundation Source Reader
#  Видео : OpenCV cv::VideoWriter (AVI / MJPEG)
#  Снимки: OpenCV cv::imwrite (JPEG), резервный путь — ручная запись BMP
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "Проект собирается только под Windows (Win32 API, V4L2-вариант не используется)")
endif()

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)

message(STATUS "OpenCV найден: ${OpenCV_VERSION} (${OpenCV_DIR})")

add_executable(CameraLab4 WIN32
    src/main.cpp
    src/App.cpp
    src/AppActions.cpp
    src/Camera.cpp
    src/CameraOpenCV.cpp
    src/CameraMF.cpp
    src/CaptureController.cpp
    src/DeviceMonitor.cpp
    src/FrameConvert.cpp
    src/Logger.cpp
    src/MediaSaver.cpp
    src/Theme.cpp
    res/app.rc
)

target_include_directories(CameraLab4 PRIVATE src ${OpenCV_INCLUDE_DIRS})

target_link_libraries(CameraLab4 PRIVATE
    ${OpenCV_LIBS}
    comctl32
    shell32
    shlwapi
    ole32
    oleaut32
    uuid
    advapi32
    user32
    gdi32
    mfplat
    mfreadwrite
    mfuuid
)

# В MinGW-w64 часть импортов Media Foundation (MFEnumDeviceSources) лежит в mf.dll,
# в Windows SDK для MSVC -- в mfplat.lib.
if(MINGW)
    target_link_libraries(CameraLab4 PRIVATE mf)
endif()

target_compile_definitions(CameraLab4 PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    # /utf-8 обязателен: исходники в UTF-8 и содержат кириллицу в литералах.
    target_compile_options(CameraLab4 PRIVATE /utf-8 /W4 /permissive- /MP)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT CameraLab4)
else()
    target_compile_options(CameraLab4 PRIVATE -Wall -Wextra -finput-charset=UTF-8)
    target_link_options(CameraLab4 PRIVATE -municode)
endif()

set_target_properties(CameraLab4 PROPERTIES
    OUTPUT_NAME "CameraLab4"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# --- Копирование DLL OpenCV рядом с .exe (best effort) ---------------------
if(WIN32)
    set(_opencv_dlls "")
    if(DEFINED OpenCV_DIR)
        get_filename_component(_opencv_root "${OpenCV_DIR}/.." ABSOLUTE)
        file(GLOB _opencv_dlls
            "${_opencv_root}/bin/opencv_world*.dll"
            "${_opencv_root}/x64/vc*/bin/opencv_world*.dll"
            "${_opencv_root}/x64/mingw/bin/*.dll"
        )
    endif()
    if(_opencv_dlls)
        add_custom_command(TARGET CameraLab4 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_opencv_dlls}
                    "$<TARGET_FILE_DIR:CameraLab4>"
            COMMENT "Копирование DLL OpenCV рядом с исполняемым файлом"
        )
    else()
        message(STATUS "DLL OpenCV не найдены автоматически: добавьте <opencv>/build/x64/vc16/bin в PATH")
    endif()
endif()
