// Tony - tiny adaptive energy VAD used for hands-free mode and push-to-talk trimming.
// Deliberately simple: Whisper does the heavy lifting, this only decides "someone started/stopped talking".
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace tony {

class EnergyVad {
 public:
  enum class Event { None, SpeechStart, SpeechEnd };

  explicit EnergyVad(int sampleRate = 16000) : sr_(sampleRate) {}

  // 0 = needs loud, close speech; 1 = triggers on quiet speech (and more noise)
  void setSensitivity(float s) { sens_ = std::min(1.0f, std::max(0.0f, s)); }
  void setHangMs(int ms) { hangMs_ = ms; }
  bool inSpeech() const { return speech_; }
  float noiseFloor() const { return noise_; }
  void reset() { speech_ = false; voicedMs_ = silenceMs_ = 0; }

  // Feed one frame (10-50 ms recommended). Returns a state change, if any.
  Event process(const float* x, size_t n) {
    if (n == 0) return Event::None;
    double acc = 0;
    for (size_t i = 0; i < n; ++i) acc += static_cast<double>(x[i]) * x[i];
    float rms = static_cast<float>(std::sqrt(acc / n));
    float ms = 1000.0f * n / sr_;

    float factor = 6.0f - 3.5f * sens_;        // 6x .. 2.5x above the noise floor
    float absMin = 0.020f - 0.014f * sens_;    // never trigger below this level
    float on = std::max(noise_ * factor, absMin), off = on * 0.5f;

    if (!speech_) {
      noise_ = std::max(0.0005f, noise_ * 0.95f + rms * 0.05f);  // adapt only while quiet
      if (rms > on) voicedMs_ += ms; else voicedMs_ = std::max(0.0f, voicedMs_ - ms);
      if (voicedMs_ >= minSpeechMs_) { speech_ = true; silenceMs_ = 0; return Event::SpeechStart; }
    } else {
      if (rms < off) silenceMs_ += ms; else silenceMs_ = 0;
      if (silenceMs_ >= hangMs_) { speech_ = false; voicedMs_ = 0; silenceMs_ = 0; return Event::SpeechEnd; }
    }
    return Event::None;
  }

 private:
  int sr_;
  float sens_ = 0.5f;
  float noise_ = 0.004f;
  float voicedMs_ = 0, silenceMs_ = 0;
  float minSpeechMs_ = 120;
  float hangMs_ = 800;
  bool speech_ = false;
};

}  // namespace tony
