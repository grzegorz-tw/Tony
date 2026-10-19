#include "theme.h"

namespace tony {

static ImVec4 hex(unsigned rgb, float a = 1.0f) { return ImVec4(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a); }

static Palette g_pal;
static float g_scale = 1.0f;
float uiScale() { return g_scale; }
void setUiScale(float s) { g_scale = s < 0.5f ? 0.5f : (s > 4.0f ? 4.0f : s); }
const Palette& palette() { return g_pal; }

void applyTheme(const std::string& name) {
  const bool dark = name != "light";
  Palette& p = g_pal;
  if (dark) {
    p = {hex(0x0F1318), hex(0x151B22), hex(0x1B232C), hex(0x2A3541), hex(0xE6EBF0), hex(0x8A97A6), hex(0x4C8DFF), hex(0xFFFFFF),
         hex(0x1E3A66), hex(0x1B232C), hex(0x131A21), hex(0x0B0F14), hex(0x3DDC97), hex(0xFFB454), hex(0xFF6B6B)};
  } else {
    p = {hex(0xEEF1F5), hex(0xFFFFFF), hex(0xF4F6F9), hex(0xD5DCE4), hex(0x141C26), hex(0x5C6B7A), hex(0x2F6FEB), hex(0xFFFFFF),
         hex(0xDCE8FF), hex(0xFFFFFF), hex(0xE8EEF4), hex(0xEDF1F5), hex(0x1E9E6A), hex(0xB26A00), hex(0xD64545)};
  }
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 10; s.ChildRounding = 8; s.FrameRounding = 6; s.PopupRounding = 8; s.ScrollbarRounding = 8; s.GrabRounding = 6; s.TabRounding = 6;
  s.WindowPadding = ImVec2(14, 12); s.FramePadding = ImVec2(10, 6); s.ItemSpacing = ImVec2(10, 8); s.ItemInnerSpacing = ImVec2(8, 6);
  s.ScrollbarSize = 12; s.WindowBorderSize = 0; s.ChildBorderSize = 0; s.PopupBorderSize = 1; s.FrameBorderSize = 0;
  s.ScaleAllSizes(g_scale);
  ImVec4* c = s.Colors;
  auto lerp = [](ImVec4 a, ImVec4 b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w); };
  c[ImGuiCol_Text] = p.text; c[ImGuiCol_TextDisabled] = p.dim;
  c[ImGuiCol_WindowBg] = p.bg; c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_PopupBg] = p.panel;
  c[ImGuiCol_Border] = p.border; c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = p.panel2; c[ImGuiCol_FrameBgHovered] = lerp(p.panel2, p.accent, 0.12f); c[ImGuiCol_FrameBgActive] = lerp(p.panel2, p.accent, 0.20f);
  c[ImGuiCol_TitleBg] = p.panel; c[ImGuiCol_TitleBgActive] = p.panel; c[ImGuiCol_TitleBgCollapsed] = p.panel;
  c[ImGuiCol_MenuBarBg] = p.panel;
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_ScrollbarGrab] = p.border; c[ImGuiCol_ScrollbarGrabHovered] = p.dim; c[ImGuiCol_ScrollbarGrabActive] = p.accent;
  c[ImGuiCol_CheckMark] = p.accent; c[ImGuiCol_SliderGrab] = p.accent; c[ImGuiCol_SliderGrabActive] = lerp(p.accent, p.text, 0.2f);
  c[ImGuiCol_Button] = p.panel2; c[ImGuiCol_ButtonHovered] = lerp(p.panel2, p.accent, 0.25f); c[ImGuiCol_ButtonActive] = lerp(p.panel2, p.accent, 0.45f);
  c[ImGuiCol_Header] = lerp(p.panel2, p.accent, 0.15f); c[ImGuiCol_HeaderHovered] = lerp(p.panel2, p.accent, 0.28f); c[ImGuiCol_HeaderActive] = lerp(p.panel2, p.accent, 0.40f);
  c[ImGuiCol_Separator] = p.border; c[ImGuiCol_SeparatorHovered] = p.accent; c[ImGuiCol_SeparatorActive] = p.accent;
  c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0); c[ImGuiCol_ResizeGripHovered] = p.accent; c[ImGuiCol_ResizeGripActive] = p.accent;
  c[ImGuiCol_Tab] = p.panel; c[ImGuiCol_TabHovered] = lerp(p.panel, p.accent, 0.3f); c[ImGuiCol_TabSelected] = lerp(p.panel, p.accent, 0.25f);
  c[ImGuiCol_TextSelectedBg] = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.35f);
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, dark ? 0.55f : 0.30f);
}

}  // namespace tony
