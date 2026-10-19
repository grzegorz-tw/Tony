// Tony - colours and ImGui style. Two considered palettes (dark "midnight steel", light "paper"), shared geometry.
#pragma once
#include <imgui.h>

#include <string>

namespace tony {

struct Palette {
  ImVec4 bg, panel, panel2, border, text, dim, accent, accentText;
  ImVec4 userBubble, assistantBubble, toolBubble, codeBg;
  ImVec4 ok, warn, danger;
};

const Palette& palette();
float uiScale();            // DPI scale (1.0 = 96 dpi)
void setUiScale(float s);   // call before applyTheme
void applyTheme(const std::string& name);  // "dark" | "light"
inline ImU32 col(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

}  // namespace tony
