#include "hf.h"

#include <algorithm>

#include "core/strutil.h"
#include "net/http.h"
#include "net/netpolicy.h"

namespace tony {

bool validRepoId(const std::string& r) {
  size_t slash = r.find('/');
  if (slash == std::string::npos || slash == 0 || slash + 1 >= r.size() || r.find('/', slash + 1) != std::string::npos) return false;
  for (unsigned char c : r) if (!(std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/')) return false;
  return r.find("..") == std::string::npos;
}

static std::string encodePath(const std::string& p) {
  std::string out;
  for (auto& seg : split(p, '/')) { if (seg.empty()) continue; if (!out.empty()) out += "/"; out += urlEncode(seg); }
  return out;
}

std::vector<HfFile> parseHfTree(const std::string& body) {
  std::vector<HfFile> out;
  json j = json::parse(body, nullptr, false);
  if (j.is_discarded() || !j.is_array()) return out;
  for (auto& e : j) {
    if (!e.is_object() || !e.contains("path") || !e["path"].is_string()) continue;
    HfFile f;
    f.path = e["path"].get<std::string>();
    f.isDir = e.value("type", "") == "directory";
    f.size = e.value("size", 0LL);
    if (e.contains("lfs") && e["lfs"].is_object()) f.size = e["lfs"].value("size", f.size);  // real size of LFS-stored weights
    out.push_back(std::move(f));
  }
  std::sort(out.begin(), out.end(), [](const HfFile& a, const HfFile& b) { return a.isDir != b.isDir ? a.isDir : a.path < b.path; });
  return out;
}

std::string hfFileUrl(const std::string& repo, const std::string& path) {
  return "https://huggingface.co/" + repo + "/resolve/main/" + encodePath(path) + "?download=true";
}

bool hfList(const std::string& repo, const std::string& subpath, const NetworkSettings& ns, std::vector<HfFile>* out, std::string* err) {
  if (!validRepoId(repo)) { if (err) *err = "Enter a repository like  ggml-org/gpt-oss-20b-GGUF"; return false; }
  HttpRequest r;
  r.url = "https://huggingface.co/api/models/" + repo + "/tree/main" + (subpath.empty() ? "" : "/" + encodePath(subpath));
  r.timeoutSec = 30;
  std::string blocked;
  if (!applyNetworkPolicy(ns, r, &blocked)) { if (err) *err = blocked; return false; }
  HttpResponse h = httpPerform(r);
  if (!h.error.empty()) { if (err) *err = h.error; return false; }
  if (h.status == 404) { if (err) *err = "Repository or folder not found."; return false; }
  if (!h.ok()) { if (err) *err = "Hugging Face answered HTTP " + std::to_string(h.status); return false; }
  *out = parseHfTree(h.body);
  return true;
}

}  // namespace tony
