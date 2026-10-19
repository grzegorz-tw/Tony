#include "image_args.h"

#include <algorithm>
#include <regex>

#include "core/strutil.h"

namespace tony {

std::vector<std::string> buildSdArgs(const ImagePreset& p, const ImageJob& j, const std::string& outPath, int defaultThreads, std::string* err) {
  std::vector<std::string> a;
  if (trim(p.model).empty() && trim(p.diffusionModel).empty()) { if (err) *err = "The image preset has no model. Set one in Settings > Models > Images."; return {}; }
  if (j.prompt.empty()) { if (err) *err = "An image prompt is required."; return {}; }
  if (j.mode == "edit" && j.refImages.empty() && j.initImage.empty()) { if (err) *err = "Editing needs an input image."; return {}; }

  auto flag = [&](const char* name, const std::string& v) { if (!trim(v).empty()) { a.push_back(name); a.push_back(v); } };
  a.push_back("-M"); a.push_back("img_gen");
  flag("-m", p.model);
  flag("--diffusion-model", p.diffusionModel);
  flag("--vae", p.vae);
  flag("--clip_l", p.clipL);
  flag("--clip_g", p.clipG);
  flag("--t5xxl", p.t5xxl);

  a.push_back("-p"); a.push_back(j.prompt);
  if (!j.negative.empty()) { a.push_back("-n"); a.push_back(j.negative); }
  int w = j.width > 0 ? j.width : p.width, h = j.height > 0 ? j.height : p.height;
  w = std::clamp(w / 8 * 8, 64, 2048);   // latent grids are multiples of 8
  h = std::clamp(h / 8 * 8, 64, 2048);
  a.push_back("-W"); a.push_back(std::to_string(w));
  a.push_back("-H"); a.push_back(std::to_string(h));
  a.push_back("--steps"); a.push_back(std::to_string(j.steps > 0 ? j.steps : std::max(1, p.steps)));
  a.push_back("--cfg-scale"); a.push_back(std::to_string(j.cfg > 0 ? j.cfg : p.cfg));
  flag("--sampling-method", p.sampler);
  a.push_back("-s"); a.push_back(std::to_string(j.seed));
  int threads = p.threads > 0 ? p.threads : defaultThreads;
  if (threads > 0) { a.push_back("-t"); a.push_back(std::to_string(threads)); }

  if (!j.initImage.empty()) {
    a.push_back("-i"); a.push_back(j.initImage);
    if (j.maskImage.empty()) { a.push_back("--strength"); a.push_back(std::to_string(std::clamp(j.strength, 0.05, 1.0))); }
    else { a.push_back("--mask"); a.push_back(j.maskImage); }
  }
  for (auto& r : j.refImages) { a.push_back("-r"); a.push_back(r); }

  if (p.offloadToCpu) a.push_back("--offload-to-cpu");
  if (p.vaeTiling) a.push_back("--vae-tiling");
  if (p.flashAttn) a.push_back("--diffusion-fa");
  for (auto& x : splitArgs(p.extraArgs)) a.push_back(x);
  a.push_back("-o"); a.push_back(outPath);
  return a;
}

bool parseSdProgress(const std::string& chunk, int* step, int* total) {
  static const std::regex re(R"((\d+)\s*/\s*(\d+)\s*-)");
  std::smatch m;
  std::string last;
  auto begin = std::sregex_iterator(chunk.begin(), chunk.end(), re);
  bool found = false;
  for (auto it = begin; it != std::sregex_iterator(); ++it) { m = *it; found = true; }
  if (!found) return false;
  *step = std::stoi(m[1].str());
  *total = std::stoi(m[2].str());
  return *total > 0 && *step <= *total;
}

}  // namespace tony
