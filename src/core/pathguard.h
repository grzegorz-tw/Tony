// Tony - file access policy. Pure text logic so it can be tested on any OS.
// The Windows layer additionally resolves symlinks/junctions/8.3 names (see platform/winutil)
// and runs every check on BOTH the path as given and its fully resolved form.
#pragma once
#include <string>
#include <vector>

namespace tony {

struct FileRoot {
  std::string path;
  bool writable = false;
};

class PathGuard {
 public:
  struct Verdict {
    bool allowed = false;
    std::string reason;      // human readable when denied
    std::string normalized;  // normalized form that was evaluated
  };

  void configure(bool restrictToRoots, std::vector<FileRoot> roots, std::vector<std::string> denyPatterns);
  bool restricted() const { return restrict_; }

  // `absPath` must be absolute. Relative paths are rejected - callers resolve them against the workspace first.
  Verdict check(const std::string& absPath, bool forWrite) const;

  static std::string normalize(const std::string& p);  // lower-case, '/' separators, '.'/'..' collapsed
  static bool globMatch(const std::string& pattern, const std::string& text);
  static std::vector<std::string> defaultDenyPatterns();

 private:
  struct Root { std::string norm; bool writable; };
  bool restrict_ = true;
  std::vector<Root> roots_;
  std::vector<std::string> deny_;
};

}  // namespace tony
