// Tony - portable helpers around speech-to-text: WAV packing and filtering of Whisper's well-known silence hallucinations.
#pragma once
#include <string>
#include <vector>

namespace tony {

// 16 kHz mono float [-1,1] -> 16-bit PCM WAV bytes.
std::string makeWav16k(const std::vector<float>& pcm);

// Whisper "hears" things in silence/noise ("Thanks for watching!", "[BLANK_AUDIO]", music notes...). Returns "" for those.
std::string cleanTranscript(const std::string& text);

// True if `text` starts with the wake phrase (within the first few words), ignoring case and punctuation. Strips it into `rest`.
bool matchWakeWord(const std::string& text, const std::string& phrase, std::string* rest);

}  // namespace tony
