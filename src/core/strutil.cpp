#include "strutil.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace tony {

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool startsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool endsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }
bool containsI(const std::string& hay, const std::string& needle) { return lower(hay).find(lower(needle)) != std::string::npos; }

std::vector<std::string> split(const std::string& s, char delim) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    size_t p = s.find(delim, start);
    if (p == std::string::npos) { out.push_back(s.substr(start)); break; }
    out.push_back(s.substr(start, p - start));
    start = p + 1;
  }
  return out;
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) { s.replace(pos, from.size(), to); pos += to.size(); }
  return s;
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) { if (i) out += sep; out += v[i]; }
  return out;
}

std::vector<std::string> splitArgs(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  bool have = false;
  char q = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (q) {
      if (c == q) q = 0;
      else if (q == '"' && c == '\\' && i + 1 < s.size() && s[i + 1] == '"') { cur += '"'; ++i; }
      else cur += c;
    } else if (c == '"' || c == '\'') { q = c; have = true; }
    else if (std::isspace(static_cast<unsigned char>(c))) { if (have || !cur.empty()) { out.push_back(cur); cur.clear(); have = false; } }
    else cur += c;
  }
  if (have || !cur.empty()) out.push_back(cur);
  return out;
}

std::string truncateMiddle(const std::string& s, size_t maxBytes) {
  if (s.size() <= maxBytes) return s;
  size_t head = maxBytes * 2 / 3, tail = maxBytes - head;
  while (head > 0 && (static_cast<unsigned char>(s[head]) & 0xC0) == 0x80) --head;
  size_t tailStart = s.size() - tail;
  while (tailStart < s.size() && (static_cast<unsigned char>(s[tailStart]) & 0xC0) == 0x80) ++tailStart;
  return s.substr(0, head) + "\n...[" + std::to_string(tailStart - head) + " bytes omitted]...\n" + s.substr(tailStart);
}

std::string sanitizeUtf8(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    bool ok = len > 0 && i + len <= s.size();
    for (size_t k = 1; ok && k < len; ++k) ok = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
    if (ok && len == 2 && c < 0xC2) ok = false;  // overlong
    if (ok && len == 3) {
      unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
      if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) ok = false;  // overlong / surrogates
    }
    if (ok && len == 4) {
      unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
      if (c > 0xF4 || (c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) ok = false;
    }
    if (c == 0) { out += ' '; ++i; continue; }
    if (ok) { out.append(s, i, len); i += len; } else { out += "\xEF\xBF\xBD"; ++i; }
  }
  return out;
}

std::string urlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

std::string urlDecode(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() + 0 && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else if (s[i] == '+') out += ' ';
    else out += s[i];
  }
  return out;
}

std::string urlHost(const std::string& url) {
  size_t p = url.find("://");
  size_t b = (p == std::string::npos) ? 0 : p + 3;
  size_t e = url.find_first_of("/?#", b);
  std::string auth = url.substr(b, e == std::string::npos ? std::string::npos : e - b);
  size_t at = auth.rfind('@');
  if (at != std::string::npos) auth = auth.substr(at + 1);
  if (!auth.empty() && auth[0] == '[') {
    size_t close = auth.find(']');
    return lower(auth.substr(1, close == std::string::npos ? std::string::npos : close - 1));
  }
  size_t colon = auth.find(':');
  if (colon != std::string::npos) auth = auth.substr(0, colon);
  return lower(auth);
}

std::string base64Encode(const unsigned char* d, size_t n) {
  static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) {
    unsigned v = d[i] << 16;
    if (i + 1 < n) v |= d[i + 1] << 8;
    if (i + 2 < n) v |= d[i + 2];
    out += T[(v >> 18) & 63];
    out += T[(v >> 12) & 63];
    out += (i + 1 < n) ? T[(v >> 6) & 63] : '=';
    out += (i + 2 < n) ? T[v & 63] : '=';
  }
  return out;
}
std::string base64Encode(const std::string& s) { return base64Encode(reinterpret_cast<const unsigned char*>(s.data()), s.size()); }

static void nextCodepoint(const std::string& s, size_t& i, unsigned& cp) {
  unsigned char c = static_cast<unsigned char>(s[i]);
  int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
  if (extra < 0 || i + static_cast<size_t>(extra) >= s.size()) { cp = 0xFFFD; ++i; return; }
  cp = extra == 0 ? c : (c & (0x3F >> extra));
  for (int k = 1; k <= extra; ++k) {
    if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) { cp = 0xFFFD; ++i; return; }
    cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
  }
  i += extra + 1;
}

std::string toUtf16LeBytes(const std::string& utf8) {
  std::string out;
  auto put = [&](unsigned u) { out += static_cast<char>(u & 0xFF); out += static_cast<char>((u >> 8) & 0xFF); };
  for (size_t i = 0; i < utf8.size();) {
    unsigned cp;
    nextCodepoint(utf8, i, cp);
    if (cp >= 0x10000) { cp -= 0x10000; put(0xD800 + (cp >> 10)); put(0xDC00 + (cp & 0x3FF)); }
    else put(cp);
  }
  return out;
}

// ---------------------------------------------------------------- HTML
static void appendUtf8(std::string& out, unsigned cp) {
  if (cp < 0x80) out += static_cast<char>(cp);
  else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
  else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
}

std::string decodeHtmlEntities(const std::string& s) {
  static const std::map<std::string, unsigned> named = {
      {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '}, {"hellip", 0x2026},
      {"mdash", 0x2014}, {"ndash", 0x2013}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
      {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"euro", 0x20AC}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"bull", 0x2022}};
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') { out += s[i]; continue; }
    size_t semi = s.find(';', i + 1);
    if (semi == std::string::npos || semi - i > 10) { out += s[i]; continue; }
    std::string name = s.substr(i + 1, semi - i - 1);
    unsigned cp = 0;
    bool ok = false;
    if (!name.empty() && name[0] == '#') {
      char* end = nullptr;
      long v = (name.size() > 1 && (name[1] == 'x' || name[1] == 'X')) ? std::strtol(name.c_str() + 2, &end, 16) : std::strtol(name.c_str() + 1, &end, 10);
      if (end && *end == 0 && v > 0 && v < 0x110000) { cp = static_cast<unsigned>(v); ok = true; }
    } else {
      auto it = named.find(name);
      if (it != named.end()) { cp = it->second; ok = true; }
    }
    if (ok) { appendUtf8(out, cp); i = semi; } else out += s[i];
  }
  return out;
}

static std::string lowerAscii(const std::string& s) {
  std::string o = s;
  for (auto& c : o) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  return o;
}

static std::string stripBlocks(const std::string& html, const char* tag) {
  std::string low = lowerAscii(html), open = std::string("<") + tag, close = std::string("</") + tag;
  std::string out;
  size_t pos = 0;
  for (;;) {
    size_t a = low.find(open, pos);
    // make sure it's really the tag (<script> or <script ...>), not <scripted>
    while (a != std::string::npos) {
      char n = a + open.size() < low.size() ? low[a + open.size()] : '>';
      if (n == '>' || n == ' ' || n == '\t' || n == '\n' || n == '\r' || n == '/') break;
      a = low.find(open, a + 1);
    }
    if (a == std::string::npos) { out.append(html, pos, std::string::npos); break; }
    out.append(html, pos, a - pos);
    size_t c = low.find(close, a);
    if (c == std::string::npos) break;  // unterminated: drop the rest
    size_t gt = low.find('>', c);
    pos = gt == std::string::npos ? html.size() : gt + 1;
  }
  return out;
}

std::string htmlTitle(const std::string& html) {
  std::string low = lowerAscii(html);
  size_t a = low.find("<title");
  if (a == std::string::npos) return "";
  size_t gt = low.find('>', a);
  size_t e = low.find("</title>", gt == std::string::npos ? a : gt);
  if (gt == std::string::npos || e == std::string::npos) return "";
  return trim(decodeHtmlEntities(html.substr(gt + 1, e - gt - 1)));
}

static bool isBlockTag(const std::string& t) {
  static const char* b[] = {"p", "div", "section", "article", "header", "footer", "main", "nav", "aside", "ul", "ol", "table", "tr", "h1", "h2", "h3",
                            "h4", "h5", "h6", "blockquote", "pre", "form", "figure", "figcaption", "dl", "dt", "dd", "hr", "br", "details", "summary"};
  for (auto* x : b) if (t == x) return true;
  return false;
}

static std::string attrValue(const std::string& tagText, const std::string& name) {
  std::string low = lowerAscii(tagText);
  size_t p = 0;
  while ((p = low.find(name, p)) != std::string::npos) {
    bool boundary = p > 0 && std::isspace(static_cast<unsigned char>(low[p - 1]));
    size_t q = p + name.size();
    while (q < low.size() && std::isspace(static_cast<unsigned char>(low[q]))) ++q;
    if (boundary && q < low.size() && low[q] == '=') {
      ++q;
      while (q < tagText.size() && std::isspace(static_cast<unsigned char>(tagText[q]))) ++q;
      if (q >= tagText.size()) return "";
      if (tagText[q] == '"' || tagText[q] == '\'') {
        char qc = tagText[q];
        size_t e = tagText.find(qc, q + 1);
        return tagText.substr(q + 1, e == std::string::npos ? std::string::npos : e - q - 1);
      }
      size_t e = tagText.find_first_of(" \t\r\n>", q);
      return tagText.substr(q, e == std::string::npos ? std::string::npos : e - q);
    }
    p = q;
  }
  return "";
}

static std::string collapseWs(const std::string& in) {
  std::string out;
  bool space = false;
  for (unsigned char c : in) {
    if (std::isspace(c)) space = true;
    else { if (space && !out.empty()) out += ' '; space = false; out += static_cast<char>(c); }
  }
  return out;
}

std::string htmlToText(const std::string& htmlIn, size_t maxChars, std::vector<HtmlLink>* links) {
  std::string html = htmlIn;
  // comments
  for (size_t a; (a = html.find("<!--")) != std::string::npos;) {
    size_t e = html.find("-->", a + 4);
    html.erase(a, e == std::string::npos ? std::string::npos : e + 3 - a);
  }
  for (const char* t : {"script", "style", "noscript", "svg", "head", "template", "iframe"}) html = stripBlocks(html, t);

  std::string out;
  std::string curHref;
  size_t hrefStart = 0;
  for (size_t i = 0; i < html.size();) {
    if (html[i] != '<') { out += html[i++]; continue; }
    // find end of tag, respecting quotes
    size_t j = i + 1;
    char q = 0;
    for (; j < html.size(); ++j) {
      char c = html[j];
      if (q) { if (c == q) q = 0; }
      else if (c == '"' || c == '\'') q = c;
      else if (c == '>') break;
    }
    if (j >= html.size()) break;
    std::string tagText = html.substr(i + 1, j - i - 1);
    i = j + 1;
    bool closing = !tagText.empty() && tagText[0] == '/';
    size_t ns = closing ? 1 : 0, ne = ns;
    while (ne < tagText.size() && (std::isalnum(static_cast<unsigned char>(tagText[ne])))) ++ne;
    std::string name = lowerAscii(tagText.substr(ns, ne - ns));
    if (name.empty()) continue;
    if (name == "a") {
      if (!closing) { curHref = decodeHtmlEntities(attrValue(tagText, "href")); hrefStart = out.size(); }
      else if (!curHref.empty() && links) {
        std::string text = collapseWs(out.substr(hrefStart));
        if (!text.empty() && !startsWith(curHref, "#") && !startsWith(lower(curHref), "javascript:")) links->push_back({decodeHtmlEntities(text), curHref});
        curHref.clear();
      }
    } else if (name == "li" && !closing) out += "\n- ";
    else if (name == "td" || name == "th") out += closing ? " | " : "";
    else if (name == "img" && !closing) {
      std::string alt = attrValue(tagText, "alt");
      if (!alt.empty()) out += " [image: " + alt + "] ";
    } else if (isBlockTag(name) || name == "li") out += "\n";
  }
  out = decodeHtmlEntities(out);

  // collapse whitespace
  std::string res;
  int nl = 0;
  bool space = false;
  for (unsigned char c : out) {
    if (c == '\n') { if (nl < 2) res += '\n'; ++nl; space = false; }
    else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') space = true;
    else {
      if (space && nl == 0 && !res.empty()) res += ' ';
      else if (space && nl > 0) { /* leading indentation dropped */ }
      space = false; nl = 0;
      res += static_cast<char>(c);
    }
  }
  res = trim(res);
  if (res.size() > maxChars) {
    size_t cut = maxChars;
    while (cut > 0 && (static_cast<unsigned char>(res[cut]) & 0xC0) == 0x80) --cut;
    res = res.substr(0, cut) + "\n...[truncated]";
  }
  return res;
}

// ---------------------------------------------------------------- templates
std::string psSingleQuote(const std::string& v) {
  std::string out = "'";
  for (size_t i = 0; i < v.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(v[i]);
    if (c == 0) continue;
    if (c == '\'') { out += "''"; continue; }
    // PowerShell also treats U+2018..U+201B as single-quote characters
    if (c == 0xE2 && i + 2 < v.size() && static_cast<unsigned char>(v[i + 1]) == 0x80 && static_cast<unsigned char>(v[i + 2]) >= 0x98 &&
        static_cast<unsigned char>(v[i + 2]) <= 0x9B) {
      out.append(v, i, 3); out.append(v, i, 3); i += 2; continue;
    }
    out += static_cast<char>(c);
  }
  return out + "'";
}

std::string expandTemplate(const std::string& tmpl, const std::map<std::string, std::string>& values,
                           const std::function<std::string(const std::string&)>& quote) {
  std::string out;
  for (size_t i = 0; i < tmpl.size();) {
    if (tmpl[i] == '{') {
      size_t e = tmpl.find('}', i + 1);
      if (e != std::string::npos) {
        std::string key = tmpl.substr(i + 1, e - i - 1);
        auto it = values.find(key);
        if (it != values.end()) { out += quote ? quote(it->second) : it->second; i = e + 1; continue; }
      }
    }
    out += tmpl[i++];
  }
  return out;
}

// ---------------------------------------------------------------- speech
std::string SentenceStream::cleanForSpeech(const std::string& in) {
  std::string s = in, out;
  // [text](url) -> text ; bare URLs -> "link"
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '[') {
      size_t cb = s.find("](", i);
      size_t cp = cb == std::string::npos ? cb : s.find(')', cb);
      if (cb != std::string::npos && cp != std::string::npos) { out += s.substr(i + 1, cb - i - 1); i = cp + 1; continue; }
    }
    if (s.compare(i, 7, "http://") == 0 || s.compare(i, 8, "https://") == 0) {
      size_t e = s.find_first_of(" \t\r\n)", i);
      out += "link";
      i = e == std::string::npos ? s.size() : e;
      continue;
    }
    out += s[i++];
  }
  // line-leading markdown
  std::string res;
  for (auto& line : split(out, '\n')) {
    std::string l = trim(line);
    while (!l.empty() && (l[0] == '#' || l[0] == '>')) l = trim(l.substr(1));
    if (startsWith(l, "- ") || startsWith(l, "* ") || startsWith(l, "+ ")) l = l.substr(2);
    if (!l.empty()) { if (!res.empty()) res += ' '; res += l; }
  }
  std::string fin;
  for (size_t i = 0; i < res.size(); ++i) {
    char c = res[i];
    if (c == '*' || c == '`' || c == '~' || c == '|') { if (c == '|') fin += ','; continue; }
    if (c == '_' && i + 1 < res.size() && res[i + 1] == '_') continue;
    fin += c;
  }
  return trim(fin);
}

static bool isTerminator(const std::string& s, size_t i, size_t& len) {
  unsigned char c = static_cast<unsigned char>(s[i]);
  if (c == '.' || c == '!' || c == '?') { len = 1; return true; }
  if (i + 2 < s.size()) {
    std::string t = s.substr(i, 3);
    if (t == "\xE2\x80\xA6" || t == "\xE3\x80\x82" || t == "\xEF\xBC\x81" || t == "\xEF\xBC\x9F") { len = 3; return true; }
  }
  return false;
}

void SentenceStream::drain(bool final, std::vector<std::string>& out) {
  auto emit = [&](const std::string& t) {
    std::string c = cleanForSpeech(t);
    if (!c.empty()) out.push_back(c);
  };
  for (;;) {
    if (inFence_) {
      size_t e = buf_.find("```");
      if (e == std::string::npos) {
        size_t keep = 0;  // a trailing partial fence marker must survive until more text arrives
        while (keep < 2 && keep < buf_.size() && buf_[buf_.size() - 1 - keep] == '`') ++keep;
        buf_.erase(0, buf_.size() - keep);
        if (final) buf_.clear();
        return;
      }
      buf_.erase(0, e + 3);
      inFence_ = false;
      continue;
    }
    size_t fence = buf_.find("```");
    size_t limit = fence == std::string::npos ? buf_.size() : fence;
    size_t cut = std::string::npos;
    for (size_t i = 0; i < limit; ++i) {
      if (buf_[i] == '\n') { cut = i + 1; break; }
      size_t tl = 0;
      if (isTerminator(buf_, i, tl)) {
        size_t k = i + tl;
        while (k < limit && (buf_[k] == '.' || buf_[k] == '!' || buf_[k] == '?' || buf_[k] == '"' || buf_[k] == '\'' || buf_[k] == ')')) ++k;
        if (k >= limit) { if (final && fence == std::string::npos) { cut = limit; } break; }  // need lookahead
        if (!std::isspace(static_cast<unsigned char>(buf_[k]))) { i = k - 1; continue; }     // 3.14, e.g.
        // "1." list numbering
        size_t b = i;
        while (b > 0 && !std::isspace(static_cast<unsigned char>(buf_[b - 1]))) --b;
        bool digits = b < i;
        for (size_t x = b; x < i; ++x) if (!std::isdigit(static_cast<unsigned char>(buf_[x]))) digits = false;
        if (buf_[i] == '.' && digits) continue;
        cut = k;
        break;
      }
      if (i >= 240 && (buf_[i] == ',' || buf_[i] == ';' || buf_[i] == ' ')) { cut = i + 1; break; }
    }
    if (cut != std::string::npos) { emit(buf_.substr(0, cut)); buf_.erase(0, cut); continue; }
    if (fence != std::string::npos) { emit(buf_.substr(0, fence)); buf_.erase(0, fence + 3); inFence_ = true; continue; }
    if (final) { emit(buf_); buf_.clear(); }
    return;
  }
}

std::vector<std::string> SentenceStream::push(const std::string& delta) {
  std::vector<std::string> out;
  buf_ += delta;
  drain(false, out);
  return out;
}
std::vector<std::string> SentenceStream::flush() {
  std::vector<std::string> out;
  drain(true, out);
  buf_.clear();
  inFence_ = false;
  return out;
}
void SentenceStream::reset() { buf_.clear(); inFence_ = false; }

// ---------------------------------------------------------------- SSE
void SseParser::feed(const char* data, size_t n, const std::function<void(const std::string&)>& onData) {
  buf_.append(data, n);
  size_t start = 0;
  for (;;) {
    size_t nl = buf_.find('\n', start);
    if (nl == std::string::npos) break;
    std::string line = buf_.substr(start, nl - start);
    start = nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (startsWith(line, "data:")) {
      std::string payload = line.substr(5);
      if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
      onData(payload);
    }
  }
  buf_.erase(0, start);
}

}  // namespace tony
