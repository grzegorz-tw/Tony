// Tony - voice I/O: microphone capture with push-to-talk / hands-free VAD, and text-to-speech (Windows SAPI or offline Piper).
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/settings.h"

namespace tony {

class VoiceInput {
 public:
  using SegmentFn = std::function<void(std::vector<float>&& pcm16kMono)>;  // called from the audio thread: hand off quickly
  VoiceInput();
  ~VoiceInput();

  bool start(const VoiceSettings& v, SegmentFn onSegment, std::function<void()> onSpeechStart, std::string* err);
  void stop();
  void apply(const VoiceSettings& v);  // live: mode, key, sensitivity, wake word...
  void setMuted(bool muted);           // half-duplex: ignore the mic while Tony talks
  bool active() const;
  bool capturing() const;              // currently inside a spoken segment
  float level() const;                 // 0..1 smoothed input level for the UI meter
  static std::vector<std::string> devices();

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

class TtsService {
 public:
  TtsService();
  ~TtsService();
  void apply(const TtsSettings& t, const AppPaths& p);
  void speak(const std::string& sentence);  // queued, spoken in order
  void stop();                              // drop the queue and cut off the current sentence
  bool speaking() const;
  std::string lastError() const;

  static std::vector<std::string> sapiVoices();                       // installed SAPI5 voice names
  static std::vector<std::string> piperVoices(const std::string& dir);  // *.onnx files under dir

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

}  // namespace tony
