#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

#include "core/strutil.h"
#include "net/http.h"
#include "net/netpolicy.h"
#include "platform/winutil.h"
#include "tools.h"
#include "web_parse.h"

namespace tony {

struct Aria2Daemon {
  ManagedProcess proc;
  int port = 0;
  std::string secret, fingerprint;

  bool ensure(Services& svc, std::string* err) {
    const TorrentSettings& t = svc.settings->network.torrent;
    if (!fileExists(t.aria2Path)) { *err = "aria2c was not found at:\n" + t.aria2Path + "\nRun scripts\\setup.ps1 or set the path in Settings > Network > Torrents."; return false; }
    std::string fp = t.aria2Path + "|" + t.downloadDir + "|" + t.bindInterface + "|" + std::to_string(t.maxDownKBs) + "|" + (t.seed ? "seed" : "noseed");
    if (proc.running() && fp == fingerprint) return true;
    proc.stop();
    port = findFreePort(t.rpcPort);
    secret = randomHex(16);
    ensureDir(t.downloadDir);
    std::vector<std::string> a = {"--enable-rpc", "--rpc-listen-port=" + std::to_string(port), "--rpc-secret=" + secret, "--rpc-listen-all=false",
                                  "--dir=" + t.downloadDir, "--file-allocation=none", "--console-log-level=warn", "--summary-interval=0",
                                  "--max-concurrent-downloads=3", "--auto-file-renaming=true", "--allow-overwrite=false", "--bt-save-metadata=false"};
    if (!t.seed) a.push_back("--seed-time=0");
    if (!t.bindInterface.empty()) a.push_back("--interface=" + t.bindInterface);
    if (t.maxDownKBs > 0) a.push_back("--max-overall-download-limit=" + std::to_string(t.maxDownKBs) + "K");
    ensureDir(joinPath(svc.paths.cacheDir, "logs"));
    if (!proc.start(t.aria2Path, a, parentDir(t.aria2Path), joinPath(joinPath(svc.paths.cacheDir, "logs"), "aria2.log"), err)) return false;
    fingerprint = fp;
    for (int i = 0; i < 40; ++i) {  // wait for the RPC port
      json r; std::string e;
      if (rpc("aria2.getVersion", json::array(), &r, &e)) return true;
      if (!proc.running()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    *err = "aria2c did not start (see logs\\aria2.log).";
    return false;
  }

  bool rpc(const std::string& method, json params, json* result, std::string* err) {
    params.insert(params.begin(), "token:" + secret);
    HttpRequest r;
    r.method = "POST";
    r.url = "http://127.0.0.1:" + std::to_string(port) + "/jsonrpc";
    r.noProxy = true;
    r.timeoutSec = 15;
    r.headers = {"Content-Type: application/json"};
    r.body = json{{"jsonrpc", "2.0"}, {"id", "tony"}, {"method", method}, {"params", params}}.dump(-1, ' ', false, json::error_handler_t::replace);
    HttpResponse h = httpPerform(r);
    json j = json::parse(h.body, nullptr, false);
    if (j.is_discarded()) { if (err) *err = h.error.empty() ? "no answer from aria2" : h.error; return false; }
    if (j.contains("error")) { if (err) *err = j["error"].value("message", "aria2 error"); return false; }
    if (result) *result = j.value("result", json());
    return true;
  }
};

namespace {

bool isExecutableName(const std::string& n) {
  std::string l = lower(n);
  for (const char* e : {".exe", ".msi", ".bat", ".cmd", ".ps1", ".scr", ".vbs", ".js", ".jar", ".lnk", ".dll", ".com"}) if (endsWith(l, e)) return true;
  return false;
}

// ------------------------------------------------------------------ web_search
class WebSearchTool : public Tool {
 public:
  explicit WebSearchTool(Services& s) : svc_(s) {}
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "web_search";
    t.description = "Search the web. Returns titles, URLs and snippets; follow up with fetch_url to read a page. Use for anything that may have changed recently or that you are unsure about.";
    t.parameters = {{"type", "object"}, {"properties", {{"query", {{"type", "string"}}}, {"count", {{"type", "integer"}, {"description", "Results, default 8"}}}}}, {"required", json::array({"query"})}};
    return t;
  }
  Assessment assess(const json& a, ToolContext&) override {
    Assessment r;
    r.summary = "Search the web";
    r.detail = argStr(a, "query");
    return r;
  }
  ToolResult run(const json& a, ToolContext& ctx) override {
    const NetworkSettings& ns = ctx.settings.network;
    std::string q = trim(argStr(a, "query"));
    if (q.empty()) return ToolResult::error("empty query");
    size_t n = static_cast<size_t>(std::max(1, std::min(argInt(a, "count", ns.search.results), 20)));
    HttpRequest r;
    r.timeoutSec = 25;
    r.headers = {"Accept-Language: en-US,en;q=0.8"};
    const std::string prov = ns.search.provider;
    if (prov == "searxng") {
      if (ns.search.searxngUrl.empty()) return ToolResult::error("SearXNG is selected but no URL is set (Settings > Network > Search).");
      std::string base = ns.search.searxngUrl;
      while (!base.empty() && base.back() == '/') base.pop_back();
      r.url = base + "/search?format=json&q=" + urlEncode(q);
    } else if (prov == "brave") {
      if (ns.search.braveKey.empty()) return ToolResult::error("Brave Search is selected but no API key is set (Settings > Network > Search).");
      r.url = "https://api.search.brave.com/res/v1/web/search?count=" + std::to_string(n) + "&q=" + urlEncode(q);
      r.headers.push_back("X-Subscription-Token: " + ns.search.braveKey);
      r.headers.push_back("Accept: application/json");
    } else {
      r.url = "https://html.duckduckgo.com/html/?q=" + urlEncode(q);
    }
    std::string blocked;
    if (!applyNetworkPolicy(ns, r, &blocked)) return ToolResult::error("Blocked: " + blocked);
    HttpResponse h = httpPerform(r, {}, &ctx.cancel);
    if (!h.error.empty()) return ToolResult::error("Search failed: " + h.error);
    std::vector<SearchHit> hits;
    if (prov == "searxng") hits = parseSearxngJson(h.body);
    else if (prov == "brave") hits = parseBraveJson(h.body);
    else {
      if (looksLikeBotChallenge(h.body) || h.status == 202) return ToolResult::error("DuckDuckGo asked for a human check. Switch to SearXNG or Brave in Settings > Network > Search for reliable results.");
      hits = parseDuckDuckGoHtml(h.body);
    }
    if (h.status >= 400) return ToolResult::error("The search service answered HTTP " + std::to_string(h.status) + (h.status == 403 && prov == "searxng" ? " (does the instance allow format=json?)" : ""));
    if (hits.empty()) return ToolResult::ok("No results for: " + q);
    ToolResult out = ToolResult::ok(formatHits(hits, n));
    out.untrusted = true;
    out.source = "web search results";
    return out;
  }

 private:
  Services& svc_;
};

// ------------------------------------------------------------------ fetch_url
class FetchUrlTool : public Tool {
 public:
  explicit FetchUrlTool(Services& s) : svc_(s) {}
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "fetch_url";
    t.description =
        "Download a web page (or text/JSON file) and return its readable text plus main links. Works for normal sites and, when enabled in Settings, "
        ".onion (Tor) and .i2p (I2P) addresses. Read-only: it cannot log in or submit forms.";
    t.parameters = {{"type", "object"},
                    {"properties", {{"url", {{"type", "string"}}}, {"max_chars", {{"type", "integer"}, {"description", "Text limit, default 20000"}}}, {"links", {{"type", "boolean"}, {"description", "Include a list of links. Default true"}}}}},
                    {"required", json::array({"url"})}};
    return t;
  }
  Assessment assess(const json& a, ToolContext&) override {
    std::string url = argStr(a, "url");
    Assessment r;
    r.summary = "Open web page";
    r.detail = url;
    std::string l = lower(url);
    if (!startsWith(l, "http://") && !startsWith(l, "https://")) return Assessment::deny("Only http:// and https:// addresses are supported.");
    if (isPrivateHost(urlHost(url))) {
      r.level = Assessment::Level::Ask;
      r.risk = "medium";
      r.reason = "This address is on your own computer or local network (router, NAS, services).";
    }
    return r;
  }
  ToolResult run(const json& a, ToolContext& ctx) override {
    std::string url = argStr(a, "url");
    HttpRequest r;
    r.url = url;
    r.timeoutSec = 40;
    r.maxBytes = 4u << 20;
    r.headers = {"Accept: text/html,application/xhtml+xml,text/plain,application/json;q=0.9,*/*;q=0.5", "Accept-Language: en-US,en;q=0.8"};
    std::string blocked;
    if (!applyNetworkPolicy(ctx.settings.network, r, &blocked)) return ToolResult::error("Blocked: " + blocked);
    HttpResponse h = httpPerform(r, {}, &ctx.cancel);
    if (!h.error.empty()) return ToolResult::error("Could not load the page: " + h.error);
    size_t maxc = static_cast<size_t>(std::max(500, std::min(argInt(a, "max_chars", ctx.settings.network.fetchMaxChars), 100000)));
    std::string ct = lower(h.contentType);
    std::ostringstream o;
    o << "URL: " << h.finalUrl << "\nStatus: " << h.status << "\n";
    bool wantLinks = !(a.contains("links") && a["links"].is_boolean() && !a["links"].get<bool>());
    if (ct.find("html") != std::string::npos || (ct.empty() && h.body.find("<html") != std::string::npos)) {
      std::vector<HtmlLink> links;
      std::string title = htmlTitle(h.body);
      if (!title.empty()) o << "Title: " << title << "\n";
      o << "\n" << htmlToText(h.body, maxc, &links) << "\n";
      if (wantLinks && !links.empty()) {
        o << "\nLinks:\n";
        std::string base = h.finalUrl;
        std::string scheme = base.substr(0, base.find("://") == std::string::npos ? 0 : base.find("://") + 3);
        std::string origin = scheme + urlHost(base);
        size_t k = 0;
        for (auto& l : links) {
          if (k++ >= 25) break;
          std::string href = l.href;
          if (startsWith(href, "//")) href = (scheme.empty() ? "https://" : scheme) + href.substr(2);
          else if (startsWith(href, "/")) href = origin + href;
          o << "- " << truncateMiddle(l.text, 80) << " -> " << href << "\n";
        }
      }
    } else if (ct.find("text") != std::string::npos || ct.find("json") != std::string::npos || ct.find("xml") != std::string::npos || ct.find("javascript") != std::string::npos) {
      o << "\n" << truncateMiddle(sanitizeUtf8(h.body), maxc) << "\n";
    } else {
      o << "\nBinary content (" << (ct.empty() ? "unknown type" : ct) << ", " << humanSize(h.body.size()) << (h.truncated ? "+" : "") << "). Use download_file to save it.\n";
    }
    if (h.truncated) o << "(page was cut at " << humanSize(r.maxBytes) << ")\n";
    ToolResult out = ToolResult::ok(o.str());
    out.isError = h.status >= 400;
    out.untrusted = true;
    out.source = h.finalUrl.empty() ? url : h.finalUrl;
    return out;
  }

 private:
  Services& svc_;
};

// ------------------------------------------------------------------ download_file
class DownloadFileTool : public Tool {
 public:
  explicit DownloadFileTool(Services& s) : svc_(s) {}
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "download_file";
    t.description = "Download a file from an http(s) URL into an allowed folder (dest may be a folder or a full file path). Does not run it.";
    t.parameters = {{"type", "object"}, {"properties", {{"url", {{"type", "string"}}}, {"dest", {{"type", "string"}, {"description", "Folder or file path. Default: Downloads folder"}}}}}, {"required", json::array({"url"})}};
    return t;
  }
  std::string target(const json& a, std::string* err) {
    std::string url = argStr(a, "url");
    std::string d = argStr(a, "dest", svc_.paths.downloadsDir);
    auto p = resolvePath(svc_, d, true);
    if (!p.ok) { *err = p.error; return ""; }
    if (dirExists(p.path)) {
      std::string path = url.substr(0, url.find_first_of("?#"));
      std::string name = urlDecode(path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1));
      for (auto& c : name) if (std::string("<>:\"/\\|?*").find(c) != std::string::npos) c = '_';
      if (name.empty() || name == "..") name = "download";
      auto full = resolvePath(svc_, joinPath(p.path, name), true);
      if (!full.ok) { *err = full.error; return ""; }
      return full.path;
    }
    return p.path;
  }
  Assessment assess(const json& a, ToolContext&) override {
    std::string url = argStr(a, "url"), err;
    if (!startsWith(lower(url), "http://") && !startsWith(lower(url), "https://")) return Assessment::deny("Only http:// and https:// addresses are supported.");
    std::string t = target(a, &err);
    if (t.empty()) return Assessment::deny(err);
    Assessment r;
    r.level = Assessment::Level::Ask;
    r.summary = "Download a file";
    r.detail = url + "\n-> " + t;
    r.risk = isExecutableName(t) ? "high" : "medium";
    if (isExecutableName(t)) { r.alwaysAsk = true; r.reason = "This is a program file. Tony will not run it, but be sure you trust the source."; }
    else if (isPrivateHost(urlHost(url))) r.reason = "The address is on your local network.";
    return r;
  }
  ToolResult run(const json& a, ToolContext& ctx) override {
    std::string err, dest = target(a, &err);
    if (dest.empty()) return ToolResult::error(err);
    HttpRequest r;
    r.url = argStr(a, "url");
    std::string blocked;
    if (!applyNetworkPolicy(ctx.settings.network, r, &blocked)) return ToolResult::error("Blocked: " + blocked);
    ensureDir(parentDir(dest));
    double lastShown = -1;
    bool ok = httpDownload(r, dest, [&](double done, double total) {
      if (ctx.progress && done - lastShown > 2e6) { lastShown = done; ctx.progress("Downloaded " + humanSize(static_cast<unsigned long long>(done)) + (total > 0 ? " of " + humanSize(static_cast<unsigned long long>(total)) : "")); }
    }, &ctx.cancel, &err);
    if (!ok) return ToolResult::error("Download failed: " + err);
    return ToolResult::ok("Saved to " + dest);
  }

 private:
  Services& svc_;
};

// ------------------------------------------------------------------ torrent
class TorrentTool : public Tool {
 public:
  explicit TorrentTool(Services& s) : svc_(s) {}
  bool defaultEnabled() const override { return false; }
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "torrent";
    t.description = "Manage BitTorrent downloads through aria2. action=add (magnet link, .torrent URL or local .torrent path), list, remove (by gid). Only use for content the user is entitled to download.";
    t.parameters = {{"type", "object"},
                    {"properties", {{"action", {{"type", "string"}, {"enum", json::array({"add", "list", "remove"})}}},
                                    {"source", {{"type", "string"}, {"description", "For add"}}},
                                    {"dir", {{"type", "string"}, {"description", "Download folder for add. Default from Settings"}}},
                                    {"gid", {{"type", "string"}, {"description", "For remove"}}}}},
                    {"required", json::array({"action"})}};
    return t;
  }
  Assessment assess(const json& a, ToolContext& ctx) override {
    std::string act = argStr(a, "action");
    Assessment r;
    r.summary = "Torrent: " + act;
    if (act == "list") return r;
    r.level = Assessment::Level::Ask;
    r.risk = "high";
    r.alwaysAsk = true;
    if (act == "add") {
      std::string dir = argStr(a, "dir", ctx.settings.network.torrent.downloadDir);
      auto p = resolvePath(svc_, dir, true);
      if (!p.ok) return Assessment::deny(p.error);
      r.detail = argStr(a, "source") + "\n-> " + p.path;
      r.reason = "Torrents exchange data with other people's computers, who can see your IP address.";
      if (ctx.settings.network.torrent.bindInterface.empty() && !ctx.settings.network.vpn.requireForTorrent) r.reason += " No VPN/interface binding is configured.";
    } else r.detail = "gid " + argStr(a, "gid");
    return r;
  }
  ToolResult run(const json& a, ToolContext& ctx) override {
    std::string act = argStr(a, "action"), err;
    if (act == "add" && ctx.settings.network.vpn.requireForTorrent) {
      if (!vpnConnected(ctx.settings.network.vpn, &err)) return ToolResult::error("Blocked: " + err + ". (Settings > Network > VPN)");
    }
    if (!svc_.aria) svc_.aria = std::make_shared<Aria2Daemon>();
    if (!svc_.aria->ensure(svc_, &err)) return ToolResult::error(err);
    Aria2Daemon& d = *svc_.aria;
    json res;
    if (act == "add") {
      std::string src = trim(argStr(a, "source"));
      auto p = resolvePath(svc_, argStr(a, "dir", ctx.settings.network.torrent.downloadDir), true);
      if (!p.ok) return ToolResult::error(p.error);
      json opts = {{"dir", p.path}};
      bool ok;
      if (startsWith(lower(src), "magnet:") || startsWith(lower(src), "http://") || startsWith(lower(src), "https://")) ok = d.rpc("aria2.addUri", json::array({json::array({src}), opts}), &res, &err);
      else {
        auto tp = resolvePath(svc_, src, false);
        if (!tp.ok) return ToolResult::error(tp.error);
        std::string bytes = readTextFile(tp.path, 16u << 20);
        if (bytes.empty()) return ToolResult::error("Cannot read " + tp.path);
        ok = d.rpc("aria2.addTorrent", json::array({base64Encode(bytes), json::array(), opts}), &res, &err);
      }
      return ok ? ToolResult::ok("Added. gid=" + res.get<std::string>() + " -> " + p.path) : ToolResult::error("aria2: " + err);
    }
    if (act == "remove") {
      std::string gid = argStr(a, "gid");
      bool ok = d.rpc("aria2.remove", json::array({gid}), &res, &err) || d.rpc("aria2.removeDownloadResult", json::array({gid}), &res, &err);
      return ok ? ToolResult::ok("Removed " + gid) : ToolResult::error("aria2: " + err);
    }
    if (act == "list") {
      json keys = json::array({"gid", "status", "totalLength", "completedLength", "downloadSpeed", "bittorrent", "files"});
      std::ostringstream o;
      int n = 0;
      for (auto* m : {"aria2.tellActive", "aria2.tellWaiting", "aria2.tellStopped"}) {
        json r;
        json params = std::string(m) == "aria2.tellActive" ? json::array({keys}) : json::array({0, 20, keys});
        if (!d.rpc(m, params, &r, &err) || !r.is_array()) continue;
        for (auto& t : r) {
          double total = std::atof(t.value("totalLength", "0").c_str()), done = std::atof(t.value("completedLength", "0").c_str());
          std::string name = t.contains("bittorrent") && t["bittorrent"].contains("info") ? t["bittorrent"]["info"].value("name", "") : "";
          if (name.empty() && t.contains("files") && t["files"].is_array() && !t["files"].empty()) name = fileName(t["files"][0].value("path", ""));
          char pct[16];
          std::snprintf(pct, sizeof(pct), "%.0f%%", total > 0 ? 100.0 * done / total : 0.0);
          o << t.value("gid", "") << "  " << t.value("status", "") << "  " << pct << "  " << humanSize(static_cast<unsigned long long>(std::atof(t.value("downloadSpeed", "0").c_str()))) << "/s  " << sanitizeUtf8(name) << "\n";
          ++n;
        }
      }
      return ToolResult::ok(n ? o.str() : "(no torrents)");
    }
    return ToolResult::error("Unknown action.");
  }

 private:
  Services& svc_;
};

}  // namespace

void registerWebTools(ToolRegistry& reg, Services& svc) {
  reg.add(std::make_unique<WebSearchTool>(svc));
  reg.add(std::make_unique<FetchUrlTool>(svc));
  reg.add(std::make_unique<DownloadFileTool>(svc));
  reg.add(std::make_unique<TorrentTool>(svc));
}

}  // namespace tony
