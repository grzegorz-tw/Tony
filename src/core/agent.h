// Tony - the agent loop: model <-> tools, with approvals, taint tracking and context management.
// Portable on purpose (no Windows headers) so the safety logic is unit-tested with a fake backend.
#pragma once
#include <atomic>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "settings.h"
#include "tool.h"
#include "types.h"

namespace tony {

struct AgentEvent {
  enum class Type { Delta, ReasoningDelta, AssistantDone, ToolStart, ToolProgress, ToolDone, Notice, Error, TaintChanged, TurnDone };
  Type type = Type::Notice;
  std::string text;      // delta text / notice / error / progress
  Message message;       // AssistantDone, ToolDone
  std::string tool;      // ToolStart/ToolProgress/ToolDone
  std::string args;      // ToolStart
};
using EventFn = std::function<void(const AgentEvent&)>;

enum class ApprovalDecision { Deny, AllowOnce, AllowSession };

struct ApprovalRequest {
  std::string tool, summary, detail, risk, reason;
  bool tainted = false;
  bool canRemember = true;
};
using ApprovalFn = std::function<ApprovalDecision(const ApprovalRequest&)>;

struct AgentDeps {
  Settings* settings = nullptr;
  IChatBackend* backend = nullptr;
  ToolRegistry* tools = nullptr;
  const PathGuard* guard = nullptr;
  ApprovalFn approve;
  EventFn emit;
  AskUserFn askUser;
  std::function<std::string()> environmentBlock;  // date, OS, folders...
  std::function<std::string()> memoryBlock;       // saved notes
  std::function<void(const std::string&)> audit;  // one line per tool call
};

class Agent {
 public:
  explicit Agent(AgentDeps deps) : d_(std::move(deps)) {}

  // Blocking. Run on a worker thread. `images` are data: URLs attached to the user's message.
  void runTurn(const std::string& userText, const std::vector<std::string>& images, const std::atomic<bool>& cancel);

  void reset();  // new conversation: clears history, taint and session approvals
  void setHistory(std::vector<Message> h);
  const std::vector<Message>& history() const { return history_; }
  bool tainted() const { return tainted_; }

  // exposed for tests
  std::string buildSystemPrompt() const;
  static size_t estimateTokens(const std::vector<Message>& msgs);
  void compact(int ctxTokens);

 private:
  AgentDeps d_;
  std::vector<Message> history_;
  std::set<std::string> sessionAllowed_;
  bool tainted_ = false;

  void emit(AgentEvent::Type t, const std::string& text = "") const;
  Message executeCall(const ToolCall& call, const std::atomic<bool>& cancel);
  std::string modelFor(const std::vector<Message>& msgs, SamplingParams& sampling) const;
};

}  // namespace tony
