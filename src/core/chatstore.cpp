#include "chatstore.h"

#include <sstream>

#include "strutil.h"

namespace tony {

json chatToJson(const Chat& c) {
  json j = {{"id", c.id}, {"title", c.title}, {"updated", c.updated}, {"messages", json::array()}};
  for (auto& m : c.msgs) {
    json o = {{"role", m.role}, {"content", sanitizeUtf8(m.content)}};
    if (!m.reasoning.empty()) o["reasoning"] = sanitizeUtf8(m.reasoning);
    if (!m.toolCalls.empty()) {
      json calls = json::array();
      for (auto& t : m.toolCalls) calls.push_back({{"id", t.id}, {"name", t.name}, {"args", sanitizeUtf8(t.argsJson)}});
      o["toolCalls"] = calls;
    }
    if (!m.toolCallId.empty()) o["toolCallId"] = m.toolCallId;
    if (!m.toolName.empty()) o["toolName"] = m.toolName;
    if (!m.shownImages.empty()) o["shownImages"] = m.shownImages;
    if (!m.imageDataUrls.empty()) o["hadImages"] = static_cast<int>(m.imageDataUrls.size());
    if (m.uiOnly) o["uiOnly"] = true;
    if (m.isError) o["isError"] = true;
    if (m.tokPerSec > 0) o["tokPerSec"] = m.tokPerSec;
    j["messages"].push_back(std::move(o));
  }
  return j;
}

bool chatFromJson(const json& j, Chat* out) {
  if (!j.is_object() || !j.contains("messages") || !j["messages"].is_array()) return false;
  Chat c;
  c.id = j.value("id", "");
  c.title = j.value("title", "");
  c.updated = j.value("updated", 0LL);
  if (c.id.empty()) return false;
  for (auto& o : j["messages"]) {
    if (!o.is_object()) continue;
    Message m;
    m.role = o.value("role", "");
    m.content = o.value("content", "");
    m.reasoning = o.value("reasoning", "");
    m.toolCallId = o.value("toolCallId", "");
    m.toolName = o.value("toolName", "");
    m.uiOnly = o.value("uiOnly", false);
    m.isError = o.value("isError", false);
    m.tokPerSec = o.value("tokPerSec", 0.0);
    if (o.contains("toolCalls") && o["toolCalls"].is_array())
      for (auto& t : o["toolCalls"]) m.toolCalls.push_back({t.value("id", ""), t.value("name", ""), t.value("args", "")});
    if (o.contains("shownImages") && o["shownImages"].is_array())
      for (auto& s : o["shownImages"]) if (s.is_string()) m.shownImages.push_back(s.get<std::string>());
    if (m.role.empty()) continue;
    if (o.value("hadImages", 0) > 0) m.content += "\n[image attachment not stored]";
    c.msgs.push_back(std::move(m));
  }
  *out = std::move(c);
  return true;
}

std::string chatToMarkdown(const Chat& c) {
  std::ostringstream o;
  o << "# " << (c.title.empty() ? "Conversation with Tony" : c.title) << "\n\n";
  for (auto& m : c.msgs) {
    if (m.role == "user") o << "## You\n\n" << m.content << "\n\n";
    else if (m.role == "assistant") {
      if (!m.content.empty()) o << "## Tony\n\n" << m.content << "\n\n";
      for (auto& t : m.toolCalls) o << "> tool call: `" << t.name << "` " << truncateMiddle(t.argsJson, 300) << "\n\n";
    } else if (m.role == "tool") o << "<details><summary>" << m.toolName << (m.isError ? " (error)" : "") << "</summary>\n\n```\n" << truncateMiddle(m.content, 2000) << "\n```\n</details>\n\n";
    else if (m.role == "notice" || m.role == "error") o << "_" << m.content << "_\n\n";
    for (auto& img : m.shownImages) o << "![image](" << img << ")\n\n";
  }
  return o.str();
}

std::string autoTitle(const std::string& s) {
  std::string t = trim(replaceAll(replaceAll(s, "\r", " "), "\n", " "));
  if (t.size() > 48) {
    size_t cut = 48;
    while (cut > 0 && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) --cut;
    size_t sp = t.rfind(' ', cut);
    t = t.substr(0, sp != std::string::npos && sp > 20 ? sp : cut) + "...";
  }
  return t.empty() ? "New chat" : t;
}

}  // namespace tony
