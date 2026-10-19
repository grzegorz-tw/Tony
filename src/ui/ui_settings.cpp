#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <filesystem>

#include "app.h"
#include "core/cmdrisk.h"
#include "core/strutil.h"
#include "net/http.h"
#include "net/netpolicy.h"
#include "platform/dialogs.h"
#include "platform/process.h"
#include "platform/winutil.h"
#include "theme.h"

namespace tony {

// ===================================================================== widgets
#define kLabelW (210.0f * uiScale())

void App::help(const char* text) {
  ImGui::SameLine();
  ImGui::TextColored(palette().dim, "(?)");
  if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(420); ImGui::TextUnformatted(text); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
}

void App::section(const char* title) {
  ImGui::Spacing();
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.12f);
  ImGui::TextColored(palette().accent, "%s", title);
  ImGui::PopFont();
  ImGui::Separator();
  ImGui::Spacing();
}

static void label(const char* l) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(l);
  ImGui::SameLine(kLabelW);
}

bool App::fText(const char* l, std::string& v, const char* hint, bool password) {
  label(l);
  ImGui::PushID(l);
  ImGui::SetNextItemWidth(-1);
  bool ch = hint ? ImGui::InputTextWithHint("##v", hint, &v, password ? ImGuiInputTextFlags_Password : 0) : ImGui::InputText("##v", &v, password ? ImGuiInputTextFlags_Password : 0);
  ImGui::PopID();
  dirty_ |= ch;
  return ch;
}

bool App::fileOk(const std::string& p) const { return p.empty() || fileExists(p) || dirExists(p); }

bool App::fPath(const char* l, std::string& v, const char* kind, const char* filterName, const char* filterSpec) {
  label(l);
  ImGui::PushID(l);
  ImGui::SetNextItemWidth(-90);
  bool ch = ImGui::InputText("##v", &v);
  ImGui::SameLine();
  if (ImGui::Button("Browse", ImVec2(80, 0))) {
    std::string r = std::string(kind) == "folder" ? pickFolder(hwnd_, l) : pickOpenFile(hwnd_, l, filterName ? std::vector<std::pair<std::string, std::string>>{{filterName, filterSpec}} : std::vector<std::pair<std::string, std::string>>{});
    if (!r.empty()) { v = r; ch = true; }
  }
  if (!v.empty() && !fileOk(v)) { ImGui::SetCursorPosX(kLabelW); ImGui::TextColored(palette().danger, "Not found on disk"); }
  ImGui::PopID();
  dirty_ |= ch;
  return ch;
}

bool App::fCombo(const char* l, std::string& v, const std::vector<std::string>& options) {
  label(l);
  ImGui::PushID(l);
  ImGui::SetNextItemWidth(-1);
  bool ch = false;
  if (ImGui::BeginCombo("##v", v.empty() ? "(default)" : v.c_str())) {
    for (auto& o : options) if (ImGui::Selectable(o.empty() ? "(default)" : o.c_str(), o == v)) { v = o; ch = true; }
    ImGui::EndCombo();
  }
  ImGui::PopID();
  dirty_ |= ch;
  return ch;
}

bool App::fCheck(const char* l, bool& v) {
  label("");
  ImGui::PushID(l);
  bool ch = ImGui::Checkbox(l, &v);
  ImGui::PopID();
  dirty_ |= ch;
  return ch;
}

bool App::fInt(const char* l, int& v, int lo, int hi) {
  label(l);
  ImGui::PushID(l);
  ImGui::SetNextItemWidth(200);
  bool ch = ImGui::InputInt("##v", &v, 1, 10);
  ImGui::PopID();
  v = std::max(lo, std::min(hi, v));
  dirty_ |= ch;
  return ch;
}

bool App::fDouble(const char* l, double& v, double lo, double hi, const char* fmt) {
  label(l);
  ImGui::PushID(l);
  ImGui::SetNextItemWidth(260);
  bool ch = ImGui::SliderScalar("##v", ImGuiDataType_Double, &v, &lo, &hi, fmt);
  ImGui::PopID();
  dirty_ |= ch;
  return ch;
}

bool App::fMulti(const char* l, std::string& v, float height) {
  label(l);
  ImGui::PushID(l);
  ImGui::SetNextItemWidth(-1);
  bool ch = ImGui::InputTextMultiline("##v", &v, ImVec2(-1, height));
  ImGui::PopID();
  dirty_ |= ch;
  return ch;
}

void App::runNetOp(const std::string& lbl, std::function<std::string()> fn) {
  if (netOp_.valid()) return;
  netOpLabel_ = lbl;
  netOpResult_ = "Working...";
  netOp_ = std::async(std::launch::async, std::move(fn));
}

// ===================================================================== window
void App::drawSettings() {
  const Palette& p = palette();
  if (netOp_.valid() && netOp_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) netOpResult_ = netOp_.get();
  if (!voicesLoaded_) {
    sapiVoices_ = TtsService::sapiVoices();
    inputDevices_ = VoiceInput::devices();
    voicesLoaded_ = true;
  }
  piperVoices_ = frameNo_ % 120 == 0 || piperVoices_.empty() ? TtsService::piperVoices(joinPath(paths_.modelsDir(), "tts")) : piperVoices_;

  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x / 2, vp->WorkPos.y + vp->WorkSize.y / 2), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(1080.0f, vp->WorkSize.x - 40), std::min(740.0f, vp->WorkSize.y - 40)), ImGuiCond_Appearing);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, p.panel);
  if (!ImGui::Begin("Settings", &showSettings_, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); ImGui::PopStyleColor(); return; }

  const float footer = 52.0f;
  static const char* tabs[] = {"General", "Models", "Images", "Voice", "Prompt", "Tools", "Network", "Files & Security", "About"};
  ImGui::BeginChild("##tabs", ImVec2(180 * uiScale(), -footer), false);
  for (int i = 0; i < 9; ++i) if (ImGui::Selectable(tabs[i], settingsTab_ == i, 0, ImVec2(0, 34))) settingsTab_ = i;
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("##tabcontent", ImVec2(0, -footer), false);
  switch (settingsTab_) {
    case 0: tabGeneral(); break;
    case 1: tabModels(); break;
    case 2: tabImages(); break;
    case 3: tabVoice(); break;
    case 4: tabPrompt(); break;
    case 5: tabTools(); break;
    case 6: tabNetwork(); break;
    case 7: tabSecurity(); break;
    default: tabAbout(); break;
  }
  ImGui::EndChild();

  ImGui::Separator();
  ImGui::Spacing();
  bool b = busy();
  ImGui::BeginDisabled(!dirty_ || b);
  ImGui::PushStyleColor(ImGuiCol_Button, p.accent);
  ImGui::PushStyleColor(ImGuiCol_Text, p.accentText);
  if (ImGui::Button("Apply", ImVec2(120, 34))) applySettings();
  ImGui::PopStyleColor(2);
  ImGui::SameLine();
  if (ImGui::Button("Revert", ImVec2(100, 34))) { draft_ = live_; dirty_ = false; tts_.apply(live_.tts, paths_); }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (b) ImGui::TextColored(p.warn, "Tony is busy - changes can be applied when the current task is finished.");
  else if (dirty_) ImGui::TextColored(p.warn, "Unsaved changes");
  else if (!settingsError_.empty()) ImGui::TextColored(p.danger, "%s", settingsError_.c_str());
  else ImGui::TextColored(p.dim, "All changes applied.");
  ImGui::End();
  ImGui::PopStyleColor();
}

// ===================================================================== General
void App::tabGeneral() {
  section("Appearance");
  if (fCombo("Theme", draft_.ui.theme, {"dark", "light"})) { setThemeLive(draft_.ui.theme); }
  label("Text size");
  float fs = draft_.ui.fontSize;
  ImGui::SetNextItemWidth(260);
  if (ImGui::SliderFloat("##fs", &fs, 12.0f, 26.0f, "%.0f px")) { draft_.ui.fontSize = fs; dirty_ = true; }
  fCheck("Show the model's reasoning (\"thinking\") while it works", draft_.ui.showReasoning);
  fCheck("Expand tool results by default", draft_.ui.showToolDetails);

  section("Behaviour");
  fInt("Max tool rounds per message", draft_.agent.maxSteps, 1, 100);
  help("How many times Tony may call tools in a row before pausing and asking you to say \"continue\".");
  fCheck("Remember things between chats (memory tool)", draft_.agent.memory);
  label("Saved notes");
  if (ImGui::Button("Open notes file")) { std::string f = joinPath(paths_.dataDir, "memory.json"); if (fileExists(f)) shellOpen(f); }
  ImGui::SameLine();
  if (ImGui::Button("Forget everything")) { std::error_code ec; std::filesystem::remove(u8p(joinPath(paths_.dataDir, "memory.json")), ec); }

  section("Folders");
  label("Settings and chats");
  if (ImGui::Button("Open data folder")) shellOpen(paths_.dataDir);
  ImGui::SameLine();
  if (ImGui::Button("Open models folder")) { ensureDir(paths_.modelsDir()); shellOpen(paths_.modelsDir()); }
}

// ===================================================================== Models
static ModelEntry* selected(std::vector<ModelEntry>& v, int idx) { return idx >= 0 && idx < static_cast<int>(v.size()) ? &v[static_cast<size_t>(idx)] : nullptr; }

void App::tabModels() {
  const Palette& p = palette();
  LlamaSettings& L = draft_.llama;
  section("Language-model engine");
  fCombo("Engine", L.mode, {"managed", "remote"});
  help("managed: Tony starts and controls its own llama.cpp server (fully local, recommended).\nremote: use any OpenAI-compatible server (LM Studio, Ollama, vLLM, another PC...).");
  if (L.mode == "managed") {
    fPath("llama-server.exe", L.exePath, "file", "Programs (*.exe)", "*.exe");
    fInt("Port", L.port, 1024, 65000);
    fCombo("GPU memory policy", L.gpuPolicy, {"swap", "coexist"});
    help("swap: only one model is on the GPU at a time. Chat, vision and image models take turns; best for 16 GB cards.\ncoexist: keep the chat and vision models loaded together (needs enough VRAM for both).");
    fInt("Unload after idle (seconds)", L.idleUnloadSec, 0, 86400);
    help("0 = keep the model loaded. Otherwise free VRAM after this many idle seconds; the next message reloads it.");
    fText("Extra server arguments", L.extraArgs, "advanced, e.g. --no-mmap");
  } else {
    fText("Server URL", L.remoteUrl, "http://127.0.0.1:1234/v1");
    fText("API key", L.remoteKey, "optional", true);
    fText("Model name", L.remoteModel, "name sent to the server");
    ImGui::TextColored(p.warn, "Remote mode sends your messages (and screenshots, if you use the screen tools) to that server.");
  }

  section("Models");
  ImGui::BeginChild("##modellist", ImVec2(230, 330), true);
  for (int i = 0; i < static_cast<int>(L.models.size()); ++i) {
    auto& m = L.models[static_cast<size_t>(i)];
    std::string tag = (m.name == L.activeChat ? "[chat] " : "") + std::string(m.name == L.activeVision ? "[vision] " : "");
    if (ImGui::Selectable((tag + (m.name.empty() ? "(unnamed)" : m.name) + "##m" + std::to_string(i)).c_str(), selModel_ == i)) selModel_ = i;
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("##modeledit", ImVec2(0, 330), false);
  if (ModelEntry* m = selected(L.models, selModel_)) {
    fText("Name", m->name);
    fCombo("Role", m->role, {"chat", "vision", "other"});
    fPath("Model file (.gguf)", m->path, "file", "GGUF models (*.gguf)", "*.gguf");
    fPath("Vision projector (mmproj)", m->mmproj, "file", "GGUF (*.gguf)", "*.gguf");
    help("Only for models that can see images. Download the matching mmproj-*.gguf from the same repository.");
    fInt("Context size (tokens)", m->ctx, 1024, 1048576);
    fInt("GPU layers (-1 = auto)", m->gpuLayers, -1, 999);
    help("-1 lets llama.cpp fit as much as possible into free VRAM automatically (recommended).");
    fInt("MoE experts on CPU (-1 = auto)", m->cpuMoe, -1, 999);
    help("For Mixture-of-Experts models (gpt-oss, Qwen3-30B-A3B...): keep the expert weights of the first N layers in system RAM and only the attention on the GPU. With 128 GB RAM this lets huge MoE models run on a 16 GB card. -1 = auto.");
    fCombo("Flash attention", m->flashAttn, {"auto", "on", "off"});
    fCombo("KV cache type (K)", m->cacheK, {"f16", "q8_0", "q4_0"});
    fCombo("KV cache type (V)", m->cacheV, {"f16", "q8_0", "q4_0"});
    fCombo("Reasoning", m->reasoning, {"auto", "on", "off"});
    fInt("CPU threads (0 = auto)", m->threads, 0, 256);
    fText("Device", m->device, "empty = auto, e.g. Vulkan0 or SYCL0");
    fText("Extra arguments", m->extraArgs, "advanced llama-server flags");
    fDouble("Temperature", m->temperature, 0.0, 2.0);
    fDouble("Top-p", m->topP, 0.0, 1.0);
    fDouble("Min-p", m->minP, 0.0, 1.0);
    fInt("Top-k", m->topK, 0, 1000);
    fInt("Max reply tokens", m->maxTokens, 64, 131072);
    label("");
    if (ImGui::Button("Use as chat model")) { L.activeChat = m->name; dirty_ = true; }
    ImGui::SameLine();
    if (ImGui::Button("Use as vision model")) { L.activeVision = m->name; dirty_ = true; }
    ImGui::SameLine();
    if (ImGui::Button("No separate vision model")) { L.activeVision.clear(); dirty_ = true; }
  } else ImGui::TextColored(p.dim, "Select a model on the left, or add one.");
  ImGui::EndChild();
  if (ImGui::Button("Add model")) { ModelEntry m; m.name = "model-" + std::to_string(L.models.size() + 1); L.models.push_back(m); selModel_ = static_cast<int>(L.models.size()) - 1; if (L.models.size() == 1) L.activeChat = m.name; dirty_ = true; }
  ImGui::SameLine();
  if (ImGui::Button("Duplicate") && selected(L.models, selModel_)) { ModelEntry m = *selected(L.models, selModel_); m.name += "-copy"; L.models.push_back(m); selModel_ = static_cast<int>(L.models.size()) - 1; dirty_ = true; }
  ImGui::SameLine();
  if (ImGui::Button("Remove") && selected(L.models, selModel_)) { L.models.erase(L.models.begin() + selModel_); selModel_ = -1; dirty_ = true; }
  ImGui::SameLine();
  ImGui::BeginDisabled(dirty_ || busy() || L.mode != "managed");
  if (ImGui::Button("Load chat model now")) {
    llamaBusyOp_ = true;
    llamaOpMessage_ = "Loading...";
    runNetOp("load", [this]() -> std::string {
      std::atomic<bool> never{false};
      Endpoint ep = llama_->acquire(never);
      if (!ep.ok) { llamaBusyOp_ = false; return "Failed: " + ep.error; }
      ChatRequest r;
      const ModelEntry* m = live_.chatModel();
      r.model = m ? m->name : "";
      Message u; u.role = "user"; u.content = "Reply with one word: ready";
      r.messages = {u};
      r.sampling.maxTokens = 8;
      ChatResult res = backend_->chat(r, nullptr, never);
      llamaBusyOp_ = false;
      return res.error.empty() ? "Model loaded and answering." : "Failed: " + res.error;
    });
  }
  ImGui::SameLine();
  if (ImGui::Button("Unload all (free VRAM)")) { llama_->unloadAll(); }
  ImGui::EndDisabled();
  if (!netOpResult_.empty() && netOpLabel_ == "load") { ImGui::SameLine(); ImGui::TextColored(netOpResult_.rfind("Failed", 0) == 0 ? p.danger : p.dim, "%s", netOpResult_.c_str()); }

  section("Download models from Hugging Face");
  ImGui::TextColored(p.dim, "Pick a repository, browse its files and download. Models are saved in the models folder next to Tony.");
  label("Repository");
  ImGui::SetNextItemWidth(-250);
  ImGui::InputText("##repo", &hfRepo_);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(120);
  if (ImGui::BeginCombo("##dest", ("save in: " + hfDest_).c_str())) { for (const char* d : {"llm", "stt", "tts", "image"}) if (ImGui::Selectable(d, hfDest_ == d)) hfDest_ = d; ImGui::EndCombo(); }
  ImGui::SameLine();
  if (ImGui::Button(hfLoading_ ? "Loading..." : "Browse files")) { hfPath_.clear(); hfBrowse(); }
  ImGui::TextColored(p.dim, "Examples:  ggml-org/gpt-oss-20b-GGUF   ·   unsloth/Qwen3-30B-A3B-GGUF   ·   ggml-org/Qwen2.5-VL-7B-Instruct-GGUF (vision, with mmproj)");
  {
    std::lock_guard<std::mutex> l(hfMu_);
    if (!hfError_.empty()) ImGui::TextColored(p.danger, "%s", hfError_.c_str());
    if (!hfFiles_.empty()) {
      if (!hfPath_.empty()) {
        if (ImGui::SmallButton("..  up")) { size_t s = hfPath_.find_last_of('/'); hfPath_ = s == std::string::npos ? "" : hfPath_.substr(0, s); hfLoading_ = false; if (hfThread_.joinable()) hfThread_.join(); }
        ImGui::SameLine();
        ImGui::TextColored(p.dim, "/%s", hfPath_.c_str());
      }
      ImGui::BeginChild("##hffiles", ImVec2(0, 200), true);
      std::string browseInto;
      for (size_t i = 0; i < hfFiles_.size(); ++i) {
        const HfFile& f = hfFiles_[i];
        ImGui::PushID(static_cast<int>(i));
        std::string base = f.path.substr(f.path.find_last_of('/') == std::string::npos ? 0 : f.path.find_last_of('/') + 1);
        if (f.isDir) { if (ImGui::Selectable(("[dir] " + base).c_str())) browseInto = f.path; }
        else {
          ImGui::TextUnformatted(base.c_str());
          ImGui::SameLine(ImGui::GetContentRegionAvail().x - 210);
          ImGui::TextColored(p.dim, "%s", humanSize(static_cast<unsigned long long>(f.size)).c_str());
          ImGui::SameLine();
          if (ImGui::SmallButton("Download")) startDownload(trim(hfRepo_), f.path, hfDest_);
        }
        ImGui::PopID();
      }
      ImGui::EndChild();
      if (!browseInto.empty()) { hfPath_ = browseInto; hfLoading_ = false; }
    }
  }
  if (!hfPath_.empty() && !hfLoading_ && hfFiles_.empty() && hfError_.empty()) {}
  static std::string lastBrowsed;
  if (hfPath_ != lastBrowsed && !hfLoading_ && !hfFiles_.empty()) { lastBrowsed = hfPath_; hfBrowse(); }

  for (size_t i = 0; i < downloads_.size();) {
    Download& d = *downloads_[i];
    ImGui::PushID(static_cast<int>(i));
    int st = d.state.load();
    double tot = d.total.load(), done = d.done.load();
    ImGui::TextUnformatted(d.name.c_str());
    ImGui::SameLine();
    if (st == 0) { if (ImGui::SmallButton("Cancel")) d.cancel = true; }
    else if (st == 1) {
      ImGui::TextColored(p.ok, "done");
      ImGui::SameLine();
      if (ImGui::SmallButton("Add as chat model")) { ModelEntry m; m.name = fileName(d.dest); m.name = m.name.substr(0, m.name.find_last_of('.')); m.path = d.dest; L.models.push_back(m); selModel_ = static_cast<int>(L.models.size()) - 1; if (L.activeChat.empty()) L.activeChat = m.name; dirty_ = true; }
      ImGui::SameLine();
      if (ImGui::SmallButton("Add as vision projector") && selected(L.models, selModel_)) { selected(L.models, selModel_)->mmproj = d.dest; dirty_ = true; }
      ImGui::SameLine();
      if (ImGui::SmallButton("Dismiss")) { downloads_.erase(downloads_.begin() + static_cast<long>(i)); ImGui::PopID(); continue; }
    } else {
      ImGui::TextColored(st == 3 ? p.dim : p.danger, "%s", st == 3 ? "cancelled" : d.error.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Dismiss")) { downloads_.erase(downloads_.begin() + static_cast<long>(i)); ImGui::PopID(); continue; }
    }
    if (st == 0) ImGui::ProgressBar(tot > 0 ? static_cast<float>(done / tot) : 0.0f, ImVec2(-1, 6), "");
    if (st == 0 && tot > 0) ImGui::TextColored(p.dim, "%s of %s", humanSize(static_cast<unsigned long long>(done)).c_str(), humanSize(static_cast<unsigned long long>(tot)).c_str());
    ImGui::PopID();
    ++i;
  }
  ImGui::Dummy(ImVec2(0, 20));
}

// ===================================================================== Images
void App::tabImages() {
  const Palette& p = palette();
  ImageSettings& I = draft_.image;
  section("Image engine");
  fPath("sd.exe", I.sdCliPath, "file", "Programs (*.exe)", "*.exe");
  fPath("Save images to", I.outputDir, "folder");
  help("Image generation uses stable-diffusion.cpp. Each image runs in its own process, so all GPU memory returns to the language model afterwards.\nKeep model paths free of non-ASCII characters.");
  fCombo("Active preset", I.active, [&] { std::vector<std::string> v; for (auto& x : I.presets) v.push_back(x.name); return v; }());

  section("Presets");
  ImGui::BeginChild("##imglist", ImVec2(230, 420), true);
  for (int i = 0; i < static_cast<int>(I.presets.size()); ++i)
    if (ImGui::Selectable((I.presets[static_cast<size_t>(i)].name + " (" + I.presets[static_cast<size_t>(i)].kind + ")##p" + std::to_string(i)).c_str(), selImage_ == i)) selImage_ = i;
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("##imgedit", ImVec2(0, 420), false);
  if (selImage_ >= 0 && selImage_ < static_cast<int>(I.presets.size())) {
    ImagePreset& m = I.presets[static_cast<size_t>(selImage_)];
    fText("Name", m.name);
    fCombo("Can do", m.kind, {"generate", "edit", "both"});
    help("generate: text-to-image. edit: instruction-based editing of existing images (FLUX Kontext, Qwen-Image-Edit...). both: the model supports both.");
    fPath("Checkpoint (all-in-one)", m.model, "file", "Models", "*.safetensors;*.ckpt;*.gguf");
    help("SD 1.x / SDXL style single-file models.");
    fPath("Diffusion model", m.diffusionModel, "file", "Models", "*.safetensors;*.gguf");
    help("FLUX, SD3, Z-Image and similar models ship the diffusion model separately from the text encoders and VAE.");
    fPath("VAE", m.vae, "file", "Models", "*.safetensors;*.gguf");
    fPath("CLIP-L", m.clipL, "file", "Models", "*.safetensors;*.gguf");
    fPath("CLIP-G", m.clipG, "file", "Models", "*.safetensors;*.gguf");
    fPath("T5-XXL", m.t5xxl, "file", "Models", "*.safetensors;*.gguf");
    fInt("Default steps", m.steps, 1, 200);
    fDouble("CFG scale", m.cfg, 0.0, 20.0);
    fCombo("Sampler", m.sampler, {"euler_a", "euler", "heun", "dpm2", "dpm++2s_a", "dpm++2m", "dpm++2mv2", "lcm"});
    fInt("Default width", m.width, 256, 2048);
    fInt("Default height", m.height, 256, 2048);
    fCheck("Keep weights in RAM, stream to GPU (fits big models in 16 GB)", m.offloadToCpu);
    fCheck("Tile the VAE (less VRAM for large images)", m.vaeTiling);
    fCheck("Flash attention in the diffusion model", m.flashAttn);
    fInt("CPU threads (0 = auto)", m.threads, 0, 256);
    fText("Extra arguments", m.extraArgs, "e.g. --guidance 3.5");
  } else ImGui::TextColored(p.dim, "Select a preset on the left, or add one from a template.");
  ImGui::EndChild();
  auto add = [&](ImagePreset m) { I.presets.push_back(m); selImage_ = static_cast<int>(I.presets.size()) - 1; if (I.active.empty()) I.active = m.name; dirty_ = true; };
  if (ImGui::Button("+ SD 1.5 / SDXL")) { ImagePreset m; m.name = "sdxl"; m.cfg = 7.0; m.steps = 25; m.offloadToCpu = false; add(m); }
  ImGui::SameLine();
  if (ImGui::Button("+ FLUX.1")) { ImagePreset m; m.name = "flux"; m.sampler = "euler"; m.cfg = 1.0; m.steps = 20; m.extraArgs = "--guidance 3.5"; add(m); }
  ImGui::SameLine();
  if (ImGui::Button("+ FLUX Kontext (edit)")) { ImagePreset m; m.name = "kontext"; m.kind = "edit"; m.sampler = "euler"; m.cfg = 1.0; m.steps = 28; m.extraArgs = "--guidance 2.5"; add(m); }
  ImGui::SameLine();
  if (ImGui::Button("Remove") && selImage_ >= 0 && selImage_ < static_cast<int>(I.presets.size())) { I.presets.erase(I.presets.begin() + selImage_); selImage_ = -1; dirty_ = true; }
  ImGui::Dummy(ImVec2(0, 10));
  ImGui::TextColored(p.dim, "Get the model files from Hugging Face with the downloader in Settings > Models (save in: image).");
}

// ===================================================================== Voice
static std::string keyLabel(int vk) {
  wchar_t buf[64] = {0};
  UINT sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
  LONG lp = static_cast<LONG>(sc << 16) | ((vk >= 0x21 && vk <= 0x2E) ? (1 << 24) : 0);
  if (GetKeyNameTextW(lp, buf, 64) > 0) return narrow(buf);
  return "key " + std::to_string(vk);
}

void App::tabVoice() {
  const Palette& p = palette();
  section("Listening (speech to text)");
  fCombo("Microphone mode", draft_.voice.mode, {"off", "ptt", "handsfree"});
  help("off: type only.\nptt: hold the key to talk, release to send.\nhandsfree: Tony listens continuously and detects when you speak (use the wake word to avoid accidental triggers).");
  label("Push-to-talk key");
  static bool capturing = false;
  if (ImGui::Button(capturing ? "Press a key..." : (keyLabel(draft_.voice.pttVk) + "  (click to change)").c_str(), ImVec2(260, 0))) capturing = true;
  if (capturing) for (int vk = 8; vk < 255; ++vk) if ((GetAsyncKeyState(vk) & 0x8000) && vk != VK_LBUTTON && vk != VK_RBUTTON && vk != VK_MBUTTON) { draft_.voice.pttVk = vk; capturing = false; dirty_ = true; break; }
  {
    std::vector<std::string> devs = {""};
    for (auto& d : inputDevices_) devs.push_back(d);
    fCombo("Input device", draft_.voice.inputDevice, devs);
  }
  fDouble("Sensitivity (hands-free)", draft_.voice.sensitivity, 0.0, 1.0);
  fInt("End of speech after (ms of silence)", draft_.voice.hangMs, 300, 3000);
  fCheck("Require wake word in hands-free mode", draft_.voice.wakeWordEnabled);
  fText("Wake word", draft_.voice.wakeWord, "tony");
  fCheck("Send recognised speech immediately", draft_.voice.autoSend);
  fCheck("Interrupt Tony when I start talking (use headphones)", draft_.voice.bargeIn);
  label("Input level");
  if (voiceRunning_) ImGui::ProgressBar(std::min(1.0f, voice_.level()), ImVec2(260, 10), ""); else ImGui::TextColored(p.dim, "(turn the microphone on and press Apply to see the level)");
  {
    std::lock_guard<std::mutex> l(voiceInMu_);
    if (!voiceError_.empty()) { ImGui::SetCursorPosX(kLabelW); ImGui::TextColored(p.danger, "%s", voiceError_.c_str()); }
  }

  section("Speech recognition model (Whisper)");
  WhisperSettings& W = draft_.whisper;
  fPath("whisper-server.exe", W.exePath, "file", "Programs (*.exe)", "*.exe");
  fPath("Model (.bin)", W.modelPath, "file", "Whisper models (*.bin)", "*.bin");
  fCombo("Language", W.language, {"auto", "en", "pl", "de", "fr", "es", "it", "pt", "nl", "cs", "uk", "ru", "tr", "sv", "ja", "zh", "ko", "ar", "hi"});
  help("auto detects the language of every utterance. Fixing it is faster and avoids mistakes on short phrases.");
  fInt("Threads (0 = auto)", W.threads, 0, 128);
  fCheck("Use the GPU for speech recognition", W.gpu);
  help("Off by default: your CPU transcribes in about a second and the GPU stays free for the language model.");
  label("Quick download");
  struct Q { const char* name; const char* file; };
  for (const Q& q : {Q{"large-v3-turbo q5 (best, ~570 MB)", "ggml-large-v3-turbo-q5_0.bin"}, Q{"small (multilingual, ~470 MB)", "ggml-small.bin"}, Q{"base (fast, ~140 MB)", "ggml-base.bin"}}) {
    if (ImGui::Button(q.name)) {
      startDownload("ggerganov/whisper.cpp", q.file, "stt");
      if (W.modelPath.empty()) W.modelPath = joinPath(joinPath(paths_.modelsDir(), "stt"), q.file);
      dirty_ = true;
    }
    ImGui::SameLine();
  }
  ImGui::NewLine();
  for (auto& d : downloads_) if (d->state == 0 && d->name.find("whisper") != std::string::npos) ImGui::TextColored(p.dim, "Downloading %s: %s", d->name.c_str(), humanSize(static_cast<unsigned long long>(d->done.load())).c_str());

  section("Speaking (text to speech)");
  TtsSettings& T = draft_.tts;
  fCombo("Voice engine", T.engine, {"sapi", "piper", "off"});
  help("sapi: voices built into Windows. Works immediately.\npiper: natural-sounding offline neural voices (download a voice below).");
  fCombo("Speak replies", T.speak, {"voice_only", "always", "off"});
  help("voice_only: read answers aloud only when you spoke to Tony.");
  if (T.engine == "sapi") {
    std::vector<std::string> v = {""};
    for (auto& s : sapiVoices_) v.push_back(s);
    fCombo("Windows voice", T.sapiVoice, v);
    if (sapiVoices_.empty()) ImGui::TextColored(p.dim, "No SAPI voices found. Add voices in Windows Settings > Time & language > Speech.");
  } else if (T.engine == "piper") {
    fPath("piper.exe", T.piperExe, "file", "Programs (*.exe)", "*.exe");
    std::vector<std::string> v = {""};
    for (auto& s : piperVoices_) v.push_back(s);
    fCombo("Installed Piper voice", T.piperVoice, v);
    fPath("...or choose a file", T.piperVoice, "file", "Piper voices (*.onnx)", "*.onnx");
    label("Download a voice");
    struct V { const char* label; const char* lang; const char* region; const char* name; };
    for (const V& v2 : {V{"English (US) Lessac", "en", "en_US", "lessac"}, V{"English (UK) Alan", "en", "en_GB", "alan"}, V{"Deutsch Thorsten", "de", "de_DE", "thorsten"},
                        V{"Polski Gosia", "pl", "pl_PL", "gosia"}, V{"Espanol Davefx", "es", "es_ES", "davefx"}, V{"Francais Siwis", "fr", "fr_FR", "siwis"}}) {
      if (ImGui::Button(v2.label)) {
        std::string base = std::string(v2.lang) + "/" + v2.region + "/" + v2.name + "/medium/" + v2.region + "-" + v2.name + "-medium.onnx";
        startDownload("rhasspy/piper-voices", base, "tts");
        startDownload("rhasspy/piper-voices", base + ".json", "tts");
        T.piperVoice = joinPath(joinPath(paths_.modelsDir(), "tts"), std::string(v2.region) + "-" + v2.name + "-medium.onnx");
        dirty_ = true;
      }
      ImGui::SameLine();
    }
    ImGui::NewLine();
  }
  fDouble("Speed", T.rate, 0.5, 2.0);
  fDouble("Volume", T.volume, 0.0, 1.0);
  label("");
  if (ImGui::Button("Test voice")) { tts_.apply(draft_.tts, paths_); tts_.speak("Hello, I'm Tony. This is how I sound."); }
  ImGui::SameLine();
  if (ImGui::Button("Stop")) tts_.stop();
  std::string e = tts_.lastError();
  if (!e.empty()) ImGui::TextColored(p.danger, "%s", e.c_str());
}

// ===================================================================== Prompt
void App::tabPrompt() {
  const Palette& p = palette();
  PromptSettings& P = draft_.prompt;
  section("System prompt");
  ImGui::TextColored(p.dim, "The base instructions Tony always follows. The date, your PC, folders and tools are added automatically.");
  if (ImGui::InputTextMultiline("##sys", &P.systemPrompt, ImVec2(-1, 230))) dirty_ = true;
  if (ImGui::Button("Reset to default")) { P.systemPrompt = defaultSystemPrompt(); dirty_ = true; }
  ImGui::Spacing();
  fText("Your name", P.userName, "optional");
  fText("Reply language", P.replyLanguage, "empty = same language as you");
  fCheck("Tell Tony about this PC, its folders and enabled tools", P.includeEnvironment);

  section("Custom instructions");
  ImGui::TextColored(p.dim, "Extra standing rules, added after the system prompt. Switch them on and off without deleting.");
  for (size_t i = 0; i < P.custom.size(); ++i) {
    CustomInstruction& c = P.custom[i];
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::Checkbox("##en", &c.enabled)) dirty_ = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    if (ImGui::InputTextWithHint("##title", "Title", &c.title)) dirty_ = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) { P.custom.erase(P.custom.begin() + static_cast<long>(i)); dirty_ = true; ImGui::PopID(); break; }
    if (ImGui::InputTextMultiline("##text", &c.text, ImVec2(-1, 60))) dirty_ = true;
    ImGui::PopID();
  }
  if (ImGui::Button("Add instruction")) { CustomInstruction c; c.id = randomHex(3); c.title = "New instruction"; P.custom.push_back(c); dirty_ = true; }
  ImGui::SameLine();
  if (ImGui::BeginCombo("##examples", "Add an example...", ImGuiComboFlags_WidthFitPreview)) {
    struct Ex { const char* t; const char* x; };
    for (const Ex& e : {Ex{"Brief answers", "Keep answers as short as possible. No preambles or summaries unless asked."},
                        Ex{"Explain commands", "Before running a PowerShell command, explain in one sentence what it does."},
                        Ex{"Coding style", "When writing code, prefer small functions, clear names and comments only for non-obvious reasoning. Run the tests after changes."},
                        Ex{"Cite sources", "When using web results, name the site each fact came from."}})
      if (ImGui::Selectable(e.t)) { CustomInstruction c; c.id = randomHex(3); c.title = e.t; c.text = e.x; P.custom.push_back(c); dirty_ = true; }
    ImGui::EndCombo();
  }
}

// ===================================================================== Tools
void App::tabTools() {
  const Palette& p = palette();
  ToolSettings& T = draft_.tools;
  section("Built-in and custom tools");
  ImGui::TextColored(p.dim, "Turn tools on or off, and choose how each one is approved.");
  if (ImGui::BeginTable("##tools", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 40);
    ImGui::TableSetupColumn("Tool", ImGuiTableColumnFlags_WidthFixed, 150);
    ImGui::TableSetupColumn("What it does");
    ImGui::TableSetupColumn("Approval", ImGuiTableColumnFlags_WidthFixed, 140);
    ImGui::TableHeadersRow();
    for (auto& name : tools_.names()) {
      Tool* t = tools_.find(name);
      if (!t) continue;
      ToolSpec spec = t->spec();
      ImGui::TableNextRow();
      ImGui::PushID(name.c_str());
      ImGui::TableSetColumnIndex(0);
      bool on = T.enabled.count(name) ? T.enabled[name] : t->defaultEnabled();
      if (ImGui::Checkbox("##on", &on)) { T.enabled[name] = on; dirty_ = true; }
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(name.c_str());
      if (!t->defaultEnabled()) { ImGui::SameLine(); ImGui::TextColored(p.warn, "(off by default)"); }
      ImGui::TableSetColumnIndex(2);
      std::string d = spec.description.substr(0, spec.description.find(". ") == std::string::npos ? 110 : std::min<size_t>(110, spec.description.find(". ") + 1));
      ImGui::TextColored(p.dim, "%s", d.c_str());
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", spec.description.c_str());
      ImGui::TableSetColumnIndex(3);
      std::string cur = T.approval.count(name) ? T.approval[name] : "default";
      ImGui::SetNextItemWidth(-1);
      if (ImGui::BeginCombo("##ap", cur.c_str())) {
        for (const char* o : {"default", "ask", "auto", "deny"}) if (ImGui::Selectable(o, cur == o)) { T.approval[name] = o; dirty_ = true; }
        ImGui::EndCombo();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::TextColored(p.dim, "default = the tool decides (e.g. read-only commands run, changes ask).  ask = always ask.  auto = run without asking (dangerous actions and web-tainted chats still ask).  deny = never.");

  section("Custom tools (PowerShell templates)");
  ImGui::TextColored(p.dim, "Teach Tony your own actions. Use {placeholders} for the parameters; each value is safely quoted as a literal string, never executed.");
  ImGui::BeginChild("##ctlist", ImVec2(230, 380), true);
  for (int i = 0; i < static_cast<int>(T.custom.size()); ++i) if (ImGui::Selectable((T.custom[static_cast<size_t>(i)].name + "##c" + std::to_string(i)).c_str(), selCustom_ == i)) selCustom_ = i;
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("##cted", ImVec2(0, 380), false);
  if (selCustom_ >= 0 && selCustom_ < static_cast<int>(T.custom.size())) {
    CustomTool& c = T.custom[static_cast<size_t>(selCustom_)];
    fText("Tool name", c.name, "letters, digits and _");
    for (auto& ch : c.name) if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) ch = '_';
    fText("Description (for the model)", c.description, "when should Tony use this?");
    fMulti("PowerShell command", c.command, 90);
    fPath("Working folder", c.workingDir, "folder");
    fCheck("Ask for approval every time", c.requireApproval);
    fInt("Timeout (s)", c.timeoutSec, 1, 900);
    ImGui::TextColored(p.dim, "Parameters");
    for (size_t i = 0; i < c.params.size(); ++i) {
      CustomToolParam& pr = c.params[i];
      ImGui::PushID(static_cast<int>(i));
      ImGui::SetNextItemWidth(130);
      if (ImGui::InputTextWithHint("##n", "name", &pr.name)) dirty_ = true;
      ImGui::SameLine();
      ImGui::SetNextItemWidth(100);
      if (ImGui::BeginCombo("##t", pr.type.c_str())) { for (const char* t : {"string", "number", "integer", "boolean"}) if (ImGui::Selectable(t, pr.type == t)) { pr.type = t; dirty_ = true; } ImGui::EndCombo(); }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(260);
      if (ImGui::InputTextWithHint("##d", "description", &pr.description)) dirty_ = true;
      ImGui::SameLine();
      if (ImGui::Checkbox("required", &pr.required)) dirty_ = true;
      ImGui::SameLine();
      if (ImGui::SmallButton("x")) { c.params.erase(c.params.begin() + static_cast<long>(i)); dirty_ = true; ImGui::PopID(); break; }
      ImGui::PopID();
    }
    if (ImGui::Button("Add parameter")) { c.params.push_back({"param", "string", "", true}); dirty_ = true; }
    RiskReport rr = classifyPowerShell(c.command);
    if (rr.risk == Risk::Dangerous) ImGui::TextColored(p.danger, "This command looks dangerous (%s). Tony will always ask before running it.", join(rr.reasons, "; ").c_str());
  } else ImGui::TextColored(p.dim, "Select a custom tool on the left, or add one.");
  ImGui::EndChild();
  if (ImGui::Button("Add custom tool")) {
    CustomTool c;
    c.name = "my_tool";
    c.description = "Describe what this does";
    c.command = "Get-ChildItem -Path {folder} | Select-Object -First 20 Name, Length";
    c.params.push_back({"folder", "string", "Folder to list", true});
    T.custom.push_back(c);
    selCustom_ = static_cast<int>(T.custom.size()) - 1;
    dirty_ = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Remove##ct") && selCustom_ >= 0 && selCustom_ < static_cast<int>(T.custom.size())) { T.custom.erase(T.custom.begin() + selCustom_); selCustom_ = -1; dirty_ = true; }
  ImGui::Dummy(ImVec2(0, 20));
}

// ===================================================================== Network
void App::tabNetwork() {
  const Palette& p = palette();
  NetworkSettings& N = draft_.network;
  section("Routing");
  fCombo("Outgoing traffic", N.mode, {"direct", "proxy", "tor"});
  help("Applies to web search, page fetching, downloads and remote model servers. Your local engines (llama-server, Whisper, aria2) are never routed through a proxy.\ndirect: normal connection.\nproxy: through the HTTP/SOCKS proxy below.\ntor: everything through Tor.");
  if (N.mode == "proxy") {
    fText("Proxy URL", N.proxyUrl, "http://host:8080  or  socks5://host:1080");
    fText("Proxy user", N.proxyUser, "optional");
    fText("Proxy password", N.proxyPass, "optional", true);
    ImGui::SetCursorPosX(kLabelW);
    ImGui::TextColored(p.dim, "socks5:// is upgraded to socks5h:// so DNS lookups also go through the proxy.");
  }
  label("");
  if (ImGui::Button("Show my outgoing IP address")) {
    NetworkSettings ns = N;
    runNetOp("ip", [ns] {
      HttpRequest r;
      r.url = "https://api.ipify.org";
      r.timeoutSec = 20;
      std::string why;
      if (!applyNetworkPolicy(ns, r, &why)) return "Blocked: " + why;
      HttpResponse h = httpPerform(r);
      return h.ok() ? "Websites see: " + trim(h.body) : "Failed: " + (h.error.empty() ? "HTTP " + std::to_string(h.status) : h.error);
    });
  }
  if (netOpLabel_ == "ip") { ImGui::SameLine(); ImGui::TextColored(p.dim, "%s", netOpResult_.c_str()); }

  section("VPN");
  ImGui::TextColored(p.dim, "Tony does not bundle a VPN. It can start/stop the one you already use and refuse to go online without it (kill switch).");
  fText("Connect command", N.vpn.connectCmd, "PowerShell, e.g.  rasdial \"My VPN\"   or   wg-quick up wg0");
  fText("Disconnect command", N.vpn.disconnectCmd, "e.g.  rasdial \"My VPN\" /disconnect");
  fText("Adapter name pattern", N.vpn.interfaceMatch, "regex, e.g.  wireguard|proton|mullvad|nord|tap");
  help("Used to detect whether the VPN is up: any connected network adapter whose name or description matches this regular expression counts.");
  fCheck("Block web tools and remote models while the VPN is down (kill switch)", N.vpn.requireForWeb);
  fCheck("Block torrents while the VPN is down", N.vpn.requireForTorrent);
  label("");
  std::string vpnCmdC = N.vpn.connectCmd, vpnCmdD = N.vpn.disconnectCmd, pat = N.vpn.interfaceMatch;
  if (ImGui::Button("Connect") && !vpnCmdC.empty()) runNetOp("vpn", [vpnCmdC] { RunResult r = runPowerShell(vpnCmdC, "", 60, 8000, nullptr); return "exit " + std::to_string(r.exitCode) + ": " + truncateMiddle(trim(sanitizeUtf8(r.output)), 300); });
  ImGui::SameLine();
  if (ImGui::Button("Disconnect") && !vpnCmdD.empty()) runNetOp("vpn", [vpnCmdD] { RunResult r = runPowerShell(vpnCmdD, "", 60, 8000, nullptr); return "exit " + std::to_string(r.exitCode) + ": " + truncateMiddle(trim(sanitizeUtf8(r.output)), 300); });
  ImGui::SameLine();
  if (ImGui::Button("Check status")) runNetOp("vpn", [pat] { return pat.empty() ? std::string("Set an adapter pattern first.") : vpnAdapterUp(pat) ? std::string("VPN adapter is UP.") : std::string("No matching VPN adapter is up."); });
  if (netOpLabel_ == "vpn") { ImGui::SameLine(); ImGui::TextColored(p.dim, "%s", netOpResult_.c_str()); }

  section("Tor and I2P");
  ImGui::TextColored(p.dim, "Needs Tor (tor.exe / Tor Browser) or I2P (i2pd) running on your PC. .onion and .i2p addresses are routed automatically.");
  fCheck("Allow .onion sites (via Tor)", N.tor.enabled);
  fText("Tor SOCKS proxy", N.tor.socks, "socks5h://127.0.0.1:9050  (Tor Browser: 9150)");
  fCheck("Allow .i2p sites (via I2P)", N.i2p.enabled);
  fText("I2P HTTP proxy", N.i2p.proxy, "http://127.0.0.1:4444");
  label("");
  if (ImGui::Button("Test Tor connection")) {
    NetworkSettings ns = N;
    ns.mode = "tor";
    ns.vpn.requireForWeb = false;
    runNetOp("tor", [ns] {
      HttpRequest r;
      r.url = "https://check.torproject.org/api/ip";
      r.timeoutSec = 40;
      std::string why;
      if (!applyNetworkPolicy(ns, r, &why)) return "Blocked: " + why;
      HttpResponse h = httpPerform(r);
      if (!h.error.empty()) return "Failed: " + h.error + " (is Tor running?)";
      json j = json::parse(h.body, nullptr, false);
      return j.is_object() && j.value("IsTor", false) ? "Connected through Tor (exit " + j.value("IP", "") + ")." : std::string("Reached the internet, but not through Tor.");
    });
  }
  if (netOpLabel_ == "tor") { ImGui::SameLine(); ImGui::TextColored(p.dim, "%s", netOpResult_.c_str()); }

  section("Web search");
  fCombo("Search provider", N.search.provider, {"duckduckgo", "searxng", "brave"});
  help("duckduckgo works without setup but may occasionally ask for a human check.\nsearxng: your own or a trusted instance (JSON output must be enabled) - private and reliable.\nbrave: needs a Brave Search API key.");
  if (N.search.provider == "searxng") fText("SearXNG URL", N.search.searxngUrl, "https://searx.example.org");
  if (N.search.provider == "brave") fText("Brave API key", N.search.braveKey, "", true);
  fInt("Results per search", N.search.results, 1, 20);
  fInt("Max characters per page", N.fetchMaxChars, 1000, 100000);
  fText("User agent", N.userAgent);

  section("Torrents (aria2)");
  ImGui::TextColored(p.dim, "The torrent tool is off by default (Settings > Tools). Only download content you have the right to. Peers can see your IP address: bind to your VPN adapter.");
  fPath("aria2c.exe", N.torrent.aria2Path, "file", "Programs (*.exe)", "*.exe");
  fPath("Download folder", N.torrent.downloadDir, "folder");
  fText("Bind to interface or IP", N.torrent.bindInterface, "your VPN adapter name or its IP address");
  fInt("Download limit (KB/s, 0 = none)", N.torrent.maxDownKBs, 0, 1000000);
  fInt("RPC port", N.torrent.rpcPort, 1024, 65000);
  fCheck("Keep seeding after the download finishes", N.torrent.seed);
  ImGui::Dummy(ImVec2(0, 20));
}

// ===================================================================== Security
void App::tabSecurity() {
  const Palette& p = palette();
  SecuritySettings& S = draft_.security;
  section("File access");
  fCheck("Restrict Tony's file tools to the folders below", S.restrictFiles);
  help("When on, read_file / write_file / edit_file / list_dir / search_files / file_op can only touch these folders. Protected locations (Windows, Program Files, credentials, SSH keys, browser profiles...) are blocked even inside them.");
  ImGui::BeginDisabled(!S.restrictFiles);
  for (size_t i = 0; i < S.roots.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    ImGui::SetNextItemWidth(-250);
    if (ImGui::InputText("##path", &S.roots[i].path)) dirty_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Browse")) { std::string r = pickFolder(hwnd_, "Allowed folder"); if (!r.empty()) { S.roots[i].path = r; dirty_ = true; } }
    ImGui::SameLine();
    if (ImGui::Checkbox("write", &S.roots[i].writable)) dirty_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Remove")) { S.roots.erase(S.roots.begin() + static_cast<long>(i)); dirty_ = true; ImGui::PopID(); break; }
    ImGui::PopID();
  }
  if (ImGui::Button("Add folder...")) { std::string r = pickFolder(hwnd_, "Allowed folder"); if (!r.empty()) { S.roots.push_back({r, true}); dirty_ = true; } }
  ImGui::EndDisabled();
  ImGui::Spacing();
  fPath("Workspace folder", S.workspaceDir, "folder");
  help("Relative paths and new files go here. Writes inside the workspace are the only file changes that can run without a prompt (when no external content is in the chat).");
  {
    std::string deny = join(S.denyPatterns, "\n");
    label("Always-blocked patterns");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextMultiline("##deny", &deny, ImVec2(-1, 110))) { S.denyPatterns.clear(); for (auto& l : split(deny, '\n')) if (!trim(l).empty()) S.denyPatterns.push_back(trim(l)); dirty_ = true; }
    ImGui::SetCursorPosX(kLabelW);
    ImGui::TextColored(p.dim, "One per line. A leading ^ anchors to the drive root (e.g. ^/windows/); * is a wildcard.");
    ImGui::SetCursorPosX(kLabelW);
    if (ImGui::SmallButton("Reset to defaults")) { S.denyPatterns = PathGuard::defaultDenyPatterns(); dirty_ = true; }
  }

  section("PowerShell and automation");
  fCombo("PowerShell approval", S.shellApproval, {"smart", "always_ask", "off"});
  help("smart: read-only commands (Get-*, dir, git status...) run automatically, everything else asks, dangerous commands always ask.\nalways_ask: every command asks.\noff: the PowerShell tool and custom tools are disabled.\nNote: PowerShell itself is not limited to the folders above - approval is the protection.");
  fCheck("Always ask before keyboard / mouse control", S.confirmUiInput);
  fCheck("External content in the chat turns off automatic approval", S.taintBlocksAuto);
  help("Web pages, files and screenshots can contain hidden instructions (prompt injection). Once such content is in the conversation, Tony asks before every action that has side effects and cannot be set to \"allow for this chat\".");
  fInt("Command timeout (s)", S.shellTimeoutSec, 5, 900);
  fInt("Max tool output kept (chars)", S.maxToolOutput, 2000, 100000);
  fCheck("Keep an audit log of every tool call", S.audit);
  label("Logs and backups");
  if (ImGui::Button("Open audit log")) { std::string f = joinPath(paths_.dataDir, "audit.log"); if (fileExists(f)) shellOpen(f); }
  ImGui::SameLine();
  if (ImGui::Button("Open file backups")) { std::string d = joinPath(paths_.dataDir, "backups"); ensureDir(d); shellOpen(d); }
  ImGui::Dummy(ImVec2(0, 20));
}

// ===================================================================== About
void App::tabAbout() {
  const Palette& p = palette();
  section("Tony 1.0");
  ImGui::TextWrapped("A local, voice-capable computer assistant. Language, vision, speech recognition and image models all run on this PC through llama.cpp, whisper.cpp and stable-diffusion.cpp.");
  section("This installation");
  ImGui::TextColored(p.dim, "Program folder:  %s", paths_.exeDir.c_str());
  ImGui::TextColored(p.dim, "Data folder:     %s", paths_.dataDir.c_str());
  ImGui::TextColored(p.dim, "Cache / logs:    %s", paths_.cacheDir.c_str());
  ImGui::Spacing();
  if (ImGui::Button("Open logs folder")) { std::string d = joinPath(paths_.cacheDir, "logs"); ensureDir(d); shellOpen(d); }
  ImGui::SameLine();
  if (ImGui::Button("Show llama-server log")) { std::string f = llama_->logPath(); if (fileExists(f)) shellOpen(f); }
  section("Engines");
  struct E { const char* n; const std::string* path; };
  for (const E& e : {E{"llama-server", &live_.llama.exePath}, E{"whisper-server", &live_.whisper.exePath}, E{"sd (images)", &live_.image.sdCliPath}, E{"piper", &live_.tts.piperExe}, E{"aria2c", &live_.network.torrent.aria2Path}})
    ImGui::TextColored(fileExists(*e.path) ? p.ok : p.warn, "%-14s %s", e.n, fileExists(*e.path) ? "found" : "not found (optional unless you use it)");
  section("This PC");
  ImGui::TextUnformatted(systemSummary().c_str());
  section("Safety in a nutshell");
  ImGui::BulletText("Everything that changes your system asks first, showing the exact command, file or URL.");
  ImGui::BulletText("Web and file content is treated as data. If it is in the chat, automatic approvals switch off.");
  ImGui::BulletText("File tools are limited to your chosen folders; secrets and system folders are always blocked.");
  ImGui::BulletText("Local engines listen on 127.0.0.1 only, with a random key created at every start.");
  ImGui::Dummy(ImVec2(0, 20));
}

}  // namespace tony
