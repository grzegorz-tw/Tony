#include "cmdrisk.h"

#include <regex>

#include "strutil.h"

namespace tony {

const char* riskName(Risk r) {
  switch (r) {
    case Risk::ReadOnly: return "read-only";
    case Risk::Normal: return "normal";
    default: return "dangerous";
  }
}

namespace {
struct Pattern { const char* re; const char* why; };

const Pattern kDangerous[] = {
    {R"((^|[\s;|&(])(remove-item|ri|rm|del|erase|rd|rmdir)\b[^;|]*(-recurse|-r\b|\s/s\b|-force))", "recursive or forced delete"},
    {R"(\bformat-volume\b|\bformat\s+[a-z]:|\bclear-disk\b|\bdiskpart\b|\binitialize-disk\b|\bremove-partition\b)", "disk formatting or partitioning"},
    {R"(\breg(\.exe)?\s+(delete|add|import|load|save|restore)\b)", "registry modification"},
    {R"(\b(set|new|remove|clear)-itemproperty\b[^;|]*(hk(lm|cu|cr|u|cc)|registry::))", "registry modification"},
    {R"(\bset-executionpolicy\b)", "changes the execution policy"},
    {R"(\b(invoke-expression|iex)\b)", "executes dynamically built code"},
    {R"(-e(nc|ncodedcommand)?\s+[a-z0-9+/=]{20,})", "encoded (hidden) command"},
    {R"(\bdownload(string|file|data)\b)", "downloads content that may be executed"},
    {R"(\b(shutdown|restart-computer|stop-computer)\b)", "shuts down or restarts the PC"},
    {R"(\bbcdedit\b|\bvssadmin\b|\bwbadmin\b|\bcipher\s+/w)", "boot/recovery tampering"},
    {R"(\bnet(\.exe)?\s+(user|localgroup|accounts)\b|\bnew-localuser\b|\badd-localgroupmember\b)", "user account changes"},
    {R"(\bschtasks(\.exe)?\s+/create|\bregister-scheduledtask\b|\bnew-service\b|\bsc(\.exe)?\s+(create|config)\b)", "installs a service or scheduled task"},
    {R"(\b(add|set)-mppreference\b|\bdisable-windowsoptionalfeature\b)", "changes security features"},
    {R"(start-process\b[^;|]*-verb\s+runas)", "requests administrator elevation"},
    {R"(\bstop-process\b[^;|]*-force|\btaskkill(\.exe)?\b[^;|]*/f)", "force-kills processes"},
    {R"(\badd-type\b|\[system\.reflection\.assembly\]::load|\bnew-object\s+-comobject\b)", "loads native/.NET code"},
    {R"((set-content|out-file|add-content|copy-item|move-item|new-item|>)[^;|]*(\\windows\\|system32|program files))", "writes into protected Windows folders"},
};

const Pattern kNetwork[] = {
    {R"(\b(invoke-webrequest|iwr|invoke-restmethod|irm|curl|wget|start-bitstransfer|test-netconnection)\b)", "makes network requests"},
    {R"(\bnew-object\s+(system\.)?net\.webclient\b|\[net\.webclient\])", "makes network requests"},
};

bool matches(const char* re, const std::string& s) {
  try { return std::regex_search(s, std::regex(re, std::regex::ECMAScript | std::regex::icase)); }
  catch (const std::regex_error&) { return false; }
}

bool segmentIsReadOnly(const std::string& seg) {
  std::string s = lower(trim(seg));
  if (s.empty()) return true;
  std::string tok = s.substr(0, s.find_first_of(" \t"));
  static const char* exact[] = {"select-object", "sort-object", "measure-object", "group-object", "format-table", "format-list", "out-string",
                                "convertto-json", "convertfrom-json", "convertto-csv", "test-path", "resolve-path", "split-path", "join-path",
                                "write-output", "write-host", "echo", "dir", "ls", "cat", "type", "pwd", "hostname", "whoami", "systeminfo",
                                "ipconfig", "tasklist", "where.exe", "ver", "date", "nvidia-smi", "findstr", "select-string", "compare-object",
                                "wmic", "driverquery", "netstat", "nslookup", "ping"};
  if (startsWith(tok, "get-")) return true;
  for (auto* e : exact) if (tok == e) return true;
  if (tok == "git") {
    std::string rest = trim(s.substr(3));
    for (const char* sub : {"status", "log", "diff", "branch", "show", "remote", "rev-parse", "ls-files", "--version", "describe", "blame"})
      if (startsWith(rest, sub)) return true;
  }
  return false;
}
}  // namespace

RiskReport classifyPowerShell(const std::string& command) {
  RiskReport r;
  for (auto& p : kDangerous)
    if (matches(p.re, command)) { r.risk = Risk::Dangerous; r.reasons.push_back(p.why); }
  if (r.risk == Risk::Dangerous) return r;

  bool network = false;
  for (auto& p : kNetwork)
    if (matches(p.re, command)) { network = true; r.reasons.push_back(p.why); break; }

  bool readOnly = !network;
  // constructs that make a "harmless-looking" command able to do anything
  for (const char* bad : {">", "$(", "`", "{", "::", "&", "="})
    if (command.find(bad) != std::string::npos) { readOnly = false; break; }
  if (readOnly) {
    std::string flat = replaceAll(replaceAll(replaceAll(command, "&&", ";"), "||", ";"), "\n", ";");
    flat = replaceAll(flat, "|", ";");
    for (auto& seg : split(flat, ';'))
      if (!segmentIsReadOnly(seg)) { readOnly = false; break; }
  }
  r.risk = readOnly ? Risk::ReadOnly : Risk::Normal;
  if (!readOnly && r.reasons.empty()) r.reasons.push_back("can change files, settings or processes");
  return r;
}

}  // namespace tony
