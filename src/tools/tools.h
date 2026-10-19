// Tony - built-in tools. Each tool declares what it needs (Assessment); the Agent enforces approval, taint and audit.
#pragma once
#include <memory>
#include <mutex>
#include <string>

#include "core/agent.h"
#include "core/tool.h"
#include "engines/image_service.h"
#include "engines/llama_service.h"
#include "platform/process.h"

namespace tony {

struct Aria2Daemon;  // defined in tools_web.cpp

struct Services {
  Settings* settings = nullptr;
  AppPaths paths;
  PathGuard* guard = nullptr;
  IChatBackend* backend = nullptr;  // used by the vision tools
  LlamaService* llama = nullptr;
  ImageService* image = nullptr;
  std::mutex memoryMu;
  std::shared_ptr<Aria2Daemon> aria;  // lazily started
};

void registerBuiltinTools(ToolRegistry& reg, Services& svc);
void registerCustomTools(ToolRegistry& reg, Services& svc, const std::vector<CustomTool>& old);  // removes `old`, adds settings.tools.custom
std::string memoryBlock(Services& svc);                                                         // saved notes for the system prompt

// registration units (one per tool file)
void registerFileTools(ToolRegistry& reg, Services& svc);
void registerShellTools(ToolRegistry& reg, Services& svc);
void registerSystemTools(ToolRegistry& reg, Services& svc);
void registerWebTools(ToolRegistry& reg, Services& svc);
void registerMediaTools(ToolRegistry& reg, Services& svc);

// ---- helpers shared by the tool files
struct ResolvedPath {
  bool ok = false;
  std::string path;    // absolute, symlinks resolved
  std::string error;   // reason, phrased for the model and the user
};
ResolvedPath resolvePath(Services& svc, const std::string& raw, bool forWrite);
std::string workspaceDir(Services& svc);
std::string argStr(const json& a, const char* key, const std::string& def = "");
int argInt(const json& a, const char* key, int def);
bool backupBeforeChange(Services& svc, const std::string& absPath);  // copies to <data>\backups; returns false only on real failure
std::string humanSize(unsigned long long bytes);

}  // namespace tony
