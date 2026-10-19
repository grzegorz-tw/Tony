#include "llama_preset.h"

#include <algorithm>
#include <sstream>

#include "core/strutil.h"

namespace tony {

std::string presetId(const std::string& name) {
  std::string out;
  for (unsigned char c : name) out += (std::isalnum(c) || c == '.' || c == '_' || c == ':' || c == '-') ? static_cast<char>(c) : '-';
  return out.empty() ? "model" : out;
}

static std::string cleanValue(std::string v) {
  v = replaceAll(replaceAll(v, "\r", " "), "\n", " ");
  return trim(v);
}
static std::string pathValue(const std::string& p) { return cleanValue(replaceAll(p, "\\", "/")); }

// "--flag value --other" -> ini lines "flag = value" / "other = true"
static void appendExtraArgs(std::ostringstream& o, const std::string& extra) {
  auto t = splitArgs(extra);
  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i].size() < 2 || t[i][0] != '-') continue;
    std::string key = t[i].substr(t[i][1] == '-' ? 2 : 1);
    size_t eq = key.find('=');
    std::string val;
    if (eq != std::string::npos) { val = key.substr(eq + 1); key = key.substr(0, eq); }
    else if (i + 1 < t.size() && (t[i + 1].empty() || t[i + 1][0] != '-' || (t[i + 1].size() > 1 && (std::isdigit(static_cast<unsigned char>(t[i + 1][1])) || t[i + 1][1] == '.')))) val = t[++i];
    else val = "true";
    if (!key.empty()) o << key << " = " << pathValue(val) << "\n";
  }
}

std::string buildPresetIni(const Settings& s) {
  std::ostringstream o;
  o << "version = 1\n\n[*]\n";
  o << "jinja = true\n";
  o << "parallel = " << std::max(1, s.llama.parallel) << "\n";
  if (s.llama.idleUnloadSec > 0) o << "sleep-idle-seconds = " << s.llama.idleUnloadSec << "\n";
  o << "\n";
  for (auto& m : s.llama.models) {
    if (trim(m.path).empty()) continue;
    o << "[" << presetId(m.name) << "]\n";
    o << "model = " << pathValue(m.path) << "\n";
    if (!trim(m.mmproj).empty()) o << "mmproj = " << pathValue(m.mmproj) << "\n";
    o << "c = " << std::max(512, m.ctx) << "\n";
    if (m.gpuLayers >= 0) o << "n-gpu-layers = " << m.gpuLayers << "\n";   // otherwise llama.cpp's --fit picks what fits in VRAM
    if (m.cpuMoe >= 0) o << "n-cpu-moe = " << m.cpuMoe << "\n";
    if (m.flashAttn == "on" || m.flashAttn == "off") o << "flash-attn = " << m.flashAttn << "\n";
    if (!m.cacheK.empty() && m.cacheK != "f16") o << "cache-type-k = " << cleanValue(m.cacheK) << "\n";
    if (!m.cacheV.empty() && m.cacheV != "f16") o << "cache-type-v = " << cleanValue(m.cacheV) << "\n";
    if (m.reasoning == "on" || m.reasoning == "off") o << "reasoning = " << m.reasoning << "\n";
    if (m.threads > 0) o << "threads = " << m.threads << "\n";
    if (!trim(m.device).empty()) o << "device = " << cleanValue(m.device) << "\n";
    appendExtraArgs(o, m.extraArgs);
    o << "\n";
  }
  return o.str();
}

std::vector<std::string> buildServerArgs(const Settings& s, const std::string& iniPath, int port, const std::string& apiKey) {
  std::vector<std::string> a = {"--models-preset", iniPath, "--models-max", s.llama.gpuPolicy == "coexist" ? "2" : "1",
                                "--host", "127.0.0.1", "--port", std::to_string(port), "--api-key", apiKey, "--no-webui"};
  for (auto& x : splitArgs(s.llama.extraArgs)) a.push_back(x);
  return a;
}

}  // namespace tony
