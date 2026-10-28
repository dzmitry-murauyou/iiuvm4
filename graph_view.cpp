#define _WIN32_WINNT 0x0601

#include "graph_view.h"
#include "battery.h"
#include "theme.h"
#include "gfx.h"
#include <vector>
#include <string>

using namespace Gdiplus;

struct Sample {
  ULONGLONG t;
  int pct;
};

static std::vector<Sample> g_hist;

void AddHist() {
  int p = GetPower().pct;
  if (p < 0) return;
  g_hist.push_back({ GetTickCount64(), p });
  if (g_hist.size() > 300) g_hist.erase(g_hist.begin());
}

static void PaintGraph(Graphics& g, int W, int H) {
  FontFamily ff(L"Segoe UI");
  SolidBrush ink(Theme::Txt), gray(Theme::Sub), card(Theme::Card), faint(Theme::Dim);
  StringFormat center;
  center.SetAlignment(StringAlignmentCenter);

  Font title(&ff, 24, FontStyleBold, UnitPixel);
  g.DrawString(L"График изменения уровня заряда", -1, &title,
               RectF(0, 25, (REAL)W, 40), &center, &ink);

  int x0 = 90, y0 = 85, x1 = W - 60, y1 = H - 90;
  Card(g, x0 - 20, y0 - 20, x1 - x0 + 40, y1 - y0 + 40, card, Pen(Theme::Line, 1.5f));

  Pen axis(Theme::Txt, 3);
  g.DrawLine(&axis, x0, y0, x0, y1);
  g.DrawLine(&axis, x0, y1, x1, y1);

  Pen grid(Theme::Line, 1);
  grid.SetDashStyle(DashStyleDot);
  Font lab(&ff, 13, FontStyleRegular, UnitPixel);
  StringFormat toRight;
  toRight.SetAlignment(StringAlignmentFar);
  toRight.SetLineAlignment(StringAlignmentCenter);
  for (int p = 0; p <= 100; p += 25) {
    int y = y1 - (y1 - y0) * p / 100;
    g.DrawLine(&grid, x0 + 1, y, x1, y);
    std::wstring s = std::to_wstring(p) + L"%";
    g.DrawString(s.c_str(), -1, &lab, RectF(5, (REAL)(y - 14), 80, 28), &toRight, &gray);
  }

  Font cap(&ff, 12, FontStyleItalic, UnitPixel);
  g.DrawString(L"Заряд, %", -1, &cap, RectF(5, (REAL)(y0 - 30), 200, 25), 0, &gray);

  if (g_hist.size() < 2) {
    Font f(&ff, 16, FontStyleRegular, UnitPixel);
    StringFormat mc;
    mc.SetAlignment(StringAlignmentCenter);
    mc.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(g_bat.ok ? L"Сбор данных...\n\nГрафик появится через несколько секунд"
                          : L"Батарея не обнаружена\n\nГрафик недоступен",
                 -1, &f, RectF((REAL)x0, (REAL)y0, (REAL)(x1 - x0), (REAL)(y1 - y0)), &mc, &faint);
    return;
  }

  ULONGLONG t0 = g_hist.front().t, span = g_hist.back().t - t0;
  if (!span) span = 1;
  Font tick(&ff, 12, FontStyleRegular, UnitPixel);
  for (int i = 0; i <= 4; i++) {
    int x = x0 + (x1 - x0) * i / 4;
    g.DrawLine(&grid, x, y0 + 1, x, y1);
    std::wstring s = L"+" + std::to_wstring((int)(span * i / 4 / 1000)) + L" с";
    g.DrawString(s.c_str(), -1, &tick, RectF((REAL)(x - 60), (REAL)(y1 + 4), 120, 20), &center, &gray);
  }

  std::vector<PointF> pts;
  for (auto& s : g_hist)
    pts.push_back(PointF((REAL)(x0 + (x1 - x0) * (s.t - t0) / span),
                         (REAL)(y1 - (y1 - y0) * s.pct / 100)));

  int lastPct = g_hist.back().pct;
  Color line = lastPct >= 50 ? Theme::Good : lastPct >= 20 ? Theme::Warn : Theme::Bad;

  std::vector<PointF> area = pts;
  area.push_back(PointF((REAL)x1, (REAL)y1));
  area.push_back(PointF((REAL)x0, (REAL)y1));
  SolidBrush wash(Color(45, line.GetR(), line.GetG(), line.GetB()));
  g.FillPolygon(&wash, area.data(), (INT)area.size());

  Pen curve(line, 4);
  curve.SetLineJoin(LineJoinRound);
  g.DrawLines(&curve, pts.data(), (INT)pts.size());

  SolidBrush dot(line), hole(Color(255, 255, 255, 255));
  for (auto& q : pts) {
    g.FillEllipse(&dot, q.X - 5.0f, q.Y - 5.0f, 10.0f, 10.0f);
    g.FillEllipse(&hole, q.X - 2.5f, q.Y - 2.5f, 5.0f, 5.0f);
  }

  PointF lp = pts.back();
  std::wstring vs = std::to_wstring(lastPct) + L"%";
  Font vf(&ff, 18, FontStyleBold, UnitPixel);
  SolidBrush vb(line), vbg(Color(235, 10, 16, 28));
  RectF vr(lp.X - 45.0f, lp.Y - 55.0f, 90.0f, 40.0f);
  Pen vbd(line, 3);
  StringFormat vfc;
  vfc.SetAlignment(StringAlignmentCenter);
  vfc.SetLineAlignment(StringAlignmentCenter);
  g.FillRectangle(&vbg, vr);
  g.DrawRectangle(&vbd, vr);
  g.DrawString(vs.c_str(), -1, &vf, vr, &vfc, &vb);
}

static void DrawGraph(HDC dc, int W, int H) {
  if (W < 400 || H < 250) return;
  Graphics screen(dc);
  Bitmap buf(W, H);
  Graphics g(&buf);
  g.SetSmoothingMode(SmoothingModeHighQuality);
  g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
  PaintBg(g, W, H);
  PaintGraph(g, W, H);
  screen.DrawImage(&buf, 0, 0);
}

LRESULT CALLBACK GraphProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_PAINT: {
    PAINTSTRUCT ps{};
    HDC d = BeginPaint(w, &ps);
    RECT rc{};
    GetClientRect(w, &rc);
    if (rc.right > 0 && rc.bottom > 0) DrawGraph(d, rc.right, rc.bottom);
    EndPaint(w, &ps);
    return 0;
  }
  case WM_ERASEBKGND:
    return 1;
  case WM_SIZE:
    InvalidateRect(w, 0, FALSE);
    return 0;
  }
  return DefWindowProcW(w, msg, wp, lp);
}
