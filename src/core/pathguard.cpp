#include "pathguard.h"

#include <algorithm>
#include <cctype>

#include "strutil.h"

namespace tony {

static bool isReservedName(const std::string& seg) {
  std::string base = seg.substr(0, seg.find('.'));
  if (base == "con" || base == "prn" || base == "aux" || base == "nul") return true;
  if (base.size() == 4 && (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0) && base[3] >= '0' && base[3] <= '9') return true;
  return false;
}

// DOS 8.3 alias such as "progra~1": resolved by the OS layer, rejected here as a bypass vector.
static bool looksLikeShortName(const std::string& seg) {
  size_t t = seg.find('~');
  if (t == std::string::npos || t == 0 || t > 6 || t + 1 >= seg.size()) return false;
  if (!std::isdigit(static_cast<unsigned char>(seg[t + 1]))) return false;
  size_t dot = seg.find('.');
  return (dot == std::string::npos ? seg.size() : dot) <= 8;
}

std::string PathGuard::normalize(const std::string& in) {
  std::string p = in;
  std::replace(p.begin(), p.end(), '\\', '/');
  p = lower(p);
  // \\?\C:\x -> c:/x ; \\?\UNC\srv\share -> //srv/share
  if (startsWith(p, "//?/unc/")) p = "//" + p.substr(8);
  else if (startsWith(p, "//?/")) p = p.substr(4);
  bool unc = startsWith(p, "//") && !startsWith(p, "//./");
  std::vector<std::string> out;
  for (auto& seg : split(p, '/')) {
    std::string s = seg;
    if (s.empty() || s == ".") continue;
    if (s == "..") { if (!out.empty() && !(out.size() == 1 && out[0].size() == 2 && out[0][1] == ':')) out.pop_back(); continue; }
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();  // Windows ignores trailing dots/spaces
    if (s.empty()) continue;
    out.push_back(s);
  }
  std::string res = join(out, "/");
  if (unc) res = "//" + res;
  else if (startsWith(p, "/") && !(res.size() >= 2 && res[1] == ':')) res = "/" + res;
  if (res.size() == 2 && res[1] == ':') res += "/";
  return res;
}

bool PathGuard::globMatch(const std::string& pat, const std::string& text) {
  size_t p = 0, t = 0, star = std::string::npos, mark = 0;
  while (t < text.size()) {
    if (p < pat.size() && (pat[p] == '?' || pat[p] == text[t])) { ++p; ++t; }
    else if (p < pat.size() && pat[p] == '*') { star = p++; mark = t; }
    else if (star != std::string::npos) { p = star + 1; t = ++mark; }
    else return false;
  }
  while (p < pat.size() && pat[p] == '*') ++p;
  return p == pat.size();
}

std::vector<std::string> PathGuard::defaultDenyPatterns() {
  return {
      "^/windows/", "^/program files/", "^/program files (x86)/", "^/programdata/", "^/$recycle.bin/", "^/system volume information/",
      "/.ssh/", "/.gnupg/", "/.aws/", "/.azure/", "/.kube/", "/.docker/config.json",
      "/appdata/roaming/microsoft/credentials/", "/appdata/local/microsoft/credentials/", "/appdata/roaming/microsoft/protect/",
      "/appdata/local/google/chrome/user data/", "/appdata/local/microsoft/edge/user data/", "/appdata/local/bravesoftware/",
      "/appdata/roaming/mozilla/firefox/", "/appdata/roaming/tony/secrets",
      "/ntuser.dat", "*.kdbx", "*.pfx", "*.ppk", "/id_rsa", "/id_ed25519", "/.git-credentials", "/.netrc",
  };
}

void PathGuard::configure(bool restrictToRoots, std::vector<FileRoot> roots, std::vector<std::string> denyPatterns) {
  restrict_ = restrictToRoots;
  roots_.clear();
  for (auto& r : roots) {
    std::string n = normalize(r.path);
    if (!n.empty()) roots_.push_back({n, r.writable});
  }
  deny_.clear();
  for (auto& d : denyPatterns) {
    std::string n = d;
    bool anchored = !n.empty() && n[0] == '^';
    if (anchored) n.erase(0, 1);
    std::replace(n.begin(), n.end(), '\\', '/');
    n = lower(n);
    if (!n.empty()) deny_.push_back((anchored ? "^" : "") + n);
  }
}

static bool isUnder(const std::string& root, const std::string& path) {
  if (path == root) return true;
  std::string r = root;
  if (r.empty() || r.back() != '/') r += '/';
  return startsWith(path, r);
}

PathGuard::Verdict PathGuard::check(const std::string& absPath, bool forWrite) const {
  Verdict v;
  if (absPath.find('\0') != std::string::npos) { v.reason = "path contains a NUL byte"; return v; }
  std::string raw = absPath;
  std::replace(raw.begin(), raw.end(), '\\', '/');
  if (startsWith(raw, "//./") || startsWith(lower(raw), "//?/globalroot")) { v.reason = "device paths are not allowed"; return v; }

  std::string n = normalize(absPath);
  v.normalized = n;
  bool drive = n.size() >= 3 && std::isalpha(static_cast<unsigned char>(n[0])) && n[1] == ':' && n[2] == '/';
  if (!drive && !startsWith(n, "//")) { v.reason = "path must be absolute (e.g. C:\\Users\\...)"; return v; }
  if (n.find(':', drive ? 2 : 0) != std::string::npos) { v.reason = "alternate data streams / colons in path are not allowed"; return v; }
  for (auto& seg : split(n, '/')) {
    if (seg.empty()) continue;
    if (isReservedName(seg)) { v.reason = "reserved device name '" + seg + "'"; return v; }
    if (looksLikeShortName(seg)) { v.reason = "8.3 short names are not allowed; use the full name"; return v; }
  }

  std::string withSlash = n + "/";
  std::string noDrive = drive ? n.substr(2) + "/" : withSlash;
  for (auto& d : deny_) {
    bool hit = d[0] == '^' ? startsWith(noDrive, d.substr(1)) : globMatch("*" + d + "*", withSlash);
    if (hit) { v.reason = "blocked by the protected-locations list (" + (d[0] == '^' ? d.substr(1) : d) + ")"; return v; }
  }

  if (!restrict_) { v.allowed = true; return v; }
  const Root* best = nullptr;
  for (auto& r : roots_)
    if (isUnder(r.norm, n) && (!best || r.norm.size() > best->norm.size())) best = &r;
  if (!best) { v.reason = "outside the folders Tony is allowed to access (Settings > Files & Security)"; return v; }
  if (forWrite && !best->writable) { v.reason = "this folder is read-only for Tony"; return v; }
  v.allowed = true;
  return v;
}

}  // namespace tony
