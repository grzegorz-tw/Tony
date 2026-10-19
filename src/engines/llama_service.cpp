#include "llama_service.h"

#include <chrono>
#include <thread>

#include "core/strutil.h"
#include "llama_preset.h"
#include "net/http.h"
#include "platform/winutil.h"

namespace tony {

std::string LlamaService::modelId(const std::string& name) { return presetId(name); }

std::string LlamaService::logPath() const { return joinPath(joinPath(paths_.cacheDir, "logs"), "llama-server.log"); }

std::string LlamaService::tailLog(int lines) const {
  std::string t = readTextFile(logPath(), 1u << 20);
  auto v = split(t, '\n');
  size_t from = v.size() > static_cast<size_t>(lines) ? v.size() - lines : 0;
  std::string out;
  for (size_t i = from; i < v.size(); ++i) out += v[i] + "\n";
  return trim(out);
}

bool LlamaService::get(const std::string& path, std::string* body) {
  HttpRequest r;
  r.url = "http://127.0.0.1:" + std::to_string(port_) + path;
  r.noProxy = true;
  r.timeoutSec = 5;
  r.headers = {"Authorization: Bearer " + apiKey_};
  HttpResponse h = httpPerform(r);
  if (body) *body = h.body;
  return h.ok();
}

bool LlamaService::post(const std::string& path, const std::string& json, std::string* body) {
  HttpRequest r;
  r.method = "POST";
  r.url = "http://127.0.0.1:" + std::to_string(port_) + path;
  r.noProxy = true;
  r.timeoutSec = 60;
  r.body = json;
  r.headers = {"Content-Type: application/json", "Authorization: Bearer " + apiKey_};
  HttpResponse h = httpPerform(r);
  if (body) *body = h.body;
  return h.ok();
}

Endpoint LlamaService::acquire(const std::atomic<bool>& cancel) {
  std::lock_guard<std::mutex> lock(mu_);
  Endpoint ep;
  const std::string exe = s_.llama.exePath;
  if (!fileExists(exe)) {
    ep.error = "llama-server was not found at:\n" + exe + "\nRun scripts\\setup.ps1 (downloads the engines) or point Settings > Models to your llama-server.exe.";
    return ep;
  }
  const ModelEntry* chat = s_.chatModel();
  if (!chat || chat->path.empty()) { ep.error = "No chat model is configured. Open Settings > Models, add a .gguf file and choose it as the chat model."; return ep; }
  if (!fileExists(chat->path)) { ep.error = "The chat model file does not exist:\n" + chat->path; return ep; }
  for (auto* m : {s_.visionModel()})
    if (m && !m->mmproj.empty() && !fileExists(m->mmproj)) { ep.error = "The vision projector (mmproj) file does not exist:\n" + m->mmproj; return ep; }

  std::string fp = exe + "|" + buildPresetIni(s_) + "|" + s_.llama.gpuPolicy + "|" + s_.llama.extraArgs;
  if (proc_.running() && fp == fingerprint_) {
    ep.ok = true;
  } else {
    proc_.stop();
    ensureDir(joinPath(paths_.cacheDir, "logs"));
    ensureDir(joinPath(paths_.cacheDir, "presets"));
    std::string ini = joinPath(joinPath(paths_.cacheDir, "presets"), "models.ini");
    if (!writeTextFile(ini, buildPresetIni(s_))) { ep.error = "Cannot write " + ini; return ep; }
    apiKey_ = randomHex(16);
    port_ = findFreePort(s_.llama.port);
    std::string err;
    if (!proc_.start(exe, buildServerArgs(s_, ini, port_, apiKey_), parentDir(exe), logPath(), &err)) { ep.error = err; return ep; }
    fingerprint_ = fp;

    // wait for the HTTP server (models themselves load lazily on the first request)
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
      if (cancel.load()) { ep.error = "cancelled"; return ep; }
      HttpRequest r;
      r.url = "http://127.0.0.1:" + std::to_string(port_) + "/health";
      r.noProxy = true;
      r.timeoutSec = 2;
      HttpResponse h = httpPerform(r);
      if (h.status == 200) break;
      if (!proc_.running()) { ep.error = "llama-server exited during start-up.\n" + tailLog(15); fingerprint_.clear(); return ep; }
      if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(90)) { ep.error = "llama-server did not become ready within 90 s.\n" + tailLog(15); return ep; }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    ep.ok = true;
  }
  ep.baseUrl = "http://127.0.0.1:" + std::to_string(port_) + "/v1";
  ep.apiKey = apiKey_;
  return ep;
}

void LlamaService::stop() {
  std::lock_guard<std::mutex> lock(mu_);
  proc_.stop();
  fingerprint_.clear();
}

std::vector<LlamaService::ModelState> LlamaService::models() {
  std::vector<ModelState> out;
  if (!proc_.running()) return out;
  std::string body;
  if (!get("/models", &body)) return out;
  json j = json::parse(body, nullptr, false);
  if (j.is_discarded()) return out;
  const json* arr = j.is_array() ? &j : (j.contains("data") ? &j["data"] : (j.contains("models") ? &j["models"] : nullptr));
  if (!arr || !arr->is_array()) return out;
  for (auto& m : *arr) {
    ModelState ms;
    ms.id = m.value("id", m.value("name", ""));
    if (m.contains("status") && m["status"].is_object()) ms.state = m["status"].value("value", "");
    else if (m.contains("status") && m["status"].is_string()) ms.state = m["status"].get<std::string>();
    if (!ms.id.empty()) out.push_back(ms);
  }
  return out;
}

bool LlamaService::unload(const std::string& name) { return post("/models/unload", json{{"model", presetId(name)}}.dump()); }

bool LlamaService::unloadAll() {
  bool any = false;
  for (auto& m : models()) if (m.state == "loaded" || m.state == "loading") any = unload(m.id) || any;
  if (any) std::this_thread::sleep_for(std::chrono::milliseconds(800));  // give the driver a moment to release VRAM
  return any;
}

}  // namespace tony
