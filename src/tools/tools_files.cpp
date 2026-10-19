#include <algorithm>
#include <chrono>
#include <filesystem>
#include <regex>
#include <sstream>

#include "core/strutil.h"
#include "platform/winutil.h"
#include "tools.h"

namespace fs = std::filesystem;

namespace tony {
namespace {

bool insideWorkspace(Services& svc, const std::string& absPath) {
  std::string ws = PathGuard::normalize(workspaceDir(svc)), p = PathGuard::normalize(absPath);
  return p == ws || startsWith(p, ws + "/");
}

bool looksBinary(const std::string& s) {
  size_t n = std::min<size_t>(s.size(), 8192);
  for (size_t i = 0; i < n; ++i) if (s[i] == '\0') return true;
  return false;
}

std::string preview(const std::string& s, size_t n) {
  std::string t = s.substr(0, n);
  return s.size() > n ? t + "\n..." : t;
}

// ------------------------------------------------------------------ read_file
class ReadFileTool : public Tool {
 public:
  explicit ReadFileTool(Services& s) : svc_(s) {}
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "read_file";
    t.description = "Read a text file (source code, notes, logs, config). Returns numbered lines. For big files use offset/limit. Relative paths resolve against the workspace folder.";
    t.parameters = {{"type", "object"},
                    {"properties", {{"path", {{"type", "string"}, {"description", "File path"}}},
                                    {"offset", {{"type", "integer"}, {"description", "First line to return (1-based). Default 1"}}},
                                    {"limit", {{"type", "integer"}, {"description", "Maximum number of lines. Default 400"}}}}},
                    {"required", json::array({"path"})}};
    return t;
  }
  Assessment assess(const json& a, ToolContext&) override {
    Assessment r;
    auto p = resolvePath(svc_, argStr(a, "path"), false);
    if (!p.ok) return Assessment::deny(p.error);
    r.summary = "Read file";
    r.detail = p.path;
    return r;
  }
  ToolResult run(const json& a, ToolContext&) override {
    auto p = resolvePath(svc_, argStr(a, "path"), false);
    if (!p.ok) return ToolResult::error(p.error);
    std::error_code ec;
    if (!fs::is_regular_file(u8p(p.path), ec)) return ToolResult::error(fs::is_directory(u8p(p.path), ec) ? "That is a folder; use list_dir." : "File not found: " + p.path);
    auto size = fs::file_size(u8p(p.path), ec);
    std::string data = readTextFile(p.path, 2u << 20);
    if (looksBinary(data)) return ToolResult::error("Binary file (" + humanSize(size) + "). For images use look_at_image; for other formats use powershell to inspect it.");
    std::vector<std::string> lines = split(data, '\n');
    for (auto& l : lines) if (!l.empty() && l.back() == '\r') l.pop_back();
    int offset = std::max(1, argInt(a, "offset", 1)), limit = std::max(1, std::min(argInt(a, "limit", 400), 2000));
    int total = static_cast<int>(lines.size());
    std::ostringstream o;
    o << "File: " << p.path << " (" << total << " lines, " << humanSize(size) << (size > (2u << 20) ? ", only the first 2 MB were read" : "") << ")\n";
    int last = std::min(total, offset - 1 + limit);
    if (offset > total) o << "(offset is past the end of the file)\n";
    for (int i = offset; i <= last; ++i) o << i << "\t" << lines[static_cast<size_t>(i - 1)] << "\n";
    if (last < total) o << "... " << (total - last) << " more lines. Use offset=" << (last + 1) << " to continue.\n";
    ToolResult r = ToolResult::ok(o.str());
    r.untrusted = true;
    r.source = p.path;
    return r;
  }

 private:
  Services& svc_;
};

// ------------------------------------------------------------------ write_file
class WriteFileTool : public Tool {
 public:
  explicit WriteFileTool(Services& s) : svc_(s) {}
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "write_file";
    t.description = "Create or overwrite a text file (UTF-8). The previous version of an overwritten file is backed up automatically. For small changes to an existing file prefer edit_file.";
    t.parameters = {{"type", "object"},
                    {"properties", {{"path", {{"type", "string"}}},
                                    {"content", {{"type", "string"}, {"description", "Full file content"}}},
                                    {"mode", {{"type", "string"}, {"enum", json::array({"create", "overwrite", "append"})}, {"description", "create fails if the file exists. Default overwrite"}}}}},
                    {"required", json::array({"path", "content"})}};
    return t;
  }
  Assessment assess(const json& a, ToolContext&) override {
    auto p = resolvePath(svc_, argStr(a, "path"), true);
    if (!p.ok) return Assessment::deny(p.error);
    Assessment r;
    bool exists = fileExists(p.path);
    r.summary = std::string(exists ? "Overwrite" : "Create") + " file";
    r.detail = p.path + "\n\n" + preview(argStr(a, "content"), 600);
    r.risk = "medium";
    bool ws = insideWorkspace(svc_, p.path);
    if (!ws) { r.level = Assessment::Level::Ask; r.reason = "This file is outside the Tony workspace folder."; }
    return r;
  }
  ToolResult run(const json& a, ToolContext&) override {
    auto p = resolvePath(svc_, argStr(a, "path"), true);
    if (!p.ok) return ToolResult::error(p.error);
    std::string mode = argStr(a, "mode", "overwrite"), content = argStr(a, "content");
    bool exists = fileExists(p.path);
    if (mode == "create" && exists) return ToolResult::error("The file already exists. Use mode=overwrite or edit_file.");
    if (!ensureDir(parentDir(p.path))) return ToolResult::error("Cannot create the folder " + parentDir(p.path));
    bool backed = exists && backupBeforeChange(svc_, p.path);
    if (mode == "append" && exists) content = readTextFile(p.path, 256u << 20) + content;
    if (!writeTextFile(p.path, content)) return ToolResult::error("Writing failed (is the file open in another program, or read-only?)");
    return ToolResult::ok("Wrote " + humanSize(content.size()) + " to " + p.path + (backed ? " (previous version backed up)" : ""));
  }

 private:
  Services& svc_;
};

// ------------------------------------------------------------------ edit_file
class EditFileTool : public Tool {
 public:
  explicit EditFileTool(Services& s) : svc_(s) {}
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "edit_file";
    t.description = "Replace exact text in an existing file. old_text must match exactly once (include enough surrounding lines to be unique) unless replace_all is true. Read the file first.";
    t.parameters = {{"type", "object"},
                    {"properties", {{"path", {{"type", "string"}}},
                                    {"old_text", {{"type", "string"}}},
                                    {"new_text", {{"type", "string"}}},
                                    {"replace_all", {{"type", "boolean"}}}}},
                    {"required", json::array({"path", "old_text", "new_text"})}};
    return t;
  }
  Assessment assess(const json& a, ToolContext&) override {
    auto p = resolvePath(svc_, argStr(a, "path"), true);
    if (!p.ok) return Assessment::deny(p.error);
    Assessment r;
    r.summary = "Edit file";
    r.detail = p.path + "\n\n- " + replaceAll(preview(argStr(a, "old_text"), 400), "\n", "\n- ") + "\n+ " + replaceAll(preview(argStr(a, "new_text"), 400), "\n", "\n+ ");
    r.risk = "medium";
    if (!insideWorkspace(svc_, p.path)) { r.level = Assessment::Level::Ask; r.reason = "This file is outside the Tony workspace folder."; }
    return r;
  }
  ToolResult run(const json& a, ToolContext&) override {
    auto p = resolvePath(svc_, argStr(a, "path"), true);
    if (!p.ok) return ToolResult::error(p.error);
    if (!fileExists(p.path)) return ToolResult::error("File not found: " + p.path);
    std::string data = readTextFile(p.path, 256u << 20), oldT = argStr(a, "old_text"), newT = argStr(a, "new_text");
    if (oldT.empty()) return ToolResult::error("old_text must not be empty (use write_file to create content).");
    if (looksBinary(data)) return ToolResult::error("Binary file; refusing to edit as text.");
    bool crlf = data.find("\r\n") != std::string::npos;
    if (crlf && oldT.find('\r') == std::string::npos) { oldT = replaceAll(oldT, "\n", "\r\n"); newT = replaceAll(newT, "\n", "\r\n"); }  // keep the file's line endings
    size_t count = 0, first = std::string::npos;
    for (size_t pos = 0; (pos = data.find(oldT, pos)) != std::string::npos; pos += oldT.size()) { if (!count) first = pos; ++count; }
    if (count == 0) return ToolResult::error("old_text was not found. Re-read the file and copy the text exactly (whitespace and line breaks matter).");
    bool all = a.contains("replace_all") && a["replace_all"].is_boolean() && a["replace_all"].get<bool>();
    if (count > 1 && !all) return ToolResult::error("old_text matches " + std::to_string(count) + " places. Add more surrounding context to make it unique, or set replace_all=true.");
    int line = 1 + static_cast<int>(std::count(data.begin(), data.begin() + static_cast<long>(first), '\n'));
    backupBeforeChange(svc_, p.path);
    std::string out;
    if (all) out = replaceAll(data, oldT, newT);
    else out = data.substr(0, first) + newT + data.substr(first + oldT.size());
    if (!writeTextFile(p.path, out)) return ToolResult::error("Writing failed (is the file open in another program, or read-only?)");
    return ToolResult::ok("Edited " + p.path + ": replaced " + std::to_string(all ? count : 1) + " occurrence(s), first at line " + std::to_string(line) + ". Previous version backed up.");
  }

 private:
  Services& svc_;
};

// ------------------------------------------------------------------ list_dir
class ListDirTool : public Tool {
 public:
  explicit ListDirTool(Services& s) : svc_(s) {}
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "list_dir";
    t.description = "List the contents of a folder (name, size, modified). depth 1 = just this folder, up to 3 levels.";
    t.parameters = {{"type", "object"},
                    {"properties", {{"path", {{"type", "string"}, {"description", "Folder path. Default: workspace"}}}, {"depth", {{"type", "integer"}}}}}};
    return t;
  }
  Assessment assess(const json& a, ToolContext&) override {
    auto p = resolvePath(svc_, argStr(a, "path", "."), false);
    if (!p.ok) return Assessment::deny(p.error);
    Assessment r; r.summary = "List folder"; r.detail = p.path;
    return r;
  }
  ToolResult run(const json& a, ToolContext& ctx) override {
    auto p = resolvePath(svc_, argStr(a, "path", "."), false);
    if (!p.ok) return ToolResult::error(p.error);
    std::error_code ec;
    if (!fs::is_directory(u8p(p.path), ec)) return ToolResult::error("Not a folder: " + p.path);
    int depth = std::max(1, std::min(argInt(a, "depth", 1), 3));
    std::ostringstream o;
    o << p.path << "\n";
    int shown = 0;
    std::function<void(const fs::path&, int)> walk = [&](const fs::path& dir, int d) {
      std::vector<fs::directory_entry> es;
      for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) es.push_back(*it);
      std::sort(es.begin(), es.end(), [](const fs::directory_entry& x, const fs::directory_entry& y) {
        bool dx = x.is_directory(), dy = y.is_directory();
        return dx != dy ? dx : x.path().filename() < y.path().filename();
      });
      for (auto& e : es) {
        if (shown >= 300 || ctx.cancel.load()) return;
        std::string full = e.path().u8string();
        if (!svc_.guard->check(full, false).allowed) continue;  // protected items are simply not listed
        bool isDir = e.is_directory(ec);
        o << std::string(static_cast<size_t>(d) * 2, ' ') << (isDir ? "[dir]  " : "       ") << e.path().filename().u8string();
        if (!isDir) o << "  (" << humanSize(e.file_size(ec)) << ")";
        o << "\n";
        ++shown;
        if (isDir && d + 1 < depth && !e.is_symlink(ec)) walk(e.path(), d + 1);
      }
    };
    walk(u8p(p.path), 0);
    if (shown >= 300) o << "... (list truncated at 300 entries)\n";
    if (shown == 0) o << "(empty)\n";
    ToolResult r = ToolResult::ok(o.str());
    r.untrusted = true;  // file names can carry instructions too
    r.source = p.path;
    return r;
  }

 private:
  Services& svc_;
};

// ------------------------------------------------------------------ search_files
class SearchFilesTool : public Tool {
 public:
  explicit SearchFilesTool(Services& s) : svc_(s) {}
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "search_files";
    t.description = "Search file contents with a regular expression and/or find files by name glob, recursively. Skips binary files, .git and node_modules.";
    t.parameters = {{"type", "object"},
                    {"properties", {{"path", {{"type", "string"}, {"description", "Folder to search. Default: workspace"}}},
                                    {"pattern", {{"type", "string"}, {"description", "Regex to look for inside files (optional)"}}},
                                    {"name_glob", {{"type", "string"}, {"description", "File name filter such as *.cpp (optional)"}}},
                                    {"ignore_case", {{"type", "boolean"}}},
                                    {"max_results", {{"type", "integer"}}}}}};
    return t;
  }
  Assessment assess(const json& a, ToolContext&) override {
    auto p = resolvePath(svc_, argStr(a, "path", "."), false);
    if (!p.ok) return Assessment::deny(p.error);
    Assessment r; r.summary = "Search files"; r.detail = p.path + "  pattern=" + argStr(a, "pattern") + "  glob=" + argStr(a, "name_glob");
    return r;
  }
  ToolResult run(const json& a, ToolContext& ctx) override {
    auto p = resolvePath(svc_, argStr(a, "path", "."), false);
    if (!p.ok) return ToolResult::error(p.error);
    std::string pat = argStr(a, "pattern"), glob = lower(argStr(a, "name_glob"));
    if (pat.empty() && glob.empty()) return ToolResult::error("Give a pattern and/or name_glob.");
    std::regex re;
    if (!pat.empty()) {
      try {
        auto flags = std::regex::ECMAScript;
        if (a.contains("ignore_case") && a["ignore_case"].is_boolean() && a["ignore_case"].get<bool>()) flags |= std::regex::icase;
        re = std::regex(pat, flags);
      } catch (const std::regex_error& e) { return ToolResult::error(std::string("Invalid regular expression: ") + e.what()); }
    }
    int maxRes = std::max(1, std::min(argInt(a, "max_results", 60), 300));
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    std::ostringstream o;
    int found = 0, files = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(u8p(p.path), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
      if (ctx.cancel.load() || found >= maxRes || std::chrono::steady_clock::now() > deadline) break;
      std::string name = it->path().filename().u8string(), full = it->path().u8string();
      if (it->is_directory(ec)) {
        if (name == ".git" || name == "node_modules" || name == ".vs" || name == "$RECYCLE.BIN" || !svc_.guard->check(full, false).allowed) it.disable_recursion_pending();
        continue;
      }
      if (!it->is_regular_file(ec) || !svc_.guard->check(full, false).allowed) continue;
      if (!glob.empty() && !PathGuard::globMatch(glob, lower(name))) continue;
      ++files;
      if (pat.empty()) { o << full << "\n"; ++found; continue; }
      if (it->file_size(ec) > (2u << 20)) continue;
      std::string data = readTextFile(full, 2u << 20);
      if (looksBinary(data)) continue;
      int ln = 0;
      std::istringstream in(data);
      std::string line;
      while (std::getline(in, line) && found < maxRes) {
        ++ln;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 2000) continue;
        if (std::regex_search(line, re)) { o << full << ":" << ln << ": " << truncateMiddle(trim(line), 220) << "\n"; ++found; }
      }
    }
    if (found == 0) o << "No matches (" << files << " files checked).\n";
    else if (found >= maxRes) o << "... stopped at " << maxRes << " results.\n";
    ToolResult r = ToolResult::ok(o.str());
    r.untrusted = !pat.empty();
    r.source = p.path;
    return r;
  }

 private:
  Services& svc_;
};

// ------------------------------------------------------------------ file_op
class FileOpTool : public Tool {
 public:
  explicit FileOpTool(Services& s) : svc_(s) {}
  ToolSpec spec() const override {
    ToolSpec t;
    t.name = "file_op";
    t.description = "mkdir, copy, move/rename, or delete (to the Recycle Bin) a file or folder.";
    t.parameters = {{"type", "object"},
                    {"properties", {{"op", {{"type", "string"}, {"enum", json::array({"mkdir", "copy", "move", "delete"})}}},
                                    {"path", {{"type", "string"}}},
                                    {"dest", {{"type", "string"}, {"description", "Target path for copy/move"}}}}},
                    {"required", json::array({"op", "path"})}};
    return t;
  }
  Assessment assess(const json& a, ToolContext&) override {
    std::string op = argStr(a, "op");
    bool needsDest = op == "copy" || op == "move";
    auto src = resolvePath(svc_, argStr(a, "path"), op != "copy");
    if (!src.ok) return Assessment::deny(src.error);
    Assessment r;
    r.summary = op == "delete" ? "Move to Recycle Bin" : "File operation: " + op;
    r.detail = src.path;
    if (needsDest) {
      auto dst = resolvePath(svc_, argStr(a, "dest"), true);
      if (!dst.ok) return Assessment::deny(dst.error);
      r.detail += "\n-> " + dst.path;
      if (fileExists(dst.path) || dirExists(dst.path)) r.reason = "The destination already exists and would be replaced.";
    }
    r.risk = op == "delete" ? "high" : "medium";
    if (op != "mkdir") r.level = Assessment::Level::Ask;
    return r;
  }
  ToolResult run(const json& a, ToolContext&) override {
    std::string op = argStr(a, "op");
    auto src = resolvePath(svc_, argStr(a, "path"), op != "copy");
    if (!src.ok) return ToolResult::error(src.error);
    std::error_code ec;
    if (op == "mkdir") return ensureDir(src.path) ? ToolResult::ok("Created " + src.path) : ToolResult::error("Could not create " + src.path);
    if (op == "delete") {
      if (PathGuard::normalize(src.path).size() <= 3) return ToolResult::error("Refusing to delete a drive root.");
      return moveToRecycleBin(src.path) ? ToolResult::ok("Moved to the Recycle Bin: " + src.path) : ToolResult::error("Could not delete (in use or permission denied).");
    }
    auto dst = resolvePath(svc_, argStr(a, "dest"), true);
    if (!dst.ok) return ToolResult::error(dst.error);
    if (!fs::exists(u8p(src.path), ec)) return ToolResult::error("Source not found: " + src.path);
    if (op == "copy") {
      fs::copy(u8p(src.path), u8p(dst.path), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
      return ec ? ToolResult::error("Copy failed: " + ec.message()) : ToolResult::ok("Copied to " + dst.path);
    }
    if (op == "move") {
      if (fileExists(dst.path)) backupBeforeChange(svc_, dst.path);
      fs::rename(u8p(src.path), u8p(dst.path), ec);
      if (ec) {  // across drives rename fails: copy then recycle
        fs::copy(u8p(src.path), u8p(dst.path), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (!ec) moveToRecycleBin(src.path);
      }
      return ec ? ToolResult::error("Move failed: " + ec.message()) : ToolResult::ok("Moved to " + dst.path);
    }
    return ToolResult::error("Unknown op '" + op + "'.");
  }

 private:
  Services& svc_;
};

}  // namespace

void registerFileTools(ToolRegistry& reg, Services& svc) {
  reg.add(std::make_unique<ReadFileTool>(svc));
  reg.add(std::make_unique<WriteFileTool>(svc));
  reg.add(std::make_unique<EditFileTool>(svc));
  reg.add(std::make_unique<ListDirTool>(svc));
  reg.add(std::make_unique<SearchFilesTool>(svc));
  reg.add(std::make_unique<FileOpTool>(svc));
}

}  // namespace tony
