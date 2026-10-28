#pragma once
#include <windows.h>
#include <string>

struct Battery {
  bool ok = false;
  std::wstring name, chem;
  DWORD design = 0, full = 0, left = 0, volt = 0, temp = 0;
  LONG rate = 0;
  ULONG cycles = 0;
  double wear = 0;
  bool cap = false, hasLeft = false, hasVolt = false, hasTemp = false,
       hasRate = false, hasCycles = false, hasWear = false;
};

struct Power {
  bool ac = false, acUnk = true, present = true, charging = false;
  int pct = -1;
  DWORD secs = 0;
  bool hasTime = false;
};

extern Battery g_bat;

Battery GetBattery();
Power GetPower();
std::wstring Num(double v, int pr);
std::wstring Dur(DWORD s);
std::wstring SrcText(const Power& p);
std::wstring RateText(LONG r);
