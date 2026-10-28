@echo off
REM ---------------------------------------------------------------------------
REM  build.bat -- сборка ЛР №4 (вариант А5) через CMake + Visual Studio.
REM
REM  Требования:
REM    1) Visual Studio 2019/2022 (Desktop development with C++);
REM    2) OpenCV 4.x для Windows (opencv-4.x.x-windows.exe), переменная
REM       окружения OpenCV_DIR должна указывать на <opencv>/build,
REM       например: set OpenCV_DIR=C:\opencv\build
REM ---------------------------------------------------------------------------
setlocal

if "%OpenCV_DIR%"=="" (
    echo [!] Переменная OpenCV_DIR не задана.
    echo     Пример: set OpenCV_DIR=C:\opencv\build
    exit /b 1
)

where cmake >nul 2>nul
if errorlevel 1 (
    echo [!] CMake не найден в PATH.
    exit /b 1
)

cmake -S "%~dp0." -B "%~dp0build" -G "Visual Studio 17 2022" -A x64 -DOpenCV_DIR="%OpenCV_DIR%"
if errorlevel 1 exit /b 1

cmake --build "%~dp0build" --config Release
if errorlevel 1 exit /b 1

echo.
echo [OK] Готово: %~dp0build\bin\Release\CameraLab4.exe
endlocal
