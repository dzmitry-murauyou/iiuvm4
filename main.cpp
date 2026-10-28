#define INITGUID
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <devguid.h>
#include <powersetting.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <string>
#include "battery.h"
#include "gfx.h"
#include "diag_view.h"
#include "graph_view.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace Gdiplus;

#define ID_TAB 1001
#define ID_TIMER 1002
#define ID_GRAPH 1003
#define ID_DIAG 1004
#define TICK_MS 5000

static HWND g_tab, g_diag, g_graph;
static HPOWERNOTIFY g_ntf1, g_ntf2;
static ULONG_PTR g_token;
static HFONT g_tabFont;

static void MkTabs(HWND w) {
  RECT pr{};
  GetClientRect(w, &pr);
  g_tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0,
                           pr.right - pr.left, pr.bottom - pr.top, w,
                           (HMENU)(INT_PTR)ID_TAB, GetModuleHandleW(0), 0);
  g_tabFont = CreateFontW(22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
  SendMessageW(g_tab, WM_SETFONT, (WPARAM)g_tabFont, TRUE);
  TCITEMW t{};
  t.mask = TCIF_TEXT;
  t.pszText = (LPWSTR)L"Диагностика батареи";
  TabCtrl_InsertItem(g_tab, 0, &t);
  t.pszText = (LPWSTR)L"График заряда";
  TabCtrl_InsertItem(g_tab, 1, &t);
  RECT rc{};
  GetClientRect(g_tab, &rc);
  TabCtrl_AdjustRect(g_tab, FALSE, &rc);
  int rw = rc.right - rc.left, rh = rc.bottom - rc.top;
  g_diag = CreateWindowExW(0, L"BatteryDiagA2", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL, rc.left, rc.top, rw, rh,
                            g_tab, (HMENU)(INT_PTR)ID_DIAG, GetModuleHandleW(0), 0);
  g_graph = CreateWindowExW(0, L"BatteryGraphA2", L"", WS_CHILD, rc.left, rc.top, rw, rh,
                             g_tab, (HMENU)(INT_PTR)ID_GRAPH, GetModuleHandleW(0), 0);
}

static void Resize(HWND w) {
  if (!g_tab) return;
  RECT m{};
  GetClientRect(w, &m);
  SetWindowPos(g_tab, 0, 0, 0, m.right, m.bottom, SWP_NOZORDER);
  RECT rc{};
  GetClientRect(g_tab, &rc);
  TabCtrl_AdjustRect(g_tab, FALSE, &rc);
  SetWindowPos(g_diag, 0, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER);
  SetWindowPos(g_graph, 0, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER);
  UpdScroll(g_diag);
}

static void Redraw() {
  int sel = TabCtrl_GetCurSel(g_tab);
  if (sel == 0) {
    UpdScroll(g_diag);
    InvalidateRect(g_diag, 0, FALSE);
  } else {
    InvalidateRect(g_graph, 0, FALSE);
  }
}

static void SetTitle(HWND w) {
  Power p = GetPower();
  std::wstring t = L"Диагностика батареи";
  if (p.pct >= 0) {
    t += L" — " + std::to_wstring(p.pct) + L"%";
    if (!p.acUnk) t += p.ac ? L" (сеть)" : L" (батарея)";
  }
  SetWindowTextW(w, t.c_str());
}

static void PollBattery(HWND w) {
  g_bat = GetBattery();
  AddHist();
  Redraw();
  SetTitle(w);
}

static LRESULT CALLBACK MainProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_CREATE:
    MkTabs(w);
    Resize(w);
    g_ntf1 = RegisterPowerSettingNotification(w, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);
    g_ntf2 = RegisterPowerSettingNotification(w, &GUID_BATTERY_PERCENTAGE_REMAINING, DEVICE_NOTIFY_WINDOW_HANDLE);
    SetTimer(w, ID_TIMER, TICK_MS, 0);
    PollBattery(w);
    return 0;
  case WM_TIMER:
    if (wp == ID_TIMER) PollBattery(w);
    return 0;
  case WM_NOTIFY: {
    NMHDR* h = (NMHDR*)lp;
    if (h->hwndFrom == g_tab && h->code == TCN_SELCHANGE) {
      int s = TabCtrl_GetCurSel(g_tab);
      ShowWindow(g_diag, s == 0 ? SW_SHOW : SW_HIDE);
      ShowWindow(g_graph, s == 1 ? SW_SHOW : SW_HIDE);
      InvalidateRect(s == 0 ? g_diag : g_graph, 0, FALSE);
      return 0;
    }
    break;
  }
  case WM_SIZE:
    Resize(w);
    Redraw();
    return 0;
  case WM_GETMINMAXINFO:
    ((MINMAXINFO*)lp)->ptMinTrackSize.x = 720;
    ((MINMAXINFO*)lp)->ptMinTrackSize.y = 520;
    return 0;
  case WM_POWERBROADCAST:
    if (wp == PBT_APMPOWERSTATUSCHANGE || wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
      PollBattery(w);
      return TRUE;
    }
    if (wp == PBT_POWERSETTINGCHANGE) {
      GUID g = ((PPOWERBROADCAST_SETTING)lp)->PowerSetting;
      if (IsEqualGUID(g, GUID_ACDC_POWER_SOURCE) || IsEqualGUID(g, GUID_BATTERY_PERCENTAGE_REMAINING)) {
        PollBattery(w);
        return TRUE;
      }
    }
    break;
  case WM_DESTROY:
    KillTimer(w, ID_TIMER);
    if (g_tabFont) DeleteObject(g_tabFont);
    if (g_ntf1) UnregisterPowerSettingNotification(g_ntf1);
    if (g_ntf2) UnregisterPowerSettingNotification(g_ntf2);
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(w, msg, wp, lp);
}

static ATOM RegCls(HINSTANCE h, const wchar_t* n, WNDPROC p) {
  WNDCLASSW c{};
  c.style = CS_HREDRAW | CS_VREDRAW;
  c.lpfnWndProc = p;
  c.hInstance = h;
  c.hCursor = LoadCursorW(0, IDC_ARROW);
  c.hbrBackground = (HBRUSH)(INT_PTR)(COLOR_WINDOW + 1);
  c.lpszClassName = n;
  return RegisterClassW(&c);
}

int WINAPI WinMain(_In_ HINSTANCE h, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int show) {
  GdiplusStartupInput gi;
  if (GdiplusStartup(&g_token, &gi, 0) != Ok) return 1;
  INITCOMMONCONTROLSEX cc{ sizeof(cc), ICC_TAB_CLASSES };
  InitCommonControlsEx(&cc);
  if (!RegCls(h, L"BatteryMainA2", MainProc)) {
    GdiplusShutdown(g_token);
    return 1;
  }
  RegCls(h, L"BatteryDiagA2", DiagProc);
  RegCls(h, L"BatteryGraphA2", GraphProc);
  HWND w = CreateWindowExW(0, L"BatteryMainA2", L"Диагностика батареи",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 800, 0, 0, h, 0);
  if (!w) {
    GdiplusShutdown(g_token);
    return 1;
  }
  g_bg = LoadBg(h);
  ShowWindow(w, show);
  UpdateWindow(w);
  MSG m{};
  while (GetMessageW(&m, 0, 0, 0)) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  delete g_bg;
  GdiplusShutdown(g_token);
  return (int)m.wParam;
}
