#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sapi.h>

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "audio.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "core/vad.h"
#include "process.h"
#include "winutil.h"
#include "core/strutil.h"

namespace tony {

// ===================================================================== VoiceInput
struct VoiceInput::Impl {
  ma_context ctx{};
  ma_device dev{};
  bool ctxOk = false, devOk = false;
  ma_device_id devId{};

  std::mutex mu;  // guards settings + callbacks
  VoiceSettings vs;
  SegmentFn onSegment;
  std::function<void()> onSpeechStart;
  EnergyVad vad{16000};

  std::atomic<bool> muted{false}, capturing{false};
  std::atomic<float> level{0};

  // audio-thread state
  std::deque<float> preroll;
  std::vector<float> seg;
  bool recording = false, pttWasDown = false;
  bool usingPtt = false;

  static constexpr size_t kPreroll = 16000 * 4 / 10;   // 0.4 s kept before the trigger so the first word is not clipped
  static constexpr size_t kMaxSeg = 16000 * 45;
  static constexpr size_t kMinSeg = 16000 * 3 / 10;

  void finish() {
    recording = false;
    capturing = false;
    SegmentFn cb;
    { std::lock_guard<std::mutex> l(mu); cb = onSegment; }
    if (seg.size() >= kMinSeg && cb) cb(std::move(seg));
    seg.clear();
  }
  void begin() {
    recording = true;
    capturing = true;
    seg.assign(preroll.begin(), preroll.end());
    std::function<void()> cb;
    { std::lock_guard<std::mutex> l(mu); cb = onSpeechStart; }
    if (cb) cb();
  }

  void process(const float* in, size_t n) {
    VoiceSettings v;
    { std::lock_guard<std::mutex> l(mu); v = vs; }

    double acc = 0;
    for (size_t i = 0; i < n; ++i) acc += static_cast<double>(in[i]) * in[i];
    float rms = n ? static_cast<float>(std::sqrt(acc / n)) : 0.0f;
    level = level.load() * 0.8f + std::min(1.0f, rms * 6.0f) * 0.2f;

    if (muted.load() || v.mode == "off") {
      if (recording) { seg.clear(); recording = false; capturing = false; }
      vad.reset();
      return;
    }

    if (v.mode == "ptt") {
      bool down = (GetAsyncKeyState(v.pttVk) & 0x8000) != 0;
      if (down && !pttWasDown) begin();
      if (recording) {
        seg.insert(seg.end(), in, in + n);
        if (!down || seg.size() > kMaxSeg) finish();
      }
      pttWasDown = down;
    } else {  // handsfree
      vad.setSensitivity(static_cast<float>(v.sensitivity));
      vad.setHangMs(v.hangMs);
      auto ev = vad.process(in, n);
      if (ev == EnergyVad::Event::SpeechStart && !recording) begin();
      if (recording) {
        if (!(ev == EnergyVad::Event::SpeechStart)) seg.insert(seg.end(), in, in + n);
        if (ev == EnergyVad::Event::SpeechEnd || seg.size() > kMaxSeg) finish();
      }
    }
    if (!recording) {
      preroll.insert(preroll.end(), in, in + n);
      while (preroll.size() > kPreroll) preroll.pop_front();
    }
  }

  static void dataCb(ma_device* d, void*, const void* in, ma_uint32 frames) {
    if (in) static_cast<Impl*>(d->pUserData)->process(static_cast<const float*>(in), frames);
  }
};

VoiceInput::VoiceInput() : p_(new Impl) {}
VoiceInput::~VoiceInput() { stop(); }

std::vector<std::string> VoiceInput::devices() {
  std::vector<std::string> out;
  ma_context c;
  if (ma_context_init(nullptr, 0, nullptr, &c) != MA_SUCCESS) return out;
  ma_device_info* caps = nullptr;
  ma_uint32 nCaps = 0;
  ma_device_info* play = nullptr;
  ma_uint32 nPlay = 0;
  if (ma_context_get_devices(&c, &play, &nPlay, &caps, &nCaps) == MA_SUCCESS)
    for (ma_uint32 i = 0; i < nCaps; ++i) out.push_back(caps[i].name);
  ma_context_uninit(&c);
  return out;
}

bool VoiceInput::start(const VoiceSettings& v, SegmentFn onSegment, std::function<void()> onSpeechStart, std::string* err) {
  stop();
  Impl& s = *p_;
  { std::lock_guard<std::mutex> l(s.mu); s.vs = v; s.onSegment = std::move(onSegment); s.onSpeechStart = std::move(onSpeechStart); }
  if (ma_context_init(nullptr, 0, nullptr, &s.ctx) != MA_SUCCESS) { if (err) *err = "audio system could not be initialised"; return false; }
  s.ctxOk = true;

  ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
  cfg.capture.format = ma_format_f32;
  cfg.capture.channels = 1;
  cfg.sampleRate = 16000;  // miniaudio resamples from the device's native rate
  cfg.dataCallback = Impl::dataCb;
  cfg.pUserData = &s;
  if (!v.inputDevice.empty()) {
    ma_device_info* caps = nullptr; ma_uint32 nCaps = 0; ma_device_info* play = nullptr; ma_uint32 nPlay = 0;
    if (ma_context_get_devices(&s.ctx, &play, &nPlay, &caps, &nCaps) == MA_SUCCESS)
      for (ma_uint32 i = 0; i < nCaps; ++i)
        if (v.inputDevice == caps[i].name) { s.devId = caps[i].id; cfg.capture.pDeviceID = &s.devId; break; }
  }
  if (ma_device_init(&s.ctx, &cfg, &s.dev) != MA_SUCCESS) { if (err) *err = "cannot open the microphone"; ma_context_uninit(&s.ctx); s.ctxOk = false; return false; }
  s.devOk = true;
  s.preroll.clear(); s.seg.clear(); s.recording = false; s.pttWasDown = false; s.vad.reset();
  if (ma_device_start(&s.dev) != MA_SUCCESS) { if (err) *err = "cannot start recording"; stop(); return false; }
  return true;
}

void VoiceInput::stop() {
  Impl& s = *p_;
  if (s.devOk) { ma_device_uninit(&s.dev); s.devOk = false; }
  if (s.ctxOk) { ma_context_uninit(&s.ctx); s.ctxOk = false; }
  s.capturing = false;
  s.level = 0;
}

void VoiceInput::apply(const VoiceSettings& v) { std::lock_guard<std::mutex> l(p_->mu); p_->vs = v; }
void VoiceInput::setMuted(bool m) { p_->muted = m; }
bool VoiceInput::active() const { return p_->devOk; }
bool VoiceInput::capturing() const { return p_->capturing; }
float VoiceInput::level() const { return p_->level; }

// ===================================================================== PCM player (for Piper)
namespace {
class PcmPlayer {
 public:
  ~PcmPlayer() { close(); }
  bool open(int rate) {
    if (devOk_ && rate_ == rate) return true;
    close();
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_s16;
    cfg.playback.channels = 1;
    cfg.sampleRate = static_cast<ma_uint32>(rate);
    cfg.dataCallback = &PcmPlayer::cb;
    cfg.pUserData = this;
    if (ma_device_init(nullptr, &cfg, &dev_) != MA_SUCCESS) return false;
    devOk_ = true;
    rate_ = rate;
    return ma_device_start(&dev_) == MA_SUCCESS;
  }
  void close() { if (devOk_) { ma_device_uninit(&dev_); devOk_ = false; } clear(); }
  void feed(const int16_t* s, size_t n) { std::lock_guard<std::mutex> l(mu_); buf_.insert(buf_.end(), s, s + n); }
  void clear() { std::lock_guard<std::mutex> l(mu_); buf_.clear(); }
  size_t buffered() { std::lock_guard<std::mutex> l(mu_); return buf_.size(); }
  void setVolume(float v) { vol_ = std::max(0.0f, std::min(1.0f, v)); }

 private:
  static void cb(ma_device* d, void* out, const void*, ma_uint32 frames) {
    auto* p = static_cast<PcmPlayer*>(d->pUserData);
    auto* o = static_cast<int16_t*>(out);
    std::lock_guard<std::mutex> l(p->mu_);
    for (ma_uint32 i = 0; i < frames; ++i) {
      if (p->buf_.empty()) { o[i] = 0; continue; }
      o[i] = static_cast<int16_t>(p->buf_.front() * p->vol_);
      p->buf_.pop_front();
    }
  }
  ma_device dev_{};
  bool devOk_ = false;
  int rate_ = 0;
  std::mutex mu_;
  std::deque<int16_t> buf_;
  std::atomic<float> vol_{1.0f};
};

std::string xmlEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) { case '&': o += "&amp;"; break; case '<': o += "&lt;"; break; case '>': o += "&gt;"; break; case '"': o += "&quot;"; break; default: o += c; }
  }
  return o;
}

// Reads a PCM16 mono WAV written by Piper; returns false if it is not one.
bool parseWav(const std::string& w, int* rate, std::vector<int16_t>* pcm) {
  if (w.size() < 44 || w.compare(0, 4, "RIFF") != 0 || w.compare(8, 4, "WAVE") != 0) return false;
  size_t pos = 12;
  *rate = 0;
  while (pos + 8 <= w.size()) {
    uint32_t sz;
    std::memcpy(&sz, &w[pos + 4], 4);
    std::string id = w.substr(pos, 4);
    if (id == "fmt " && pos + 8 + 16 <= w.size()) { uint32_t sr; std::memcpy(&sr, &w[pos + 12], 4); *rate = static_cast<int>(sr); }
    if (id == "data") {
      size_t avail = std::min<size_t>(sz, w.size() - pos - 8);
      pcm->resize(avail / 2);
      std::memcpy(pcm->data(), &w[pos + 8], (avail / 2) * 2);
      return *rate > 0;
    }
    pos += 8 + sz + (sz & 1);
  }
  return false;
}
}  // namespace

// ===================================================================== TtsService
struct TtsService::Impl {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::string> queue;
  TtsSettings ts;
  AppPaths paths;
  std::thread worker;
  bool quit = false;
  std::atomic<bool> busy{false};
  std::atomic<bool> abortSynth{false};
  std::atomic<unsigned> generation{0};
  std::string error;
  ISpVoice* voice = nullptr;  // owned by the worker thread; purged from stop()
  std::mutex voiceMu;
  PcmPlayer player;

  void run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
      std::string text;
      unsigned gen;
      TtsSettings cur;
      {
        std::unique_lock<std::mutex> l(mu);
        cv.wait(l, [&] { return quit || !queue.empty(); });
        if (quit) break;
        text = std::move(queue.front());
        queue.pop_front();
        gen = generation.load();
        cur = ts;
        busy = true;
      }
      if (cur.engine == "sapi") speakSapi(text, cur, gen);
      else if (cur.engine == "piper") speakPiper(text, cur, gen);
      busy = false;
    }
    { std::lock_guard<std::mutex> l(voiceMu); if (voice) { voice->Release(); voice = nullptr; } }
    CoUninitialize();
  }

  void setError(const std::string& e) { std::lock_guard<std::mutex> l(mu); error = e; }

  void speakSapi(const std::string& text, const TtsSettings& t, unsigned gen) {
    {
      std::lock_guard<std::mutex> l(voiceMu);
      if (!voice) {
        CLSID clsid;
        if (FAILED(CLSIDFromProgID(L"SAPI.SpVoice", &clsid)) || FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_ALL, __uuidof(ISpVoice), reinterpret_cast<void**>(&voice)))) {
          setError("Windows speech (SAPI) is not available");
          return;
        }
      }
      voice->SetRate(std::max(-10, std::min(10, static_cast<int>(std::lround((t.rate - 1.0) * 10)))));
      voice->SetVolume(static_cast<USHORT>(std::max(0.0, std::min(1.0, t.volume)) * 100));
    }
    if (gen != generation.load()) return;
    std::string xml = t.sapiVoice.empty() ? xmlEscape(text) : "<voice required=\"Name=" + xmlEscape(t.sapiVoice) + "\">" + xmlEscape(text) + "</voice>";
    voice->Speak(widen(xml).c_str(), SPF_IS_XML, nullptr);  // blocks until spoken or purged by stop()
  }

  void speakPiper(const std::string& text, const TtsSettings& t, unsigned gen) {
    if (!fileExists(t.piperExe) || !fileExists(t.piperVoice)) { setError("Piper or its voice file was not found (Settings > Voice)."); return; }
    std::string wav = tempFilePath(".wav");
    std::vector<std::string> args = {"--model", t.piperVoice, "--output_file", wav, "--quiet", "--length_scale", std::to_string(1.0 / std::max(0.3, t.rate))};
    abortSynth = false;
    RunResult rr = runProcess(t.piperExe, args, parentDir(t.piperExe), 60000, 4096, &abortSynth, {}, text + "\n");
    std::string data = readTextFile(wav, 256u << 20);
    std::error_code ec;
    std::filesystem::remove(u8p(wav), ec);
    if (gen != generation.load()) return;  // stopped while synthesising
    int rate = 0;
    std::vector<int16_t> pcm;
    if (!rr.started || !parseWav(data, &rate, &pcm)) { setError("Piper produced no audio" + (rr.error.empty() ? std::string() : ": " + rr.error)); return; }
    player.setVolume(static_cast<float>(t.volume));
    if (!player.open(rate)) { setError("cannot open the audio output"); return; }
    player.feed(pcm.data(), pcm.size());
  }
};

TtsService::TtsService() : p_(new Impl) { p_->worker = std::thread([this] { p_->run(); }); }

TtsService::~TtsService() {
  stop();
  { std::lock_guard<std::mutex> l(p_->mu); p_->quit = true; }
  p_->cv.notify_all();
  if (p_->worker.joinable()) p_->worker.join();
}

void TtsService::apply(const TtsSettings& t, const AppPaths& p) {
  std::lock_guard<std::mutex> l(p_->mu);
  p_->ts = t;
  p_->paths = p;
}

void TtsService::speak(const std::string& s) {
  if (trim(s).empty()) return;
  { std::lock_guard<std::mutex> l(p_->mu); if (p_->ts.engine == "off") return; p_->queue.push_back(s); }
  p_->cv.notify_one();
}

void TtsService::stop() {
  { std::lock_guard<std::mutex> l(p_->mu); p_->queue.clear(); p_->generation++; }
  p_->abortSynth = true;
  { std::lock_guard<std::mutex> l(p_->voiceMu); if (p_->voice) p_->voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr); }
  p_->player.clear();
}

bool TtsService::speaking() const {
  std::lock_guard<std::mutex> l(p_->mu);
  return p_->busy.load() || !p_->queue.empty() || p_->player.buffered() > 0;
}

std::string TtsService::lastError() const { std::lock_guard<std::mutex> l(p_->mu); return p_->error; }

std::vector<std::string> TtsService::sapiVoices() {
  std::vector<std::string> out;
  HKEY k;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens", 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return out;
  wchar_t name[256];
  for (DWORD i = 0;; ++i) {
    DWORD n = 256;
    if (RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
    std::wstring sub = std::wstring(L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens\\") + name + L"\\Attributes";
    wchar_t val[256];
    DWORD sz = sizeof(val);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, sub.c_str(), L"Name", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, val, &sz) == ERROR_SUCCESS) out.push_back(narrow(val));
  }
  RegCloseKey(k);
  return out;
}

std::vector<std::string> TtsService::piperVoices(const std::string& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  if (dir.empty() || !dirExists(dir)) return out;
  for (auto it = std::filesystem::recursive_directory_iterator(u8p(dir), ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    if (it->is_regular_file() && it->path().extension() == ".onnx") out.push_back(it->path().u8string());
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace tony
