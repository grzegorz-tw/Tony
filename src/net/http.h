// Tony - HTTP client (libcurl). Used for web tools, the OpenAI-compatible LLM API, whisper, model downloads, aria2 RPC.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace tony {

struct HttpRequest {
  std::string method = "GET";
  std::string url;
  std::vector<std::string> headers;
  std::string body;

  struct Part { std::string name, value, filename, mime; bool isFile = false; };
  std::vector<Part> form;  // when non-empty the request is multipart/form-data

  int timeoutSec = 60;       // total time (0 = none, used for streaming LLM replies)
  int connectTimeoutSec = 15;
  size_t maxBytes = 8u << 20;  // buffered responses are cut here (truncated=true)
  bool followRedirects = true;

  std::string proxy;      // "" = none. http://h:p | socks5h://h:p
  std::string proxyUser, proxyPass;
  bool noProxy = false;   // force a direct connection (local engines)
  std::string userAgent;
};

struct HttpResponse {
  long status = 0;
  std::string body, contentType, finalUrl, error;
  bool truncated = false;
  bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

using HttpWriteFn = std::function<bool(const char* data, size_t n)>;  // return false to abort
using HttpProgressFn = std::function<void(double done, double total)>;

void httpGlobalInit();
void httpGlobalCleanup();

// If `onData` is set the body is streamed to it (and not buffered); otherwise buffered up to maxBytes.
HttpResponse httpPerform(const HttpRequest& req, const HttpWriteFn& onData = {}, const std::atomic<bool>* cancel = nullptr,
                         const HttpProgressFn& progress = {});

// Download to a file, resuming a partial "<dest>.part" when the server supports ranges.
bool httpDownload(HttpRequest req, const std::string& destPath, const HttpProgressFn& progress, const std::atomic<bool>* cancel, std::string* err);

}  // namespace tony
