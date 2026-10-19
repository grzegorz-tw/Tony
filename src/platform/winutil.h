// Tony - thin wrappers over the Win32 APIs the rest of the app needs. Windows only.
#pragma once
#include <string>
#include <vector>

#include "core/settings.h"

namespace tony {

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);

AppPaths detectPaths();
bool ensureDir(const std::string& path);                     // creates parents too
bool fileExists(const std::string& path);
bool dirExists(const std::string& path);
std::string readTextFile(const std::string& path, size_t maxBytes = 64u << 20);
bool writeTextFile(const std::string& path, const std::string& data);  // atomic (temp + replace)
std::string parentDir(const std::string& path);
std::string joinPath(const std::string& a, const std::string& b);
std::string fileName(const std::string& path);

// Resolves symlinks/junctions/8.3 names for the existing part of `path`; the rest is appended unchanged.
std::string canonicalize(const std::string& path);
std::string absolutePath(const std::string& path, const std::string& baseDir);

std::string dpapiProtect(const std::string& plain);        // returns base64
std::string dpapiUnprotect(const std::string& b64);        // returns "" on failure

bool moveToRecycleBin(const std::string& path);
bool shellOpen(const std::string& pathOrUrl);
bool revealInExplorer(const std::string& path);

std::string getClipboardText();
bool setClipboardText(const std::string& utf8);

struct WindowInfo { std::string title, process; unsigned long pid = 0; bool foreground = false; int x = 0, y = 0, w = 0, h = 0; };
std::vector<WindowInfo> listWindows();

bool vpnAdapterUp(const std::string& regex);               // any up adapter whose name/description matches
std::string systemSummary();                               // OS, CPU, RAM, GPUs, user (for the system prompt / system_info tool)
std::string nowLocalString();                              // "Saturday, 17 October 2026, 14:03 (UTC+02:00)"
std::string randomHex(size_t bytes);
bool isTcpPortFree(int port);
int findFreePort(int preferred);
std::string tempFilePath(const std::string& ext);
std::string timestampForFilename();
bool whichExe(const std::string& name, std::string* fullPath);  // PATH lookup

}  // namespace tony
