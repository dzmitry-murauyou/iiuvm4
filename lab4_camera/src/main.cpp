// ---------------------------------------------------------------------------
//  main.cpp -- точка входа приложения (ЛР №4, вариант А5).
//
//  Приложение оконное (подсистема WINDOWS), поэтому точка входа wWinMain.
//  Помимо обычного режима оно умеет работать в фоне: окно скрывается в трей,
//  а глобальная горячая клавиша Ctrl+Shift+F12 создаёт снимок без открытия окна.
// ---------------------------------------------------------------------------
#include <windows.h>

#include <commctrl.h>
#include <objbase.h>

#include "App.h"

#pragma comment(lib, "comctl32.lib")

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, LPWSTR, int show_cmd) {
    // COM нужен для Shell_NotifyIcon/BIF_NEWDIALOGSTYLE и Media Foundation.
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&controls);

    cam::App app;
    if (!app.init(hinst, show_cmd)) {
        MessageBoxW(nullptr, L"Не удалось создать главное окно приложения.", L"Ошибка",
                    MB_OK | MB_ICONERROR);
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }

    const int code = app.run();
    if (SUCCEEDED(hr)) CoUninitialize();
    return code;
}
