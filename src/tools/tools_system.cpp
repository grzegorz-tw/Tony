#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <sstream>

#include "core/strutil.h"
#include "platform/screen.h"
#include "platform/winutil.h"
#include "tools.h"

namespace tony {
namespace {

// ------------------------------------------------------------------ memory store
std::string memoryPath(Services& s) { return joinPath(s.paths.dataDir, "memory.json"); }

json loadMemory(Services& s) {
  json j = json::parse(readTextFile(memoryPath(s), 4u << 20), nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("items") || !j["items"].is_array()) j = {{"items", json::array()}, {"next", 1}};
  return j;
}

class MemoryTool : public Tool {
 public:
  explicit MemoryTool(Services& s) : svc_(s) {}
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "memory";
    t.description = "Long-term notes that persist across chats. save = remember a durable fact or preference about the user (not secrets); list = show notes; delete = forget one by id.";
    t.parameters = {{"type", "object"},
                    {"properties", {{"action", {{"type", "string"}, {"enum", json::array({"save", "list", "delete"})}}},
                                    {"text", {{"type", "string"}, {"description", "For save: one short, self-contained note"}}},
                                    {"id", {{"type", "integer"}, {"description", "For delete"}}}}},
                    {"required", json::array({"action"})}};
    return t;
  }
  Assessment assess(const json& a, ToolContext&) override {
    Assessment r;
    std::string act = argStr(a, "action");
    r.summary = "Memory: " + act;
    r.detail = argStr(a, "text");
    r.risk = act == "list" ? "low" : "medium";  // saved notes re-enter every future prompt, so poisoned content must not auto-save
    return r;
  }
  ToolResult run(const json& a, ToolContext&) override {
    std::lock_guard<std::mutex> l(svc_.memoryMu);
    std::string act = argStr(a, "action");
    json j = loadMemory(svc_);
    if (act == "list") {
      std::ostringstream o;
      for (auto& it : j["items"]) o << it.value("id", 0) << ". " << it.value("text", "") << "\n";
      return ToolResult::ok(j["items"].empty() ? "(no notes saved)" : o.str());
    }
    if (act == "save") {
      std::string text = trim(argStr(a, "text"));
      if (text.empty() || text.size() > 500) return ToolResult::error("Give a short note (1-500 characters).");
      int id = j.value("next", 1);
      j["items"].push_back({{"id", id}, {"text", sanitizeUtf8(text)}, {"created", nowLocalString()}});
      j["next"] = id + 1;
      if (!writeTextFile(memoryPath(svc_), j.dump(2, ' ', false, json::error_handler_t::replace))) return ToolResult::error("Could not save the note.");
      return ToolResult::ok("Saved note #" + std::to_string(id) + ".");
    }
    if (act == "delete") {
      int id = argInt(a, "id", -1);
      json keep = json::array();
      bool found = false;
      for (auto& it : j["items"]) { if (it.value("id", -2) == id) found = true; else keep.push_back(it); }
      if (!found) return ToolResult::error("No note with that id.");
      j["items"] = keep;
      writeTextFile(memoryPath(svc_), j.dump(2, ' ', false, json::error_handler_t::replace));
      return ToolResult::ok("Deleted note #" + std::to_string(id) + ".");
    }
    return ToolResult::error("Unknown action.");
  }

 private:
  Services& svc_;
};

class AskUserTool : public Tool {
 public:
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "ask_user";
    t.description = "Ask the user a short clarifying question (optionally with 2-4 quick-reply options) and wait for the answer. Use only when a decision really matters and cannot be inferred.";
    t.parameters = {{"type", "object"},
                    {"properties", {{"question", {{"type", "string"}}}, {"options", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
                    {"required", json::array({"question"})}};
    return t;
  }
  ToolResult run(const json& a, ToolContext& ctx) override {
    if (!ctx.askUser) return ToolResult::error("The user cannot be asked right now.");
    std::vector<std::string> opts;
    if (a.contains("options") && a["options"].is_array()) for (auto& o : a["options"]) if (o.is_string()) opts.push_back(o.get<std::string>());
    std::string ans = ctx.askUser(argStr(a, "question"), opts);
    return ToolResult::ok(ans.empty() ? "(the user did not answer)" : "User answered: " + ans);
  }
};

class SystemInfoTool : public Tool {
 public:
  explicit SystemInfoTool(Services& s) : svc_(s) {}
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "system_info";
    t.description = "Facts about this PC. section=summary (OS, CPU, RAM, GPUs, time), windows (open windows and their apps), monitors (screen layout), disks (drives and free space).";
    t.parameters = {{"type", "object"}, {"properties", {{"section", {{"type", "string"}, {"enum", json::array({"summary", "windows", "monitors", "disks"})}}}}}};
    return t;
  }
  ToolResult run(const json& a, ToolContext&) override {
    std::string sec = argStr(a, "section", "summary");
    std::ostringstream o;
    if (sec == "summary") o << "Time: " << nowLocalString() << "\n" << systemSummary();
    else if (sec == "windows") {
      for (auto& w : listWindows()) o << (w.foreground ? "* " : "  ") << w.title << "  [" << w.process << ", " << w.w << "x" << w.h << " at " << w.x << "," << w.y << "]\n";
    } else if (sec == "monitors") {
      for (auto& m : listMonitors()) o << "monitor:" << m.index << (m.primary ? " (primary)" : "") << "  " << m.w << "x" << m.h << " at " << m.x << "," << m.y << "\n";
    } else if (sec == "disks") {
      DWORD mask = GetLogicalDrives();
      for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
        UINT type = GetDriveTypeW(root.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_REMOTE) continue;
        ULARGE_INTEGER freeB, total, totalFree;
        if (GetDiskFreeSpaceExW(root.c_str(), &freeB, &total, &totalFree)) o << narrow(root) << "  " << humanSize(freeB.QuadPart) << " free of " << humanSize(total.QuadPart) << (type == DRIVE_REMOTE ? " (network)" : "") << "\n";
      }
    } else return ToolResult::error("Unknown section.");
    ToolResult r = ToolResult::ok(o.str());
    r.untrusted = sec == "windows";  // window titles come from arbitrary web pages and documents
    r.source = "window titles";
    return r;
  }

 private:
  Services& svc_;
};

class ClipboardTool : public Tool {
 public:
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "clipboard";
    t.description = "Read the text on the clipboard (action=get) or replace it (action=set).";
    t.parameters = {{"type", "object"},
                    {"properties", {{"action", {{"type", "string"}, {"enum", json::array({"get", "set"})}}}, {"text", {{"type", "string"}}}}},
                    {"required", json::array({"action"})}};
    return t;
  }
  Assessment assess(const json& a, ToolContext&) override {
    Assessment r;
    bool set = argStr(a, "action") == "set";
    r.level = Assessment::Level::Ask;
    r.risk = "medium";
    r.summary = set ? "Replace the clipboard text" : "Read the clipboard";
    r.detail = set ? argStr(a, "text") : "The clipboard may contain passwords or private text.";
    return r;
  }
  ToolResult run(const json& a, ToolContext&) override {
    if (argStr(a, "action") == "set") return setClipboardText(argStr(a, "text")) ? ToolResult::ok("Clipboard updated.") : ToolResult::error("Could not access the clipboard.");
    std::string t = getClipboardText();
    ToolResult r = ToolResult::ok(t.empty() ? "(the clipboard has no text)" : t);
    r.untrusted = true;
    r.source = "clipboard";
    return r;
  }
};

class OpenItemTool : public Tool {
 public:
  explicit OpenItemTool(Services& s) : svc_(s) {}
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "open_item";
    t.description = "Open a file, folder, program or web page with its default app (like double-clicking it). reveal=true shows a file in Explorer instead.";
    t.parameters = {{"type", "object"},
                    {"properties", {{"target", {{"type", "string"}, {"description", "Path or http(s) URL"}}}, {"reveal", {{"type", "boolean"}}}}},
                    {"required", json::array({"target"})}};
    return t;
  }
  Assessment assess(const json& a, ToolContext&) override {
    std::string target = argStr(a, "target");
    Assessment r;
    r.level = Assessment::Level::Ask;
    r.risk = "medium";
    r.summary = "Open on your PC";
    r.detail = target;
    if (!startsWith(lower(target), "http://") && !startsWith(lower(target), "https://")) {
      auto p = resolvePath(svc_, target, false);
      if (!p.ok) return Assessment::deny(p.error);
      std::string ext = lower(p.path.substr(p.path.find_last_of('.') == std::string::npos ? p.path.size() : p.path.find_last_of('.')));
      if (ext == ".exe" || ext == ".bat" || ext == ".cmd" || ext == ".ps1" || ext == ".msi" || ext == ".scr" || ext == ".lnk" || ext == ".js" || ext == ".vbs" || ext == ".jar") {
        r.risk = "high"; r.alwaysAsk = true; r.reason = "This would run a program.";
      }
      r.detail = p.path;
    }
    return r;
  }
  ToolResult run(const json& a, ToolContext&) override {
    std::string target = argStr(a, "target");
    bool web = startsWith(lower(target), "http://") || startsWith(lower(target), "https://");
    if (!web) {
      auto p = resolvePath(svc_, target, false);
      if (!p.ok) return ToolResult::error(p.error);
      target = p.path;
    }
    bool reveal = a.contains("reveal") && a["reveal"].is_boolean() && a["reveal"].get<bool>();
    bool ok = (reveal && !web) ? revealInExplorer(target) : shellOpen(target);
    return ok ? ToolResult::ok("Opened " + target) : ToolResult::error("Windows could not open " + target);
  }

 private:
  Services& svc_;
};

}  // namespace

std::string memoryBlock(Services& svc) {
  std::lock_guard<std::mutex> l(svc.memoryMu);
  json j = loadMemory(svc);
  std::string out;
  for (auto& it : j["items"]) {
    std::string line = "- " + it.value("text", "") + "\n";
    if (out.size() + line.size() > 3000) break;
    out += line;
  }
  return out;
}

void registerSystemTools(ToolRegistry& reg, Services& svc) {
  reg.add(std::make_unique<MemoryTool>(svc));
  reg.add(std::make_unique<AskUserTool>());
  reg.add(std::make_unique<SystemInfoTool>(svc));
  reg.add(std::make_unique<ClipboardTool>());
  reg.add(std::make_unique<OpenItemTool>(svc));
}

void registerBuiltinTools(ToolRegistry& reg, Services& svc) {
  registerSystemTools(reg, svc);
  registerFileTools(reg, svc);
  registerShellTools(reg, svc);
  registerWebTools(reg, svc);
  registerMediaTools(reg, svc);
  registerCustomTools(reg, svc, {});
}

}  // namespace tony
