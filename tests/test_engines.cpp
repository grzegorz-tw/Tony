#include <algorithm>
#include <cstdio>

#include "core/strutil.h"
#include "engines/image_args.h"
#include "engines/llama_preset.h"
#include "engines/hf.h"
#include "engines/whisper_util.h"
#include "tools/web_parse.h"

using namespace tony;
static int g_fail = 0, g_total = 0;
#define CHECK(cond) do { ++g_total; if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
static bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }
static bool hasPair(const std::vector<std::string>& a, const std::string& k, const std::string& v) {
  for (size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == k && a[i + 1] == v) return true;
  return false;
}

int main() {
  CHECK(presetId("gpt oss/120b [Q4]") == "gpt-oss-120b--Q4-");
  CHECK(presetId("") == "model");
  Settings s;
  ModelEntry big; big.name = "gpt-oss-120b"; big.path = "D:\\models\\gpt oss.gguf"; big.ctx = 65536; big.cpuMoe = 24; big.flashAttn = "on";
  big.cacheK = "q8_0"; big.reasoning = "off"; big.extraArgs = "--no-mmap --override-kv \"a=b c\" --temp -1";
  ModelEntry eyes; eyes.name = "eyes"; eyes.role = "vision"; eyes.path = "D:\\m\\vl.gguf"; eyes.mmproj = "D:\\m\\mmproj.gguf"; eyes.gpuLayers = 99; eyes.ctx = 8192;
  ModelEntry empty; empty.name = "unset";
  s.llama.models = {big, eyes, empty};
  s.llama.idleUnloadSec = 300;
  std::string ini = buildPresetIni(s);
  CHECK(has(ini, "version = 1\n\n[*]\njinja = true\nparallel = 1\nsleep-idle-seconds = 300\n"));
  CHECK(has(ini, "[gpt-oss-120b]\nmodel = D:/models/gpt oss.gguf\n"));
  CHECK(has(ini, "c = 65536\n") && has(ini, "n-cpu-moe = 24\n") && has(ini, "flash-attn = on\n") && has(ini, "cache-type-k = q8_0\n"));
  CHECK(!has(ini, "cache-type-v") && !has(ini, "n-gpu-layers = -1"));  // defaults and "auto" are left to llama.cpp (--fit)
  CHECK(has(ini, "reasoning = off\n") && has(ini, "no-mmap = true\n") && has(ini, "override-kv = a=b c\n") && has(ini, "temp = -1\n"));
  CHECK(has(ini, "[eyes]\nmodel = D:/m/vl.gguf\nmmproj = D:/m/mmproj.gguf\nc = 8192\nn-gpu-layers = 99\n"));
  CHECK(!has(ini, "[unset]"));

  auto args = buildServerArgs(s, "C:\\x\\models.ini", 18080, "KEY");
  CHECK(hasPair(args, "--models-max", "1") && hasPair(args, "--port", "18080") && hasPair(args, "--host", "127.0.0.1") && hasPair(args, "--api-key", "KEY"));
  CHECK(std::find(args.begin(), args.end(), "--no-webui") != args.end());
  s.llama.gpuPolicy = "coexist";
  CHECK(hasPair(buildServerArgs(s, "x", 1, "k"), "--models-max", "2"));

  // images
  ImagePreset p; p.name = "flux"; p.diffusionModel = "C:\\i\\flux.gguf"; p.vae = "C:\\i\\ae.safetensors"; p.clipL = "C:\\i\\clip_l.safetensors"; p.t5xxl = "C:\\i\\t5.gguf";
  p.sampler = "euler"; p.cfg = 1.0; p.steps = 20; p.extraArgs = "--guidance 3.5";
  ImageJob j; j.prompt = "a red fox; rm -rf /";  // metacharacters are harmless: argv, never a shell
  std::string err;
  auto a = buildSdArgs(p, j, "C:\\out\\a.png", 16, &err);
  CHECK(!a.empty() && hasPair(a, "-p", "a red fox; rm -rf /") && hasPair(a, "--diffusion-model", "C:\\i\\flux.gguf") && hasPair(a, "-o", "C:\\out\\a.png"));
  CHECK(hasPair(a, "-W", "1024") && hasPair(a, "--steps", "20") && hasPair(a, "-t", "16") && hasPair(a, "--guidance", "3.5"));
  CHECK(std::find(a.begin(), a.end(), "--offload-to-cpu") != a.end() && std::find(a.begin(), a.end(), "-m") == a.end());
  j.width = 1000; j.height = 33;
  a = buildSdArgs(p, j, "o.png", 0, &err);
  CHECK(hasPair(a, "-W", "1000") && hasPair(a, "-H", "64") && std::find(a.begin(), a.end(), "-t") == a.end());
  j.mode = "edit";
  a = buildSdArgs(p, j, "o.png", 0, &err);
  CHECK(a.empty() && has(err, "input image"));
  j.refImages = {"C:\\in\\a.png", "C:\\in\\b.png"};
  a = buildSdArgs(p, j, "o.png", 0, &err);
  CHECK(!a.empty() && hasPair(a, "-r", "C:\\in\\a.png") && hasPair(a, "-r", "C:\\in\\b.png"));
  j.initImage = "C:\\in\\i.png"; j.maskImage = "C:\\in\\m.png";
  a = buildSdArgs(p, j, "o.png", 0, &err);
  CHECK(hasPair(a, "-i", "C:\\in\\i.png") && hasPair(a, "--mask", "C:\\in\\m.png") && std::find(a.begin(), a.end(), "--strength") == a.end());
  ImagePreset none; ImageJob jj; jj.prompt = "x";
  CHECK(buildSdArgs(none, jj, "o.png", 0, &err).empty() && has(err, "no model"));

  int st = 0, tot = 0;
  CHECK(parseSdProgress("  |==========>        | 7/20 - 1.42s/it\r  |===========>       | 8/20 - 1.40s/it", &st, &tot) && st == 8 && tot == 20);
  CHECK(!parseSdProgress("loading model 3 of 9 ...", &st, &tot));

  // speech helpers
  std::string wav = makeWav16k({0.0f, 1.0f, -1.0f, 2.0f});
  CHECK(wav.size() == 44 + 8 && wav.substr(0, 4) == "RIFF" && wav.substr(8, 4) == "WAVE" && wav.substr(36, 4) == "data");
  CHECK((unsigned char)wav[40] == 8 && (unsigned char)wav[44 + 2] == 0xFF && (unsigned char)wav[44 + 3] == 0x7F);  // +1.0 -> 32767, clipped 2.0 too
  CHECK(cleanTranscript(" Thanks for watching! ") == "" && cleanTranscript("[BLANK_AUDIO]") == "" && cleanTranscript("(music)") == "");
  CHECK(cleanTranscript("Thank you.") == "" && cleanTranscript("\xE2\x99\xAA \xE2\x99\xAA") == "");
  CHECK(cleanTranscript("Open the downloads folder.") == "Open the downloads folder." && cleanTranscript("Thank you for the help with my code") != "");
  std::string rest;
  CHECK(matchWakeWord("Hey, Tony! What's the time?", "tony", &rest) && rest == "What's the time?");
  CHECK(matchWakeWord("ok hey tony, open notepad", "hey tony", &rest) && rest == "open notepad");
  CHECK(!matchWakeWord("I talked to Tony yesterday about that thing we did", "tony", &rest));
  CHECK(!matchWakeWord("nothing here", "tony", &rest) && !matchWakeWord("x", "", &rest));

  // search result parsing
  std::string ddg =
      "<div class=\"result results_links\"><h2 class=\"result__title\"><a rel=\"nofollow\" class=\"result__a\" "
      "href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fa%3Fx%3D1%26y%3D2&amp;rut=abc\">Example &amp; <b>Title</b></a></h2>"
      "<a class=\"result__snippet\" href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fa\">First <b>snippet</b> text</a></div>"
      "<div class=\"result\"><a class=\"result__a\" href=\"https://second.org/page\">Second</a></div>"
      "<div class=\"result\"><a class=\"result__a\" href=\"//duckduckgo.com/y.js?ad_provider=x\">Sponsored</a></div>";
  auto hits = parseDuckDuckGoHtml(ddg);
  CHECK(hits.size() == 2);
  if (hits.size() == 2) {
    CHECK(hits[0].url == "https://example.com/a?x=1&y=2" && hits[0].title == "Example & Title" && hits[0].snippet == "First snippet text");
    CHECK(hits[1].url == "https://second.org/page" && hits[1].snippet.empty());
  }
  CHECK(looksLikeBotChallenge("<html>Unfortunately, bots use DuckDuckGo too. anomaly challenge</html>") && !looksLikeBotChallenge(ddg));
  auto sx = parseSearxngJson(R"({"results":[{"title":"T <b>1</b>","url":"https://a.b","content":"c &amp; d"},{"title":"no url"}]})");
  CHECK(sx.size() == 1 && sx[0].title == "T 1" && sx[0].snippet == "c & d");
  auto br = parseBraveJson(R"({"web":{"results":[{"title":"B","url":"https://b.c","description":"<strong>hi</strong> there"}]}})");
  CHECK(br.size() == 1 && br[0].snippet == "hi there");
  CHECK(parseBraveJson("not json").empty() && parseSearxngJson("{}").empty());
  CHECK(formatHits(hits, 1).find("2. ") == std::string::npos && formatHits(hits, 5).find("2. Second") != std::string::npos);
  CHECK(urlDecode("a%20b+c%2Fd%zz") == "a b c/d%zz");

  // Hugging Face
  auto tree = parseHfTree(R"([{"type":"file","size":120,"path":"m-Q4_K_M.gguf","lfs":{"size":123456789}},{"type":"directory","path":"sub"},{"type":"file","size":7,"path":"README.md"}])");
  CHECK(tree.size() == 3 && tree[0].isDir && tree[0].path == "sub" && tree[1].path == "README.md" && tree[2].size == 123456789);
  CHECK(parseHfTree("nope").empty());
  CHECK(validRepoId("ggml-org/gpt-oss-20b-GGUF") && !validRepoId("a") && !validRepoId("a/b/c") && !validRepoId("../x/y") && !validRepoId("a/b c") && !validRepoId("/b"));
  CHECK(hfFileUrl("o/r", "en/en US/voice name.onnx") == "https://huggingface.co/o/r/resolve/main/en/en%20US/voice%20name.onnx?download=true");

  std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
  return g_fail ? 1 : 0;
}
