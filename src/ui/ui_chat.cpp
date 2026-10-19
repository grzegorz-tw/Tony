#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <windows.h>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <ctime>

#include "app.h"
#include "core/strutil.h"
#include "platform/dialogs.h"
#include "platform/winutil.h"
#include "theme.h"

namespace tony {

namespace {
ImVec4 withAlpha(ImVec4 c, float a) { c.w = a; return c; }
std::string keyName(int vk) {
  UINT sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
  wchar_t buf[64] = {0};
  LONG lp = static_cast<LONG>(sc << 16);
  if (vk >= 0x21 && vk <= 0x2E) lp |= (1 << 24);  // extended keys
  if (GetKeyNameTextW(lp, buf, 64) > 0) return narrow(buf);
  return "key " + std::to_string(vk);
}
}  // namespace

// ===================================================================== textures
const Texture* App::textureFromBitmap(const std::string& key, const Bitmap& b) {
  auto it = textures_.find(key);
  if (it != textures_.end()) { it->second.lastUse = frameNo_; return &it->second; }
  if (b.empty() || !dev_) return nullptr;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = static_cast<UINT>(b.w); desc.Height = static_cast<UINT>(b.h); desc.MipLevels = 1; desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA sub{};
  sub.pSysMem = b.rgba.data();
  sub.SysMemPitch = static_cast<UINT>(b.w * 4);
  ID3D11Texture2D* tex = nullptr;
  if (FAILED(dev_->CreateTexture2D(&desc, &sub, &tex)) || !tex) return nullptr;
  D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
  sd.Format = desc.Format; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
  ID3D11ShaderResourceView* srv = nullptr;
  dev_->CreateShaderResourceView(tex, &sd, &srv);
  tex->Release();
  if (!srv) return nullptr;
  if (textures_.size() > 64)
    for (auto i = textures_.begin(); i != textures_.end();) {
      if (i->second.lastUse + 600 < frameNo_) { if (i->second.srv) i->second.srv->Release(); i = textures_.erase(i); } else ++i;
    }
  Texture t;
  t.srv = srv; t.w = b.w; t.h = b.h; t.lastUse = frameNo_;
  return &(textures_[key] = t);
}

const Texture* App::texture(const std::string& path) {
  auto it = textures_.find(path);
  if (it != textures_.end()) { it->second.lastUse = frameNo_; return &it->second; }
  Bitmap b;
  if (!fileExists(path) || !loadImageFile(path, b, nullptr)) {
    Bitmap none;  // remember failures as empty entries so we do not retry every frame
    Texture t;
    t.lastUse = frameNo_;
    textures_[path] = t;
    return &textures_[path];
  }
  return textureFromBitmap(path, downscale(b, 1400));
}

// ===================================================================== sidebar
void App::drawSidebar(float w, float h) {
  const Palette& p = palette();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, p.panel);
  ImGui::BeginChild("##side", ImVec2(w, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.45f);
  ImGui::TextColored(p.text, "Tony");
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::TextColored(p.dim, " local assistant");
  ImGui::Spacing();

  ImGui::PushStyleColor(ImGuiCol_Button, p.accent);
  ImGui::PushStyleColor(ImGuiCol_Text, p.accentText);
  ImGui::BeginDisabled(running_);
  if (ImGui::Button("+  New chat", ImVec2(-1, 36))) newChat();
  ImGui::EndDisabled();
  ImGui::PopStyleColor(2);
  ImGui::Spacing();

  const float footer = 178.0f;
  ImGui::BeginChild("##chatlist", ImVec2(0, h - footer - 100), false);
  int toDelete = -1;
  for (int i = 0; i < static_cast<int>(chats_.size()); ++i) {
    Chat& c = chats_[static_cast<size_t>(i)];
    ImGui::PushID(c.id.c_str());
    ImGui::BeginDisabled(running_ && i != curIdx_);
    if (ImGui::Selectable(c.title.c_str(), i == curIdx_, 0, ImVec2(0, 30)) && i != curIdx_) {
      if (cur() && !cur()->msgs.empty()) saveChat(*cur());
      curIdx_ = i;
      agent_->reset();
      tainted_ = false;
      scrollToEnd_ = true;
      stickBottom_ = true;
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopupContextItem("##ctx")) {
      if (ImGui::MenuItem("Rename")) { renameIdx_ = i; renameBuf_ = c.title; }
      if (ImGui::MenuItem("Export as Markdown...")) {
        std::string path = pickSaveFile(hwnd_, "Export chat", (c.title.empty() ? "chat" : c.title) + ".md");
        if (!path.empty()) writeTextFile(path, chatToMarkdown(c));
      }
      ImGui::BeginDisabled(running_);
      if (ImGui::MenuItem("Delete")) toDelete = i;
      ImGui::EndDisabled();
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  if (toDelete >= 0) deleteChat(toDelete);
  ImGui::EndChild();

  // ---- status block
  ImGui::Separator();
  ImGui::Spacing();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  auto chip = [&](ImVec4 dot, const std::string& text, const char* tip) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float lh = ImGui::GetTextLineHeight();
    dl->AddCircleFilled(ImVec2(pos.x + 5, pos.y + lh * 0.5f), 4.0f, col(dot));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16);
    ImGui::TextColored(p.dim, "%s", text.c_str());
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
  };
  {
    std::string state = "idle";
    std::string name;
    if (const ModelEntry* m = live_.chatModel()) name = m->name;
    { std::lock_guard<std::mutex> l(statusMu_); for (auto& s : modelStates_) if (s.id == presetId(name)) state = s.state; }
    ImVec4 dot = state == "loaded" ? p.ok : (state == "loading" || state == "downloading") ? p.warn : p.dim;
    if (live_.llama.mode == "remote") chip(p.accent, "Remote model server", live_.llama.remoteUrl.c_str());
    else if (name.empty()) chip(p.danger, "No chat model set", "Open Settings > Models");
    else chip(dot, name + " - " + state, "State of the active chat model in the local engine");
  }
  {
    const VoiceSettings& v = live_.voice;
    std::string t = v.mode == "off" ? "Voice input off" : v.mode == "ptt" ? "Hold " + keyName(v.pttVk) + " to talk" : v.wakeWordEnabled ? "Hands-free: say \"" + v.wakeWord + "\"" : "Hands-free listening";
    ImVec4 dot = v.mode == "off" ? p.dim : voice_.capturing() ? p.warn : voiceRunning_ ? p.ok : p.danger;
    std::string err;
    { std::lock_guard<std::mutex> l(voiceInMu_); err = voiceError_; }
    chip(dot, t, err.empty() ? "Speech recognition runs locally (Whisper)" : err.c_str());
  }
  {
    const NetworkSettings& n = live_.network;
    std::string t = n.mode == "direct" ? "Network: direct" : n.mode == "proxy" ? "Network: proxy" : "Network: Tor";
    if (n.vpn.requireForWeb) t += " + VPN lock";
    chip(n.mode == "direct" && !n.vpn.requireForWeb ? p.dim : p.accent, t, "Applies to web tools, downloads and remote models. Local engines are never proxied.");
  }
  chip(live_.security.restrictFiles ? p.ok : p.warn, live_.security.restrictFiles ? "Files: restricted folders" : "Files: all folders", "Settings > Files & Security");
  if (tainted_) chip(p.warn, "External content in context", "Web or file content is in this chat, so side-effect actions always ask first. Start a new chat to clear.");
  ImGui::Spacing();
  if (ImGui::Button("Settings", ImVec2(w - 100, 32))) { showSettings_ = true; draft_ = live_; dirty_ = false; }
  ImGui::SameLine();
  if (ImGui::Button(live_.ui.theme == "dark" ? "Light" : "Dark", ImVec2(-1, 32))) setThemeLive(live_.ui.theme == "dark" ? "light" : "dark");
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void App::setThemeLive(const std::string& t) {
  live_.ui.theme = draft_.ui.theme = t;
  applyTheme(t);
  saveSettings(settingsPath_, live_, nullptr);
}

// ===================================================================== chat area
void App::drawChatWindow(float, float w, float h) {
  const Palette& p = palette();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, p.bg);
  ImGui::BeginChild("##chat", ImVec2(w, h), false, ImGuiWindowFlags_NoScrollbar);
  const float composerH = 150.0f + (attachments_.empty() ? 0.0f : 30.0f);
  Chat* c = cur();
  ImGui::BeginChild("##msgs", ImVec2(0, h - composerH), false);
  if (c) {
    if (c->msgs.empty() && !running_) drawOnboarding();
    drawMessages(*c);
  }
  ImGui::EndChild();
  drawComposer(w);
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void App::drawOnboarding() {
  const Palette& p = palette();
  float avail = ImGui::GetContentRegionAvail().x;
  float cw = std::min(avail, 760.0f);
  ImGui::Dummy(ImVec2(0, 60));
  ImGui::SetCursorPosX((avail - cw) / 2);
  ImGui::BeginGroup();
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.8f);
  ImGui::Text("Hi, I'm Tony.");
  ImGui::PopFont();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
  ImGui::TextColored(p.dim, "Everything runs on this PC. I can work with your files, run PowerShell, see your screen, search the web, make and edit images, and talk with you.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  if (live_.llama.mode == "managed" && (!live_.chatModel() || !fileExists(live_.chatModel()->path))) {
    ImGui::TextColored(p.warn, "Before we start, I need a language model:");
    ImGui::BulletText("Settings > Models > Download: pick a GGUF model from Hugging Face (or choose one you already have).");
    ImGui::BulletText("Make it the Chat model, then press Apply.");
    if (ImGui::Button("Open model settings", ImVec2(220, 34))) { showSettings_ = true; settingsTab_ = 1; draft_ = live_; dirty_ = false; }
  } else {
    ImGui::TextColored(p.dim, "Try:");
    for (const char* s : {"What's taking up space on my C: drive?", "Look at my screen and tell me what this error means.", "Summarise the latest news about open-source AI models.", "Generate an image of a lighthouse in a storm, oil painting."}) {
      if (ImGui::Selectable(s, false, 0, ImVec2(cw, 28)) && !running_) startTurn(s, false);
    }
  }
  ImGui::EndGroup();
}

void App::drawMessages(Chat& c) {
  const Palette& p = palette();
  float avail = ImGui::GetContentRegionAvail().x;
  float cw = std::min(avail - 24.0f, 880.0f);
  float pad = std::max(12.0f, (avail - cw) / 2);
  ImGui::Indent(pad);
  ImGui::PushTextWrapPos(pad + cw - 14);

  for (size_t i = 0; i < c.msgs.size(); ++i) { ImGui::PushID(static_cast<int>(i)); drawMessage(c, i); ImGui::PopID(); }

  if (running_) {
    ImGui::Spacing();
    if (!streamReasoning_.empty() && live_.ui.showReasoning) {
      ImGui::TextColored(p.dim, "Thinking:");
      ImGui::TextColored(p.dim, "%s", streamReasoning_.c_str());
    }
    if (!streamText_.empty()) {
      ImGui::TextColored(p.accent, "Tony");
      drawMarkdown(streamText_, cw);
    }
    double t = ImGui::GetTime();
    int dots = static_cast<int>(t * 3) % 4;
    if (!pendingTool_.empty()) {
      ImGui::TextColored(p.warn, "Running %s%s", pendingTool_.c_str(), std::string(static_cast<size_t>(dots), '.').c_str());
      if (!toolStatus_.empty()) ImGui::TextColored(p.dim, "   %s", toolStatus_.c_str());
    } else if (streamText_.empty()) {
      bool loading = false;
      { std::lock_guard<std::mutex> l(statusMu_); for (auto& s : modelStates_) if (s.state == "loading") loading = true; }
      ImGui::TextColored(p.dim, "%s%s", loading ? "Loading the model into memory (first reply after idle takes longer)" : (streamReasoning_.empty() ? "Thinking" : "Thinking (see Settings > General to show the reasoning)"), std::string(static_cast<size_t>(dots), '.').c_str());
    }
    ImGui::Dummy(ImVec2(0, 10));
  }
  ImGui::PopTextWrapPos();
  ImGui::Unindent(pad);

  float maxY = ImGui::GetScrollMaxY();
  if (scrollToEnd_) { ImGui::SetScrollHereY(1.0f); scrollToEnd_ = false; }
  else if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0) stickBottom_ = ImGui::GetScrollY() >= maxY - 40.0f || ImGui::GetIO().MouseWheel < 0;
}

void App::drawImages(const std::vector<std::string>& paths) {
  for (size_t k = 0; k < paths.size(); ++k) {
    const std::string& path = paths[k];
    const Texture* t = texture(path);
    ImGui::PushID(static_cast<int>(k));
    if (t && t->srv) {
      float maxW = std::min(420.0f, ImGui::GetContentRegionAvail().x), s = std::min(1.0f, maxW / t->w);
      ImVec2 size(t->w * s, t->h * s);
      ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(t->srv)), size);
      if (ImGui::IsItemHovered()) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); if (ImGui::IsMouseClicked(0)) shellOpen(path); }
      if (ImGui::SmallButton("Open")) shellOpen(path);
      ImGui::SameLine();
      if (ImGui::SmallButton("Show in folder")) revealInExplorer(path);
      ImGui::SameLine();
      if (ImGui::SmallButton("Save as...")) {
        std::string dest = pickSaveFile(hwnd_, "Save image", fileName(path));
        if (!dest.empty()) std::filesystem::copy_file(u8p(path), u8p(dest), std::filesystem::copy_options::overwrite_existing);
      }
    } else ImGui::TextColored(palette().dim, "[image not found: %s]", fileName(path).c_str());
    ImGui::PopID();
  }
}

void App::drawCodeBlock(const std::string& code, int id) {
  const Palette& p = palette();
  int lines = 1;
  for (char ch : code) if (ch == '\n') ++lines;
  float lh = ImGui::GetTextLineHeightWithSpacing();
  float h = std::min(380.0f, lines * lh * 0.95f + 24.0f);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, p.codeBg);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8);
  ImGui::BeginChild(("##code" + std::to_string(id)).c_str(), ImVec2(0, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
  if (mono_) ImGui::PushFont(mono_, ImGui::GetStyle().FontSizeBase * 0.92f);
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(code.c_str());
  ImGui::PopTextWrapPos();
  if (mono_) ImGui::PopFont();
  ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 70, ImGui::GetWindowPos().y + 6));
  if (ImGui::SmallButton(("Copy##c" + std::to_string(id)).c_str())) setClipboardText(code);
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

void App::drawMarkdown(const std::string& text, float wrap) {
  (void)wrap;
  const Palette& p = palette();
  auto lines = split(text, '\n');
  std::string code;
  bool inCode = false;
  int blockId = static_cast<int>(std::hash<std::string>{}(text) & 0x7fffff);
  for (size_t i = 0; i < lines.size(); ++i) {
    std::string line = lines[i];
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (startsWith(trim(line), "```")) {
      if (inCode) { drawCodeBlock(code, blockId + static_cast<int>(i)); code.clear(); inCode = false; }
      else inCode = true;
      continue;
    }
    if (inCode) { code += (code.empty() ? "" : "\n") + line; continue; }
    std::string t = trim(line);
    if (t.empty()) { ImGui::Dummy(ImVec2(0, 4)); continue; }
    // table: consecutive lines starting and ending with '|'
    if (t.front() == '|' && t.back() == '|' && i + 1 < lines.size() && trim(lines[i + 1]).find("---") != std::string::npos) {
      std::vector<std::vector<std::string>> rows;
      size_t j = i;
      for (; j < lines.size(); ++j) {
        std::string r = trim(lines[j]);
        if (r.empty() || r.front() != '|') break;
        if (r.find("---") != std::string::npos && r.find_first_not_of("|-: \t") == std::string::npos) continue;
        std::vector<std::string> cells;
        for (auto& cell : split(r.substr(1, r.size() - (r.back() == '|' ? 2 : 1)), '|')) cells.push_back(replaceAll(trim(cell), "**", ""));
        rows.push_back(cells);
      }
      size_t cols = 0;
      for (auto& r : rows) cols = std::max(cols, r.size());
      if (cols > 0 && ImGui::BeginTable(("##t" + std::to_string(blockId + static_cast<int>(i))).c_str(), static_cast<int>(cols), ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (size_t r = 0; r < rows.size(); ++r) {
          ImGui::TableNextRow();
          for (size_t cc = 0; cc < cols; ++cc) {
            ImGui::TableSetColumnIndex(static_cast<int>(cc));
            if (r == 0) ImGui::TextColored(p.accent, "%s", cc < rows[r].size() ? rows[r][cc].c_str() : "");
            else ImGui::TextWrapped("%s", cc < rows[r].size() ? rows[r][cc].c_str() : "");
          }
        }
        ImGui::EndTable();
      }
      i = j - 1;
      continue;
    }
    if (t[0] == '#') {
      size_t n = 0;
      while (n < t.size() && t[n] == '#') ++n;
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * (n == 1 ? 1.35f : n == 2 ? 1.22f : 1.1f));
      ImGui::TextColored(p.text, "%s", replaceAll(trim(t.substr(n)), "**", "").c_str());
      ImGui::PopFont();
      continue;
    }
    if (t == "---" || t == "***") { ImGui::Separator(); continue; }
    std::string shown = replaceAll(replaceAll(line, "**", ""), "__", "");
    if (startsWith(t, "- ") || startsWith(t, "* ")) {
      size_t indent = line.find_first_not_of(" \t");
      ImGui::Indent(14.0f + static_cast<float>(indent == std::string::npos ? 0 : indent) * 4.0f);
      ImGui::Bullet();
      ImGui::SameLine();
      ImGui::TextWrapped("%s", replaceAll(replaceAll(t.substr(2), "**", ""), "__", "").c_str());
      ImGui::Unindent(14.0f + static_cast<float>(indent == std::string::npos ? 0 : indent) * 4.0f);
    } else if (startsWith(t, "> ")) {
      ImGui::TextColored(p.dim, "| %s", t.substr(2).c_str());
    } else ImGui::TextWrapped("%s", shown.c_str());
  }
  if (inCode && !code.empty()) drawCodeBlock(code, blockId + 9999);
}

void App::drawMessage(Chat& c, size_t i) {
  const Palette& p = palette();
  const Message& m = c.msgs[i];
  float avail = ImGui::GetContentRegionAvail().x;
  ImGui::Spacing();

  auto bubble = [&](ImVec4 bg, const std::function<void()>& body) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::Indent(12);
    body();
    ImGui::Unindent(12);
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::EndGroup();
    ImVec2 p1 = ImGui::GetItemRectMax();
    p1.x = p0.x + avail;
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(p0, p1, col(bg), 10.0f);
    dl->ChannelsMerge();
  };

  if (m.role == "user") {
    bubble(p.userBubble, [&] {
      ImGui::TextColored(p.dim, "You");
      ImGui::TextWrapped("%s", m.content.c_str());
      if (!m.imageDataUrls.empty()) ImGui::TextColored(p.dim, "[%zu image attachment(s)]", m.imageDataUrls.size());
    });
  } else if (m.role == "assistant") {
    if (m.content.empty() && m.reasoning.empty() && !m.toolCalls.empty()) return;  // only tool calls: the tool cards below tell the story
    bubble(p.assistantBubble, [&] {
      ImGui::TextColored(p.accent, "Tony");
      if (m.tokPerSec > 0) { ImGui::SameLine(); ImGui::TextColored(p.dim, "  %.1f tok/s", m.tokPerSec); }
      if (!m.reasoning.empty()) {
        if (ImGui::TreeNodeEx("Thinking", live_.ui.showReasoning ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
          ImGui::TextColored(p.dim, "%s", m.reasoning.c_str());
          ImGui::TreePop();
        }
      }
      drawMarkdown(m.content, avail);
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
      if (ImGui::SmallButton("Copy")) setClipboardText(m.content);
      ImGui::SameLine();
      if (!running_ && ImGui::SmallButton("Speak")) { SentenceStream ss; for (auto& s : ss.push(m.content)) tts_.speak(s); for (auto& s : ss.flush()) tts_.speak(s); }
      ImGui::PopStyleColor();
    });
  } else if (m.role == "tool") {
    // find the call that produced this result for a readable header
    std::string args;
    for (size_t k = i; k-- > 0;) {
      bool found = false;
      for (auto& tc : c.msgs[k].toolCalls) if (tc.id == m.toolCallId) { args = tc.argsJson; found = true; }
      if (found) break;
    }
    json aj = json::parse(args, nullptr, false);
    std::string brief;
    if (aj.is_object()) {
      for (const char* key : {"command", "query", "url", "path", "prompt", "question", "action", "target"}) if (aj.contains(key) && aj[key].is_string()) { brief = aj[key].get<std::string>(); break; }
      if (brief.empty()) brief = truncateMiddle(args, 80);
    }
    brief = replaceAll(brief, "\n", " ");
    if (brief.size() > 90) brief = brief.substr(0, 90) + "...";
    bubble(p.toolBubble, [&] {
      ImGui::PushStyleColor(ImGuiCol_Text, m.isError ? p.danger : p.ok);
      bool open = ImGui::TreeNodeEx("##tool", live_.ui.showToolDetails ? ImGuiTreeNodeFlags_DefaultOpen : 0, "%s %s", m.isError ? "[x]" : "[ok]", m.toolName.c_str());
      ImGui::PopStyleColor();
      ImGui::SameLine();
      ImGui::TextColored(p.dim, "%s", brief.c_str());
      if (open) {
        if (mono_) ImGui::PushFont(mono_, ImGui::GetStyle().FontSizeBase * 0.88f);
        if (!args.empty() && args != "{}") { ImGui::TextColored(p.dim, "%s", truncateMiddle(args, 1200).c_str()); ImGui::Separator(); }
        ImGui::BeginChild("##res", ImVec2(0, std::min(240.0f, 18.0f + 17.0f * static_cast<float>(std::count(m.content.begin(), m.content.end(), '\n') + 1))), false, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(truncateMiddle(m.content, 6000).c_str());
        ImGui::EndChild();
        if (mono_) ImGui::PopFont();
        ImGui::TreePop();
      }
      if (!m.shownImages.empty()) drawImages(m.shownImages);
    });
  } else if (m.role == "notice") {
    ImGui::TextColored(p.dim, "%s", m.content.c_str());
  } else if (m.role == "error") {
    bubble(withAlpha(p.danger, 0.16f), [&] { ImGui::TextColored(p.danger, "Problem"); ImGui::TextWrapped("%s", m.content.c_str()); });
  }
}

// ===================================================================== composer
void App::attachImageFile() {
  std::string path = pickOpenFile(hwnd_, "Attach an image", {{"Images", "*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tga"}});
  if (path.empty()) return;
  Bitmap b;
  std::string err;
  if (!loadImageFile(path, b, &err)) { Message m; m.role = "error"; m.content = err; m.uiOnly = true; if (cur()) cur()->msgs.push_back(m); return; }
  Bitmap small = downscale(b, 1568);
  std::string png;
  if (encodePng(small, png)) attachments_.push_back({fileName(path), toDataUrl("image/png", png)});
}

void App::attachScreenshot() {
  Bitmap b;
  std::string err;
  if (!captureScreen("primary", b, &err)) return;
  std::string png;
  if (encodePng(downscale(b, 1568), png)) attachments_.push_back({"screenshot", toDataUrl("image/png", png)});
}

void App::send() { startTurn(input_, false); }

void App::drawComposer(float w) {
  const Palette& p = palette();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, p.panel);
  float cw = std::min(w - 40.0f, 900.0f);
  ImGui::SetCursorPosX((w - cw) / 2);
  ImGui::BeginChild("##composer", ImVec2(cw, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
  if (!attachments_.empty()) {
    for (size_t i = 0; i < attachments_.size(); ++i) {
      if (i) ImGui::SameLine();
      if (ImGui::SmallButton((attachments_[i].name + "  x##att" + std::to_string(i)).c_str())) { attachments_.erase(attachments_.begin() + static_cast<long>(i)); break; }
    }
  }
  bool submit = false;
  ImGui::PushStyleColor(ImGuiCol_FrameBg, p.panel2);
  if (ImGui::IsWindowAppearing() || (!running_ && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive())) ImGui::SetKeyboardFocusHere();
  submit = ImGui::InputTextMultiline("##input", &input_, ImVec2(-1, 64), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
  ImGui::PopStyleColor();
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {}
  if (input_.empty() && !ImGui::IsItemActive()) {
    ImVec2 pos = ImGui::GetItemRectMin();
    ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + 10, pos.y + 7), col(p.dim), "Message Tony...  (Enter to send, Ctrl+Enter for a new line)");
  }

  ImGui::BeginDisabled(running_);
  if (ImGui::Button("Attach image")) attachImageFile();
  ImGui::SameLine();
  if (ImGui::Button("Add screenshot")) attachScreenshot();
  ImGui::EndDisabled();
  ImGui::SameLine();
  {  // voice: cycle off -> push-to-talk -> hands-free
    const char* label = live_.voice.mode == "off" ? "Mic: off" : live_.voice.mode == "ptt" ? "Mic: hold to talk" : "Mic: hands-free";
    if (ImGui::Button(label) && !busy()) {
      live_.voice.mode = live_.voice.mode == "off" ? "ptt" : live_.voice.mode == "ptt" ? "handsfree" : "off";
      draft_.voice.mode = live_.voice.mode;
      saveSettings(settingsPath_, live_, nullptr);
      reconfigure();
    }
    if (voiceRunning_) { ImGui::SameLine(); ImGui::ProgressBar(std::min(1.0f, voice_.level()), ImVec2(60, 8), ""); }
  }
  ImGui::SameLine();
  {
    const char* label = live_.tts.engine == "off" || live_.tts.speak == "off" ? "Voice: silent" : live_.tts.speak == "always" ? "Voice: always" : "Voice: when I talk";
    if (ImGui::Button(label) && !busy()) {
      std::string next = live_.tts.speak == "voice_only" ? "always" : live_.tts.speak == "always" ? "off" : "voice_only";
      live_.tts.speak = draft_.tts.speak = next;
      if (next != "off" && live_.tts.engine == "off") live_.tts.engine = draft_.tts.engine = "sapi";
      saveSettings(settingsPath_, live_, nullptr);
      reconfigure();
    }
  }
  float bw = 96;
  ImGui::SameLine(ImGui::GetWindowWidth() - bw - 14);
  if (running_) {
    ImGui::PushStyleColor(ImGuiCol_Button, p.danger);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button("Stop", ImVec2(bw, 0))) stopTurn();
    ImGui::PopStyleColor(2);
  } else {
    ImGui::PushStyleColor(ImGuiCol_Button, p.accent);
    ImGui::PushStyleColor(ImGuiCol_Text, p.accentText);
    if (ImGui::Button("Send", ImVec2(bw, 0))) submit = true;
    ImGui::PopStyleColor(2);
  }
  if (submit && !running_) send();
  if (const Chat* c = cur(); c && !running_ && tts_.speaking() && ImGui::IsKeyPressed(ImGuiKey_Escape)) tts_.stop();
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

// ===================================================================== modals
void App::drawModals() {
  const Palette& p = palette();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImVec2 center(vp->WorkPos.x + vp->WorkSize.x / 2, vp->WorkPos.y + vp->WorkSize.y / 2);

  // ---- approval
  bool approvalActive;
  ApprovalRequest rq;
  { std::lock_guard<std::mutex> l(approvalP_.mu); approvalActive = approvalP_.active && !approvalP_.answered; rq = approvalReq_; }
  if (approvalActive) ImGui::OpenPopup("Approve action");
  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(680.0f, vp->WorkSize.x - 40), 0));
  if (ImGui::BeginPopupModal("Approve action", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
    if (!approvalActive) ImGui::CloseCurrentPopup();
    ImVec4 rc = rq.risk == "high" ? p.danger : rq.risk == "medium" ? p.warn : p.ok;
    ImGui::TextColored(rc, "%s", rq.risk == "high" ? "HIGH IMPACT" : rq.risk == "medium" ? "NEEDS YOUR OK" : "LOW IMPACT");
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
    ImGui::TextWrapped("Tony wants to: %s", rq.summary.c_str());
    ImGui::PopFont();
    if (!rq.reason.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, p.warn); ImGui::TextWrapped("%s", rq.reason.c_str()); ImGui::PopStyleColor(); }
    if (rq.tainted) ImGui::TextColored(p.dim, "Web or file content is part of this chat, so this action cannot be pre-approved.");
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.codeBg);
    ImGui::BeginChild("##detail", ImVec2(0, 190), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
    if (mono_) ImGui::PushFont(mono_, ImGui::GetStyle().FontSizeBase * 0.92f);
    ImGui::TextUnformatted(rq.detail.c_str());
    if (mono_) ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();
    auto answer = [&](ApprovalDecision d) {
      std::lock_guard<std::mutex> l(approvalP_.mu);
      approvalDecision_ = d;
      approvalP_.answered = true;
      approvalP_.cv.notify_all();
      ImGui::CloseCurrentPopup();
    };
    ImGui::PushStyleColor(ImGuiCol_Button, p.accent);
    ImGui::PushStyleColor(ImGuiCol_Text, p.accentText);
    if (ImGui::Button("Allow once", ImVec2(130, 36))) answer(ApprovalDecision::AllowOnce);
    ImGui::PopStyleColor(2);
    if (rq.canRemember) { ImGui::SameLine(); if (ImGui::Button("Allow for this chat", ImVec2(170, 36))) answer(ApprovalDecision::AllowSession); }
    ImGui::SameLine();
    if (rq.risk == "high") ImGui::SetItemDefaultFocus();
    if (ImGui::Button("Deny", ImVec2(110, 36))) answer(ApprovalDecision::Deny);
    ImGui::EndPopup();
  }

  // ---- question from the model
  bool askActive;
  std::string q;
  std::vector<std::string> opts;
  { std::lock_guard<std::mutex> l(askP_.mu); askActive = askP_.active && !askP_.answered; q = askQuestion_; opts = askOptions_; }
  if (askActive) ImGui::OpenPopup("Tony has a question");
  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(560.0f, vp->WorkSize.x - 40), 0));
  if (ImGui::BeginPopupModal("Tony has a question", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
    if (!askActive) ImGui::CloseCurrentPopup();
    ImGui::TextWrapped("%s", q.c_str());
    ImGui::Spacing();
    auto reply = [&](const std::string& a) {
      std::lock_guard<std::mutex> l(askP_.mu);
      askAnswer_ = a;
      askP_.answered = true;
      askP_.cv.notify_all();
      ImGui::CloseCurrentPopup();
    };
    for (auto& o : opts) { if (ImGui::Button(o.c_str())) reply(o); ImGui::SameLine(); }
    ImGui::NewLine();
    ImGui::SetNextItemWidth(-90);
    bool enter = ImGui::InputText("##ans", &askInput_, ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
    ImGui::SameLine();
    if ((ImGui::Button("Answer", ImVec2(80, 0)) || enter) && !askInput_.empty()) reply(askInput_);
    ImGui::EndPopup();
  }

  // ---- rename
  if (renameIdx_ >= 0) { ImGui::OpenPopup("Rename chat"); }
  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal("Rename chat", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::SetNextItemWidth(360);
    bool enter = ImGui::InputText("##rn", &renameBuf_, ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
    if (ImGui::Button("OK") || enter) {
      if (renameIdx_ >= 0 && renameIdx_ < static_cast<int>(chats_.size()) && !trim(renameBuf_).empty()) { chats_[static_cast<size_t>(renameIdx_)].title = trim(renameBuf_); saveChat(chats_[static_cast<size_t>(renameIdx_)]); }
      renameIdx_ = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) { renameIdx_ = -1; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
  }
}

}  // namespace tony
