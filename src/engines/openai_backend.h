// Tony - chat backend for any OpenAI-compatible endpoint: the llama-server that Tony manages, or a remote server (LM Studio, Ollama, vLLM...).
// Portable: all platform specifics (starting the engine) hide behind IEndpointProvider.
#pragma once
#include <string>

#include "core/settings.h"
#include "core/types.h"

namespace tony {

struct Endpoint {
  bool ok = false;
  std::string baseUrl;  // e.g. http://127.0.0.1:18080/v1
  std::string apiKey;
  std::string error;
};

class IEndpointProvider {
 public:
  virtual ~IEndpointProvider() = default;
  virtual Endpoint acquire(const std::atomic<bool>& cancel) = 0;  // may start the engine; blocks until it answers
  virtual std::string modelId(const std::string& settingsName) { return settingsName; }
};

// remote mode -> settings.llama.remoteUrl; otherwise delegates to the managed engine.
class ModeEndpoint : public IEndpointProvider {
 public:
  ModeEndpoint(Settings& s, IEndpointProvider* managed) : s_(s), managed_(managed) {}
  Endpoint acquire(const std::atomic<bool>& cancel) override;
  std::string modelId(const std::string& n) override { return s_.llama.mode == "remote" || !managed_ ? n : managed_->modelId(n); }

 private:
  Settings& s_;
  IEndpointProvider* managed_;
};

class OpenAiBackend : public IChatBackend {
 public:
  OpenAiBackend(Settings& s, IEndpointProvider& ep) : s_(s), ep_(ep) {}
  ChatResult chat(const ChatRequest& req, const DeltaFn& onDelta, const std::atomic<bool>& cancel) override;
  int contextSize(const std::string& model) override;

  static json buildBody(const ChatRequest& req, const std::string& modelId);  // exposed for tests

 private:
  Settings& s_;
  IEndpointProvider& ep_;
};

}  // namespace tony
