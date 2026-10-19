// Tony - owns the llama-server child process (router mode): start, restart on config change, load/unload models, status.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "openai_backend.h"
#include "platform/process.h"

namespace tony {

class LlamaService : public IEndpointProvider {
 public:
  LlamaService(Settings& s, const AppPaths& p) : s_(s), paths_(p) {}
  ~LlamaService() { stop(); }

  Endpoint acquire(const std::atomic<bool>& cancel) override;  // starts / restarts the engine as needed
  std::string modelId(const std::string& name) override;

  void stop();
  bool running() const { return proc_.running(); }
  std::string logPath() const;
  std::string tailLog(int lines) const;

  struct ModelState { std::string id, state; };  // state: loaded | loading | unloaded | sleeping | ...
  std::vector<ModelState> models();              // empty when the engine is not running
  bool unloadAll();                              // frees VRAM (used before image generation)
  bool unload(const std::string& name);

 private:
  Settings& s_;
  AppPaths paths_;
  ManagedProcess proc_;
  std::mutex mu_;
  std::string fingerprint_, apiKey_;
  int port_ = 0;

  bool post(const std::string& path, const std::string& json, std::string* body = nullptr);
  bool get(const std::string& path, std::string* body);
};

}  // namespace tony
