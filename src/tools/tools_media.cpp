#include <algorithm>
#include <filesystem>
#include <sstream>

#include "core/strutil.h"
#include "net/netpolicy.h"
#include "platform/screen.h"
#include "platform/winutil.h"
#include "tools.h"

namespace tony {
namespace {

// True when images would leave this PC (remote LLM endpoint that is not on the local network).
bool visionIsRemote(const Settings& s) {
  if (s.llama.mode != "remote") return false;
  return !isPrivateHost(urlHost(s.llama.remoteUrl));
}

// Sends one image to the vision-capable model and returns its answer.
bool askVision(Services& svc, const Bitmap& shown, const std::string& prompt, const std::atomic<bool>& cancel, std::string* answer, std::string* err) {
  const Settings& s = *svc.settings;
  const ModelEntry* vm = s.visionModel();
  if (s.llama.mode != "remote" && !vm) {
    *err = "No vision model is configured. In Settings > Models add a vision-capable model (a .gguf plus its mmproj file) and select it as the Vision model.";
    return false;
  }
  std::string png;
  if (!encodePng(shown, png)) { *err = "could not encode the image"; return false; }
  ChatRequest req;
  req.model = s.llama.mode == "remote" ? s.llama.remoteModel : vm->name;
  Message sys;
  sys.role = "system";
  sys.content =
      "You are the eyes of an assistant on the user's Windows PC. Describe precisely what is visible and answer the question. Transcribe visible text exactly. "
      "When locating something, give approximate pixel coordinates (x, y) measured in THIS image, which is " + std::to_string(shown.w) + "x" + std::to_string(shown.h) +
      " pixels with (0,0) at the top-left. Be factual; say when something is unreadable or not visible.";
  Message u;
  u.role = "user";
  u.content = prompt;
  u.imageDataUrls.push_back(toDataUrl("image/png", png));
  req.messages = {sys, u};
  req.sampling.temperature = 0.2;
  req.sampling.maxTokens = 1500;
  req.sampling.reasoning = "off";
  ChatResult r = svc.backend->chat(req, nullptr, cancel);
  if (r.cancelled) { *err = "cancelled"; return false; }
  if (!r.error.empty()) { *err = r.error; return false; }
  *answer = trim(r.message.content);
  if (answer->empty()) { *err = "the vision model returned no text"; return false; }
  return true;
}

std::string savePreview(Services& svc, const Bitmap& b, const char* prefix) {
  std::string png;
  if (!encodePng(b, png)) return "";
  std::string dir = joinPath(svc.paths.cacheDir, "screens");
  ensureDir(dir);
  std::string path = joinPath(dir, std::string(prefix) + timestampForFilename() + "_" + randomHex(2) + ".png");
  return saveBytes(path, png) ? path : "";
}

class LookAtScreenTool : public Tool {
 public:
  explicit LookAtScreenTool(Services& s) : svc_(s) {}
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "look_at_screen";
    t.description =
        "Take a screenshot and look at it with the vision model: describe what is on screen, read text, find buttons, diagnose error dialogs. "
        "target: all (every monitor, default), primary, monitor:N, window (the active window) or window:<title fragment>. "
        "Coordinates in the answer are converted to real screen pixels for ui_input.";
    t.parameters = {{"type", "object"},
                    {"properties", {{"question", {{"type", "string"}, {"description", "What to look for. Default: describe the screen"}}},
                                    {"target", {{"type", "string"}}}}}};
    return t;
  }
  Assessment assess(const json& a, ToolContext&) override {
    Assessment r;
    r.summary = "Look at your screen";
    r.detail = argStr(a, "target", "all") + ": " + argStr(a, "question", "describe the screen");
    if (visionIsRemote(*svc_.settings)) {
      r.level = Assessment::Level::Ask;
      r.risk = "high";
      r.alwaysAsk = true;
      r.reason = "Your vision model runs on a remote server: the screenshot would leave this PC.";
    }
    return r;
  }
  ToolResult run(const json& a, ToolContext& ctx) override {
    Bitmap full;
    std::string err;
    if (!captureScreen(argStr(a, "target", "all"), full, &err)) return ToolResult::error(err);
    Bitmap shown = downscale(full, 1568);
    double f = static_cast<double>(full.w) / shown.w;
    std::string q = argStr(a, "question", "Describe what is on the screen, including any text, windows and dialogs.");
    std::string preview = savePreview(svc_, shown, "screen_");
    if (ctx.progress) ctx.progress("Asking the vision model...");
    std::string answer;
    if (!askVision(svc_, shown, q, ctx.cancel, &answer, &err)) {
      ToolResult r = ToolResult::error(err);
      if (!preview.empty()) r.images.push_back(preview);
      return r;
    }
    std::ostringstream o;
    o << answer << "\n\n[Screenshot " << full.w << "x" << full.h << " px at desktop position (" << full.originX << "," << full.originY << "). Answer coordinates refer to a "
      << shown.w << "x" << shown.h << " view: screen_x = " << full.originX << " + x*" << f << ", screen_y = " << full.originY << " + y*" << f << "]";
    ToolResult r = ToolResult::ok(o.str());
    if (!preview.empty()) r.images.push_back(preview);
    r.untrusted = true;  // whatever is on screen (web pages, emails) can try to instruct the model
    r.source = "screen contents";
    return r;
  }

 private:
  Services& svc_;
};

class LookAtImageTool : public Tool {
 public:
  explicit LookAtImageTool(Services& s) : svc_(s) {}
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "look_at_image";
    t.description = "Look at an image file (photo, screenshot, diagram, scan) with the vision model and answer a question about it.";
    t.parameters = {{"type", "object"},
                    {"properties", {{"path", {{"type", "string"}}}, {"question", {{"type", "string"}, {"description", "Default: describe the image"}}}}},
                    {"required", json::array({"path"})}};
    return t;
  }
  Assessment assess(const json& a, ToolContext&) override {
    auto p = resolvePath(svc_, argStr(a, "path"), false);
    if (!p.ok) return Assessment::deny(p.error);
    Assessment r;
    r.summary = "Look at an image";
    r.detail = p.path;
    if (visionIsRemote(*svc_.settings)) {
      r.level = Assessment::Level::Ask; r.risk = "high"; r.alwaysAsk = true;
      r.reason = "Your vision model runs on a remote server: the image would leave this PC.";
    }
    return r;
  }
  ToolResult run(const json& a, ToolContext& ctx) override {
    auto p = resolvePath(svc_, argStr(a, "path"), false);
    if (!p.ok) return ToolResult::error(p.error);
    Bitmap img;
    std::string err;
    if (!loadImageFile(p.path, img, &err)) return ToolResult::error(err);
    Bitmap shown = downscale(img, 1568);
    std::string answer;
    if (!askVision(svc_, shown, argStr(a, "question", "Describe this image in detail, including any visible text."), ctx.cancel, &answer, &err)) return ToolResult::error(err);
    ToolResult r = ToolResult::ok(answer + "\n\n[Image " + std::to_string(img.w) + "x" + std::to_string(img.h) + " px]");
    r.untrusted = true;
    r.source = p.path;
    return r;
  }

 private:
  Services& svc_;
};

ToolResult imageResult(const ImageService::Result& res) {
  if (!res.ok) return ToolResult::error(res.error == "cancelled" ? "Image generation was cancelled." : res.error);
  ToolResult r = ToolResult::ok("Image saved to " + res.path + ". It is shown to the user in the chat.");
  r.images.push_back(res.path);
  return r;
}

class GenerateImageTool : public Tool {
 public:
  explicit GenerateImageTool(Services& s) : svc_(s) {}
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "generate_image";
    t.description =
        "Create a new image from a text description with the local image model (runs on the GPU; takes from several seconds to a few minutes). "
        "Write a rich, specific prompt (subject, style, lighting, composition). The result is shown to the user and saved in the gallery folder.";
    t.parameters = {{"type", "object"},
                    {"properties", {{"prompt", {{"type", "string"}}}, {"negative_prompt", {{"type", "string"}}},
                                    {"width", {{"type", "integer"}}}, {"height", {{"type", "integer"}}}, {"steps", {{"type", "integer"}}},
                                    {"seed", {{"type", "integer"}, {"description", "-1 = random"}}}, {"preset", {{"type", "string"}, {"description", "Image preset name from Settings"}}}}},
                    {"required", json::array({"prompt"})}};
    return t;
  }
  Assessment assess(const json& a, ToolContext&) override {
    Assessment r;
    r.summary = "Generate an image";
    r.detail = argStr(a, "prompt");
    return r;
  }
  ToolResult run(const json& a, ToolContext& ctx) override {
    if (!svc_.image) return ToolResult::error("Image generation is not available.");
    ImageJob j;
    j.mode = "generate";
    j.prompt = argStr(a, "prompt");
    j.negative = argStr(a, "negative_prompt");
    j.width = argInt(a, "width", 0); j.height = argInt(a, "height", 0); j.steps = argInt(a, "steps", 0);
    j.seed = a.contains("seed") ? argInt(a, "seed", -1) : -1;
    return imageResult(svc_.image->run(j, argStr(a, "preset"), ctx.cancel, ctx.progress));
  }

 private:
  Services& svc_;
};

class EditImageTool : public Tool {
 public:
  explicit EditImageTool(Services& s) : svc_(s) {}
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "edit_image";
    t.description =
        "Edit or restyle existing image(s) with the local image model, following an instruction such as 'make it night time' or 'remove the background clutter'. "
        "Never overwrites the original: the edited image is saved as a new file. With a mask (white = area to repaint) it inpaints instead.";
    t.parameters = {{"type", "object"},
                    {"properties", {{"images", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Input image path(s)"}}},
                                    {"prompt", {{"type", "string"}, {"description", "What the result should look like / the edit to make"}}},
                                    {"mask", {{"type", "string"}, {"description", "Optional mask image path"}}},
                                    {"strength", {{"type", "number"}, {"description", "0.05-1.0: how far to move away from the original (img2img/inpaint models)"}}},
                                    {"width", {{"type", "integer"}}}, {"height", {{"type", "integer"}}}, {"steps", {{"type", "integer"}}}, {"seed", {{"type", "integer"}}},
                                    {"preset", {{"type", "string"}}}}},
                    {"required", json::array({"images", "prompt"})}};
    return t;
  }
  bool inputs(const json& a, std::vector<std::string>& out, std::string* err) {
    std::vector<std::string> raw;
    if (a.contains("images") && a["images"].is_array()) for (auto& i : a["images"]) if (i.is_string()) raw.push_back(i.get<std::string>());
    if (a.contains("images") && a["images"].is_string()) raw.push_back(a["images"].get<std::string>());
    if (raw.empty()) { *err = "give at least one input image path"; return false; }
    for (auto& r : raw) {
      auto p = resolvePath(svc_, r, false);
      if (!p.ok) { *err = p.error; return false; }
      if (!fileExists(p.path)) { *err = "Image not found: " + p.path; return false; }
      out.push_back(p.path);
    }
    return true;
  }
  Assessment assess(const json& a, ToolContext&) override {
    std::vector<std::string> in;
    std::string err;
    if (!inputs(a, in, &err)) return Assessment::deny(err);
    Assessment r;
    r.summary = "Edit an image";
    r.detail = join(in, "\n") + "\n\n" + argStr(a, "prompt");
    return r;
  }
  ToolResult run(const json& a, ToolContext& ctx) override {
    if (!svc_.image) return ToolResult::error("Image generation is not available.");
    std::vector<std::string> in;
    std::string err;
    if (!inputs(a, in, &err)) return ToolResult::error(err);
    ImageJob j;
    j.prompt = argStr(a, "prompt");
    j.width = argInt(a, "width", 0); j.height = argInt(a, "height", 0); j.steps = argInt(a, "steps", 0);
    j.seed = a.contains("seed") ? argInt(a, "seed", -1) : -1;
    if (a.contains("strength") && a["strength"].is_number()) j.strength = a["strength"].get<double>();
    std::string mask = argStr(a, "mask");
    if (!mask.empty()) {
      auto m = resolvePath(svc_, mask, false);
      if (!m.ok) return ToolResult::error(m.error);
      j.mode = "generate";  // inpainting uses -i + --mask
      j.initImage = in[0];
      j.maskImage = m.path;
    } else {
      j.mode = "edit";
      j.refImages = in;
    }
    // size defaults to the first input so edits keep their framing
    if (j.width == 0 && j.height == 0) {
      Bitmap b;
      if (loadImageFile(in[0], b, nullptr)) { double s = std::min(1.0, 1280.0 / std::max(b.w, b.h)); j.width = static_cast<int>(b.w * s); j.height = static_cast<int>(b.h * s); }
    }
    return imageResult(svc_.image->run(j, argStr(a, "preset"), ctx.cancel, ctx.progress));
  }

 private:
  Services& svc_;
};

}  // namespace

void registerMediaTools(ToolRegistry& reg, Services& svc) {
  reg.add(std::make_unique<LookAtScreenTool>(svc));
  reg.add(std::make_unique<LookAtImageTool>(svc));
  reg.add(std::make_unique<GenerateImageTool>(svc));
  reg.add(std::make_unique<EditImageTool>(svc));
}

}  // namespace tony
