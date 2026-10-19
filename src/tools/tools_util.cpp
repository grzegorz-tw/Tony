#include <filesystem>

#include "core/strutil.h"
#include "platform/winutil.h"
#include "tools.h"

namespace tony {

std::string workspaceDir(Services& svc) {
  std::string w = svc.settings->security.workspaceDir;
  if (w.empty()) w = joinPath(svc.paths.docsDir, "Tony");
  ensureDir(w);
  return w;
}

std::string argStr(const json& a, const char* key, const std::string& def) {
  if (!a.contains(key) || a[key].is_null()) return def;
  if (a[key].is_string()) return a[key].get<std::string>();
  return a[key].dump();  // models sometimes send numbers/bools where text is expected
}

int argInt(const json& a, const char* key, int def) {
  if (!a.contains(key)) return def;
  if (a[key].is_number()) return a[key].get<int>();
  if (a[key].is_string()) return std::atoi(a[key].get<std::string>().c_str());
  return def;
}

std::string humanSize(unsigned long long b) {
  const char* u[] = {"B", "KB", "MB", "GB", "TB"};
  double v = static_cast<double>(b);
  int i = 0;
  while (v >= 1024 && i < 4) { v /= 1024; ++i; }
  char buf[32];
  std::snprintf(buf, sizeof(buf), i == 0 ? "%.0f %s" : "%.1f %s", v, u[i]);
  return buf;
}

// Both the path as written and its fully resolved form (junctions, symlinks, 8.3 names) must pass the guard,
// and the resolved form is what the tool then uses, so a link inside an allowed folder cannot lead outside it.
ResolvedPath resolvePath(Services& svc, const std::string& raw, bool forWrite) {
  ResolvedPath r;
  if (trim(raw).empty()) { r.error = "no path given"; return r; }
  std::string abs = absolutePath(trim(raw), workspaceDir(svc));
  auto v1 = svc.guard->check(abs, forWrite);
  if (!v1.allowed) { r.error = "Access denied for '" + abs + "': " + v1.reason; return r; }
  std::string real = canonicalize(abs);
  auto v2 = svc.guard->check(real, forWrite);
  if (!v2.allowed) { r.error = "Access denied: '" + abs + "' resolves to '" + real + "' which is " + v2.reason; return r; }
  r.ok = true;
  r.path = real;
  return r;
}

bool backupBeforeChange(Services& svc, const std::string& absPath) {
  if (!fileExists(absPath)) return true;
  std::string dir = joinPath(svc.paths.dataDir, "backups");
  ensureDir(dir);
  std::error_code ec;
  std::filesystem::copy_file(u8p(absPath), u8p(joinPath(dir, timestampForFilename() + "_" + randomHex(2) + "_" + fileName(absPath))),
                             std::filesystem::copy_options::overwrite_existing, ec);
  return !ec;
}

}  // namespace tony
