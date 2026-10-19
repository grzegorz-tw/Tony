// Tony - the application: owns settings, services, the agent thread, voice pipeline and all UI state.
// Implemented across app.cpp (logic), ui_chat.cpp (chat window) and ui_settings.cpp (settings window).
#pragma once
#include <imgui.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/agent.h"
#include "core/chatstore.h"
#include "core/settings.h"
#include "core/strutil.h"
#include "engines/hf.h"
#include "engines/image_service.h"
#include "engines/llama_preset.h"
#include "engines/llama_service.h"
#include "engines/openai_backend.h"
#include "engines/whisper_service.h"
#include "platform/audio.h"
#include "platform/screen.h"
#include "tools/tools.h"

struct ID3D11Device;
struct ID3D11ShaderResourceView;

namespace tony {

struct Texture { ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0; unsigned long long lastUse = 0; };

struct Attachment { std::string name, dataUrl; };

struct Download {
  std::string name, url, dest, error;
  std::atomic<double> done{0}, total{0};
  std::atomic<int> state{0};  // 0 running, 1 done, 2 failed, 3 cancelled
  std::atomic<bool> cancel{false};
  std::thread th;
  ~Download() { cancel = true; if (th.joinable()) th.join(); }
};

class App {
 public:
  App();
  ~App();
  bool init(void* hwnd, ID3D11Device* dev);
  void frame();
  void shutdown();
  bool wantsQuit() const { return quit_; }
  void setMonoFont(ImFont* f) { mono_ = f; }
  void rememberWindowSize(int w, int h);
  int savedWidth() const { return live_.ui.windowW; }
  int savedHeight() const { return live_.ui.windowH; }
  float savedFontSize() const { return live_.ui.fontSize; }
  std::string savedTheme() const { return live_.ui.theme; }

 private:
  // ---------------------------------------------------------------- core (app.cpp)
  void* hwnd_ = nullptr;
  ID3D11Device* dev_ = nullptr;
  bool quit_ = false;
  AppPaths paths_;
  Settings live_, draft_;
  bool dirty_ = false;
  std::string settingsError_, settingsPath_;
  PathGuard guard_;
  ToolRegistry tools_;
  Services svc_;
  std::unique_ptr<LlamaService> llama_;
  std::unique_ptr<WhisperService> whisper_;
  std::unique_ptr<ImageService> image_;
  std::unique_ptr<ModeEndpoint> endpoint_;
  std::unique_ptr<OpenAiBackend> backend_;
  std::unique_ptr<Agent> agent_;
  VoiceInput voice_;
  TtsService tts_;

  void loadSettings();
  void applySettings();            // draft_ -> live_, save, reconfigure services
  void reconfigure();
  bool busy() const;               // anything running that reads settings_
  void loadChats();
  void saveChat(const Chat& c);
  void newChat();
  void deleteChat(int idx);
  Chat* cur();
  std::vector<Message> agentHistory(const Chat& c, size_t upTo) const;

  // agent thread + events
  std::thread agentThread_;
  std::atomic<bool> running_{false}, cancel_{false};
  std::mutex evMu_;
  std::deque<AgentEvent> events_;
  std::string streamText_, streamReasoning_, toolStatus_, pendingTool_, pendingToolArgs_;
  bool tainted_ = false;
  bool lastInputWasVoice_ = false;
  SentenceStream speech_;
  void startTurn(const std::string& text, bool fromVoice);
  void stopTurn();
  void drainEvents();
  void onEvent(const AgentEvent& e);

  // approvals / questions raised from the agent thread
  struct Pending {
    std::mutex mu;
    std::condition_variable cv;
    bool active = false, answered = false;
  };
  Pending approvalP_, askP_;
  ApprovalRequest approvalReq_;
  ApprovalDecision approvalDecision_ = ApprovalDecision::Deny;
  std::string askQuestion_, askAnswer_, askInput_;
  std::vector<std::string> askOptions_;
  ApprovalDecision requestApproval(const ApprovalRequest& r);
  std::string requestAnswer(const std::string& q, const std::vector<std::string>& options);

  // voice pipeline
  std::thread sttThread_;
  std::mutex sttMu_;
  std::condition_variable sttCv_;
  std::deque<std::vector<float>> sttQueue_;
  std::mutex voiceInMu_;
  std::deque<std::string> voiceInbox_;
  std::atomic<bool> sttQuit_{false}, sttBusy_{false};
  std::atomic<long long> wakeUntilMs_{0};
  std::string voiceError_;
  bool voiceRunning_ = false;
  void syncVoice();
  void sttLoop();

  // engine status polling
  std::thread statusThread_;
  std::mutex statusMu_;
  std::vector<LlamaService::ModelState> modelStates_;
  std::atomic<bool> statusQuit_{false}, llamaBusyOp_{false};
  std::string llamaOpMessage_;

  // downloads / Hugging Face browser
  std::vector<std::unique_ptr<Download>> downloads_;
  std::mutex hfMu_;
  std::vector<HfFile> hfFiles_;
  std::string hfRepo_ = "ggml-org/gpt-oss-20b-GGUF", hfPath_, hfError_, hfDest_ = "llm";
  std::atomic<bool> hfLoading_{false};
  std::thread hfThread_;
  void startDownload(const std::string& repo, const std::string& file, const std::string& destDir);
  void hfBrowse();

  // ---------------------------------------------------------------- chat view (ui_chat.cpp)
  std::vector<Chat> chats_;
  int curIdx_ = -1;
  std::string input_;
  std::vector<Attachment> attachments_;
  bool scrollToEnd_ = false, stickBottom_ = true;
  int renameIdx_ = -1;
  std::string renameBuf_;
  std::map<std::string, Texture> textures_;
  unsigned long long frameNo_ = 0;
  ImFont* mono_ = nullptr;
  void drawChatWindow(float x, float w, float h);
  void drawSidebar(float w, float h);
  void drawTopBar();
  void drawMessages(Chat& c);
  void drawMessage(Chat& c, size_t i);
  void drawMarkdown(const std::string& text, float wrap);
  void drawCodeBlock(const std::string& code, int id);
  void drawImages(const std::vector<std::string>& paths);
  void drawComposer(float w);
  void drawOnboarding();
  void drawModals();
  const Texture* texture(const std::string& path);
  const Texture* textureFromBitmap(const std::string& key, const Bitmap& b);
  void attachImageFile();
  void attachScreenshot();
  void send();
  std::string chipStatus(const char* what) const;

  // ---------------------------------------------------------------- settings view (ui_settings.cpp)
  bool showSettings_ = false;
  int settingsTab_ = 0;
  int selModel_ = -1, selImage_ = -1, selInstr_ = -1, selCustom_ = -1, selRoot_ = -1;
  std::string testMsg_;
  std::future<std::string> netOp_;
  std::string netOpResult_, netOpLabel_;
  void runNetOp(const std::string& label, std::function<std::string()> fn);
  std::vector<std::string> sapiVoices_, piperVoices_, inputDevices_;
  bool voicesLoaded_ = false;
  void drawSettings();
  void tabGeneral();
  void tabModels();
  void tabImages();
  void tabVoice();
  void tabPrompt();
  void tabTools();
  void tabNetwork();
  void tabSecurity();
  void tabAbout();
  // widgets (set dirty_ when changed)
  bool fText(const char* label, std::string& v, const char* hint = nullptr, bool password = false);
  bool fPath(const char* label, std::string& v, const char* kind, const char* filterName = nullptr, const char* filterSpec = nullptr);
  bool fCombo(const char* label, std::string& v, const std::vector<std::string>& options);
  bool fCheck(const char* label, bool& v);
  bool fInt(const char* label, int& v, int lo, int hi);
  bool fDouble(const char* label, double& v, double lo, double hi, const char* fmt = "%.2f");
  bool fMulti(const char* label, std::string& v, float height);
  void help(const char* text);
  void section(const char* title);
  bool fileOk(const std::string& p) const;
  void setThemeLive(const std::string& t);
};

}  // namespace tony
