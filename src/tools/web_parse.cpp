#include "web_parse.h"

#include <nlohmann/json.hpp>
#include <sstream>

#include "core/strutil.h"

namespace tony {

static std::string stripTags(const std::string& s) {
  std::string out;
  bool in = false;
  for (char c : s) {
    if (c == '<') in = true;
    else if (c == '>') in = false;
    else if (!in) out += c;
  }
  return trim(decodeHtmlEntities(out));
}

static std::string hrefOf(const std::string& tag) {
  size_t p = tag.find("href=");
  if (p == std::string::npos) return "";
  p += 5;
  if (p >= tag.size()) return "";
  char q = tag[p];
  if (q == '"' || q == '\'') { size_t e = tag.find(q, p + 1); return tag.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1); }
  size_t e = tag.find_first_of(" >", p);
  return tag.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

static std::string unwrapDdg(std::string href) {
  href = decodeHtmlEntities(href);
  size_t u = href.find("uddg=");
  if (u != std::string::npos) {
    std::string v = href.substr(u + 5);
    size_t amp = v.find('&');
    return urlDecode(v.substr(0, amp));
  }
  if (startsWith(href, "//")) return "https:" + href;
  return href;
}

bool looksLikeBotChallenge(const std::string& html) {
  std::string l = lower(html.substr(0, 20000));
  return (l.find("anomaly") != std::string::npos && l.find("challenge") != std::string::npos) || l.find("unusual traffic") != std::string::npos ||
         l.find("captcha") != std::string::npos;
}

std::vector<SearchHit> parseDuckDuckGoHtml(const std::string& html) {
  std::vector<SearchHit> hits;
  size_t pos = 0;
  while (true) {
    size_t cls = html.find("result__a", pos);
    if (cls == std::string::npos) break;
    size_t open = html.rfind("<a", cls);
    size_t gt = html.find('>', cls);
    size_t close = gt == std::string::npos ? gt : html.find("</a>", gt);
    if (open == std::string::npos || gt == std::string::npos || close == std::string::npos) break;
    SearchHit h;
    h.url = unwrapDdg(hrefOf(html.substr(open, gt - open + 1)));
    h.title = stripTags(html.substr(gt + 1, close - gt - 1));
    pos = close;
    size_t next = html.find("result__a", pos);
    size_t sn = html.find("result__snippet", pos);
    if (sn != std::string::npos && (next == std::string::npos || sn < next)) {
      size_t sgt = html.find('>', sn);
      size_t sclose = sgt == std::string::npos ? sgt : html.find("</a>", sgt);
      if (sclose != std::string::npos) h.snippet = stripTags(html.substr(sgt + 1, sclose - sgt - 1));
    }
    if (startsWith(h.url, "http") && !h.title.empty() && h.url.find("duckduckgo.com/y.js") == std::string::npos) hits.push_back(std::move(h));  // y.js = ads
  }
  return hits;
}

std::vector<SearchHit> parseSearxngJson(const std::string& body) {
  std::vector<SearchHit> hits;
  auto j = nlohmann::json::parse(body, nullptr, false);
  if (j.is_discarded() || !j.contains("results") || !j["results"].is_array()) return hits;
  for (auto& r : j["results"]) {
    SearchHit h;
    h.title = stripTags(r.value("title", ""));
    h.url = r.value("url", "");
    h.snippet = stripTags(r.value("content", ""));
    if (!h.url.empty()) hits.push_back(std::move(h));
  }
  return hits;
}

std::vector<SearchHit> parseBraveJson(const std::string& body) {
  std::vector<SearchHit> hits;
  auto j = nlohmann::json::parse(body, nullptr, false);
  if (j.is_discarded() || !j.contains("web") || !j["web"].contains("results") || !j["web"]["results"].is_array()) return hits;
  for (auto& r : j["web"]["results"]) {
    SearchHit h;
    h.title = stripTags(r.value("title", ""));
    h.url = r.value("url", "");
    h.snippet = stripTags(r.value("description", ""));
    if (!h.url.empty()) hits.push_back(std::move(h));
  }
  return hits;
}

std::string formatHits(const std::vector<SearchHit>& hits, size_t maxHits) {
  std::ostringstream o;
  size_t n = 0;
  for (auto& h : hits) {
    if (n >= maxHits) break;
    o << ++n << ". " << h.title << "\n   " << h.url << "\n";
    if (!h.snippet.empty()) o << "   " << truncateMiddle(h.snippet, 300) << "\n";
  }
  return o.str();
}

}  // namespace tony
