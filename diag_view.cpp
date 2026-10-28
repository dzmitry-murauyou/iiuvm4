#define _WIN32_WINNT 0x0601

#include "diag_view.h"
#include "battery.h"
#include "theme.h"
#include "gfx.h"
#include <windowsx.h>
#include <string>

using namespace Gdiplus;

static int g_scroll = 0;

static void ScrollBy(HWND w, int d) {
  RECT rc{};
  GetClientRect(w, &rc);
  int mx = Lay::FullH() - (rc.bottom - rc.top);
  if (mx < 0) mx = 0;
  int p = g_scroll + d;
  if (p < 0) p = 0;
  if (p > mx) p = mx;
  if (p != g_scroll) { g_scroll = p; SetScrollPos(w, SB_VERT, p, TRUE); InvalidateRect(w, 0, FALSE); }
}

void UpdScroll(HWND w) {
  RECT rc{};
  GetClientRect(w, &rc);
  int ch = rc.bottom - rc.top;
  if (ch <= 0) ch = 1;
  int full = Lay::FullH();
  if (!g_bat.ok || full <= ch) { g_scroll = 0; ShowScrollBar(w, SB_VERT, FALSE); return; }
  ShowScrollBar(w, SB_VERT, TRUE);
  int mx = full - ch;
  if (g_scroll > mx) g_scroll = mx;
  if (g_scroll < 0) g_scroll = 0;
  SCROLLINFO si{};
  si.cbSize = sizeof(si);
  si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
  si.nMax = full - 1;
  si.nPage = ch;
  si.nPos = g_scroll;
  SetScrollInfo(w, SB_VERT, &si, TRUE);
}

static void DrawDiag(HDC dc, int W, int H) {
  if (W <= 0 || H <= 0) return;
  Graphics screen(dc);
  Bitmap buf(W, H);
  Graphics g(&buf);
  g.SetSmoothingMode(SmoothingModeHighQuality);
  g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
  PaintBg(g, W, H);
  FontFamily ff(L"Segoe UI");
  if (!g_bat.ok) {
    Font f1(&ff, 17, FontStyleBold, UnitPixel), f2(&ff, 13, FontStyleRegular, UnitPixel);
    SolidBrush b1(Theme::Bad), b2(Theme::Sub);
    StringFormat c;
    c.SetAlignment(StringAlignmentCenter);
    c.SetLineAlignment(StringAlignmentCenter);
    REAL hh = H / 2.0f;
    g.DrawString(L"Батарея не обнаружена", -1, &f1, RectF(0, hh - 60, (REAL)W, 40), &c, &b1);
    StringFormat c2;
    c2.SetAlignment(StringAlignmentCenter);
    g.DrawString(L"На стационарном ПК без батареи это ожидаемое состояние", -1, &f2,
                 RectF(40, hh - 12, (REAL)W - 80, 60), &c2, &b2);
    screen.DrawImage(&buf, 0, 0);
    return;
  }
  int full = Lay::FullH(), mx = full > H ? full - H : 0;
  if (g_scroll > mx) g_scroll = mx;
  if (g_scroll < 0) g_scroll = 0;
  g.TranslateTransform(0, (REAL)-g_scroll);
  Power p = GetPower();
  int cw = W - 2 * Lay::Pad, lx = Lay::Pad + Lay::LX, vx = Lay::Pad + Lay::VX,
      lw = Lay::VX - Lay::LX - 8, vw = cw - Lay::VX - 24;
  Font tFont(&ff, 26, FontStyleBold, UnitPixel), sFont(&ff, 18, FontStyleBold, UnitPixel),
       lFont(&ff, 14, FontStyleRegular, UnitPixel), vFont(&ff, 14, FontStyleBold, UnitPixel),
       wFont(&ff, 32, FontStyleBold, UnitPixel);
  SolidBrush tBr(Theme::Txt), lBr(Theme::Sub), vBr(Theme::Txt), cBr(Theme::Card), uBr(Theme::Dim);
  Pen bd(Theme::Line, 1);
  g.DrawString(L"Диагностика батареи", -1, &tFont,
               RectF((REAL)Lay::Pad, (REAL)Lay::Pad, (REAL)(W - 2 * Lay::Pad), 40), 0, &tBr);
  int cy = Lay::Pad + Lay::Title, y;
  Card(g, Lay::Pad, cy, cw, Lay::C1, cBr, bd);
  y = cy + Lay::Top;
  g.DrawString(L"Основная информация", -1, &sFont, RectF((REAL)lx, (REAL)y, 400, 30), 0, &tBr);
  y += Lay::Sec;
  Row(g, y, lx, lw, vx, vw, L"Устройство:", g_bat.name.empty() ? L"Неизвестно" : g_bat.name.c_str(),
      lFont, vFont, lBr, vBr);
  y += Lay::Row;
  Row(g, y, lx, lw, vx, vw, L"Химический тип:", g_bat.chem.empty() ? L"Неизвестно" : g_bat.chem.c_str(),
      lFont, vFont, lBr, vBr);
  y += Lay::Row;
  std::wstring src = SrcText(p);
  Row(g, y, lx, lw, vx, vw, L"Источник питания:", src.c_str(), lFont, vFont, lBr, vBr);
  y += Lay::Row;
  if (p.pct >= 0) {
    SolidBrush cb(p.pct >= 50 ? Theme::Good : p.pct >= 20 ? Theme::Warn : Theme::Bad);
    std::wstring ch = std::to_wstring(p.pct) + L"%";
    Row(g, y, lx, lw, vx, vw, L"Текущий заряд:", ch.c_str(), lFont, vFont, lBr, cb);
  } else {
    Row(g, y, lx, lw, vx, vw, L"Текущий заряд:", L"Неизвестно", lFont, vFont, lBr, uBr);
  }
  cy += Lay::C1 + Lay::Gap;
  Card(g, Lay::Pad, cy, cw, Lay::C2, cBr, bd);
  y = cy + Lay::Top;
  g.DrawString(L"Ёмкость и износ", -1, &sFont, RectF((REAL)lx, (REAL)y, 400, 30), 0, &tBr);
  y += Lay::Sec;
  std::wstring dCap = L"Недоступно", fCap = L"Недоступно", rCap = L"Недоступно";
  if (g_bat.cap) {
    dCap = std::to_wstring(g_bat.design) + L" mWh";
    fCap = std::to_wstring(g_bat.full) + L" mWh";
  }
  if (g_bat.hasLeft) rCap = std::to_wstring(g_bat.left) + L" mWh";
  const SolidBrush *cB = g_bat.cap ? &vBr : &uBr, *rB = g_bat.hasLeft ? &vBr : &uBr;
  Row(g, y, lx, lw, vx, vw, L"Проектная ёмкость:", dCap.c_str(), lFont, vFont, lBr, *cB);
  y += Lay::Row;
  Row(g, y, lx, lw, vx, vw, L"Текущая ёмкость:", fCap.c_str(), lFont, vFont, lBr, *cB);
  y += Lay::Row;
  Row(g, y, lx, lw, vx, vw, L"Оставшаяся ёмкость:", rCap.c_str(), lFont, vFont, lBr, *rB);
  y += Lay::Row + Lay::SepB;
  Pen sep(Theme::Line, 1);
  g.DrawLine(&sep, (REAL)lx, (REAL)y, (REAL)(lx + cw - 2 * Lay::LX), (REAL)y);
  y += Lay::SepA;
  g.DrawString(L"Степень износа:", -1, &sFont, RectF((REAL)lx, (REAL)y, 220, 35), 0, &lBr);
  if (g_bat.hasWear) {
    SolidBrush wb(g_bat.wear > 40 ? Theme::Bad : g_bat.wear > 20 ? Theme::Warn : Theme::Good);
    std::wstring wt = Num(g_bat.wear, 2) + L"%";
    g.DrawString(wt.c_str(), -1, &wFont, RectF((REAL)vx, (REAL)(y - 2), 300, 40), 0, &wb);
  } else {
    g.DrawString(L"Недоступно", -1, &vFont, RectF((REAL)vx, (REAL)(y + 4), 300, 30), 0, &uBr);
  }
  cy += Lay::C2 + Lay::Gap;
  Card(g, Lay::Pad, cy, cw, Lay::C3, cBr, bd);
  y = cy + Lay::Top;
  g.DrawString(L"Электрические параметры и ресурс", -1, &sFont, RectF((REAL)lx, (REAL)y, 500, 30), 0, &tBr);
  y += Lay::Sec;
  std::wstring vt = L"Недоступно", rt = L"Недоступно", tt = L"Недоступно", ct = L"Недоступно";
  if (g_bat.hasVolt) vt = Num(g_bat.volt / 1000.0, 3) + L" V";
  if (g_bat.hasRate) rt = RateText(g_bat.rate);
  if (g_bat.hasTemp) tt = Num(g_bat.temp / 10.0 - 273.15, 1) + L" °C";
  if (g_bat.hasCycles) ct = std::to_wstring(g_bat.cycles);
  const SolidBrush *vB = g_bat.hasVolt ? &vBr : &uBr, *tB = g_bat.hasRate ? &vBr : &uBr,
                   *eB = g_bat.hasTemp ? &vBr : &uBr, *kB = g_bat.hasCycles ? &vBr : &uBr;
  Row(g, y, lx, lw, vx, vw, L"Напряжение:", vt.c_str(), lFont, vFont, lBr, *vB);
  y += Lay::Row;
  Row(g, y, lx, lw, vx, vw, L"Скорость заряда:", rt.c_str(), lFont, vFont, lBr, *tB);
  y += Lay::Row;
  Row(g, y, lx, lw, vx, vw, L"Температура:", tt.c_str(), lFont, vFont, lBr, *eB);
  y += Lay::Row;
  Row(g, y, lx, lw, vx, vw, L"Циклы заряда:", ct.c_str(), lFont, vFont, lBr, *kB);
  screen.DrawImage(&buf, 0, 0);
}

LRESULT CALLBACK DiagProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_PAINT: {
    PAINTSTRUCT ps{};
    HDC d = BeginPaint(w, &ps);
    RECT rc{};
    GetClientRect(w, &rc);
    DrawDiag(d, rc.right, rc.bottom);
    EndPaint(w, &ps);
    return 0;
  }
  case WM_ERASEBKGND:
    return 1;
  case WM_SIZE:
    UpdScroll(w);
    InvalidateRect(w, 0, FALSE);
    return 0;
  case WM_VSCROLL: {
    RECT rc{};
    GetClientRect(w, &rc);
    int ch = rc.bottom - rc.top, mx = Lay::FullH() - ch, np = g_scroll;
    if (mx < 0) mx = 0;
    switch (LOWORD(wp)) {
    case SB_LINEUP: np -= Lay::Row; break;
    case SB_LINEDOWN: np += Lay::Row; break;
    case SB_PAGEUP: np -= ch - 40; break;
    case SB_PAGEDOWN: np += ch - 40; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: {
      SCROLLINFO si{};
      si.cbSize = sizeof(si);
      si.fMask = SIF_TRACKPOS;
      if (GetScrollInfo(w, SB_VERT, &si)) np = si.nTrackPos;
      break;
    }
    case SB_TOP: np = 0; break;
    case SB_BOTTOM: np = mx; break;
    }
    ScrollBy(w, np - g_scroll);
    return 0;
  }
  case WM_MOUSEWHEEL:
    ScrollBy(w, -(GET_WHEEL_DELTA_WPARAM(wp) / 120) * Lay::Row * 2);
    return 0;
  }
  return DefWindowProcW(w, msg, wp, lp);
}
