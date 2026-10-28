#pragma once
#include <windows.h>
#include <gdiplus.h>

extern Gdiplus::Bitmap* g_bg;

Gdiplus::Bitmap* LoadBg(HINSTANCE h);
void PaintBg(Gdiplus::Graphics& g, int w, int h);
void Card(Gdiplus::Graphics& g, int x, int y, int w, int h,
          const Gdiplus::SolidBrush& f, const Gdiplus::Pen& p);
void Row(Gdiplus::Graphics& g, int y, int lx, int lw, int vx, int vw,
         const wchar_t* t, const wchar_t* v,
         const Gdiplus::Font& f1, const Gdiplus::Font& f2,
         const Gdiplus::SolidBrush& b1, const Gdiplus::SolidBrush& b2);
