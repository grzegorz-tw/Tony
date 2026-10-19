// Agent / policy tests using a scripted backend and fake tools. Portable C++17.
#include <cstdio>
#include <deque>
#include <fstream>
#include <sstream>
#include <iostream>

#include "core/agent.h"
#include "core/chatstore.h"
#include "core/strutil.h"
#include "net/netpolicy.h"

using namespace tony;

static int g_fail = 0, g_total = 0;
#define CHECK(cond) do { ++g_total; if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

struct ScriptedBackend : IChatBackend {
  std::deque<ChatResult> script;
  int calls = 0;
  int ctx = 0;
  std::vector<ChatRequest> seen;
  ChatResult chat(const ChatRequest& req, const DeltaFn& onDelta, const std::atomic<bool>&) override {
    ++calls;
    seen.push_back(req);
    ChatResult r;
    if (script.empty()) { r.message.content = "done"; return r; }
    r = script.front();
    script.pop_front();
    if (!r.message.content.empty()) onDelta(r.message.content, false);
    return r;
  }
  int contextSize(const std::string&) override { return ctx; }
};

static ChatResult say(const std::string& t) { ChatResult r; r.message.content = t; return r; }
static ChatResult callTool(const std::string& name, const std::string& args) {
  ChatResult r;
  ToolCall c; c.name = name; c.argsJson = args;
  r.message.toolCalls.push_back(c);
  return r;
}

struct FakeTool : Tool {
  std::string name_; Assessment::Level level_; std::string risk_; bool untrusted_; bool alwaysAsk_;
  int* runs;
  FakeTool(std::string n, Assessment::Level l, std::string risk, bool untrusted, int* r, bool alwaysAsk = false)
      : name_(std::move(n)), level_(l), risk_(std::move(risk)), untrusted_(untrusted), alwaysAsk_(alwaysAsk), runs(r) {}
  ToolSpec spec() const override {
    ToolSpec s; s.name = name_; s.description = "fake";
    s.parameters = json{{"type", "object"}, {"properties", {{"x", {{"type", "string"}}}}}, {"required", json::array({"x"})}};
    return s;
  }
  Assessment assess(const json&, ToolContext&) override {
    Assessment a; a.level = level_; a.risk = risk_; a.summary = "do " + name_; a.alwaysAsk = alwaysAsk_;
    if (level_ == Assessment::Level::Deny) a.reason = "nope";
    return a;
  }
  ToolResult run(const json& a, ToolContext&) override {
    ++*runs;
    ToolResult r = ToolResult::ok("ran " + name_ + " x=" + a.value("x", ""));
    r.untrusted = untrusted_; r.source = "http://example.test";
    return r;
  }
};

struct Rig {
  Settings settings;
  ScriptedBackend backend;
  ToolRegistry tools;
  PathGuard guard;
  std::vector<AgentEvent> events;
  std::vector<ApprovalRequest> asked;
  ApprovalDecision answer = ApprovalDecision::AllowOnce;
  std::vector<std::string> audit;
  std::unique_ptr<Agent> agent;
  std::atomic<bool> cancel{false};
  Rig() {
    settings.prompt.systemPrompt = "SYS";
    AgentDeps d;
    d.settings = &settings; d.backend = &backend; d.tools = &tools; d.guard = &guard;
    d.approve = [this](const ApprovalRequest& r) { asked.push_back(r); return answer; };
    d.emit = [this](const AgentEvent& e) { events.push_back(e); };
    d.audit = [this](const std::string& l) { audit.push_back(l); };
    d.environmentBlock = [] { return std::string("ENV"); };
    agent = std::make_unique<Agent>(d);
  }
  void turn(const std::string& t) { agent->runTurn(t, {}, cancel); }
};

static std::string lastToolText(const Agent& a) {
  for (auto it = a.history().rbegin(); it != a.history().rend(); ++it) if (it->role == "tool") return it->content;
  return "";
}

static void testBasic() {
  Rig r;
  r.backend.script = {say("hello")};
  r.turn("hi");
  CHECK(r.agent->history().size() == 2);
  CHECK(r.agent->history()[1].content == "hello");
  CHECK(r.events.back().type == AgentEvent::Type::TurnDone);
  CHECK(r.backend.seen[0].messages[0].role == "system");
  CHECK(r.backend.seen[0].messages[0].content.find("SYS") == 0);
  CHECK(r.backend.seen[0].messages[0].content.find("ENV") != std::string::npos);
}

static void testToolFlow() {
  Rig r; int runs = 0;
  r.tools.add(std::make_unique<FakeTool>("echo", Assessment::Level::Auto, "low", false, &runs));
  r.backend.script = {callTool("echo", "{\"x\":\"1\"}"), say("finished")};
  r.turn("go");
  CHECK(runs == 1);
  CHECK(r.asked.empty());
  CHECK(r.agent->history().size() == 4);  // user, assistant(call), tool, assistant
  CHECK(r.agent->history()[2].role == "tool" && r.agent->history()[2].content == "ran echo x=1");
  CHECK(!r.agent->history()[1].toolCalls[0].id.empty());
  CHECK(r.agent->history()[2].toolCallId == r.agent->history()[1].toolCalls[0].id);
  CHECK(!r.audit.empty() && r.audit[0].find("echo | auto | ok") == 0);
  // tools disabled in settings are not offered and cannot be called
  r.settings.tools.enabled["echo"] = false;
  r.backend.script = {callTool("echo", "{\"x\":\"2\"}"), say("ok")};
  r.turn("again");
  CHECK(runs == 1);
  CHECK(lastToolText(*r.agent).find("Unknown or disabled") == 0);
}

static void testBadArgs() {
  Rig r; int runs = 0;
  r.tools.add(std::make_unique<FakeTool>("echo", Assessment::Level::Auto, "low", false, &runs));
  r.backend.script = {callTool("echo", "{not json"), say("x")};
  r.turn("a");
  CHECK(runs == 0 && lastToolText(*r.agent).find("not a valid JSON") != std::string::npos);
  r.backend.script = {callTool("echo", "{}"), say("x")};
  r.turn("b");
  CHECK(runs == 0 && lastToolText(*r.agent).find("Missing required") != std::string::npos);
  r.backend.script = {callTool("nosuch", "{}"), say("x")};
  r.turn("c");
  CHECK(lastToolText(*r.agent).find("Unknown") == 0);
}

static void testApproval() {
  Rig r; int runs = 0;
  r.tools.add(std::make_unique<FakeTool>("risky", Assessment::Level::Ask, "high", false, &runs));
  r.answer = ApprovalDecision::Deny;
  r.backend.script = {callTool("risky", "{\"x\":\"1\"}"), say("ok")};
  r.turn("do it");
  CHECK(r.asked.size() == 1 && runs == 0);
  CHECK(lastToolText(*r.agent).find("declined") != std::string::npos);

  r.answer = ApprovalDecision::AllowSession;
  r.backend.script = {callTool("risky", "{\"x\":\"1\"}"), say("ok")};
  r.turn("again");
  CHECK(r.asked.size() == 2 && runs == 1);
  r.backend.script = {callTool("risky", "{\"x\":\"2\"}"), say("ok")};
  r.turn("third");
  CHECK(r.asked.size() == 2 && runs == 2);  // remembered for the session

  r.agent->reset();  // new chat forgets session approvals
  r.backend.script = {callTool("risky", "{\"x\":\"3\"}"), say("ok")};
  r.turn("fourth");
  CHECK(r.asked.size() == 3);

  // per-tool settings
  r.settings.tools.approval["risky"] = "deny";
  r.backend.script = {callTool("risky", "{\"x\":\"4\"}"), say("ok")};
  r.turn("fifth");
  CHECK(r.asked.size() == 3 && runs == 3);
  CHECK(lastToolText(*r.agent).find("turned off") != std::string::npos);
}

static void testPolicyOverrides() {
  Rig r; int runs = 0;
  r.tools.add(std::make_unique<FakeTool>("shell", Assessment::Level::Ask, "medium", false, &runs));
  r.tools.add(std::make_unique<FakeTool>("danger", Assessment::Level::Ask, "high", false, &runs, /*alwaysAsk*/ true));
  r.tools.add(std::make_unique<FakeTool>("blocked", Assessment::Level::Deny, "high", false, &runs));
  r.settings.tools.approval["shell"] = "auto";   // user pre-trusts
  r.settings.tools.approval["danger"] = "auto";  // ...but dangerous calls still ask
  r.backend.script = {callTool("shell", "{\"x\":\"1\"}"), callTool("danger", "{\"x\":\"1\"}"), callTool("blocked", "{\"x\":\"1\"}"), say("ok")};
  r.turn("go");
  CHECK(r.asked.size() == 1 && r.asked[0].tool == "danger");
  CHECK(runs == 2);
  CHECK(lastToolText(*r.agent).find("Blocked: nope") == 0 || r.agent->history().size() > 0);
  bool sawBlocked = false;
  for (auto& m : r.agent->history()) if (m.role == "tool" && m.content.find("Blocked: nope") == 0) sawBlocked = true;
  CHECK(sawBlocked);
}

static void testTaint() {
  Rig r; int runs = 0;
  r.tools.add(std::make_unique<FakeTool>("web", Assessment::Level::Auto, "low", true, &runs));
  r.tools.add(std::make_unique<FakeTool>("write", Assessment::Level::Auto, "medium", false, &runs));
  r.tools.add(std::make_unique<FakeTool>("read", Assessment::Level::Auto, "low", false, &runs));
  r.settings.tools.approval["write"] = "auto";
  r.backend.script = {callTool("write", "{\"x\":\"1\"}"), say("ok")};
  r.turn("write first");
  CHECK(r.asked.empty() && runs == 1);  // clean context: pre-trusted tool runs silently
  CHECK(!r.agent->tainted());

  r.backend.script = {callTool("web", "{\"x\":\"u\"}"), say("ok")};
  r.turn("browse");
  CHECK(r.agent->tainted());
  CHECK(lastToolText(*r.agent).find("[EXTERNAL CONTENT from http://example.test") == 0);
  CHECK(lastToolText(*r.agent).find("[END EXTERNAL CONTENT]") != std::string::npos);

  r.answer = ApprovalDecision::AllowSession;
  r.backend.script = {callTool("write", "{\"x\":\"2\"}"), say("ok")};
  r.turn("write after browsing");
  CHECK(r.asked.size() == 1 && r.asked[0].tainted && !r.asked[0].canRemember);  // prompts, cannot be remembered
  r.backend.script = {callTool("write", "{\"x\":\"3\"}"), say("ok")};
  r.turn("again");
  CHECK(r.asked.size() == 2);                                                     // still prompts
  r.backend.script = {callTool("read", "{\"x\":\"3\"}"), say("ok")};
  r.turn("read-only is fine");
  CHECK(r.asked.size() == 2);
  CHECK(r.backend.seen.back().messages[0].content.find("external content") != std::string::npos);

  r.agent->reset();
  CHECK(!r.agent->tainted());
  // restored history that carries external content re-taints
  std::vector<Message> h; Message m; m.role = "tool"; m.content = "[EXTERNAL CONTENT from x]"; h.push_back(m);
  r.agent->setHistory(h);
  CHECK(r.agent->tainted());
}

static void testLoopGuardAndSteps() {
  Rig r; int runs = 0;
  r.tools.add(std::make_unique<FakeTool>("echo", Assessment::Level::Auto, "low", false, &runs));
  for (int i = 0; i < 10; ++i) r.backend.script.push_back(callTool("echo", "{\"x\":\"same\"}"));
  r.turn("loop");
  CHECK(runs == 2);
  CHECK(r.backend.calls <= 4);
  // every assistant tool call has a tool message after it
  const auto& h = r.agent->history();
  for (size_t i = 0; i < h.size(); ++i)
    if (h[i].role == "assistant")
      for (size_t k = 0; k < h[i].toolCalls.size(); ++k) CHECK(i + 1 + k < h.size() && h[i + 1 + k].role == "tool");

  Rig s; int runs2 = 0;
  s.tools.add(std::make_unique<FakeTool>("echo", Assessment::Level::Auto, "low", false, &runs2));
  s.settings.agent.maxSteps = 3;
  for (int i = 0; i < 10; ++i) s.backend.script.push_back(callTool("echo", "{\"x\":\"" + std::to_string(i) + "\"}"));
  s.turn("many");
  CHECK(runs2 == 3);
}

static void testCancel() {
  Rig r; int runs = 0;
  r.tools.add(std::make_unique<FakeTool>("echo", Assessment::Level::Auto, "low", false, &runs));
  ChatResult two = callTool("echo", "{\"x\":\"a\"}");
  ToolCall c2; c2.name = "echo"; c2.argsJson = "{\"x\":\"b\"}";
  two.message.toolCalls.push_back(c2);
  r.backend.script = {two, say("never")};
  r.cancel = true;
  r.turn("x");
  CHECK(runs == 0);
  CHECK(r.backend.calls == 0);
}

static void testCompaction() {
  Rig r;
  std::vector<Message> hist;
  for (int i = 0; i < 20; ++i) {
    Message u; u.role = "user"; u.content = std::string(2000, 'u');
    Message a; a.role = "assistant"; a.content = std::string(2000, 'a');
    hist.push_back(u); hist.push_back(a);
  }
  Message t; t.role = "tool"; t.content = std::string(5000, 't');
  hist.insert(hist.begin() + 2, t);
  r.agent->setHistory(hist);
  size_t before = Agent::estimateTokens(r.agent->history());
  r.agent->compact(8192);
  size_t after = Agent::estimateTokens(r.agent->history());
  CHECK(after < before && after <= 8192 * 0.75);
  CHECK(r.agent->history().front().role == "user");
  CHECK(r.agent->history().back().role == "assistant");
  bool notice = false;
  for (auto& e : r.events) if (e.type == AgentEvent::Type::Notice) notice = true;
  CHECK(notice);
}

static void testSettings() {
  Settings s;
  AppPaths p; p.exeDir = "C:\\Tony"; p.dataDir = "C:\\Users\\me\\AppData\\Roaming\\Tony"; p.docsDir = "C:\\Users\\me\\Documents";
  p.desktopDir = "C:\\Users\\me\\Desktop"; p.downloadsDir = "C:\\Users\\me\\Downloads";
  applyFirstRunDefaults(s, p);
  CHECK(!s.prompt.systemPrompt.empty() && s.security.roots.size() == 3 && !s.security.denyPatterns.empty());
  CHECK(s.llama.exePath == "C:\\Tony\\engines\\llama\\llama-server.exe");
  ModelEntry m; m.name = "big"; m.path = "C:\\m\\big.gguf"; m.cpuMoe = 20;
  ModelEntry v; v.name = "eyes"; v.role = "vision"; v.mmproj = "C:\\m\\mm.gguf";
  s.llama.models = {m, v};
  s.llama.activeChat = "big";
  s.llama.remoteKey = "sk-secret";
  s.network.proxyPass = "pw";
  CustomTool ct; ct.name = "t"; ct.params.push_back({"a", "string", "d", true});
  s.tools.custom.push_back(ct);
  s.tools.approval["powershell"] = "ask";

  auto rev = [](const std::string& x) { return std::string(x.rbegin(), x.rend()); };  // stand-in for DPAPI
  setSecretCodec({rev, rev});
  std::string path = "/tmp/tony_settings_test.json", err;
  CHECK(saveSettings(path, s, &err));
  { std::ifstream f(path); std::stringstream ss; ss << f.rdbuf(); CHECK(ss.str().find("sk-secret") == std::string::npos && ss.str().find("enc:terces-ks") != std::string::npos); }
  Settings l;
  CHECK(loadSettings(path, l, &err));
  CHECK(l.llama.remoteKey == "sk-secret" && l.network.proxyPass == "pw");
  CHECK(l.llama.models.size() == 2 && l.llama.models[0].cpuMoe == 20 && l.chatModel() && l.chatModel()->name == "big");
  CHECK(l.visionModel() && l.visionModel()->name == "eyes");
  CHECK(l.tools.custom.size() == 1 && l.tools.custom[0].params.size() == 1);
  CHECK(l.approvalFor("powershell") == "ask" && l.approvalFor("zzz") == "default");
  // unknown / missing keys are tolerated
  { std::ofstream f(path); f << R"({"ui":{"theme":"light"},"future_key":1})"; }
  Settings k;
  CHECK(loadSettings(path, k, &err) && k.ui.theme == "light" && k.llama.port == 18080);
  { std::ofstream f(path, std::ios::binary); f << "\xEF\xBB\xBF" << R"({"ui":{"theme":"light"}})"; }
  CHECK(loadSettings(path, k, &err) && k.ui.theme == "light");   // BOM tolerated
  { std::ofstream f(path); f << "garbage"; }
  CHECK(!loadSettings(path, k, &err));
  setSecretCodec({});
}

static void testPrivateHosts() {
  for (auto* h : {"192.168.1.1", "10.0.0.5", "172.16.0.1", "172.31.255.255", "169.254.169.254", "127.0.0.1", "localhost", "nas", "printer.local", "fd12::1", "fe80::1", "100.64.0.1"})
    CHECK(isPrivateHost(h));
  for (auto* h : {"example.com", "8.8.8.8", "172.32.0.1", "172.15.0.1", "192.169.0.1", "1.2.3.4.example.org", "abc.onion", "100.128.0.1"})
    CHECK(!isPrivateHost(h));
  VpnSettings v; v.interfaceMatch = "wg";
  setVpnProbe([](const std::string&) { return true; });
  CHECK(vpnConnected(v));
  v.interfaceMatch.clear();
  std::string why;
  CHECK(!vpnConnected(v, &why) && why.find("pattern") != std::string::npos);
  setVpnProbe({});
}

static void testNetPolicy() {
  NetworkSettings n;
  auto run = [&](const std::string& url, std::string* why = nullptr) { HttpRequest r; r.url = url; bool ok = applyNetworkPolicy(n, r, why); return std::make_pair(ok, r); };
  auto a = run("http://127.0.0.1:18080/v1/chat/completions");
  CHECK(a.first && a.second.noProxy);
  a = run("https://example.com/");
  CHECK(a.first && a.second.noProxy && a.second.proxy.empty() && a.second.userAgent.find("Tony") != std::string::npos);
  std::string why;
  a = run("http://abcdefghijklmnop.onion/", &why);
  CHECK(!a.first && why.find("Tor") != std::string::npos);
  n.tor.enabled = true; n.tor.socks = "socks5://127.0.0.1:9050";
  a = run("http://abcdefghijklmnop.onion/");
  CHECK(a.first && a.second.proxy == "socks5h://127.0.0.1:9050");   // remote DNS forced: no leak of .onion lookups
  a = run("http://site.i2p/", &why);
  CHECK(!a.first);
  n.i2p.enabled = true;
  a = run("http://site.i2p/");
  CHECK(a.first && a.second.proxy == "http://127.0.0.1:4444");
  n.mode = "proxy";
  a = run("https://example.com/", &why);
  CHECK(!a.first);
  n.proxyUrl = "socks5://10.0.0.1:1080"; n.proxyUser = "u"; n.proxyPass = "p";
  a = run("https://example.com/");
  CHECK(a.first && a.second.proxy == "socks5h://10.0.0.1:1080" && a.second.proxyUser == "u");
  a = run("http://localhost:6800/jsonrpc");
  CHECK(a.first && a.second.noProxy && a.second.proxy.empty());     // local engines never go through the proxy
  n.mode = "direct";
  n.vpn.requireForWeb = true; n.vpn.interfaceMatch = "wireguard|proton";
  setVpnProbe([](const std::string&) { return false; });
  a = run("https://example.com/", &why);
  CHECK(!a.first && why.find("VPN") != std::string::npos);
  a = run("http://127.0.0.1:18080/");
  CHECK(a.first);                                                    // kill-switch does not break local engines
  setVpnProbe([](const std::string& re) { return re == "wireguard|proton"; });
  a = run("https://example.com/");
  CHECK(a.first);
  setVpnProbe({});
}

static void testChatStore() {
  Chat c; c.id = "abc"; c.title = "T"; c.updated = 1700000000;
  Message u; u.role = "user"; u.content = "hi \xC5\x82"; u.imageDataUrls.push_back("data:image/png;base64,AAAA");
  Message a; a.role = "assistant"; a.content = "calling"; a.reasoning = "thinking"; a.toolCalls.push_back({"c1", "echo", "{\"x\":1}"}); a.tokPerSec = 12.5;
  Message t; t.role = "tool"; t.toolCallId = "c1"; t.toolName = "echo"; t.content = "ran"; t.shownImages = {"C:\\g\\a.png"}; t.isError = true;
  Message n; n.role = "notice"; n.content = "Stopped."; n.uiOnly = true;
  c.msgs = {u, a, t, n};
  json j = chatToJson(c);
  CHECK(j.dump().find("AAAA") == std::string::npos);   // attachments are not persisted
  Chat d;
  CHECK(chatFromJson(json::parse(j.dump()), &d));
  CHECK(d.id == "abc" && d.msgs.size() == 4 && d.msgs[1].toolCalls.size() == 1 && d.msgs[1].toolCalls[0].argsJson == "{\"x\":1}" && d.msgs[1].tokPerSec == 12.5);
  CHECK(d.msgs[2].isError && d.msgs[2].shownImages.size() == 1 && d.msgs[3].uiOnly && d.msgs[0].content.find("not stored") != std::string::npos);
  CHECK(!chatFromJson(json::parse("{\"id\":\"\",\"messages\":[]}"), &d) && !chatFromJson(json::parse("[]"), &d));
  std::string md = chatToMarkdown(c);
  CHECK(md.find("# T") == 0 && md.find("## You") != std::string::npos && md.find("tool call: `echo`") != std::string::npos);
  CHECK(autoTitle("  How do I\nfix my printer? It keeps jamming whenever I try printing a long document  ").size() <= 52);
  CHECK(autoTitle("") == "New chat" && autoTitle("short") == "short");
}

int main() {
  testChatStore();
  testNetPolicy(); testPrivateHosts();
  testBasic(); testToolFlow(); testBadArgs(); testApproval(); testPolicyOverrides(); testTaint();
  testLoopGuardAndSteps(); testCancel(); testCompaction(); testSettings();
  std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
  return g_fail ? 1 : 0;
}
