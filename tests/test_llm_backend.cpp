// Integration test: real libcurl + SSE parsing + tool-call assembly against tests/mock_llama.py.
// usage: test_llm_backend <port>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "engines/openai_backend.h"
#include "net/http.h"

using namespace tony;

static int g_fail = 0, g_total = 0;
#define CHECK(cond) do { ++g_total; if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

struct FixedEndpoint : IEndpointProvider {
  std::string url, key;
  Endpoint acquire(const std::atomic<bool>&) override { Endpoint e; e.ok = true; e.baseUrl = url; e.apiKey = key; return e; }
  std::string modelId(const std::string& n) override { return "router/" + n; }
};

static ChatRequest req(const std::string& text) {
  ChatRequest r;
  r.model = "m1";
  Message s; s.role = "system"; s.content = "sys";
  Message u; u.role = "user"; u.content = text;
  r.messages = {s, u};
  return r;
}

int main(int argc, char** argv) {
  if (argc < 2) { std::printf("usage: %s <port>\n", argv[0]); return 2; }
  httpGlobalInit();
  Settings st;
  st.llama.mode = "remote";
  ModelEntry m; m.name = "m1"; m.ctx = 4096; st.llama.models.push_back(m);
  FixedEndpoint ep;
  ep.url = std::string("http://127.0.0.1:") + argv[1] + "/v1";
  ep.key = "key123";
  OpenAiBackend be(st, ep);
  std::atomic<bool> cancel{false};

  std::string streamed;
  auto r = be.chat(req("hello"), [&](const std::string& t, bool reasoning) { if (!reasoning) streamed += t; }, cancel);
  CHECK(r.error.empty());
  CHECK(r.message.content == "Hello world" && streamed == "Hello world");
  CHECK(r.finishReason == "stop");
  CHECK(r.promptTokens == 11 && r.completionTokens == 4 && r.tokPerSec > 42.0 && r.tokPerSec < 43.0);

  // tool call arguments arrive in fragments; two calls in one reply
  r = be.chat(req("tool please"), nullptr, cancel);
  CHECK(r.error.empty() && r.message.toolCalls.size() == 2);
  if (r.message.toolCalls.size() == 2) {
    CHECK(r.message.toolCalls[0].id == "call_9" && r.message.toolCalls[0].name == "echo");
    CHECK(r.message.toolCalls[0].argsJson == "{\"x\":\"hello\"}");
    CHECK(r.message.toolCalls[1].name == "second" && r.message.toolCalls[1].argsJson == "{}");
  }
  CHECK(r.finishReason == "tool_calls");

  std::string rs, cs;
  r = be.chat(req("reason"), [&](const std::string& t, bool reasoning) { (reasoning ? rs : cs) += t; }, cancel);
  CHECK(r.message.reasoning == "think more" && r.message.content == "answer" && rs == "think more" && cs == "answer");

  // errors: in-stream error object, auth failure, unreachable server
  r = be.chat(req("error400"), nullptr, cancel);
  CHECK(r.error.find("context too long") != std::string::npos);
  ep.key = "wrong";
  r = be.chat(req("hello"), nullptr, cancel);
  CHECK(r.error.find("bad key") != std::string::npos);
  ep.key = "key123";
  FixedEndpoint dead; dead.url = "http://127.0.0.1:1/v1";
  OpenAiBackend be2(st, dead);
  r = be2.chat(req("hello"), nullptr, cancel);
  CHECK(r.error.find("Cannot reach") != std::string::npos);

  // images go out as OpenAI content parts
  ChatRequest ir = req("image describe");
  ir.messages.back().imageDataUrls.push_back("data:image/png;base64,AAAA");
  r = be.chat(ir, nullptr, cancel);
  CHECK(r.message.content == "saw image");

  // request shape: tools, ids, history round-trip, model id mapping
  ChatRequest mr = req("meta");
  mr.tools.push_back({"echo", "d", json{{"type", "object"}}});
  Message a; a.role = "assistant"; a.toolCalls.push_back({"c1", "echo", "{\"x\":\"1\"}"});
  Message t; t.role = "tool"; t.toolCallId = "c1"; t.toolName = "echo"; t.content = "bad \x80 utf8";  // must not crash JSON encoding
  mr.messages.insert(mr.messages.begin() + 1, {a, t});
  mr.sampling.temperature = 0.25;
  r = be.chat(mr, nullptr, cancel);
  json info = json::parse(r.message.content, nullptr, false);
  CHECK(!info.is_discarded());
  if (!info.is_discarded()) {
    CHECK(info["tools"][0] == "echo" && info["temp"] == 0.25 && info["model"] == "router/m1");
    CHECK(info["roles"] == json::array({"system", "assistant", "tool", "user"}));
    CHECK(info["tool_call_id"][0] == "c1" && info["assistant_calls"][0] == 1);
  }

  // cancel mid-stream returns promptly with partial text
  std::thread killer([&] { std::this_thread::sleep_for(std::chrono::milliseconds(400)); cancel = true; });
  auto t0 = std::chrono::steady_clock::now();
  r = be.chat(req("slow"), nullptr, cancel);
  killer.join();
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECK(r.cancelled && r.error.empty() && secs < 3.0 && r.message.content.find("tok0") != std::string::npos);

  CHECK(be.contextSize("m1") == 4096);
  httpGlobalCleanup();
  std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
  return g_fail ? 1 : 0;
}
