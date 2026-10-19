#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "app.h"

#include <d3d11.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/strutil.h"
#include "engines/whisper_util.h"
#include "net/http.h"
#include "net/netpolicy.h"
#include "platform/winutil.h"
#include "theme.h"

namespace tony {

namespace {
// Values the audio/STT threads read without touching live_ (which the UI thread may replace while idle).
std::mutex g_cfgMu;
VoiceSettings g_voice;
std::atomic<bool> g_bargeIn{true};
std::mutex g_auditMu;

long long nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
long long unixNow() { return static_cast<long long>(std::time(nullptr)); }
std::string g_systemSummary;
}  // namespace

App::App() {}
App::~App() { shutdown(); }

// ===================================================================== init / shutdown
bool App::init(void* hwnd, ID3D11Device* dev) {
  hwnd_ = hwnd;
  dev_ = dev;
  httpGlobalInit();
  paths_ = detectPaths();
  setSecretCodec({dpapiProtect, dpapiUnprotect});
  setVpnProbe(vpnAdapterUp);
  settingsPath_ = joinPath(paths_.dataDir, "settings.json");
  loadSettings();
  draft_ = live_;
  applyTheme(live_.ui.theme);
  g_systemSummary = systemSummary();

  guard_.configure(live_.security.restrictFiles, live_.security.roots, live_.security.denyPatterns);
  llama_ = std::make_unique<LlamaService>(live_, paths_);
  whisper_ = std::make_unique<WhisperService>(live_, paths_);
  image_ = std::make_unique<ImageService>(live_, paths_, llama_.get());
  endpoint_ = std::make_unique<ModeEndpoint>(live_, llama_.get());
  backend_ = std::make_unique<OpenAiBackend>(live_, *endpoint_);

  svc_.settings = &live_;
  svc_.paths = paths_;
  svc_.guard = &guard_;
  svc_.backend = backend_.get();
  svc_.llama = llama_.get();
  svc_.image = image_.get();
  registerBuiltinTools(tools_, svc_);

  AgentDeps d;
  d.settings = &live_;
  d.backend = backend_.get();
  d.tools = &tools_;
  d.guard = &guard_;
  d.approve = [this](const ApprovalRequest& r) { return requestApproval(r); };
  d.askUser = [this](const std::string& q, const std::vector<std::string>& o) { return requestAnswer(q, o); };
  d.emit = [this](const AgentEvent& e) { std::lock_guard<std::mutex> l(evMu_); events_.push_back(e); };
  d.memoryBlock = [this] { return memoryBlock(svc_); };
  d.audit = [this](const std::string& line) {
    std::lock_guard<std::mutex> l(g_auditMu);
    std::string path = joinPath(paths_.dataDir, "audit.log");
    std::error_code ec;
    if (std::filesystem::exists(u8p(path), ec) && std::filesystem::file_size(u8p(path), ec) > (5u << 20)) std::filesystem::rename(u8p(path), u8p(path + ".1"), ec);
    std::ofstream f(u8p(path), std::ios::app | std::ios::binary);
    f << "[" << nowLocalString() << "] " << line << "\n";
  };
  d.environmentBlock = [this] {
    std::ostringstream o;
    o << "Current time: " << nowLocalString() << "\n" << g_systemSummary;
    o << "Workspace folder (relative paths resolve here): " << workspaceDir(svc_) << "\n";
    if (live_.security.restrictFiles) {
      o << "Folders you may use:";
      for (auto& r : live_.security.roots) o << " " << r.path << (r.writable ? " (read/write)" : " (read-only)") << ";";
      o << "\n";
    } else o << "File access: any folder except protected system and credential locations.\n";
    const NetworkSettings& n = live_.network;
    o << "Network: " << (n.mode == "direct" ? "direct" : n.mode == "proxy" ? "through a proxy" : "through Tor") << "; .onion " << (n.tor.enabled ? "enabled" : "disabled") << "; .i2p "
      << (n.i2p.enabled ? "enabled" : "disabled") << "; web search provider: " << n.search.provider << "\n";
    std::vector<std::string> names;
    for (auto& s : tools_.enabledSpecs(live_)) names.push_back(s.name);
    o << "Enabled tools: " << join(names, ", ") << "\n";
    if (lastInputWasVoice_ && live_.tts.engine != "off") o << "The user is speaking to you by voice and your reply will be read aloud: keep it short, natural and free of markup.\n";
    return o.str();
  };
  agent_ = std::make_unique<Agent>(d);

  tts_.apply(live_.tts, paths_);
  { std::lock_guard<std::mutex> l(g_cfgMu); g_voice = live_.voice; }
  g_bargeIn = live_.voice.bargeIn;
  loadChats();
  if (chats_.empty()) newChat();
  else curIdx_ = 0;

  sttThread_ = std::thread([this] { sttLoop(); });
  statusThread_ = std::thread([this] {
    while (!statusQuit_) {
      std::vector<LlamaService::ModelState> st;
      if (llama_ && llama_->running()) st = llama_->models();
      { std::lock_guard<std::mutex> l(statusMu_); modelStates_ = std::move(st); }
      for (int i = 0; i < 12 && !statusQuit_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
  syncVoice();

  // housekeeping: temporary screenshots and image-job folders older than two days
  for (const char* sub : {"screens", "jobs"}) {
    std::error_code ec;
    std::string dir = joinPath(paths_.cacheDir, sub);
    if (!dirExists(dir)) continue;
    auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(48);
    for (auto it = std::filesystem::directory_iterator(u8p(dir), ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
      if (it->last_write_time(ec) < cutoff) std::filesystem::remove_all(it->path(), ec);
  }
  return true;
}

void App::rememberWindowSize(int w, int h) {
  if (w < 400 || h < 300 || (w == live_.ui.windowW && h == live_.ui.windowH)) return;
  live_.ui.windowW = draft_.ui.windowW = w;
  live_.ui.windowH = draft_.ui.windowH = h;
  saveSettings(settingsPath_, live_, nullptr);
}

void App::shutdown() {
  static bool done = false;
  if (done) return;
  done = true;
  cancel_ = true;
  { std::lock_guard<std::mutex> l(approvalP_.mu); approvalP_.cv.notify_all(); }
  { std::lock_guard<std::mutex> l(askP_.mu); askP_.cv.notify_all(); }
  tts_.stop();
  voice_.stop();
  if (agentThread_.joinable()) agentThread_.join();
  sttQuit_ = true;
  sttCv_.notify_all();
  if (sttThread_.joinable()) sttThread_.join();
  statusQuit_ = true;
  if (statusThread_.joinable()) statusThread_.join();
  if (hfThread_.joinable()) hfThread_.join();
  downloads_.clear();
  if (agent_) { for (auto& c : chats_) if (!c.msgs.empty()) saveChat(c); }
  svc_.aria.reset();
  if (whisper_) whisper_->stop();
  if (llama_) llama_->stop();
  for (auto& kv : textures_) if (kv.second.srv) { kv.second.srv->Release(); }
  textures_.clear();
  httpGlobalCleanup();
}

// ===================================================================== settings
void App::loadSettings() {
  std::string err;
  bool existed = fileExists(settingsPath_);
  if (existed && !tony::loadSettings(settingsPath_, live_, &err)) {
    std::error_code ec;
    std::filesystem::copy_file(u8p(settingsPath_), u8p(settingsPath_ + ".bad"), std::filesystem::copy_options::overwrite_existing, ec);
    settingsError_ = "Your settings file could not be read (" + err + "). A copy was kept as settings.json.bad and defaults were loaded.";
    live_ = Settings();
  }
  applyFirstRunDefaults(live_, paths_);
  if (!existed) saveSettings(settingsPath_, live_, nullptr);
}

bool App::busy() const { return running_ || sttBusy_ || llamaBusyOp_; }

void App::reconfigure() {
  applyTheme(live_.ui.theme);
  ImGui::GetStyle().FontSizeBase = live_.ui.fontSize;
  guard_.configure(live_.security.restrictFiles, live_.security.roots, live_.security.denyPatterns);
  tts_.apply(live_.tts, paths_);
  { std::lock_guard<std::mutex> l(g_cfgMu); g_voice = live_.voice; }
  g_bargeIn = live_.voice.bargeIn;
  voice_.stop();
  voiceRunning_ = false;
  syncVoice();
}

void App::applySettings() {
  if (busy()) return;
  // sanitise model names: unique, router-safe
  auto& models = draft_.llama.models;
  for (size_t i = 0; i < models.size(); ++i) {
    std::string base = models[i].name.empty() ? "model" : models[i].name;
    std::string name = base;
    for (int n = 2;; ++n) {
      bool clash = false;
      for (size_t j = 0; j < i; ++j) if (models[j].name == name) clash = true;
      if (!clash) break;
      name = base + "-" + std::to_string(n);
    }
    if (models[i].name == draft_.llama.activeChat && name != models[i].name) draft_.llama.activeChat = name;
    models[i].name = name;
  }
  if (!draft_.llama.models.empty() && !draft_.findModel(draft_.llama.activeChat)) draft_.llama.activeChat = draft_.llama.models.front().name;
  auto oldCustom = live_.tools.custom;
  live_ = draft_;
  registerCustomTools(tools_, svc_, oldCustom);
  std::string err;
  if (!saveSettings(settingsPath_, live_, &err)) settingsError_ = "Could not save settings: " + err;
  else settingsError_.clear();
  reconfigure();
  dirty_ = false;
}

// ===================================================================== chats
Chat* App::cur() { return curIdx_ >= 0 && curIdx_ < static_cast<int>(chats_.size()) ? &chats_[static_cast<size_t>(curIdx_)] : nullptr; }

void App::loadChats() {
  std::string dir = joinPath(paths_.dataDir, "chats");
  ensureDir(dir);
  std::error_code ec;
  for (auto it = std::filesystem::directory_iterator(u8p(dir), ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    if (it->path().extension() != ".json") continue;
    Chat c;
    if (chatFromJson(json::parse(readTextFile(it->path().u8string(), 64u << 20), nullptr, false), &c)) chats_.push_back(std::move(c));
  }
  std::sort(chats_.begin(), chats_.end(), [](const Chat& a, const Chat& b) { return a.updated > b.updated; });
  if (chats_.size() > 300) chats_.resize(300);
}

void App::saveChat(const Chat& c) {
  std::string dir = joinPath(paths_.dataDir, "chats");
  ensureDir(dir);
  writeTextFile(joinPath(dir, c.id + ".json"), chatToJson(c).dump(1, ' ', false, json::error_handler_t::replace));
}

void App::newChat() {
  if (running_) return;
  if (!chats_.empty() && chats_.front().msgs.empty()) { curIdx_ = 0; agent_->reset(); return; }  // reuse the empty one
  Chat c;
  c.id = randomHex(6);
  c.title = "New chat";
  c.updated = unixNow();
  chats_.insert(chats_.begin(), std::move(c));
  curIdx_ = 0;
  agent_->reset();
  tainted_ = false;
}

void App::deleteChat(int idx) {
  if (running_ || idx < 0 || idx >= static_cast<int>(chats_.size())) return;
  std::error_code ec;
  std::filesystem::remove(u8p(joinPath(joinPath(paths_.dataDir, "chats"), chats_[static_cast<size_t>(idx)].id + ".json")), ec);
  chats_.erase(chats_.begin() + idx);
  if (chats_.empty()) { curIdx_ = -1; newChat(); }
  else curIdx_ = std::min(curIdx_, static_cast<int>(chats_.size()) - 1);
  agent_->reset();
  tainted_ = false;
}

std::vector<Message> App::agentHistory(const Chat& c, size_t upTo) const {
  std::vector<Message> h;
  for (size_t i = 0; i < upTo && i < c.msgs.size(); ++i) {
    const Message& m = c.msgs[i];
    if (m.uiOnly || (m.role != "user" && m.role != "assistant" && m.role != "tool")) continue;
    h.push_back(m);
  }
  return h;
}

// ===================================================================== turns
void App::startTurn(const std::string& textIn, bool fromVoice) {
  if (running_) return;
  Chat* c = cur();
  if (!c) return;
  std::string text = trim(textIn);
  if (text.empty() && attachments_.empty()) return;
  if (text.empty()) text = "Please look at the attached image.";

  std::vector<std::string> imgs;
  for (auto& a : attachments_) imgs.push_back(a.dataUrl);
  Message u;
  u.role = "user";
  u.content = text;
  u.imageDataUrls = imgs;
  bool first = c->msgs.empty();
  c->msgs.push_back(u);
  if (first) c->title = autoTitle(text);
  c->updated = unixNow();
  saveChat(*c);
  agent_->setHistory(agentHistory(*c, c->msgs.size() - 1));
  tainted_ = agent_->tainted();

  attachments_.clear();
  input_.clear();
  streamText_.clear();
  streamReasoning_.clear();
  toolStatus_.clear();
  pendingTool_.clear();
  lastInputWasVoice_ = fromVoice;
  speech_.reset();
  tts_.stop();
  cancel_ = false;
  running_ = true;
  scrollToEnd_ = true;
  stickBottom_ = true;
  if (agentThread_.joinable()) agentThread_.join();
  agentThread_ = std::thread([this, text, imgs] {
    try { agent_->runTurn(text, imgs, cancel_); }
    catch (const std::exception& e) {
      AgentEvent ev; ev.type = AgentEvent::Type::Error; ev.text = std::string("Internal error: ") + e.what();
      std::lock_guard<std::mutex> l(evMu_); events_.push_back(ev);
      AgentEvent done; done.type = AgentEvent::Type::TurnDone; events_.push_back(done);
    }
    running_ = false;
  });
}

void App::stopTurn() {
  cancel_ = true;
  tts_.stop();
  { std::lock_guard<std::mutex> l(approvalP_.mu); approvalP_.cv.notify_all(); }
  { std::lock_guard<std::mutex> l(askP_.mu); askP_.cv.notify_all(); }
}

void App::drainEvents() {
  std::deque<AgentEvent> local;
  { std::lock_guard<std::mutex> l(evMu_); local.swap(events_); }
  for (auto& e : local) onEvent(e);
}

void App::onEvent(const AgentEvent& e) {
  Chat* c = cur();
  auto speakOn = [&] { return live_.tts.engine != "off" && (live_.tts.speak == "always" || (live_.tts.speak == "voice_only" && lastInputWasVoice_)); };
  auto push = [&](Message m) { if (c) { c->msgs.push_back(std::move(m)); c->updated = unixNow(); } scrollToEnd_ = stickBottom_; };
  switch (e.type) {
    case AgentEvent::Type::Delta:
      streamText_ += e.text;
      if (speakOn()) for (auto& s : speech_.push(e.text)) tts_.speak(s);
      scrollToEnd_ = stickBottom_;
      break;
    case AgentEvent::Type::ReasoningDelta: streamReasoning_ += e.text; scrollToEnd_ = stickBottom_; break;
    case AgentEvent::Type::AssistantDone:
      streamText_.clear();
      streamReasoning_.clear();
      push(e.message);
      if (speakOn()) for (auto& s : speech_.flush()) tts_.speak(s);
      break;
    case AgentEvent::Type::ToolStart: pendingTool_ = e.tool; pendingToolArgs_ = e.args; toolStatus_.clear(); scrollToEnd_ = stickBottom_; break;
    case AgentEvent::Type::ToolProgress: toolStatus_ = e.text; break;
    case AgentEvent::Type::ToolDone: pendingTool_.clear(); toolStatus_.clear(); push(e.message); break;
    case AgentEvent::Type::Notice: { Message m; m.role = "notice"; m.content = e.text; m.uiOnly = true; push(m); break; }
    case AgentEvent::Type::Error: { Message m; m.role = "error"; m.content = e.text; m.uiOnly = true; m.isError = true; push(m); break; }
    case AgentEvent::Type::TaintChanged: tainted_ = e.text == "1"; break;
    case AgentEvent::Type::TurnDone:
      pendingTool_.clear();
      toolStatus_.clear();
      if (speakOn()) for (auto& s : speech_.flush()) tts_.speak(s);
      if (c) saveChat(*c);
      scrollToEnd_ = stickBottom_;
      break;
  }
}

// ===================================================================== approvals
ApprovalDecision App::requestApproval(const ApprovalRequest& r) {
  if (GetForegroundWindow() != static_cast<HWND>(hwnd_)) { FLASHWINFO f{sizeof(f), static_cast<HWND>(hwnd_), FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0}; FlashWindowEx(&f); }
  if (lastInputWasVoice_ && live_.tts.engine != "off") tts_.speak("I need your approval to " + lower(r.summary.empty() ? r.tool : r.summary) + ".");
  std::unique_lock<std::mutex> lock(approvalP_.mu);
  approvalReq_ = r;
  approvalP_.active = true;
  approvalP_.answered = false;
  approvalP_.cv.wait(lock, [&] { return approvalP_.answered || cancel_.load(); });
  bool ok = approvalP_.answered;
  approvalP_.active = false;
  return ok ? approvalDecision_ : ApprovalDecision::Deny;
}

std::string App::requestAnswer(const std::string& q, const std::vector<std::string>& options) {
  if (lastInputWasVoice_ && live_.tts.engine != "off") tts_.speak(q);
  std::unique_lock<std::mutex> lock(askP_.mu);
  askQuestion_ = q;
  askOptions_ = options;
  askInput_.clear();
  askAnswer_.clear();
  askP_.active = true;
  askP_.answered = false;
  askP_.cv.wait(lock, [&] { return askP_.answered || cancel_.load(); });
  askP_.active = false;
  return askP_.answered ? askAnswer_ : std::string();
}

// ===================================================================== voice
void App::syncVoice() {
  const VoiceSettings& v = live_.voice;
  if (v.mode == "off") { if (voiceRunning_) { voice_.stop(); voiceRunning_ = false; } return; }
  if (voiceRunning_) { voice_.apply(v); return; }
  std::string err;
  bool ok = voice_.start(
      v,
      [this](std::vector<float>&& pcm) {
        { std::lock_guard<std::mutex> l(sttMu_); if (sttQueue_.size() < 4) sttQueue_.push_back(std::move(pcm)); }
        sttCv_.notify_one();
      },
      [this] { if (g_bargeIn.load()) tts_.stop(); }, &err);
  voiceRunning_ = ok;
  { std::lock_guard<std::mutex> l(voiceInMu_); voiceError_ = ok ? "" : err; }
}

void App::sttLoop() {
  while (!sttQuit_) {
    std::vector<float> pcm;
    {
      std::unique_lock<std::mutex> l(sttMu_);
      sttCv_.wait(l, [&] { return sttQuit_ || !sttQueue_.empty(); });
      if (sttQuit_) break;
      pcm = std::move(sttQueue_.front());
      sttQueue_.pop_front();
      sttBusy_ = true;
    }
    std::string text, err;
    bool ok = whisper_->transcribe(pcm, &text, &err, &sttQuit_);
    VoiceSettings v;
    { std::lock_guard<std::mutex> l(g_cfgMu); v = g_voice; }
    if (!ok) { std::lock_guard<std::mutex> l(voiceInMu_); voiceError_ = err; }
    else if (!text.empty()) {
      { std::lock_guard<std::mutex> l(voiceInMu_); voiceError_.clear(); }
      bool accept = true;
      if (v.mode == "handsfree" && v.wakeWordEnabled) {
        std::string rest;
        if (matchWakeWord(text, v.wakeWord, &rest)) {
          text = rest;
          wakeUntilMs_ = text.empty() ? nowMs() + 10000 : 0;  // "Hey Tony." alone opens a 10 s listening window
          accept = !text.empty();
        } else accept = nowMs() < wakeUntilMs_.load();
        if (accept) wakeUntilMs_ = 0;
      }
      if (accept) { std::lock_guard<std::mutex> l(voiceInMu_); voiceInbox_.push_back(text); }
    }
    sttBusy_ = false;
  }
}

// ===================================================================== downloads
void App::startDownload(const std::string& repo, const std::string& file, const std::string& destSub) {
  auto d = std::make_unique<Download>();
  std::string base = file.substr(file.find_last_of('/') == std::string::npos ? 0 : file.find_last_of('/') + 1);
  d->name = repo + " / " + base;
  d->url = hfFileUrl(repo, file);
  std::string dir = joinPath(paths_.modelsDir(), destSub);
  ensureDir(dir);
  d->dest = joinPath(dir, base);
  Download* raw = d.get();
  NetworkSettings ns = live_.network;
  raw->th = std::thread([raw, ns] {
    HttpRequest r;
    r.url = raw->url;
    std::string blocked, err;
    if (!applyNetworkPolicy(ns, r, &blocked)) { raw->error = blocked; raw->state = 2; return; }
    bool ok = httpDownload(r, raw->dest, [raw](double done, double total) { raw->done = done; raw->total = total; }, &raw->cancel, &err);
    if (ok) raw->state = 1;
    else { raw->error = err; raw->state = err == "cancelled" ? 3 : 2; }
  });
  downloads_.push_back(std::move(d));
}

void App::hfBrowse() {
  if (hfLoading_) return;
  if (hfThread_.joinable()) hfThread_.join();
  hfLoading_ = true;
  std::string repo = trim(hfRepo_), sub = hfPath_;
  NetworkSettings ns = live_.network;
  hfThread_ = std::thread([this, repo, sub, ns] {
    std::vector<HfFile> files;
    std::string err;
    bool ok = hfList(repo, sub, ns, &files, &err);
    { std::lock_guard<std::mutex> l(hfMu_); hfFiles_ = ok ? files : std::vector<HfFile>(); hfError_ = ok ? "" : err; }
    hfLoading_ = false;
  });
}

// ===================================================================== frame
void App::frame() {
  ++frameNo_;
  drainEvents();

  // voice results -> chat
  std::string spoken;
  { std::lock_guard<std::mutex> l(voiceInMu_); if (!voiceInbox_.empty() && !running_ && !approvalP_.active) { spoken = voiceInbox_.front(); voiceInbox_.pop_front(); } }
  if (!spoken.empty()) {
    if (live_.voice.autoSend) startTurn(spoken, true);
    else input_ += (input_.empty() ? "" : " ") + spoken;
  }
  voice_.setMuted(live_.voice.mode == "handsfree" && !live_.voice.bargeIn && tts_.speaking());

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##main", nullptr, wf);
  ImGui::PopStyleVar();
  const float sideW = 270.0f * uiScale();
  drawSidebar(sideW, vp->WorkSize.y);
  ImGui::SameLine(0, 0);
  drawChatWindow(sideW, vp->WorkSize.x - sideW, vp->WorkSize.y);
  ImGui::End();

  drawModals();
  if (showSettings_) drawSettings();
}

}  // namespace tony
