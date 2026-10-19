#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "process.h"

#include <windows.h>

#include <chrono>
#include <mutex>
#include <thread>

#include "core/strutil.h"
#include "winutil.h"

namespace tony {

std::string quoteArg(const std::string& a) {
  if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) return a;
  std::string out = "\"";
  size_t bs = 0;
  for (char c : a) {
    if (c == '\\') { ++bs; continue; }
    if (c == '"') { out.append(bs * 2 + 1, '\\'); out += '"'; bs = 0; continue; }
    out.append(bs, '\\'); bs = 0;
    out += c;
  }
  out.append(bs * 2, '\\');
  return out + "\"";
}

static std::wstring buildCmdline(const std::string& exe, const std::vector<std::string>& args) {
  std::string s = quoteArg(exe);
  for (auto& a : args) s += " " + quoteArg(a);
  return widen(s);
}

static HANDLE makeKillOnCloseJob() {
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (!job) return nullptr;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
  info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
  return job;
}

struct Spawned { HANDLE process = nullptr, job = nullptr; std::string error; };

// Creates the child suspended, puts it in a job, then resumes it, so no grandchild can escape the job.
static Spawned spawn(const std::string& exe, const std::vector<std::string>& args, const std::string& workDir, HANDLE hIn, HANDLE hOut, HANDLE hErr) {
  Spawned sp;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdInput = hIn; si.hStdOutput = hOut; si.hStdError = hErr;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = buildCmdline(exe, args);
  std::wstring wd = widen(workDir);
  if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, nullptr,
                      wd.empty() || !dirExists(workDir) ? nullptr : wd.c_str(), &si, &pi)) {
    sp.error = "cannot start " + fileName(exe) + " (error " + std::to_string(GetLastError()) + ")";
    return sp;
  }
  sp.job = makeKillOnCloseJob();
  if (sp.job) AssignProcessToJobObject(sp.job, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  sp.process = pi.hProcess;
  return sp;
}

static SECURITY_ATTRIBUTES inheritable() {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  return sa;
}

RunResult runProcess(const std::string& exe, const std::vector<std::string>& args, const std::string& workDir, int timeoutMs, size_t maxBytes,
                     const std::atomic<bool>* cancel, const std::function<void(const std::string&)>& onChunk, const std::string& stdinData) {
  RunResult rr;
  SECURITY_ATTRIBUTES sa = inheritable();
  HANDLE outR = nullptr, outW = nullptr, inR = nullptr, inW = nullptr;
  if (!CreatePipe(&outR, &outW, &sa, 0)) { rr.error = "pipe failed"; return rr; }
  SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
  if (!CreatePipe(&inR, &inW, &sa, 0)) { CloseHandle(outR); CloseHandle(outW); rr.error = "pipe failed"; return rr; }
  SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);

  Spawned sp = spawn(exe, args, workDir, inR, outW, outW);
  CloseHandle(outW);
  CloseHandle(inR);
  if (!sp.process) { CloseHandle(outR); CloseHandle(inW); rr.error = sp.error; return rr; }
  rr.started = true;

  std::thread writer([&] {
    size_t off = 0;
    while (off < stdinData.size()) {
      DWORD n = 0;
      if (!WriteFile(inW, stdinData.data() + off, static_cast<DWORD>(std::min<size_t>(stdinData.size() - off, 1u << 16)), &n, nullptr) || n == 0) break;
      off += n;
    }
    CloseHandle(inW);
  });

  std::string head, tail;
  size_t total = 0;
  const size_t half = std::max<size_t>(maxBytes / 2, 1024);
  std::thread reader([&] {
    char buf[8192];
    DWORD n = 0;
    while (ReadFile(outR, buf, sizeof(buf), &n, nullptr) && n > 0) {
      total += n;
      if (onChunk) onChunk(std::string(buf, n));
      size_t take = n, from = 0;
      if (head.size() < half) { size_t k = std::min(take, half - head.size()); head.append(buf, k); from = k; take -= k; }
      if (take) { tail.append(buf + from, take); if (tail.size() > half) tail.erase(0, tail.size() - half); }
    }
  });

  auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    if (WaitForSingleObject(sp.process, 50) == WAIT_OBJECT_0) break;
    bool timeout = timeoutMs > 0 && std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeoutMs);
    bool canc = cancel && cancel->load();
    if (timeout || canc) {
      rr.timedOut = timeout && !canc;
      rr.cancelled = canc;
      if (sp.job) TerminateJobObject(sp.job, 1); else TerminateProcess(sp.process, 1);
      WaitForSingleObject(sp.process, 2000);
      break;
    }
  }
  DWORD code = 0;
  GetExitCodeProcess(sp.process, &code);
  rr.exitCode = static_cast<int>(code);
  CloseHandle(sp.process);
  if (sp.job) CloseHandle(sp.job);  // kills any stragglers still holding the pipe
  reader.join();
  writer.join();
  CloseHandle(outR);

  size_t omitted = total > head.size() + tail.size() ? total - head.size() - tail.size() : 0;
  rr.output = head + (omitted ? "\n...[" + std::to_string(omitted) + " bytes omitted]...\n" : "") + tail;
  return rr;
}

RunResult runPowerShell(const std::string& script, const std::string& workDir, int timeoutSec, size_t maxBytes, const std::atomic<bool>* cancel) {
  wchar_t sys[MAX_PATH];
  UINT n = GetSystemDirectoryW(sys, MAX_PATH);
  std::string exe = narrow(std::wstring(sys, n)) + "\\WindowsPowerShell\\v1.0\\powershell.exe";
  std::string full = "$ProgressPreference='SilentlyContinue'; [Console]::OutputEncoding=[Text.Encoding]::UTF8; $OutputEncoding=[Text.Encoding]::UTF8; " + script;
  std::string b64 = base64Encode(toUtf16LeBytes(full));
  return runProcess(exe, {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-EncodedCommand", b64}, workDir, timeoutSec * 1000, maxBytes, cancel);
}

// ------------------------------------------------------------------ ManagedProcess
bool ManagedProcess::start(const std::string& exe, const std::vector<std::string>& args, const std::string& workDir, const std::string& logPath, std::string* err) {
  stop();
  SECURITY_ATTRIBUTES sa = inheritable();
  HANDLE log = CreateFileW(widen(logPath).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  Spawned sp = spawn(exe, args, workDir, nul, log, log);
  if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (!sp.process) { if (err) *err = sp.error; return false; }
  process_ = sp.process;
  job_ = sp.job;
  return true;
}

bool ManagedProcess::running() const {
  return process_ && WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT;
}

void ManagedProcess::stop() {
  if (job_) { TerminateJobObject(static_cast<HANDLE>(job_), 0); CloseHandle(static_cast<HANDLE>(job_)); job_ = nullptr; }
  if (process_) {
    if (WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT) { TerminateProcess(static_cast<HANDLE>(process_), 0); WaitForSingleObject(static_cast<HANDLE>(process_), 3000); }
    CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
  }
}

// ------------------------------------------------------------------ PipedProcess
bool PipedProcess::start(const std::string& exe, const std::vector<std::string>& args, const std::string& workDir, std::string* err) {
  kill();
  SECURITY_ATTRIBUTES sa = inheritable();
  HANDLE outR, outW, inR, inW;
  if (!CreatePipe(&outR, &outW, &sa, 1 << 16) || !CreatePipe(&inR, &inW, &sa, 1 << 16)) { if (err) *err = "pipe failed"; return false; }
  SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  Spawned sp = spawn(exe, args, workDir, inR, outW, nul);
  CloseHandle(outW);
  CloseHandle(inR);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (!sp.process) { CloseHandle(outR); CloseHandle(inW); if (err) *err = sp.error; return false; }
  process_ = sp.process; job_ = sp.job; in_ = inW; out_ = outR;
  return true;
}

bool PipedProcess::write(const std::string& data) {
  if (!in_) return false;
  size_t off = 0;
  while (off < data.size()) {
    DWORD n = 0;
    if (!WriteFile(static_cast<HANDLE>(in_), data.data() + off, static_cast<DWORD>(data.size() - off), &n, nullptr) || n == 0) return false;
    off += n;
  }
  return true;
}

long PipedProcess::read(void* buf, size_t n) {
  if (!out_) return -1;
  DWORD got = 0;
  if (!ReadFile(static_cast<HANDLE>(out_), buf, static_cast<DWORD>(n), &got, nullptr)) return -1;
  return static_cast<long>(got);
}

bool PipedProcess::running() const { return process_ && WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT; }

void PipedProcess::terminate() {
  if (job_) TerminateJobObject(static_cast<HANDLE>(job_), 0);
}

void PipedProcess::kill() {
  if (job_) { TerminateJobObject(static_cast<HANDLE>(job_), 0); CloseHandle(static_cast<HANDLE>(job_)); job_ = nullptr; }
  if (process_) { WaitForSingleObject(static_cast<HANDLE>(process_), 2000); CloseHandle(static_cast<HANDLE>(process_)); process_ = nullptr; }
  if (in_) { CloseHandle(static_cast<HANDLE>(in_)); in_ = nullptr; }
  if (out_) { CloseHandle(static_cast<HANDLE>(out_)); out_ = nullptr; }  // unblocks a reader thread (read fails)
}

}  // namespace tony
