// Tony - owns whisper-server (whisper.cpp) and turns recorded audio into text. CPU by default, so VRAM stays free for the LLM.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "core/settings.h"
#include "platform/process.h"

namespace tony {

class WhisperService {
 public:
  WhisperService(Settings& s, const AppPaths& p) : s_(s), paths_(p) {}
  ~WhisperService() { stop(); }
  bool transcribe(const std::vector<float>& pcm16k, std::string* text, std::string* err, const std::atomic<bool>* cancel = nullptr);
  bool ready() const { return proc_.running(); }
  void stop();

 private:
  Settings& s_;
  AppPaths paths_;
  ManagedProcess proc_;
  std::mutex mu_;
  std::string fingerprint_;
  int port_ = 0;
  bool ensureRunning(std::string* err, const std::atomic<bool>* cancel);
};

}  // namespace tony
