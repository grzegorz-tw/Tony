#include "netpolicy.h"

#include <cstdio>

#include "core/strutil.h"

namespace tony {

static std::function<bool(const std::string&)> g_probe;
void setVpnProbe(std::function<bool(const std::string&)> p) { g_probe = std::move(p); }

bool isLocalHost(const std::string& h) {
  return h == "localhost" || h == "::1" || h == "0.0.0.0" || startsWith(h, "127.") || endsWith(h, ".localhost");
}

bool isPrivateHost(const std::string& h) {
  if (isLocalHost(h)) return true;
  if (h.empty()) return false;
  unsigned a = 0, b = 0, c = 0, d = 0;
  char tail = 0;
  if (std::sscanf(h.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && a < 256 && b < 256 && c < 256 && d < 256) {
    return a == 10 || a == 0 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254) || (a == 100 && b >= 64 && b <= 127);
  }
  if (h.find(':') != std::string::npos) return startsWith(h, "fc") || startsWith(h, "fd") || startsWith(h, "fe80") || startsWith(h, "::ffff:");
  if (h.find('.') == std::string::npos) return true;  // single-label intranet name
  return endsWith(h, ".local") || endsWith(h, ".lan") || endsWith(h, ".internal") || endsWith(h, ".home.arpa") || endsWith(h, ".corp");
}

// socks5:// resolves names locally (DNS leak); socks5h:// lets the proxy resolve them.
static std::string remoteDns(const std::string& proxy) {
  if (startsWith(lower(proxy), "socks5://")) return "socks5h://" + proxy.substr(9);
  return proxy;
}

bool vpnConnected(const VpnSettings& v, std::string* why) {
  if (v.interfaceMatch.empty()) { if (why) *why = "VPN is required but no adapter pattern is set (Settings > Network > VPN)"; return false; }
  if (!g_probe || !g_probe(v.interfaceMatch)) { if (why) *why = "VPN is required but no matching VPN adapter is connected"; return false; }
  return true;
}

bool vpnRequirementMet(const VpnSettings& v, std::string* why) { return !v.requireForWeb || vpnConnected(v, why); }

bool applyNetworkPolicy(const NetworkSettings& ns, HttpRequest& req, std::string* blocked) {
  auto block = [&](const std::string& m) { if (blocked) *blocked = m; return false; };
  const std::string host = urlHost(req.url);
  if (host.empty()) return block("invalid URL");
  if (isLocalHost(host)) { req.noProxy = true; return true; }

  if (req.userAgent.empty()) req.userAgent = ns.userAgent;
  std::string why;
  if (!vpnRequirementMet(ns.vpn, &why)) return block(why);

  if (endsWith(host, ".onion")) {
    if (!ns.tor.enabled && ns.mode != "tor") return block("Tor (.onion) access is turned off. Enable it in Settings > Network and run Tor on the configured SOCKS port.");
    req.proxy = remoteDns(ns.tor.socks);
  } else if (endsWith(host, ".i2p")) {
    if (!ns.i2p.enabled) return block("I2P (.i2p) access is turned off. Enable it in Settings > Network and run i2pd/Java I2P with its HTTP proxy.");
    req.proxy = ns.i2p.proxy;
  } else if (ns.mode == "tor") {
    req.proxy = remoteDns(ns.tor.socks);
  } else if (ns.mode == "proxy") {
    if (ns.proxyUrl.empty()) return block("proxy mode is on but no proxy URL is set (Settings > Network)");
    req.proxy = remoteDns(ns.proxyUrl);
    req.proxyUser = ns.proxyUser;
    req.proxyPass = ns.proxyPass;
  } else {
    req.noProxy = true;
  }
  return true;
}

}  // namespace tony
