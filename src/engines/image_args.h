// Tony - builds the stable-diffusion.cpp (sd-cli) command line for an image job. Portable and unit-tested.
#pragma once
#include <string>
#include <vector>

#include "core/settings.h"

namespace tony {

struct ImageJob {
  std::string mode = "generate";   // generate | edit
  std::string prompt, negative;
  std::string initImage;           // img2img / inpaint source
  std::string maskImage;           // white = repaint
  std::vector<std::string> refImages;  // edit mode: images the model should transform / take as reference
  int width = 0, height = 0, steps = 0;  // 0 = preset default
  double cfg = 0, strength = 0.75;
  long long seed = -1;
};

std::vector<std::string> buildSdArgs(const ImagePreset& p, const ImageJob& j, const std::string& outPath, int defaultThreads, std::string* err);

// "  |=====>   | 7/20 - 1.4s/it" -> true, 7, 20
bool parseSdProgress(const std::string& chunk, int* step, int* total);

}  // namespace tony
