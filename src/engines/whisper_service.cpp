#include "whisper_service.h"

#include <chrono>
#include <thread>

#include "core/strutil.h"
#include "net/http.h"
#include "platform/winutil.h"
#include "whisper_util.h"

namespace tony {

bool WhisperService::ensureRunning(std::string* err, const std::atomic<bool>* cancel) {
  const WhisperSettings& w = s_.whisper;
  if (!fileExists(w.exePath)) { if (err) *err = "whisper-server was not found at:\n" + w.exePath + "\nRun scripts\\setup.ps1 or set the path in Settings > Voice."; return false; }
  if (w.modelPath.empty() || !fileExists(w.modelPath)) { if (err) *err = "No speech-recognition model is set. Download a Whisper model in Settings > Voice."; return false; }
  std::string fp = w.exePath + "|" + w.modelPath + "|" + w.language + "|" + std::to_string(w.threads) + "|" + (w.gpu ? "gpu" : "cpu") + "|" + w.extraArgs;
  if (proc_.running() && fp == fingerprint_) return true;

  proc_.stop();
  ensureDir(joinPath(paths_.cacheDir, "logs"));
  port_ = findFreePort(w.port);
  int threads = w.threads > 0 ? w.threads : 8;
  std::vector<std::string> args = {"-m", w.modelPath, "--host", "127.0.0.1", "--port", std::to_string(port_), "-t", std::to_string(threads), "-nt", "-sns"};
  if (!w.language.empty()) { args.push_back("-l"); args.push_back(w.language); }
  if (!w.gpu) args.push_back("-ng");
  for (auto& a : splitArgs(w.extraArgs)) args.push_back(a);
  if (!proc_.start(w.exePath, args, parentDir(w.exePath), joinPath(joinPath(paths_.cacheDir, "logs"), "whisper-server.log"), err)) return false;
  fingerprint_ = fp;

  auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    if (cancel && cancel->load()) return false;
    HttpRequest r;
    r.url = "http://127.0.0.1:" + std::to_string(port_) + "/";
    r.noProxy = true;
    r.timeoutSec = 2;
    if (httpPerform(r).status > 0) return true;  // any HTTP answer means the model is loaded and the server is up
    if (!proc_.running()) { if (err) *err = "whisper-server exited during start-up (see logs\\whisper-server.log)."; fingerprint_.clear(); return false; }
    if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(90)) { if (err) *err = "whisper-server did not start within 90 s."; return false; }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

bool WhisperService::transcribe(const std::vector<float>& pcm, std::string* text, std::string* err, const std::atomic<bool>* cancel) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!ensureRunning(err, cancel)) return false;
  HttpRequest r;
  r.method = "POST";
  r.url = "http://127.0.0.1:" + std::to_string(port_) + "/inference";
  r.noProxy = true;
  r.timeoutSec = 120;
  r.form.push_back({"file", makeWav16k(pcm), "audio.wav", "audio/wav", true});
  r.form.push_back({"response_format", "json", "", "", false});
  r.form.push_back({"temperature", "0.0", "", "", false});
  HttpResponse h = httpPerform(r, {}, cancel);
  if (!h.ok()) { if (err) *err = h.error.empty() ? "speech recognition failed: HTTP " + std::to_string(h.status) : h.error; return false; }
  json j = json::parse(h.body, nullptr, false);
  if (j.is_discarded() || !j.is_object()) { if (err) *err = "unexpected answer from whisper-server"; return false; }
  *text = cleanTranscript(sanitizeUtf8(j.value("text", "")));
  return true;
}

void WhisperService::stop() {
  std::lock_guard<std::mutex> lock(mu_);
  proc_.stop();
  fingerprint_.clear();
}

}  // namespace tony
