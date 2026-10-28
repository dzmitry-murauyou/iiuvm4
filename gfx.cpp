#define _WIN32_WINNT 0x0601

#include "gfx.h"
#include "theme.h"
#include "resource.h"
#include <cstring>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

using namespace Gdiplus;

Bitmap* g_bg = 0;

Bitmap* LoadBg(HINSTANCE h) {
  HRSRC r = FindResourceW(h, MAKEINTRESOURCEW(IDR_BACKGROUND), RT_RCDATA);
  if (!r) return 0;
  DWORD sz = SizeofResource(h, r);
  if (!sz) return 0;
  HGLOBAL res = LoadResource(h, r);
  if (!res) return 0;
  void* p = LockResource(res);
  if (!p) return 0;
  HGLOBAL m = GlobalAlloc(GMEM_MOVEABLE, sz);
  if (!m) return 0;
  void* dst = GlobalLock(m);
  if (!dst) { GlobalFree(m); return 0; }
  memcpy(dst, p, sz);
  GlobalUnlock(m);
  IStream* s = 0;
  if (CreateStreamOnHGlobal(m, TRUE, &s) != S_OK) { GlobalFree(m); return 0; }
  Bitmap* b = new Bitmap(s);
  s->Release();
  if (b->GetLastStatus() != Ok) { delete b; return 0; }
  return b;
}

void PaintBg(Graphics& g, int w, int h) {
  SolidBrush base(Theme::Bg);
  g.FillRectangle(&base, 0, 0, w, h);
  if (g_bg) {
    REAL iw = (REAL)g_bg->GetWidth(), ih = (REAL)g_bg->GetHeight();
    if (iw > 0 && ih > 0) {
      REAL s = w / iw > h / ih ? w / iw : h / ih, dw = iw * s, dh = ih * s;
      g.DrawImage(g_bg, (w - dw) / 2, (h - dh) / 2, dw, dh);
    }
  }
  SolidBrush dim(Color(150, 6, 10, 18));
  g.FillRectangle(&dim, 0, 0, w, h);
}

void Card(Graphics& g, int x, int y, int w, int h, const SolidBrush& f, const Pen& p) {
  RectF r((REAL)x, (REAL)y, (REAL)w, (REAL)h);
  g.FillRectangle(&f, r);
  g.DrawRectangle(&p, r);
}

void Row(Graphics& g, int y, int lx, int lw, int vx, int vw, const wchar_t* t, const wchar_t* v,
         const Font& f1, const Font& f2, const SolidBrush& b1, const SolidBrush& b2) {
  RectF r1((REAL)lx, (REAL)y, (REAL)lw, 24), r2((REAL)vx, (REAL)y, (REAL)vw, 24);
  g.DrawString(t, -1, &f1, r1, 0, &b1);
  g.DrawString(v, -1, &f2, r2, 0, &b2);
}
