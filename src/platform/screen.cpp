#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "screen.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>

#include <stb_image.h>
#include <stb_image_write.h>

#include "core/strutil.h"
#include "winutil.h"

namespace tony {

std::vector<MonitorInfo> listMonitors() {
  std::vector<MonitorInfo> v;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR m, HDC, LPRECT r, LPARAM lp) -> BOOL {
        auto* vec = reinterpret_cast<std::vector<MonitorInfo>*>(lp);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(m, &mi);
        MonitorInfo i;
        i.index = static_cast<int>(vec->size());
        i.x = r->left; i.y = r->top; i.w = r->right - r->left; i.h = r->bottom - r->top;
        i.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        vec->push_back(i);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&v));
  return v;
}

static bool captureRect(int x, int y, int w, int h, Bitmap& out, std::string* err) {
  if (w <= 0 || h <= 0) { if (err) *err = "empty capture area"; return false; }
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;  // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  bool ok = false;
  if (bmp && bits) {
    HGDIOBJ old = SelectObject(mem, bmp);
    ok = BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT) != 0;
    if (ok) {
      out.w = w; out.h = h; out.originX = x; out.originY = y;
      out.rgba.resize(static_cast<size_t>(w) * h * 4);
      const unsigned char* s = static_cast<const unsigned char*>(bits);
      for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
        out.rgba[i * 4 + 0] = s[i * 4 + 2];
        out.rgba[i * 4 + 1] = s[i * 4 + 1];
        out.rgba[i * 4 + 2] = s[i * 4 + 0];
        out.rgba[i * 4 + 3] = 255;
      }
    }
    SelectObject(mem, old);
  }
  if (bmp) DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  if (!ok && err) *err = "screen capture failed (a protected window or a locked screen can block it)";
  return ok;
}

bool captureScreen(const std::string& target, Bitmap& out, std::string* err) {
  std::string t = lower(trim(target));
  auto mons = listMonitors();
  if (t.empty() || t == "all") {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN), vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return captureRect(vx, vy, vw, vh, out, err);
  }
  if (t == "primary") {
    for (auto& m : mons) if (m.primary) return captureRect(m.x, m.y, m.w, m.h, out, err);
    if (!mons.empty()) return captureRect(mons[0].x, mons[0].y, mons[0].w, mons[0].h, out, err);
  }
  if (startsWith(t, "monitor:")) {
    int idx = std::atoi(t.c_str() + 8);
    for (auto& m : mons) if (m.index == idx) return captureRect(m.x, m.y, m.w, m.h, out, err);
    if (err) *err = "no such monitor; there are " + std::to_string(mons.size());
    return false;
  }
  if (t == "window" || startsWith(t, "window:")) {
    std::string frag = t == "window" ? "" : trim(t.substr(7));
    for (auto& w : listWindows()) {
      if (frag.empty() ? !w.foreground : !containsI(w.title, frag)) continue;
      if (w.w <= 0 || w.h <= 0 || w.x <= -30000) { if (err) *err = "that window is minimised"; return false; }
      int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN), vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
      int x0 = std::max(w.x, vx), y0 = std::max(w.y, vy), x1 = std::min(w.x + w.w, vx + vw), y1 = std::min(w.y + w.h, vy + vh);
      return captureRect(x0, y0, x1 - x0, y1 - y0, out, err);
    }
    if (err) *err = frag.empty() ? "no foreground window" : "no visible window with '" + frag + "' in its title";
    return false;
  }
  if (err) *err = "unknown target '" + target + "' (use all, primary, monitor:N, window, window:<title>)";
  return false;
}

Bitmap downscale(const Bitmap& in, int maxDim) {
  if (in.empty() || maxDim <= 0 || std::max(in.w, in.h) <= maxDim) return in;
  double scale = static_cast<double>(maxDim) / std::max(in.w, in.h);
  Bitmap o;
  o.originX = in.originX; o.originY = in.originY;
  o.w = std::max(1, static_cast<int>(in.w * scale));
  o.h = std::max(1, static_cast<int>(in.h * scale));
  o.rgba.resize(static_cast<size_t>(o.w) * o.h * 4);
  for (int y = 0; y < o.h; ++y) {
    int sy0 = static_cast<int>(y / scale), sy1 = std::min(in.h, std::max(sy0 + 1, static_cast<int>((y + 1) / scale)));
    for (int x = 0; x < o.w; ++x) {
      int sx0 = static_cast<int>(x / scale), sx1 = std::min(in.w, std::max(sx0 + 1, static_cast<int>((x + 1) / scale)));
      unsigned long acc[4] = {0, 0, 0, 0};
      unsigned long cnt = 0;
      for (int sy = sy0; sy < sy1; ++sy)
        for (int sx = sx0; sx < sx1; ++sx) {
          const unsigned char* p = &in.rgba[(static_cast<size_t>(sy) * in.w + sx) * 4];
          for (int c = 0; c < 4; ++c) acc[c] += p[c];
          ++cnt;
        }
      unsigned char* d = &o.rgba[(static_cast<size_t>(y) * o.w + x) * 4];
      for (int c = 0; c < 4; ++c) d[c] = static_cast<unsigned char>(cnt ? acc[c] / cnt : 0);
    }
  }
  return o;
}

static void appendTo(void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), static_cast<size_t>(size)); }

bool encodePng(const Bitmap& b, std::string& out) {
  if (b.empty()) return false;
  out.clear();
  return stbi_write_png_to_func(appendTo, &out, b.w, b.h, 4, b.rgba.data(), b.w * 4) != 0;
}

bool encodeJpeg(const Bitmap& b, int quality, std::string& out) {
  if (b.empty()) return false;
  out.clear();
  return stbi_write_jpg_to_func(appendTo, &out, b.w, b.h, 4, b.rgba.data(), quality) != 0;
}

bool decodeImage(const std::string& bytes, Bitmap& out) {
  int w = 0, h = 0, n = 0;
  unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()), &w, &h, &n, 4);
  if (!px) return false;
  out.w = w; out.h = h;
  out.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
  stbi_image_free(px);
  return true;
}

bool loadImageFile(const std::string& path, Bitmap& out, std::string* err) {
  std::string bytes = readTextFile(path, 128u << 20);
  if (bytes.empty()) { if (err) *err = "cannot read " + path; return false; }
  if (!decodeImage(bytes, out)) { if (err) *err = "not a supported image: " + path; return false; }
  return true;
}

bool saveBytes(const std::string& path, const std::string& bytes) { return writeTextFile(path, bytes); }

std::string toDataUrl(const std::string& mime, const std::string& bytes) { return "data:" + mime + ";base64," + base64Encode(bytes); }

// ===================================================================== input injection
bool inputTypeText(const std::string& utf8) {
  std::wstring w = widen(utf8);
  std::vector<INPUT> in;
  for (wchar_t c : w) {
    if (c == L'\r') continue;
    INPUT d{};
    d.type = INPUT_KEYBOARD;
    if (c == L'\n') { d.ki.wVk = VK_RETURN; INPUT u = d; u.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(d); in.push_back(u); continue; }
    d.ki.wScan = c;
    d.ki.dwFlags = KEYEVENTF_UNICODE;
    INPUT u = d;
    u.ki.dwFlags |= KEYEVENTF_KEYUP;
    in.push_back(d);
    in.push_back(u);
  }
  for (size_t i = 0; i < in.size(); i += 64) {  // chunked so slow targets keep up
    UINT n = static_cast<UINT>(std::min<size_t>(64, in.size() - i));
    if (SendInput(n, &in[i], sizeof(INPUT)) != n) return false;
    Sleep(5);
  }
  return true;
}

static WORD vkFor(const std::string& name) {
  static const std::map<std::string, WORD> m = {
      {"ctrl", VK_CONTROL}, {"control", VK_CONTROL}, {"alt", VK_MENU}, {"shift", VK_SHIFT}, {"win", VK_LWIN}, {"super", VK_LWIN}, {"windows", VK_LWIN},
      {"enter", VK_RETURN}, {"return", VK_RETURN}, {"tab", VK_TAB}, {"esc", VK_ESCAPE}, {"escape", VK_ESCAPE}, {"space", VK_SPACE},
      {"backspace", VK_BACK}, {"delete", VK_DELETE}, {"del", VK_DELETE}, {"insert", VK_INSERT}, {"home", VK_HOME}, {"end", VK_END},
      {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT}, {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
      {"printscreen", VK_SNAPSHOT}, {"capslock", VK_CAPITAL}, {"menu", VK_APPS}};
  auto it = m.find(name);
  if (it != m.end()) return it->second;
  if (name.size() >= 2 && name[0] == 'f') { int n = std::atoi(name.c_str() + 1); if (n >= 1 && n <= 24) return static_cast<WORD>(VK_F1 + n - 1); }
  if (name.size() == 1) { SHORT s = VkKeyScanW(static_cast<wchar_t>(name[0])); if (s != -1) return static_cast<WORD>(s & 0xFF); }
  return 0;
}

static bool isExtended(WORD vk) {
  return vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_UP || vk == VK_DOWN ||
         vk == VK_LEFT || vk == VK_RIGHT || vk == VK_LWIN || vk == VK_APPS;
}

bool inputKeyCombo(const std::string& combo, std::string* err) {
  std::vector<WORD> keys;
  for (auto& part : split(lower(combo), '+')) {
    std::string p = trim(part);
    if (p.empty()) continue;
    WORD vk = vkFor(p);
    if (!vk) { if (err) *err = "unknown key '" + p + "'"; return false; }
    keys.push_back(vk);
  }
  if (keys.empty()) { if (err) *err = "no keys given"; return false; }
  std::vector<INPUT> in;
  auto add = [&](WORD vk, bool up) {
    INPUT i{};
    i.type = INPUT_KEYBOARD;
    i.ki.wVk = vk;
    i.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (isExtended(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    in.push_back(i);
  };
  for (WORD k : keys) add(k, false);
  for (auto it = keys.rbegin(); it != keys.rend(); ++it) add(*it, true);
  return SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT)) == in.size();
}

static void moveAbs(int x, int y) {
  int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN), vw = std::max(2, GetSystemMetrics(SM_CXVIRTUALSCREEN)), vh = std::max(2, GetSystemMetrics(SM_CYVIRTUALSCREEN));
  INPUT i{};
  i.type = INPUT_MOUSE;
  i.mi.dx = static_cast<LONG>((static_cast<long long>(x - vx) * 65535) / (vw - 1));
  i.mi.dy = static_cast<LONG>((static_cast<long long>(y - vy) * 65535) / (vh - 1));
  i.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
  SendInput(1, &i, sizeof(i));
}

bool inputClick(int x, int y, const std::string& button, int clicks, std::string* err) {
  DWORD down = MOUSEEVENTF_LEFTDOWN, up = MOUSEEVENTF_LEFTUP;
  if (button == "right") { down = MOUSEEVENTF_RIGHTDOWN; up = MOUSEEVENTF_RIGHTUP; }
  else if (button == "middle") { down = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP; }
  else if (!button.empty() && button != "left") { if (err) *err = "button must be left, right or middle"; return false; }
  moveAbs(x, y);
  Sleep(30);
  for (int c = 0; c < std::max(1, std::min(3, clicks)); ++c) {
    INPUT i[2] = {};
    i[0].type = i[1].type = INPUT_MOUSE;
    i[0].mi.dwFlags = down;
    i[1].mi.dwFlags = up;
    if (SendInput(2, i, sizeof(INPUT)) != 2) return false;
    Sleep(40);
  }
  return true;
}

bool inputScroll(int x, int y, int notches) {
  moveAbs(x, y);
  Sleep(30);
  INPUT i{};
  i.type = INPUT_MOUSE;
  i.mi.dwFlags = MOUSEEVENTF_WHEEL;
  i.mi.mouseData = static_cast<DWORD>(notches * WHEEL_DELTA);
  return SendInput(1, &i, sizeof(i)) == 1;
}

}  // namespace tony
