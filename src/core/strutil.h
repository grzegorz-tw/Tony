// Tony - portable string utilities (no Windows headers, unit-tested on any OS)
#pragma once
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace tony {

// UTF-8 std::string -> filesystem path. Needed on Windows, where narrow paths are interpreted in the ANSI code page.
inline std::filesystem::path u8p(const std::string& s) { return std::filesystem::u8path(s); }

std::string trim(const std::string& s);
std::string lower(std::string s);
bool startsWith(const std::string& s, const std::string& prefix);
bool endsWith(const std::string& s, const std::string& suffix);
bool containsI(const std::string& hay, const std::string& needle);
std::vector<std::string> split(const std::string& s, char delim);
std::string replaceAll(std::string s, const std::string& from, const std::string& to);
std::string join(const std::vector<std::string>& v, const std::string& sep);

// Shell-like tokenizer for "advanced arguments" fields: whitespace separated, "double" and 'single' quotes group, no escapes
// except \" inside double quotes. Never invokes a shell, so the result is safe to pass as argv.
std::vector<std::string> splitArgs(const std::string& s);

// Keep head and tail of an overlong string, marking the cut. Never splits a UTF-8 sequence.
std::string truncateMiddle(const std::string& s, size_t maxBytes);

// Replace invalid UTF-8 with U+FFFD so the text is always safe to put in JSON.
std::string sanitizeUtf8(const std::string& s);

std::string urlEncode(const std::string& s);
std::string urlDecode(const std::string& s);  // %XX and + -> bytes
std::string urlHost(const std::string& url);  // lower-case host without port/userinfo; "" if none
std::string base64Encode(const unsigned char* data, size_t n);
std::string base64Encode(const std::string& s);
std::string toUtf16LeBytes(const std::string& utf8);  // for PowerShell -EncodedCommand

// HTML -> readable text. Collects absolute-ish links when `links` is given.
struct HtmlLink { std::string text, href; };
std::string htmlToText(const std::string& html, size_t maxChars = 20000, std::vector<HtmlLink>* links = nullptr);
std::string htmlTitle(const std::string& html);
std::string decodeHtmlEntities(const std::string& s);

// Fill {placeholders} in a command template. `quote` is applied to each substituted value.
std::string expandTemplate(const std::string& tmpl, const std::map<std::string, std::string>& values,
                           const std::function<std::string(const std::string&)>& quote);
std::string psSingleQuote(const std::string& v);  // 'it''s' style PowerShell literal

// Streaming sentence splitter for text-to-speech. Fenced code blocks are dropped,
// markdown decoration is stripped. Feed deltas; get whole speakable sentences back.
class SentenceStream {
 public:
  std::vector<std::string> push(const std::string& delta);
  std::vector<std::string> flush();
  void reset();
  static std::string cleanForSpeech(const std::string& sentence);

 private:
  std::string buf_;
  bool inFence_ = false;
  void drain(bool final, std::vector<std::string>& out);
};

// Server-Sent-Events parser: feed raw chunks, receive each "data:" payload.
class SseParser {
 public:
  void feed(const char* data, size_t n, const std::function<void(const std::string&)>& onData);

 private:
  std::string buf_;
};

}  // namespace tony
