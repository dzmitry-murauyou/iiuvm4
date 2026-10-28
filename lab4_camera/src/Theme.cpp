#include "Theme.h"

#include <algorithm>

namespace cam {

namespace {

// --- Светлая тема ---------------------------------------------------------
const Palette kLight = {
    /*dark*/ false,
    /*window*/ RGB(244, 246, 250),
    /*header*/ RGB(255, 255, 255),
    /*panel*/ RGB(255, 255, 255),
    /*panel_alt*/ RGB(238, 242, 248),
    /*border*/ RGB(205, 212, 224),
    /*text*/ RGB(28, 33, 44),
    /*text_dim*/ RGB(110, 120, 136),
    /*accent*/ RGB(0, 120, 215),
    /*ok*/ RGB(24, 160, 88),
    /*warn*/ RGB(214, 138, 0),
    /*error*/ RGB(200, 40, 40),
    /*preview_bg*/ RGB(24, 26, 30),
    /*log_bg*/ RGB(250, 251, 253),
};

// --- Тёмная тема ---------------------------------------------------------
const Palette kDark = {
    /*dark*/ true,
    /*window*/ RGB(28, 31, 38),
    /*header*/ RGB(36, 40, 49),
    /*panel*/ RGB(38, 42, 52),
    /*panel_alt*/ RGB(46, 51, 62),
    /*border*/ RGB(64, 70, 84),
    /*text*/ RGB(232, 236, 244),
    /*text_dim*/ RGB(150, 160, 178),
    /*accent*/ RGB(78, 160, 255),
    /*ok*/ RGB(70, 200, 120),
    /*warn*/ RGB(240, 180, 60),
    /*error*/ RGB(240, 90, 90),
    /*preview_bg*/ RGB(14, 15, 18),
    /*log_bg*/ RGB(32, 35, 43),
};

}  // namespace

const Palette& light_palette() { return kLight; }
const Palette& dark_palette() { return kDark; }

void UiFonts::create(int dpi) {
    destroy();
    const int height = -MulDiv(9, dpi, 72);   // 9 pt
    ui = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    ui_bold = CreateFontW(height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    title = CreateFontW(-MulDiv(13, dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    mono = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
}

void UiFonts::destroy() {
    if (ui) DeleteObject(ui);
    if (ui_bold) DeleteObject(ui_bold);
    if (title) DeleteObject(title);
    if (mono) DeleteObject(mono);
    ui = ui_bold = title = mono = nullptr;
}

void fill_rect(HDC dc, const RECT& rc, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
}

void frame_rect(HDC dc, const RECT& rc, COLORREF color, int width) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

void draw_text(HDC dc, const std::wstring& text, RECT rc, HFONT font, COLORREF color,
               UINT format) {
    HGDIOBJ old_font = SelectObject(dc, font);
    const int old_mode = SetBkMode(dc, TRANSPARENT);
    const COLORREF old_color = SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rc, format | DT_NOPREFIX);
    SetTextColor(dc, old_color);
    SetBkMode(dc, old_mode);
    SelectObject(dc, old_font);
}

void draw_round_rect(HDC dc, const RECT& rc, COLORREF fill, COLORREF border, int radius) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void draw_status_dot(HDC dc, int cx, int cy, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    Ellipse(dc, cx - radius, cy - radius, cx + radius, cy + radius);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

}  // namespace cam
