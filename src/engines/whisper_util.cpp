#include "whisper_util.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/strutil.h"

namespace tony {

std::string makeWav16k(const std::vector<float>& pcm) {
  const uint32_t n = static_cast<uint32_t>(pcm.size()), dataBytes = n * 2, rate = 16000;
  std::string w;
  w.reserve(44 + dataBytes);
  auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) w += static_cast<char>((v >> (8 * i)) & 0xFF); };
  auto u16 = [&](uint16_t v) { w += static_cast<char>(v & 0xFF); w += static_cast<char>(v >> 8); };
  w += "RIFF"; u32(36 + dataBytes); w += "WAVEfmt "; u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
  w += "data"; u32(dataBytes);
  for (float f : pcm) {
    float c = std::max(-1.0f, std::min(1.0f, f));
    int16_t s = static_cast<int16_t>(std::lrintf(c * 32767.0f));
    u16(static_cast<uint16_t>(s));
  }
  return w;
}

static std::string normalizeWords(const std::string& in) {
  std::string out;
  bool sp = true;
  for (unsigned char c : in) {
    if (std::isalnum(c) || c >= 0x80) { out += static_cast<char>(std::tolower(c)); sp = false; }
    else if (!sp) { out += ' '; sp = true; }
  }
  return trim(out);
}

std::string cleanTranscript(const std::string& text) {
  std::string t = trim(text);
  if (t.empty()) return "";
  // [BLANK_AUDIO], (music), *sigh*, ♪ ... : whole text is a bracketed sound annotation
  std::string stripped = t;
  for (const char* pair : {"[]", "()", "**"}) {
    if (stripped.size() >= 2 && stripped.front() == pair[0] && stripped.back() == pair[1]) return "";
  }
  if (t.find("\xE2\x99\xAA") != std::string::npos && normalizeWords(replaceAll(t, "\xE2\x99\xAA", " ")).empty()) return "";
  static const char* known[] = {"thanks for watching", "thank you for watching", "thank you", "thanks", "bye", "you", "subtitles by the amaraorg community",
                                "subtitles by amaraorg", "please subscribe", "like and subscribe", "okay", "uh", "um", "hmm", "mm"};
  std::string n = normalizeWords(t);
  for (auto* k : known) if (n == k) return "";
  return t;
}

bool matchWakeWord(const std::string& text, const std::string& phrase, std::string* rest) {
  std::string p = normalizeWords(phrase);
  if (p.empty()) return false;
  // compare word by word over the first words of the text
  std::vector<std::string> words;
  std::vector<size_t> ends;  // byte offset after each word in the original text
  size_t i = 0;
  while (i < text.size() && words.size() < 6) {
    while (i < text.size() && !(std::isalnum(static_cast<unsigned char>(text[i])) || static_cast<unsigned char>(text[i]) >= 0x80)) ++i;
    size_t b = i;
    while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || static_cast<unsigned char>(text[i]) >= 0x80)) ++i;
    if (i > b) { words.push_back(lower(text.substr(b, i - b))); ends.push_back(i); }
  }
  auto pw = split(p, ' ');
  static const char* fillers[] = {"hey", "hi", "hello", "ok", "okay", "uh", "um", "so", "yo"};
  auto isFiller = [&](const std::string& w) { for (auto* f : fillers) if (w == f) return true; return false; };
  // The phrase must come first, optionally after greeting fillers ("hey tony", "ok hey tony"); "I talked to Tony" must not trigger.
  for (size_t start = 0; start + pw.size() <= words.size() && start <= 2; ++start) {
    bool ok = true;
    for (size_t k = 0; k < pw.size(); ++k) if (words[start + k] != pw[k]) { ok = false; break; }
    if (ok) {
      if (rest) {
        size_t e = ends[start + pw.size() - 1];
        *rest = trim(text.substr(e));
        while (!rest->empty() && ((*rest)[0] == ',' || (*rest)[0] == '.' || (*rest)[0] == ':' || (*rest)[0] == '!')) { rest->erase(0, 1); *rest = trim(*rest); }
      }
      return true;
    }
    if (!isFiller(words[start])) break;
  }
  return false;
}

}  // namespace tony
