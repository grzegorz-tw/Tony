// Tony - shared value types and the chat-backend interface (portable).
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tony {

using json = nlohmann::json;

struct ToolCall {
  std::string id;
  std::string name;
  std::string argsJson;  // raw arguments exactly as the model produced them
};

struct Message {
  std::string role;     // system | user | assistant | tool
  std::string content;
  std::string reasoning;  // model "thinking", shown on request, never sent back
  std::vector<ToolCall> toolCalls;                 // assistant
  std::string toolCallId, toolName;                // tool
  std::vector<std::string> imageDataUrls;          // images the model should see (data: URLs)
  std::vector<std::string> shownImages;            // image files the UI should display
  bool uiOnly = false;                             // notices: displayed, never sent to the model
  bool isError = false;
  double tokPerSec = 0;
};

struct ToolSpec {
  std::string name;
  std::string description;
  json parameters = json::object();  // JSON schema
};

struct SamplingParams {
  double temperature = 0.7, topP = 0.95, minP = 0.05;
  int topK = 40;
  int maxTokens = 4096;
  std::string reasoning = "auto";  // auto | on | off
};

struct ChatRequest {
  std::string model;  // router model id / remote model name
  std::vector<Message> messages;
  std::vector<ToolSpec> tools;
  SamplingParams sampling;
};

struct ChatResult {
  Message message;
  std::string finishReason;
  int promptTokens = 0, completionTokens = 0;
  double tokPerSec = 0;
  std::string error;  // non-empty on failure
  bool cancelled = false;
};

using DeltaFn = std::function<void(const std::string& text, bool isReasoning)>;

class IChatBackend {
 public:
  virtual ~IChatBackend() = default;
  virtual ChatResult chat(const ChatRequest& req, const DeltaFn& onDelta, const std::atomic<bool>& cancel) = 0;
  // Context window in tokens for `model` (0 = unknown).
  virtual int contextSize(const std::string& model) { (void)model; return 0; }
};

}  // namespace tony
