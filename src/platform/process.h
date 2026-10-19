// Tony - child processes: captured runs (PowerShell, sd-cli), long-lived engines (llama-server, whisper-server, aria2) and piped helpers (Piper).
// Every child lives in a Job Object with KILL_ON_JOB_CLOSE, so engines never outlive Tony - even if Tony crashes.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace tony {

struct RunResult {
  bool started = false;
  int exitCode = -1;
  bool timedOut = false, cancelled = false;
  std::string output;  // stdout + stderr, head and tail kept when very long
  std::string error;   // why it could not start
};

std::string quoteArg(const std::string& a);  // Windows command-line quoting

RunResult runProcess(const std::string& exe, const std::vector<std::string>& args, const std::string& workDir, int timeoutMs, size_t maxBytes,
                     const std::atomic<bool>* cancel, const std::function<void(const std::string&)>& onChunk = {}, const std::string& stdinData = "");

// Runs a script in Windows PowerShell 5.1 with UTF-8 output, no profile, no prompts.
RunResult runPowerShell(const std::string& script, const std::string& workDir, int timeoutSec, size_t maxBytes, const std::atomic<bool>* cancel);

class ManagedProcess {
 public:
  ManagedProcess() = default;
  ~ManagedProcess() { stop(); }
  ManagedProcess(const ManagedProcess&) = delete;
  ManagedProcess& operator=(const ManagedProcess&) = delete;

  bool start(const std::string& exe, const std::vector<std::string>& args, const std::string& workDir, const std::string& logPath, std::string* err);
  bool running() const;
  void stop();

 private:
  void* process_ = nullptr;
  void* job_ = nullptr;
};

// stdin/stdout pipes to a long-lived helper.
class PipedProcess {
 public:
  ~PipedProcess() { kill(); }
  bool start(const std::string& exe, const std::vector<std::string>& args, const std::string& workDir, std::string* err);
  bool write(const std::string& data);
  long read(void* buf, size_t n);  // blocks; returns bytes read, 0/-1 at end of stream
  bool running() const;
  void terminate();  // kills the process; a blocked read() then returns. Call before joining a reader thread.
  void kill();       // terminate + close handles

 private:
  void* process_ = nullptr;
  void* job_ = nullptr;
  void* in_ = nullptr;
  void* out_ = nullptr;
};

}  // namespace tony
