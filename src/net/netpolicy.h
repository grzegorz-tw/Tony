// Tony - decides how a request leaves the machine: direct, user proxy/VPN-gated, Tor (.onion) or I2P (.i2p).
// Local engine traffic (llama-server, whisper-server, aria2 RPC) is never proxied.
#pragma once
#include <functional>
#include <string>

#include "core/settings.h"
#include "http.h"

namespace tony {

bool isLocalHost(const std::string& host);
// RFC1918 / link-local / loopback / intranet-style names. Tools ask before touching these (router pages, NAS, cloud metadata...).
bool isPrivateHost(const std::string& host);

// Windows layer installs this; it returns true when a network adapter matching the regex is up.
void setVpnProbe(std::function<bool(const std::string& regex)> probe);

// Fills proxy/user-agent fields. Returns false (with a reason for the model/user) if policy forbids the request.
bool applyNetworkPolicy(const NetworkSettings& ns, HttpRequest& req, std::string* blockedReason);

// True when a VPN adapter matching the pattern is up (ignores requireForWeb; used by the torrent tool).
bool vpnConnected(const VpnSettings& v, std::string* why = nullptr);

// True when the VPN requirement (if enabled) is currently satisfied.
bool vpnRequirementMet(const VpnSettings& v, std::string* why = nullptr);

}  // namespace tony
