#pragma once
#include <gdiplus.h>

namespace Theme {
  const Gdiplus::Color Bg(255, 10, 14, 24), Card(215, 18, 26, 44), Txt(255, 235, 240, 248),
    Sub(255, 165, 178, 198), Dim(255, 120, 130, 150), Line(255, 70, 95, 135),
    Good(255, 46, 204, 113), Warn(255, 241, 196, 15), Bad(255, 235, 87, 87);
}

namespace Lay {
  const int Pad = 36, Gap = 18, Title = 66, Top = 22, Sec = 46, Row = 30, Bot = 22,
    LX = 24, VX = 250, SepB = 12, SepA = 24, WearH = 40, R1 = 4, R2 = 3, R3 = 4;
  const int C1 = Top + Sec + R1 * Row + Bot,
    C2 = Top + Sec + R2 * Row + SepB + SepA + WearH + Bot,
    C3 = Top + Sec + R3 * Row + Bot;
  inline int FullH() { return Pad + Title + C1 + Gap + C2 + Gap + C3 + Pad; }
}
