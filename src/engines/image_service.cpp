#include "image_service.h"

#include <filesystem>

#include "core/strutil.h"
#include "platform/process.h"
#include "platform/winutil.h"

namespace tony {

ImageService::Result ImageService::run(const ImageJob& job, const std::string& presetName, const std::atomic<bool>& cancel, const ProgressFn& progress) {
  Result res;
  const ImageSettings& is = s_.image;
  const ImagePreset* preset = nullptr;
  std::string want = presetName.empty() ? is.active : presetName;
  for (auto& p : is.presets) if (p.name == want) preset = &p;
  if (!preset) for (auto& p : is.presets) if (p.kind == "both" || p.kind == (job.mode == "edit" ? "edit" : "generate")) { preset = &p; break; }
  if (!preset && !is.presets.empty()) preset = &is.presets.front();
  if (!preset) { res.error = "No image model is configured. Add a preset in Settings > Models > Images."; return res; }
  if (job.mode == "edit" && preset->kind == "generate") { res.error = "The active image preset '" + preset->name + "' can only generate images. Choose an editing-capable preset (e.g. FLUX Kontext)."; return res; }
  if (!fileExists(is.sdCliPath)) { res.error = "The image engine (sd.exe) was not found at:\n" + is.sdCliPath + "\nRun scripts\\setup.ps1 or set the path in Settings > Images."; return res; }
  for (auto* f : {&preset->model, &preset->diffusionModel, &preset->vae, &preset->clipL, &preset->clipG, &preset->t5xxl})
    if (!f->empty() && !fileExists(*f)) { res.error = "Image model file not found:\n" + *f; return res; }

  std::string outDir = is.outputDir.empty() ? joinPath(paths_.dataDir, "gallery") : is.outputDir;
  ensureDir(outDir);
  std::string finalPath = joinPath(outDir, "img_" + timestampForFilename() + "_" + randomHex(2) + ".png");

  // sd.exe is handed only short ASCII relative names inside a private job folder (its own file I/O is not Unicode-safe on Windows);
  // the finished image is moved to its real location afterwards.
  std::string jobDir = joinPath(joinPath(paths_.cacheDir, "jobs"), randomHex(4));
  ensureDir(jobDir);
  auto cleanup = [&] { std::error_code ec; std::filesystem::remove_all(u8p(jobDir), ec); };
  auto stage = [&](const std::string& src, const std::string& base) -> std::string {
    std::string ext = lower(src.substr(src.find_last_of('.') == std::string::npos ? src.size() : src.find_last_of('.')));
    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".bmp") ext = ".png";
    std::string name = base + ext;
    std::error_code ec;
    std::filesystem::copy_file(u8p(src), u8p(joinPath(jobDir, name)), std::filesystem::copy_options::overwrite_existing, ec);
    return ec ? std::string() : name;
  };
  ImageJob j = job;
  if (!j.initImage.empty() && (j.initImage = stage(job.initImage, "init")).empty()) { cleanup(); res.error = "cannot read the input image"; return res; }
  if (!j.maskImage.empty() && (j.maskImage = stage(job.maskImage, "mask")).empty()) { cleanup(); res.error = "cannot read the mask image"; return res; }
  for (size_t i = 0; i < j.refImages.size(); ++i)
    if ((j.refImages[i] = stage(job.refImages[i], "ref" + std::to_string(i))).empty()) { cleanup(); res.error = "cannot read the input image"; return res; }

  std::string err;
  auto args = buildSdArgs(*preset, j, "out.png", 0, &err);
  if (args.empty()) { cleanup(); res.error = err; return res; }

  if (s_.llama.gpuPolicy == "swap" && llama_ && llama_->running()) {
    if (progress) progress("Freeing GPU memory...");
    llama_->unloadAll();
  }
  if (progress) progress("Starting image model (large models take a while to load)...");
  int last = -1;
  RunResult rr = runProcess(is.sdCliPath, args, jobDir, 45 * 60 * 1000, 256 * 1024, &cancel, [&](const std::string& chunk) {
    int step = 0, total = 0;
    if (progress && parseSdProgress(chunk, &step, &total) && step != last) { last = step; progress("Generating: step " + std::to_string(step) + "/" + std::to_string(total)); }
  });
  res.log = rr.output;
  std::string produced = joinPath(jobDir, "out.png");
  if (!rr.started) { cleanup(); res.error = rr.error; return res; }
  if (rr.cancelled) { cleanup(); res.error = "cancelled"; return res; }
  if (rr.timedOut) { cleanup(); res.error = "image generation timed out"; return res; }
  if (rr.exitCode != 0 || !fileExists(produced)) {
    cleanup();
    res.error = "Image generation failed (exit " + std::to_string(rr.exitCode) + "):\n" + truncateMiddle(sanitizeUtf8(trim(rr.output)), 1500);
    return res;
  }
  std::error_code ec;
  std::filesystem::copy_file(u8p(produced), u8p(finalPath), std::filesystem::copy_options::overwrite_existing, ec);
  cleanup();
  if (ec) { res.error = "could not save the image to " + finalPath + ": " + ec.message(); return res; }
  res.ok = true;
  res.path = finalPath;
  return res;
}

}  // namespace tony
