#define _WIN32_WINNT 0x0601

#include "battery.h"
#include <setupapi.h>
#include <devguid.h>
#include <batclass.h>
#include <vector>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "setupapi.lib")

Battery g_bat;

Power GetPower() {
  Power p;
  SYSTEM_POWER_STATUS s{};
  if (!GetSystemPowerStatus(&s)) return p;
  if (s.ACLineStatus == 0) { p.ac = false; p.acUnk = false; }
  if (s.ACLineStatus == 1) { p.ac = true; p.acUnk = false; }
  if (s.BatteryFlag != 255) {
    p.present = !(s.BatteryFlag & 128);
    p.charging = (s.BatteryFlag & 8) != 0;
  }
  if (s.BatteryLifePercent <= 100) p.pct = (int)s.BatteryLifePercent;
  if (s.BatteryLifeTime != (DWORD)-1) { p.secs = s.BatteryLifeTime; p.hasTime = true; }
  return p;
}

Battery GetBattery() {
  Battery b;
  HDEVINFO dev = SetupDiGetClassDevsW(&GUID_DEVCLASS_BATTERY, 0, 0, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (dev == INVALID_HANDLE_VALUE) return b;
  for (DWORD i = 0;; i++) {
    SP_DEVICE_INTERFACE_DATA ifd{};
    ifd.cbSize = sizeof(ifd);
    if (!SetupDiEnumDeviceInterfaces(dev, 0, &GUID_DEVCLASS_BATTERY, i, &ifd)) break;
    DWORD need = 0;
    SetupDiGetDeviceInterfaceDetailW(dev, &ifd, 0, 0, &need, 0);
    if (!need) continue;
    std::vector<BYTE> buf(need);
    auto* det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf.data();
    det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    SP_DEVINFO_DATA did{};
    did.cbSize = sizeof(did);
    if (!SetupDiGetDeviceInterfaceDetailW(dev, &ifd, det, need, 0, &did)) continue;
    WCHAR nm[256] = {};
    if (SetupDiGetDeviceRegistryPropertyW(dev, &did, SPDRP_FRIENDLYNAME, 0, (PBYTE)nm, sizeof(nm), 0)) b.name = nm;
    HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE)
      h = CreateFileW(det->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) continue;
    DWORD tmo = 0, tag = 0, ret = 0;
    if (!DeviceIoControl(h, IOCTL_BATTERY_QUERY_TAG, &tmo, sizeof(tmo), &tag, sizeof(tag), &ret, 0) || !tag) {
      CloseHandle(h);
      continue;
    }
    b.ok = true;
    BATTERY_QUERY_INFORMATION q{};
    q.BatteryTag = tag;
    q.InformationLevel = BatteryInformation;
    BATTERY_INFORMATION bi{};
    if (DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &q, sizeof(q), &bi, sizeof(bi), &ret, 0)) {
      WCHAR c4[5] = {};
      for (int k = 0; k < 4; k++) c4[k] = bi.Chemistry[k];
      std::wstring cs = c4;
      while (!cs.empty() && (cs.back() == 0 || cs.back() == L' ')) cs.pop_back();
      b.chem = cs.empty() ? L"Неизвестно" : cs;
      if (bi.DesignedCapacity != BATTERY_UNKNOWN_CAPACITY && bi.FullChargedCapacity != BATTERY_UNKNOWN_CAPACITY && bi.DesignedCapacity) {
        b.design = bi.DesignedCapacity;
        b.full = bi.FullChargedCapacity;
        b.cap = true;
        b.wear = ((double)bi.DesignedCapacity - bi.FullChargedCapacity) / bi.DesignedCapacity * 100;
        if (b.wear < 0) b.wear = 0;
        if (b.wear > 100) b.wear = 100;
        b.hasWear = true;
      }
      if (bi.CycleCount) { b.cycles = bi.CycleCount; b.hasCycles = true; }
    }
    BATTERY_WAIT_STATUS ws{};
    ws.BatteryTag = tag;
    BATTERY_STATUS st{};
    if (DeviceIoControl(h, IOCTL_BATTERY_QUERY_STATUS, &ws, sizeof(ws), &st, sizeof(st), &ret, 0)) {
      if (st.Capacity != BATTERY_UNKNOWN_CAPACITY) { b.left = st.Capacity; b.hasLeft = true; }
      if (st.Voltage != BATTERY_UNKNOWN_VOLTAGE) { b.volt = st.Voltage; b.hasVolt = true; }
      if (st.Rate != (LONG)BATTERY_UNKNOWN_RATE) { b.rate = st.Rate; b.hasRate = true; }
    }
    q.InformationLevel = BatteryTemperature;
    DWORD tp = 0;
    if (DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &q, sizeof(q), &tp, sizeof(tp), &ret, 0) &&
        ret == sizeof(tp) && tp >= 2000 && tp <= 4500) {
      b.temp = tp;
      b.hasTemp = true;
    }
    CloseHandle(h);
    break;
  }
  SetupDiDestroyDeviceInfoList(dev);
  return b;
}

std::wstring Num(double v, int pr) {
  std::wstringstream s;
  s << std::fixed << std::setprecision(pr) << v;
  return s.str();
}

std::wstring Dur(DWORD s) {
  DWORD h = s / 3600, m = s % 3600 / 60;
  return h ? std::to_wstring(h) + L" ч " + std::to_wstring(m) + L" мин" : std::to_wstring(m) + L" мин";
}

std::wstring SrcText(const Power& p) {
  if (!p.present) return L"Батарея отсутствует";
  if (!p.acUnk && p.ac) return p.charging ? L"Работа от сети (идёт заряд)" : L"Работа от сети";
  if (!p.acUnk) return p.hasTime ? L"Работа от батареи — осталось " + Dur(p.secs) : L"Работа от батареи";
  return L"Неизвестно";
}

std::wstring RateText(LONG r) {
  if (r > 0) return L"+" + std::to_wstring(r) + L" мВт (идёт заряд)";
  if (r < 0) return std::to_wstring(r) + L" мВт (разряд)";
  return L"0 мВт";
}
