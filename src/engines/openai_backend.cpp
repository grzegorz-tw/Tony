#include "openai_backend.h"

#include <map>

#include "core/strutil.h"
#include "net/http.h"
#include "net/netpolicy.h"

namespace tony {

Endpoint ModeEndpoint::acquire(const std::atomic<bool>& cancel) {
  if (s_.llama.mode == "remote") {
    Endpoint e;
    e.baseUrl = s_.llama.remoteUrl;
    while (!e.baseUrl.empty() && e.baseUrl.back() == '/') e.baseUrl.pop_back();
    e.apiKey = s_.llama.remoteKey;
    e.ok = !e.baseUrl.empty();
    if (!e.ok) e.error = "No remote server URL is set (Settings > Models).";
    return e;
  }
  if (!managed_) { Endpoint e; e.error = "No local engine is available."; return e; }
  return managed_->acquire(cancel);
}

static json toJsonMessage(const Message& m) {
  json j;
  j["role"] = m.role;
  if (m.role == "tool") {
    j["tool_call_id"] = m.toolCallId;
    if (!m.toolName.empty()) j["name"] = m.toolName;
    j["content"] = sanitizeUtf8(m.content);
    return j;
  }
  if (m.role == "assistant") {
    j["content"] = sanitizeUtf8(m.content);
    if (!m.toolCalls.empty()) {
      json calls = json::array();
      for (auto& c : m.toolCalls)
        calls.push_back({{"id", c.id}, {"type", "function"}, {"function", {{"name", c.name}, {"arguments", sanitizeUtf8(c.argsJson.empty() ? "{}" : c.argsJson)}}}});
      j["tool_calls"] = calls;
    }
    return j;
  }
  if (!m.imageDataUrls.empty()) {
    json parts = json::array();
    parts.push_back({{"type", "text"}, {"text", sanitizeUtf8(m.content)}});
    for (auto& u : m.imageDataUrls) parts.push_back({{"type", "image_url"}, {"image_url", {{"url", u}}}});
    j["content"] = parts;
  } else {
    j["content"] = sanitizeUtf8(m.content);
  }
  return j;
}

json OpenAiBackend::buildBody(const ChatRequest& req, const std::string& modelId) {
  json body;
  body["model"] = modelId;
  body["stream"] = true;
  body["stream_options"] = {{"include_usage", true}};
  body["messages"] = json::array();
  for (auto& m : req.messages) body["messages"].push_back(toJsonMessage(m));
  if (!req.tools.empty()) {
    json tools = json::array();
    for (auto& t : req.tools) tools.push_back({{"type", "function"}, {"function", {{"name", t.name}, {"description", t.description}, {"parameters", t.parameters}}}});
    body["tools"] = tools;
    body["tool_choice"] = "auto";
  }
  body["temperature"] = req.sampling.temperature;
  body["top_p"] = req.sampling.topP;
  body["top_k"] = req.sampling.topK;
  body["min_p"] = req.sampling.minP;
  if (req.sampling.maxTokens > 0) body["max_tokens"] = req.sampling.maxTokens;
  return body;
}

int OpenAiBackend::contextSize(const std::string& model) {
  if (auto* m = s_.findModel(model)) return m->ctx;
  if (auto* c = s_.chatModel()) return c->ctx;
  return s_.llama.mode == "remote" ? 32768 : 0;
}

static std::string errorFromBody(const std::string& raw, long status) {
  json j = json::parse(raw, nullptr, false);
  if (!j.is_discarded()) {
    if (j.contains("error")) {
      const json& e = j["error"];
      if (e.is_string()) return e.get<std::string>();
      if (e.is_object() && e.contains("message") && e["message"].is_string()) return e["message"].get<std::string>();
    }
    if (j.contains("message") && j["message"].is_string()) return j["message"].get<std::string>();
  }
  std::string t = trim(raw.substr(0, 400));
  return "HTTP " + std::to_string(status) + (t.empty() ? "" : ": " + t);
}

ChatResult OpenAiBackend::chat(const ChatRequest& req, const DeltaFn& onDelta, const std::atomic<bool>& cancel) {
  ChatResult res;
  Endpoint ep = ep_.acquire(cancel);
  if (cancel.load()) { res.cancelled = true; return res; }
  if (!ep.ok) { res.error = ep.error.empty() ? "The language-model engine is not available." : ep.error; return res; }
  if (req.model.empty() && s_.llama.mode != "remote") {
    res.error = "No chat model is configured. Open Settings > Models and add a .gguf model.";
    return res;
  }

  HttpRequest hr;
  hr.method = "POST";
  hr.url = ep.baseUrl + "/chat/completions";
  hr.headers = {"Content-Type: application/json", "Accept: text/event-stream"};
  if (!ep.apiKey.empty()) hr.headers.push_back("Authorization: Bearer " + ep.apiKey);
  hr.timeoutSec = 0;  // generation can legitimately take minutes
  std::string blocked;
  if (!applyNetworkPolicy(s_.network, hr, &blocked)) { res.error = blocked; return res; }
  hr.body = buildBody(req, ep_.modelId(req.model)).dump(-1, ' ', false, json::error_handler_t::replace);

  struct ToolAcc { std::string id, name, args; };
  std::map<int, ToolAcc> tools;
  std::string content, reasoning, raw, streamError;
  SseParser sse;
  bool sawData = false;

  auto onPayload = [&](const std::string& data) {
    if (data == "[DONE]") return;
    json j = json::parse(data, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    sawData = true;
    if (j.contains("error")) { streamError = errorFromBody(data, 0); return; }
    if (j.contains("usage") && j["usage"].is_object()) {
      res.promptTokens = j["usage"].value("prompt_tokens", 0);
      res.completionTokens = j["usage"].value("completion_tokens", 0);
    }
    if (j.contains("timings") && j["timings"].is_object()) res.tokPerSec = j["timings"].value("predicted_per_second", 0.0);
    if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) return;
    const json& ch = j["choices"][0];
    if (ch.contains("finish_reason") && ch["finish_reason"].is_string()) res.finishReason = ch["finish_reason"].get<std::string>();
    if (!ch.contains("delta") || !ch["delta"].is_object()) return;
    const json& d = ch["delta"];
    if (d.contains("reasoning_content") && d["reasoning_content"].is_string()) {
      std::string t = d["reasoning_content"].get<std::string>();
      if (!t.empty()) { reasoning += t; if (onDelta) onDelta(t, true); }
    }
    if (d.contains("content") && d["content"].is_string()) {
      std::string t = d["content"].get<std::string>();
      if (!t.empty()) { content += t; if (onDelta) onDelta(t, false); }
    }
    if (d.contains("tool_calls") && d["tool_calls"].is_array()) {
      for (auto& tc : d["tool_calls"]) {
        int idx = tc.value("index", 0);
        ToolAcc& a = tools[idx];
        if (tc.contains("id") && tc["id"].is_string()) a.id = tc["id"].get<std::string>();
        if (tc.contains("function") && tc["function"].is_object()) {
          const json& f = tc["function"];
          if (f.contains("name") && f["name"].is_string()) a.name += f["name"].get<std::string>();
          if (f.contains("arguments") && f["arguments"].is_string()) a.args += f["arguments"].get<std::string>();
        }
      }
    }
  };

  HttpResponse r = httpPerform(
      hr,
      [&](const char* d, size_t n) {
        if (raw.size() < 8192) raw.append(d, std::min(n, size_t(8192) - raw.size()));
        sse.feed(d, n, onPayload);
        return !cancel.load();
      },
      &cancel);

  if (r.error == "cancelled" || cancel.load()) res.cancelled = true;
  else if (!r.error.empty() && !sawData) res.error = "Cannot reach the language-model engine: " + r.error;
  else if (r.status >= 400) res.error = errorFromBody(raw, r.status);
  else if (!streamError.empty()) res.error = streamError;
  else if (!r.error.empty()) res.error = "The connection to the model dropped: " + r.error;

  res.message.role = "assistant";
  res.message.content = content;
  res.message.reasoning = reasoning;
  for (auto& kv : tools) {
    if (kv.second.name.empty()) continue;
    ToolCall c;
    c.id = kv.second.id;
    c.name = kv.second.name;
    c.argsJson = kv.second.args;
    res.message.toolCalls.push_back(std::move(c));
  }
  return res;
}

}  // namespace tony
