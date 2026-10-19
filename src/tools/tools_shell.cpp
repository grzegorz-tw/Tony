#include <chrono>
#include <regex>
#include <thread>

#include "core/cmdrisk.h"
#include "core/strutil.h"
#include "platform/screen.h"
#include "platform/winutil.h"
#include "tools.h"

namespace tony {
namespace {

// A "read-only" command can still read a protected file (Get-Content ~\.ssh\id_rsa). When the command names absolute paths we
// run them through the same guard as the file tools; paths we cannot evaluate (variables, relative) disable auto-approval.
std::string shellPathProblem(Services& svc, const std::string& cmd) {
  static const std::regex abs(R"(([A-Za-z]:[\\/][^\s'"|;<>*?]*))");
  for (auto it = std::sregex_iterator(cmd.begin(), cmd.end(), abs); it != std::sregex_iterator(); ++it) {
    auto v = svc.guard->check((*it)[1].str(), false);
    if (!v.allowed) return "it touches '" + (*it)[1].str() + "' - " + v.reason;
  }
  if (svc.settings->security.restrictFiles) {
    std::string l = lower(cmd);
    for (const char* w : {"$env:", "~\\", "~/", "%userprofile%", "%appdata%", "..\\", "../", "$home"})
      if (l.find(w) != std::string::npos) return "it uses a variable or relative path that cannot be checked against your folder restrictions";
  }
  return "";
}

bool readsContent(const std::string& cmd) {
  static const std::regex re(R"(\b(get-content|gc|cat|type|select-string|findstr|invoke-webrequest|invoke-restmethod|iwr|irm|curl|wget|git\s+(show|log|diff)|get-eventlog|get-winevent)\b)",
                             std::regex::ECMAScript | std::regex::icase);
  return std::regex_search(cmd, re);
}

class PowerShellTool : public Tool {
 public:
  explicit PowerShellTool(Services& s) : svc_(s) {}
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "powershell";
    t.description =
        "Run a Windows PowerShell 5.1 command or script and return its output. Use it for system tasks, running builds/tests, git, installing tools, "
        "inspecting processes/services/network, etc. Commands run non-interactively with a timeout; do not start programs that wait for input.";
    t.parameters = {{"type", "object"},
                    {"properties", {{"command", {{"type", "string"}, {"description", "PowerShell code"}}},
                                    {"working_dir", {{"type", "string"}, {"description", "Folder to run in. Default: workspace"}}},
                                    {"timeout_sec", {{"type", "integer"}, {"description", "Default 60, max 900"}}}}},
                    {"required", json::array({"command"})}};
    return t;
  }

  Assessment assess(const json& a, ToolContext& ctx) override {
    const SecuritySettings& sec = ctx.settings.security;
    if (sec.shellApproval == "off") return Assessment::deny("PowerShell is turned off (Settings > Files & Security).");
    std::string cmd = argStr(a, "command");
    if (trim(cmd).empty()) return Assessment::deny("empty command");
    auto wd = resolvePath(svc_, argStr(a, "working_dir", "."), false);
    if (!wd.ok) return Assessment::deny(wd.error);

    RiskReport rr = classifyPowerShell(cmd);
    std::string problem = shellPathProblem(svc_, cmd);
    Assessment r;
    r.summary = "Run PowerShell command";
    r.detail = cmd + (wd.path == workspaceDir(svc_) ? "" : "\n\nin: " + wd.path);
    r.risk = rr.risk == Risk::Dangerous ? "high" : rr.risk == Risk::ReadOnly ? "low" : "medium";
    r.reason = join(rr.reasons, "; ");
    r.level = Assessment::Level::Ask;
    if (rr.risk == Risk::Dangerous) { r.alwaysAsk = true; r.reason = "Looks dangerous: " + r.reason; }
    else if (!problem.empty()) { r.alwaysAsk = true; r.risk = "high"; r.reason = "Needs your approval: " + problem; }
    else if (rr.risk == Risk::ReadOnly && sec.shellApproval == "smart") r.level = Assessment::Level::Auto;
    else if (sec.restrictFiles && r.reason.empty()) r.reason = "PowerShell is not limited to your allowed folders - check what the command does.";
    return r;
  }

  ToolResult run(const json& a, ToolContext& ctx) override {
    std::string cmd = argStr(a, "command");
    auto wd = resolvePath(svc_, argStr(a, "working_dir", "."), false);
    if (!wd.ok) return ToolResult::error(wd.error);
    int timeout = std::max(1, std::min(argInt(a, "timeout_sec", ctx.settings.security.shellTimeoutSec), 900));
    size_t maxBytes = static_cast<size_t>(std::max(4000, ctx.settings.security.maxToolOutput)) * 2;
    RunResult rr = runPowerShell(cmd, wd.path, timeout, maxBytes, &ctx.cancel);
    if (!rr.started) return ToolResult::error("Could not start PowerShell: " + rr.error);
    std::string out = trim(sanitizeUtf8(rr.output));
    std::string head = rr.timedOut ? "[timed out after " + std::to_string(timeout) + " s; the process tree was killed]\n"
                       : rr.cancelled ? "[stopped by the user]\n" : "";
    ToolResult r;
    r.text = head + "exit code: " + std::to_string(rr.exitCode) + (out.empty() ? "\n(no output)" : "\n" + out);
    r.isError = rr.timedOut || rr.exitCode != 0;
    r.untrusted = readsContent(cmd);
    r.source = "powershell output";
    return r;
  }

 private:
  Services& svc_;
};

class UiInputTool : public Tool {
 public:
  explicit UiInputTool(Services& s) : svc_(s) {}
  bool defaultEnabled() const override { return false; }  // computer control must be switched on deliberately
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "ui_input";
    t.description =
        "Control the keyboard and mouse like the user would: type text, press key combinations, click or scroll at screen pixel coordinates. "
        "Look at the screen first (look_at_screen) to find coordinates. Every action asks the user for approval.";
    t.parameters = {{"type", "object"},
                    {"properties", {{"action", {{"type", "string"}, {"enum", json::array({"type", "key", "click", "scroll"})}}},
                                    {"text", {{"type", "string"}, {"description", "For action=type"}}},
                                    {"keys", {{"type", "string"}, {"description", "For action=key, e.g. ctrl+s, alt+tab, enter, win+r"}}},
                                    {"x", {{"type", "integer"}}}, {"y", {{"type", "integer"}}},
                                    {"button", {{"type", "string"}, {"enum", json::array({"left", "right", "middle"})}}},
                                    {"clicks", {{"type", "integer"}, {"description", "1 or 2 (double-click)"}}},
                                    {"amount", {{"type", "integer"}, {"description", "Scroll notches, positive = up"}}}}},
                    {"required", json::array({"action"})}};
    return t;
  }
  Assessment assess(const json& a, ToolContext& ctx) override {
    std::string act = argStr(a, "action");
    Assessment r;
    r.level = Assessment::Level::Ask;
    r.risk = "high";
    r.alwaysAsk = ctx.settings.security.confirmUiInput;
    if (act == "type") { r.summary = "Type on your keyboard"; r.detail = argStr(a, "text"); }
    else if (act == "key") { r.summary = "Press keys"; r.detail = argStr(a, "keys"); }
    else if (act == "click") { r.summary = "Click the mouse"; r.detail = argStr(a, "button", "left") + " at (" + std::to_string(argInt(a, "x", 0)) + ", " + std::to_string(argInt(a, "y", 0)) + ")"; }
    else if (act == "scroll") { r.summary = "Scroll"; r.detail = "at (" + std::to_string(argInt(a, "x", 0)) + ", " + std::to_string(argInt(a, "y", 0)) + ") by " + std::to_string(argInt(a, "amount", 0)); }
    else return Assessment::deny("unknown action '" + act + "'");
    r.reason = "Tony will control your keyboard/mouse.";
    return r;
  }
  ToolResult run(const json& a, ToolContext&) override {
    std::string act = argStr(a, "action"), err;
    bool ok = false;
    if (act == "type") ok = inputTypeText(argStr(a, "text"));
    else if (act == "key") ok = inputKeyCombo(argStr(a, "keys"), &err);
    else if (act == "click") ok = inputClick(argInt(a, "x", 0), argInt(a, "y", 0), argStr(a, "button", "left"), argInt(a, "clicks", 1), &err);
    else if (act == "scroll") ok = inputScroll(argInt(a, "x", 0), argInt(a, "y", 0), argInt(a, "amount", -3));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    return ok ? ToolResult::ok("Done: " + act) : ToolResult::error(err.empty() ? "Input was blocked (elevated windows ignore normal input)." : err);
  }

 private:
  Services& svc_;
};

// A user-defined tool: a PowerShell template with {placeholders}; every value is quoted as a literal, never interpreted.
class CustomShellTool : public Tool {
 public:
  CustomShellTool(Services& s, CustomTool d) : svc_(s), d_(std::move(d)) {}
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = d_.name;
    t.description = d_.description.empty() ? "Custom tool" : d_.description;
    json props = json::object();
    json req = json::array();
    for (auto& p : d_.params) {
      props[p.name] = {{"type", p.type.empty() ? "string" : p.type}, {"description", p.description}};
      if (p.required) req.push_back(p.name);
    }
    t.parameters = {{"type", "object"}, {"properties", props}, {"required", req}};
    return t;
  }
  std::string expand(const json& a) const {
    std::map<std::string, std::string> v;
    for (auto& p : d_.params) v[p.name] = argStr(a, p.name.c_str());
    return expandTemplate(d_.command, v, psSingleQuote);
  }
  Assessment assess(const json& a, ToolContext& ctx) override {
    if (ctx.settings.security.shellApproval == "off") return Assessment::deny("PowerShell is turned off (Settings > Files & Security).");
    std::string cmd = expand(a);
    RiskReport rr = classifyPowerShell(cmd);
    Assessment r;
    r.summary = "Custom tool: " + d_.name;
    r.detail = cmd;
    r.risk = rr.risk == Risk::Dangerous ? "high" : "medium";
    r.level = d_.requireApproval ? Assessment::Level::Ask : Assessment::Level::Auto;
    if (rr.risk == Risk::Dangerous) { r.level = Assessment::Level::Ask; r.alwaysAsk = true; r.reason = "Looks dangerous: " + join(rr.reasons, "; "); }
    return r;
  }
  ToolResult run(const json& a, ToolContext& ctx) override {
    std::string wd = d_.workingDir.empty() ? workspaceDir(svc_) : d_.workingDir;
    RunResult rr = runPowerShell(expand(a), wd, std::max(1, std::min(d_.timeoutSec, 900)), static_cast<size_t>(std::max(4000, ctx.settings.security.maxToolOutput)) * 2, &ctx.cancel);
    if (!rr.started) return ToolResult::error("Could not start PowerShell: " + rr.error);
    ToolResult r;
    std::string out = trim(sanitizeUtf8(rr.output));
    r.text = "exit code: " + std::to_string(rr.exitCode) + (out.empty() ? "\n(no output)" : "\n" + out);
    r.isError = rr.exitCode != 0 || rr.timedOut;
    r.untrusted = true;
    r.source = "custom tool " + d_.name;
    return r;
  }

 private:
  Services& svc_;
  CustomTool d_;
};

}  // namespace

void registerShellTools(ToolRegistry& reg, Services& svc) {
  reg.add(std::make_unique<PowerShellTool>(svc));
  reg.add(std::make_unique<UiInputTool>(svc));
}

void registerCustomTools(ToolRegistry& reg, Services& svc, const std::vector<CustomTool>& old) {
  for (auto& o : old) reg.remove(o.name);
  for (auto& c : svc.settings->tools.custom) {
    if (c.name.empty() || c.command.empty()) continue;
    if (reg.find(c.name)) continue;  // never shadow a built-in tool
    reg.add(std::make_unique<CustomShellTool>(svc, c));
  }
}

}  // namespace tony
