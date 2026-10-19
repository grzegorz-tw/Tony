#include "http.h"

#include <curl/curl.h>

#include <cstdio>
#include <fstream>

#include "core/strutil.h"

namespace tony {

void httpGlobalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
void httpGlobalCleanup() { curl_global_cleanup(); }

namespace {

struct Ctx {
  const HttpWriteFn* onData = nullptr;
  const std::atomic<bool>* cancel = nullptr;
  const HttpProgressFn* progress = nullptr;
  HttpResponse* resp = nullptr;
  size_t maxBytes = 0;
  bool aborted = false;
  long long resumeFrom = 0;
  CURL* h = nullptr;
  const std::function<bool(long)>* onStatus = nullptr;  // called once, when the first body bytes arrive
  bool statusChecked = false;
};

size_t writeCb(char* p, size_t sz, size_t n, void* ud) {
  auto* c = static_cast<Ctx*>(ud);
  size_t len = sz * n;
  if (c->cancel && c->cancel->load()) { c->aborted = true; return 0; }
  if (!c->statusChecked) {
    c->statusChecked = true;
    long code = 0;
    curl_easy_getinfo(c->h, CURLINFO_RESPONSE_CODE, &code);
    if (c->onStatus && *c->onStatus && !(*c->onStatus)(code)) { c->aborted = true; return 0; }
  }
  if (c->onData && *c->onData) {
    if (!(*c->onData)(p, len)) { c->aborted = true; return 0; }
    return len;
  }
  size_t room = c->maxBytes > c->resp->body.size() ? c->maxBytes - c->resp->body.size() : 0;
  if (len > room) { c->resp->body.append(p, room); c->resp->truncated = true; return 0; }
  c->resp->body.append(p, len);
  return len;
}

int xferCb(void* ud, curl_off_t dlTotal, curl_off_t dlNow, curl_off_t, curl_off_t) {
  auto* c = static_cast<Ctx*>(ud);
  if (c->cancel && c->cancel->load()) { c->aborted = true; return 1; }
  if (c->progress && *c->progress) (*c->progress)(static_cast<double>(dlNow + c->resumeFrom), dlTotal > 0 ? static_cast<double>(dlTotal + c->resumeFrom) : 0.0);
  return 0;
}

HttpResponse perform(const HttpRequest& rq, const HttpWriteFn& onData, const std::atomic<bool>* cancel, const HttpProgressFn& progress,
                     long long resumeFrom, const std::function<bool(long)>* onStatus = nullptr) {
  HttpResponse resp;
  CURL* h = curl_easy_init();
  if (!h) { resp.error = "curl init failed"; return resp; }
  char errbuf[CURL_ERROR_SIZE] = {0};
  Ctx ctx;
  ctx.onData = &onData; ctx.cancel = cancel; ctx.progress = &progress; ctx.resp = &resp; ctx.maxBytes = rq.maxBytes; ctx.resumeFrom = resumeFrom; ctx.h = h; ctx.onStatus = onStatus;

  curl_easy_setopt(h, CURLOPT_URL, rq.url.c_str());
  curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");          // never file://, ftp://, gopher:// ...
  curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, rq.followRedirects ? 1L : 0L);
  curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, static_cast<long>(rq.connectTimeoutSec));
  if (rq.timeoutSec > 0) curl_easy_setopt(h, CURLOPT_TIMEOUT, static_cast<long>(rq.timeoutSec));
  curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
  if (!rq.userAgent.empty()) curl_easy_setopt(h, CURLOPT_USERAGENT, rq.userAgent.c_str());

  // Proxy: "direct" really means direct (do not inherit HTTP_PROXY from the environment).
  if (rq.noProxy) { curl_easy_setopt(h, CURLOPT_PROXY, ""); curl_easy_setopt(h, CURLOPT_NOPROXY, "*"); }
  else if (!rq.proxy.empty()) {
    curl_easy_setopt(h, CURLOPT_PROXY, rq.proxy.c_str());
    if (!rq.proxyUser.empty()) {
      curl_easy_setopt(h, CURLOPT_PROXYUSERNAME, rq.proxyUser.c_str());
      curl_easy_setopt(h, CURLOPT_PROXYPASSWORD, rq.proxyPass.c_str());
    }
  } else curl_easy_setopt(h, CURLOPT_PROXY, "");

  curl_slist* hdrs = nullptr;
  for (auto& x : rq.headers) hdrs = curl_slist_append(hdrs, x.c_str());
  if (hdrs) curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

  curl_mime* mime = nullptr;
  if (!rq.form.empty()) {
    mime = curl_mime_init(h);
    for (auto& p : rq.form) {
      curl_mimepart* part = curl_mime_addpart(mime);
      curl_mime_name(part, p.name.c_str());
      curl_mime_data(part, p.value.data(), p.value.size());
      if (p.isFile) {
        if (!p.filename.empty()) curl_mime_filename(part, p.filename.c_str());
        if (!p.mime.empty()) curl_mime_type(part, p.mime.c_str());
      }
    }
    curl_easy_setopt(h, CURLOPT_MIMEPOST, mime);
  } else if (rq.method == "POST" || rq.method == "PUT" || rq.method == "PATCH" || !rq.body.empty()) {
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, rq.body.data());
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(rq.body.size()));
    if (rq.method != "POST") curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, rq.method.c_str());
  } else if (rq.method == "HEAD") curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
  else if (rq.method != "GET") curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, rq.method.c_str());

  if (resumeFrom > 0) curl_easy_setopt(h, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(resumeFrom));

  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, writeCb);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xferCb);
  curl_easy_setopt(h, CURLOPT_XFERINFODATA, &ctx);
  curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);

  CURLcode rc = curl_easy_perform(h);
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &resp.status);
  char* ct = nullptr;
  if (curl_easy_getinfo(h, CURLINFO_CONTENT_TYPE, &ct) == CURLE_OK && ct) resp.contentType = ct;
  char* eff = nullptr;
  if (curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff) resp.finalUrl = eff;

  if (rc != CURLE_OK) {
    if (resp.truncated && rc == CURLE_WRITE_ERROR) { /* intentionally cut: fine */ }
    else if (ctx.aborted) resp.error = "cancelled";
    else resp.error = std::string(errbuf[0] ? errbuf : curl_easy_strerror(rc));
  }
  if (mime) curl_mime_free(mime);
  if (hdrs) curl_slist_free_all(hdrs);
  curl_easy_cleanup(h);
  return resp;
}

}  // namespace

HttpResponse httpPerform(const HttpRequest& req, const HttpWriteFn& onData, const std::atomic<bool>* cancel, const HttpProgressFn& progress) {
  return perform(req, onData, cancel, progress, 0);
}

bool httpDownload(HttpRequest req, const std::string& destPath, const HttpProgressFn& progress, const std::atomic<bool>* cancel, std::string* err) {
  const std::string part = destPath + ".part";
  long long resume = 0;
  {
    std::ifstream f(u8p(part), std::ios::binary | std::ios::ate);
    if (f) resume = static_cast<long long>(f.tellg());
  }
  req.timeoutSec = 0;
  req.maxBytes = 0;

  bool restart = false, httpError = false;
  std::ofstream out;
  HttpResponse r;
  for (int attempt = 0; attempt < 2; ++attempt) {
    restart = false;
    bool opened = false;
    long long from = resume;
    std::function<bool(long)> onStatus = [&](long code) {
      if (code == 200 && from > 0) { restart = true; return false; }   // server ignored the Range header: start over
      if (code >= 400) { httpError = true; return false; }
      return true;
    };
    HttpWriteFn sink = [&](const char* d, size_t n) {
      if (!opened) {
        opened = true;
        out.open(u8p(part), std::ios::binary | (from > 0 ? std::ios::app : std::ios::trunc));
        if (!out) return false;
      }
      out.write(d, static_cast<std::streamsize>(n));
      return out.good();
    };
    r = perform(req, sink, cancel, progress, from, &onStatus);
    if (opened) out.close();
    if (restart) { std::error_code ec; std::filesystem::remove(u8p(part), ec); resume = 0; continue; }
    break;
  }
  if (r.status == 416 && resume > 0) {  // nothing left to fetch: the .part is already complete
    std::error_code ec;
    std::filesystem::rename(u8p(part), u8p(destPath), ec);
    return !ec;
  }
  if (httpError) { if (err) *err = "HTTP " + std::to_string(r.status); return false; }
  if (!r.error.empty()) { if (err) *err = r.error; return false; }
  if (r.status != 200 && r.status != 206) { if (err) *err = "HTTP " + std::to_string(r.status); return false; }
  std::error_code ec;
  std::filesystem::rename(u8p(part), u8p(destPath), ec);
  if (ec) { if (err) *err = "cannot move download into place: " + ec.message(); return false; }
  return true;
}

}  // namespace tony
