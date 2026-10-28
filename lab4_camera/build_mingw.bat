@echo off
REM ---------------------------------------------------------------------------
REM  build_mingw.bat -- сборка через MinGW-w64 (g++ 12+) и CMake.
REM
REM  Внимание: OpenCV должен быть собран тем же MinGW (официальные сборки
REM  OpenCV собраны MSVC и для MinGW не подходят). Если OpenCV под MinGW нет,
REM  используйте build.bat (Visual Studio) -- это основной путь.
REM ---------------------------------------------------------------------------
setlocal

if "%OpenCV_DIR%"=="" (
    echo [!] Задайте OpenCV_DIR (каталог с OpenCVConfig.cmake, собранный MinGW).
    exit /b 1
)

cmake -S "%~dp0." -B "%~dp0build-mingw" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DOpenCV_DIR="%OpenCV_DIR%"
if errorlevel 1 exit /b 1

cmake --build "%~dp0build-mingw" -j
if errorlevel 1 exit /b 1

echo.
echo [OK] Готово: %~dp0build-mingw\bin\CameraLab4.exe
endlocal
