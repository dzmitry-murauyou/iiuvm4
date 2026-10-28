// ---------------------------------------------------------------------------
//  Theme.h -- оформление интерфейса (единый стиль с ЛР №1).
//
//  Все цвета собраны в одном месте: чтобы привести приложение к стилю,
//  установленному в ЛР №1, достаточно поправить Palette в Theme.cpp.
// ---------------------------------------------------------------------------
#pragma once

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace cam {

struct Palette {
    bool dark = false;
    COLORREF window = 0;
    COLORREF header = 0;
    COLORREF panel = 0;
    COLORREF panel_alt = 0;
    COLORREF border = 0;
    COLORREF text = 0;
    COLORREF text_dim = 0;
    COLORREF accent = 0;
    COLORREF ok = 0;
    COLORREF warn = 0;
    COLORREF error = 0;
    COLORREF preview_bg = 0;
    COLORREF log_bg = 0;
};

const Palette& light_palette();
const Palette& dark_palette();

struct UiFonts {
    HFONT ui = nullptr;
    HFONT ui_bold = nullptr;
    HFONT title = nullptr;
    HFONT mono = nullptr;

    void create(int dpi);
    void destroy();
};

// --- небольшие помощники рисования -----------------------------------------

void fill_rect(HDC dc, const RECT& rc, COLORREF color);
void frame_rect(HDC dc, const RECT& rc, COLORREF color, int width = 1);
void draw_text(HDC dc, const std::wstring& text, RECT rc, HFONT font, COLORREF color,
               UINT format = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
void draw_round_rect(HDC dc, const RECT& rc, COLORREF fill, COLORREF border, int radius);
void draw_status_dot(HDC dc, int cx, int cy, int radius, COLORREF color);

}  // namespace cam
