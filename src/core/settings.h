// Tony - settings. Everything the Settings window edits lives here and round-trips through JSON.
// Missing keys fall back to the defaults below, so old settings files keep working after upgrades.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "pathguard.h"
#include "types.h"

namespace tony {

// ------------------------------------------------------------------ models
struct ModelEntry {
  std::string name;              // id used in presets and requests, e.g. "gpt-oss-120b"
  std::string role = "chat";     // chat | vision | other (informational + used by pickers)
  std::string path;              // main .gguf
  std::string mmproj;            // optional multimodal projector .gguf
  int ctx = 32768;
  int gpuLayers = -1;            // -1 = auto-fit to free VRAM (llama.cpp --fit), otherwise -ngl N
  int cpuMoe = -1;               // -1 = auto; N = keep expert tensors of the first N layers in system RAM
  std::string flashAttn = "auto";  // auto | on | off
  std::string cacheK = "f16", cacheV = "f16";  // f16 | q8_0 | q4_0 ...
  std::string reasoning = "auto";  // auto | on | off
  int threads = 0;               // 0 = llama.cpp default
  std::string device;            // "" = auto; e.g. "Vulkan0", "SYCL0"
  std::string extraArgs;         // advanced: appended llama-server flags
  double temperature = 0.7, topP = 0.95, minP = 0.05;
  int topK = 40, maxTokens = 4096;
};

struct LlamaSettings {
  std::string mode = "managed";  // managed = Tony runs llama-server | remote = any OpenAI-compatible endpoint
  std::string exePath;           // empty = <app>/engines/llama/llama-server.exe
  int port = 18080;
  std::string gpuPolicy = "swap";   // swap = one model on the GPU at a time | coexist = keep chat + vision resident
  int idleUnloadSec = 0;         // 0 = keep loaded; N = free VRAM after N idle seconds
  int parallel = 1;
  std::string extraArgs;         // global llama-server flags
  std::string remoteUrl = "http://127.0.0.1:11434/v1";
  std::string remoteKey;         // secret
  std::string remoteModel;
  std::vector<ModelEntry> models;
  std::string activeChat;        // ModelEntry.name
  std::string activeVision;      // ModelEntry.name ("" = use the chat model if it is multimodal)
};

struct WhisperSettings {
  std::string exePath;           // empty = <app>/engines/whisper/whisper-server.exe
  std::string modelPath;
  std::string language = "auto";  // auto | en | pl | de ...
  int threads = 0;               // 0 = half of the logical cores
  int port = 18081;
  bool gpu = false;              // CPU by default: keeps all VRAM for the LLM
  std::string extraArgs;
};

struct TtsSettings {
  std::string engine = "sapi";   // sapi (built into Windows) | piper (neural, offline) | off
  std::string speak = "voice_only";  // always | voice_only | off
  std::string sapiVoice;         // token id or display name, "" = system default
  std::string piperExe;          // empty = <app>/tts/piper/piper.exe
  std::string piperVoice;        // path to *.onnx (the .onnx.json must sit next to it)
  double rate = 1.0;             // 0.5 .. 2.0
  double volume = 1.0;           // 0 .. 1
};

struct ImagePreset {
  std::string name = "default";
  std::string kind = "generate";  // generate | edit | both
  std::string model;             // all-in-one checkpoint (SD1.x/SDXL .safetensors/.gguf), or ""
  std::string diffusionModel;    // separate diffusion model (FLUX, Qwen-Image, Z-Image...) or ""
  std::string vae, clipL, clipG, t5xxl;
  std::string extraArgs;         // advanced sd.exe flags, e.g. "--guidance 3.5 --flow-shift 3"
  int steps = 20;
  double cfg = 7.0;
  std::string sampler = "euler_a";
  int width = 1024, height = 1024;
  bool offloadToCpu = true;      // keeps weights in RAM, streams to VRAM: how big models fit in 16 GB
  bool vaeTiling = true;
  bool flashAttn = true;
  int threads = 0;
};

struct ImageSettings {
  std::string sdCliPath;         // empty = <app>/engines/sd/sd.exe
  std::string outputDir;         // empty = <data>/gallery
  std::string active;            // ImagePreset.name
  std::vector<ImagePreset> presets;
};

// ------------------------------------------------------------------ prompt & tools
struct CustomInstruction {
  std::string id, title, text;
  bool enabled = true;
};

struct PromptSettings {
  std::string systemPrompt;      // filled with the built-in default on first run
  std::vector<CustomInstruction> custom;
  bool includeEnvironment = true;
  std::string userName;
  std::string replyLanguage;     // "" = same language as the user
};

struct CustomToolParam {
  std::string name, type = "string", description;
  bool required = true;
};

struct CustomTool {
  std::string name, description;
  std::vector<CustomToolParam> params;
  std::string command;           // PowerShell with {param} placeholders; values are quoted automatically
  std::string workingDir;
  bool requireApproval = true;
  int timeoutSec = 60;
};

struct ToolSettings {
  std::map<std::string, bool> enabled;        // missing = enabled (except tools that are off by default)
  std::map<std::string, std::string> approval;  // tool -> default | ask | auto | deny
  std::vector<CustomTool> custom;
};

// ------------------------------------------------------------------ voice, network, security
struct VoiceSettings {
  std::string mode = "ptt";      // off | ptt (hold key) | handsfree
  int pttVk = 0x75;              // F6
  std::string inputDevice;
  double sensitivity = 0.5;
  int hangMs = 900;
  bool wakeWordEnabled = false;
  std::string wakeWord = "tony";
  bool autoSend = true;
  bool bargeIn = true;
};

struct VpnSettings {
  std::string connectCmd;        // e.g.  rasdial "MyVPN"    (PowerShell)
  std::string disconnectCmd;     // e.g.  rasdial "MyVPN" /disconnect
  std::string interfaceMatch;    // regex on adapter name/description, e.g. "wireguard|proton|mullvad|nord"
  bool requireForWeb = false;    // refuse web tools while no matching adapter is up (kill-switch)
  bool requireForTorrent = true;
};

struct TorSettings { bool enabled = false; std::string socks = "socks5h://127.0.0.1:9050"; };
struct I2pSettings { bool enabled = false; std::string proxy = "http://127.0.0.1:4444"; };

struct SearchSettings {
  std::string provider = "duckduckgo";  // duckduckgo | searxng | brave
  std::string searxngUrl;
  std::string braveKey;          // secret
  int results = 8;
};

struct TorrentSettings {
  std::string aria2Path;         // empty = <app>/tools/aria2c.exe
  std::string downloadDir;       // empty = <workspace>/Downloads
  int rpcPort = 6800;
  std::string bindInterface;     // adapter name or IP to bind to (use your VPN's)
  int maxDownKBs = 0;
  bool seed = false;             // false = stop when the download completes
};

struct NetworkSettings {
  std::string mode = "direct";   // direct | proxy
  std::string proxyUrl;          // http://host:port | socks5h://host:port
  std::string proxyUser;
  std::string proxyPass;         // secret
  VpnSettings vpn;
  TorSettings tor;
  I2pSettings i2p;
  SearchSettings search;
  TorrentSettings torrent;
  std::string userAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Tony/1.0";
  int fetchMaxChars = 20000;
};

struct SecuritySettings {
  bool restrictFiles = true;
  std::vector<FileRoot> roots;   // folders Tony may touch while restrictFiles is on
  std::vector<std::string> denyPatterns;  // always blocked, even inside a root
  std::string workspaceDir;      // default working directory for files/commands
  std::string shellApproval = "smart";   // smart (read-only auto, rest asks) | always_ask | off
  bool confirmUiInput = true;    // always ask before mouse/keyboard automation
  bool taintBlocksAuto = true;   // once web/file content is in context, nothing runs without a prompt
  bool audit = true;             // append every tool call to <data>/audit.log
  int maxToolOutput = 12000;     // chars of a tool result kept in context
  int shellTimeoutSec = 60;
};

struct AgentSettings {
  int maxSteps = 20;             // tool-call rounds per user message
  bool memory = true;            // let Tony save/recall notes between chats
};

struct UiSettings {
  std::string theme = "dark";    // dark | light
  float fontSize = 17.0f;
  bool showReasoning = false;
  bool showToolDetails = true;
  int windowW = 1360, windowH = 860;
};

struct Settings {
  int version = 1;
  UiSettings ui;
  LlamaSettings llama;
  WhisperSettings whisper;
  TtsSettings tts;
  ImageSettings image;
  PromptSettings prompt;
  ToolSettings tools;
  VoiceSettings voice;
  NetworkSettings network;
  SecuritySettings security;
  AgentSettings agent;

  const ModelEntry* chatModel() const;
  const ModelEntry* visionModel() const;  // falls back to the chat model when it has an mmproj
  const ModelEntry* findModel(const std::string& name) const;
  bool toolEnabled(const std::string& name, bool defaultOn = true) const;
  std::string approvalFor(const std::string& tool) const;  // default | ask | auto | deny
};

// Locations the platform layer decides; everything else derives from them.
struct AppPaths {
  std::string exeDir;    // folder containing tony.exe
  std::string dataDir;   // %APPDATA%\Tony  (settings, conversations, audit log, memory)
  std::string cacheDir;  // %LOCALAPPDATA%\Tony
  std::string homeDir, docsDir, desktopDir, downloadsDir;  // user known folders
  std::string modelsDir() const { return exeDir + "\\models"; }
  std::string enginesDir() const { return exeDir + "\\engines"; }
};

std::string defaultSystemPrompt();
void applyFirstRunDefaults(Settings& s, const AppPaths& p);  // fills empty paths/prompt/roots, never overwrites user values

json settingsToJson(const Settings& s);
Settings settingsFromJson(const json& j);

// Secrets (API keys, proxy password) are encrypted at rest by hooks the platform layer installs (Windows DPAPI).
struct SecretCodec {
  std::function<std::string(const std::string&)> protect;    // plaintext -> opaque text
  std::function<std::string(const std::string&)> unprotect;  // opaque text -> plaintext ("" on failure)
};
void setSecretCodec(SecretCodec codec);

bool loadSettings(const std::string& path, Settings& out, std::string* err = nullptr);
bool saveSettings(const std::string& path, const Settings& s, std::string* err = nullptr);  // atomic: temp file + rename

}  // namespace tony
