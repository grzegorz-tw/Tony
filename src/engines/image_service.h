// Tony - runs stable-diffusion.cpp (sd-cli) jobs. One process per image: when it exits all VRAM is returned to the LLM.
#pragma once
#include <atomic>
#include <string>

#include "image_args.h"
#include "llama_service.h"
#include "core/tool.h"

namespace tony {

class ImageService {
 public:
  ImageService(Settings& s, const AppPaths& p, LlamaService* llama) : s_(s), paths_(p), llama_(llama) {}
  struct Result { bool ok = false; std::string path, error, log; };
  Result run(const ImageJob& job, const std::string& presetName, const std::atomic<bool>& cancel, const ProgressFn& progress);

 private:
  Settings& s_;
  AppPaths paths_;
  LlamaService* llama_;
};

}  // namespace tony
