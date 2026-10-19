// Tony - tool interface. A tool is a capability the model can call; the Agent decides whether it may run.
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pathguard.h"
#include "settings.h"
#include "types.h"

namespace tony {

struct ToolResult {
  std::string text;
  std::vector<std::string> images;  // image files to show in the chat (and, when the tool asks, to attach for the model)
  bool isError = false;
  bool untrusted = false;           // content came from outside (web, files) -> taints the conversation
  std::string source;               // e.g. URL, for the untrusted-content banner
  static ToolResult error(const std::string& m) { ToolResult r; r.text = m; r.isError = true; return r; }
  static ToolResult ok(const std::string& m) { ToolResult r; r.text = m; return r; }
};

// What the Agent should do before running a call.
struct Assessment {
  enum class Level { Auto, Ask, Deny };
  Level level = Level::Auto;
  std::string risk = "low";       // low | medium | high  (drives the colour of the approval dialog)
  std::string summary;            // one line for the dialog: "Run PowerShell command"
  std::string detail;             // the exact command / path / URL
  std::string reason;             // why it asks or is denied
  bool alwaysAsk = false;         // even "allow for this session" must not skip the prompt
  static Assessment deny(const std::string& why) { Assessment a; a.level = Level::Deny; a.reason = why; return a; }
};

using AskUserFn = std::function<std::string(const std::string& question, const std::vector<std::string>& options)>;
using ProgressFn = std::function<void(const std::string& message)>;

struct ToolContext {
  Settings& settings;
  const PathGuard& guard;
  const std::atomic<bool>& cancel;
  AskUserFn askUser;
  ProgressFn progress;
  bool tainted = false;
};

class Tool {
 public:
  virtual ~Tool() = default;
  virtual ToolSpec spec() const = 0;
  virtual bool defaultEnabled() const { return true; }
  virtual Assessment assess(const json& args, ToolContext& ctx) { (void)args; (void)ctx; return {}; }
  virtual ToolResult run(const json& args, ToolContext& ctx) = 0;
};

class ToolRegistry {
 public:
  void add(std::unique_ptr<Tool> t) { std::string n = t->spec().name; tools_[n] = std::move(t); }
  void remove(const std::string& name) { tools_.erase(name); }
  Tool* find(const std::string& name) const { auto it = tools_.find(name); return it == tools_.end() ? nullptr : it->second.get(); }
  std::vector<std::string> names() const { std::vector<std::string> v; for (auto& kv : tools_) v.push_back(kv.first); return v; }

  std::vector<ToolSpec> enabledSpecs(const Settings& s) const {
    std::vector<ToolSpec> v;
    for (auto& kv : tools_)
      if (s.toolEnabled(kv.first, kv.second->defaultEnabled())) v.push_back(kv.second->spec());
    return v;
  }

 private:
  std::map<std::string, std::unique_ptr<Tool>> tools_;
};

}  // namespace tony
