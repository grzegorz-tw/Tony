// Tony - screen capture, image encode/decode and keyboard/mouse injection. Windows only.
#pragma once
#include <string>
#include <vector>

namespace tony {

struct Bitmap {
  int w = 0, h = 0;
  std::vector<unsigned char> rgba;  // w*h*4
  int originX = 0, originY = 0;     // where the top-left pixel sits on the virtual desktop (captures only)
  bool empty() const { return w <= 0 || h <= 0 || rgba.size() != static_cast<size_t>(w) * h * 4; }
};

struct MonitorInfo { int index = 0, x = 0, y = 0, w = 0, h = 0; bool primary = false; };
std::vector<MonitorInfo> listMonitors();

// target: "all" (every monitor) | "primary" | "monitor:N" | "window" (foreground) | "window:<title fragment>"
bool captureScreen(const std::string& target, Bitmap& out, std::string* err);

Bitmap downscale(const Bitmap& in, int maxDim);  // area-average; returns the input unchanged when already small enough
bool encodePng(const Bitmap& b, std::string& pngBytes);
bool encodeJpeg(const Bitmap& b, int quality, std::string& jpegBytes);
bool decodeImage(const std::string& fileBytes, Bitmap& out);  // PNG/JPG/BMP/GIF/TGA...
bool loadImageFile(const std::string& path, Bitmap& out, std::string* err);
bool saveBytes(const std::string& path, const std::string& bytes);
std::string toDataUrl(const std::string& mime, const std::string& bytes);

// ---- input injection (screen pixel coordinates; the app is per-monitor DPI aware)
bool inputTypeText(const std::string& utf8);
bool inputKeyCombo(const std::string& combo, std::string* err);                            // "ctrl+shift+s", "enter", "win+r"
bool inputClick(int x, int y, const std::string& button, int clicks, std::string* err);   // left | right | middle
bool inputScroll(int x, int y, int notches);                                               // positive = up

}  // namespace tony
