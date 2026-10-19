#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "winutil.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <dxgi.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <fstream>
#include <regex>
#include <sstream>

#include "core/strutil.h"

namespace tony {

std::wstring widen(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
  return w;
}

std::string narrow(const std::wstring& w) {
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
  return s;
}

static std::string knownFolder(const KNOWNFOLDERID& id) {
  PWSTR p = nullptr;
  std::string out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) out = narrow(p);
  CoTaskMemFree(p);
  return out;
}

std::string parentDir(const std::string& path) {
  size_t p = path.find_last_of("\\/");
  return p == std::string::npos ? "" : path.substr(0, p);
}
std::string joinPath(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  return (a.back() == '\\' || a.back() == '/') ? a + b : a + "\\" + b;
}
std::string fileName(const std::string& path) {
  size_t p = path.find_last_of("\\/");
  return p == std::string::npos ? path : path.substr(p + 1);
}

AppPaths detectPaths() {
  AppPaths p;
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
  p.exeDir = parentDir(narrow(std::wstring(buf, n)));
  p.homeDir = knownFolder(FOLDERID_Profile);
  p.docsDir = knownFolder(FOLDERID_Documents);
  p.desktopDir = knownFolder(FOLDERID_Desktop);
  p.downloadsDir = knownFolder(FOLDERID_Downloads);
  p.dataDir = joinPath(knownFolder(FOLDERID_RoamingAppData), "Tony");
  p.cacheDir = joinPath(knownFolder(FOLDERID_LocalAppData), "Tony");
  ensureDir(p.dataDir);
  ensureDir(p.cacheDir);
  return p;
}

bool ensureDir(const std::string& path) {
  if (path.empty()) return false;
  std::error_code ec;
  std::filesystem::create_directories(u8p(path), ec);
  return !ec || dirExists(path);
}
bool fileExists(const std::string& path) {
  DWORD a = GetFileAttributesW(widen(path).c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool dirExists(const std::string& path) {
  DWORD a = GetFileAttributesW(widen(path).c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string readTextFile(const std::string& path, size_t maxBytes) {
  std::ifstream f(u8p(path), std::ios::binary | std::ios::ate);
  if (!f) return "";
  std::streamoff sz = f.tellg();
  if (sz < 0) return "";
  size_t n = std::min(static_cast<size_t>(sz), maxBytes);
  std::string s(n, '\0');
  f.seekg(0);
  f.read(&s[0], static_cast<std::streamsize>(n));
  return s;
}

bool writeTextFile(const std::string& path, const std::string& data) {
  std::string tmp = path + ".tony-tmp";
  {
    std::ofstream f(u8p(tmp), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!f.good()) return false;
  }
  std::error_code ec;
  std::filesystem::rename(u8p(tmp), u8p(path), ec);
  return !ec;
}

static std::string stripVerbatimPrefix(std::string p) {
  if (startsWith(p, "\\\\?\\UNC\\")) return "\\\\" + p.substr(8);
  if (startsWith(p, "\\\\?\\")) return p.substr(4);
  return p;
}

std::string absolutePath(const std::string& path, const std::string& baseDir) {
  std::string p = path;
  bool abs = (p.size() >= 3 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) || startsWith(p, "\\\\") || startsWith(p, "//");
  if (!abs) p = joinPath(baseDir, p);
  std::wstring w = widen(p);
  DWORD n = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
  if (n == 0) return p;
  std::wstring out(n, L'\0');
  n = GetFullPathNameW(w.c_str(), n, &out[0], nullptr);
  out.resize(n);
  return stripVerbatimPrefix(narrow(out));
}

std::string canonicalize(const std::string& path) {
  std::string rest;
  std::string cur = path;
  for (int guard = 0; guard < 64 && !cur.empty(); ++guard) {
    HANDLE h = CreateFileW(widen(cur).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      wchar_t buf[4096];
      DWORD n = GetFinalPathNameByHandleW(h, buf, 4096, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      CloseHandle(h);
      if (n > 0 && n < 4096) {
        std::string real = stripVerbatimPrefix(narrow(std::wstring(buf, n)));
        return rest.empty() ? real : joinPath(real, rest);
      }
    }
    size_t cut = cur.find_last_of("\\/");
    if (cut == std::string::npos) break;
    std::string tail = cur.substr(cut + 1);
    rest = rest.empty() ? tail : joinPath(tail, rest);
    cur = cur.substr(0, cut);
    if (cur.size() == 2 && cur[1] == ':') cur += "\\";
  }
  return path;
}

std::string dpapiProtect(const std::string& plain) {
  DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))}, out{};
  if (!CryptProtectData(&in, L"Tony secret", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return "";
  DWORD n = 0;
  CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n);
  std::string b64(n, '\0');
  CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &b64[0], &n);
  b64.resize(n);
  LocalFree(out.pbData);
  return b64;
}

std::string dpapiUnprotect(const std::string& b64) {
  DWORD n = 0;
  if (!CryptStringToBinaryA(b64.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr)) return "";
  std::string bin(n, '\0');
  if (!CryptStringToBinaryA(b64.c_str(), 0, CRYPT_STRING_BASE64, reinterpret_cast<BYTE*>(&bin[0]), &n, nullptr, nullptr)) return "";
  DATA_BLOB in{n, reinterpret_cast<BYTE*>(&bin[0])}, out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return "";
  std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
  LocalFree(out.pbData);
  return plain;
}

bool moveToRecycleBin(const std::string& path) {
  std::wstring w = widen(path);
  w.push_back(L'\0');  // SHFileOperation wants a double-NUL terminated list
  SHFILEOPSTRUCTW op{};
  op.wFunc = FO_DELETE;
  op.pFrom = w.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
  return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

bool shellOpen(const std::string& target) {
  return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", widen(target).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}
bool revealInExplorer(const std::string& path) {
  std::wstring args = L"/select,\"" + widen(path) + L"\"";
  return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
}

std::string getClipboardText() {
  std::string out;
  if (!OpenClipboard(nullptr)) return out;
  if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
    if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) { out = narrow(p); GlobalUnlock(h); }
  }
  CloseClipboard();
  return out;
}

bool setClipboardText(const std::string& utf8) {
  std::wstring w = widen(utf8);
  if (!OpenClipboard(nullptr)) return false;
  EmptyClipboard();
  HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
  if (!g) { CloseClipboard(); return false; }
  std::memcpy(GlobalLock(g), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
  GlobalUnlock(g);
  bool ok = SetClipboardData(CF_UNICODETEXT, g) != nullptr;
  if (!ok) GlobalFree(g);
  CloseClipboard();
  return ok;
}

static std::string processNameFromPid(DWORD pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return "";
  wchar_t buf[MAX_PATH * 2];
  DWORD n = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
  std::string out;
  if (QueryFullProcessImageNameW(h, 0, buf, &n)) out = fileName(narrow(std::wstring(buf, n)));
  CloseHandle(h);
  return out;
}

std::vector<WindowInfo> listWindows() {
  struct Ctx { std::vector<WindowInfo> v; HWND fg; } ctx{{}, GetForegroundWindow()};
  EnumWindows(
      [](HWND hw, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (!IsWindowVisible(hw) || GetWindow(hw, GW_OWNER) != nullptr) return TRUE;
        if (GetWindowLongW(hw, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
        wchar_t title[512];
        int n = GetWindowTextW(hw, title, 512);
        if (n <= 0) return TRUE;
        WindowInfo w;
        w.title = narrow(std::wstring(title, n));
        DWORD pid = 0;
        GetWindowThreadProcessId(hw, &pid);
        w.pid = pid;
        w.process = processNameFromPid(pid);
        w.foreground = hw == c->fg;
        RECT r{};
        GetWindowRect(hw, &r);
        w.x = r.left; w.y = r.top; w.w = r.right - r.left; w.h = r.bottom - r.top;
        c->v.push_back(std::move(w));
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  return ctx.v;
}

bool vpnAdapterUp(const std::string& pattern) {
  std::regex re;
  try { re = std::regex(pattern, std::regex::ECMAScript | std::regex::icase); } catch (...) { return false; }
  ULONG size = 32 * 1024;
  std::vector<unsigned char> buf(size);
  ULONG rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
  if (rc == ERROR_BUFFER_OVERFLOW) { buf.resize(size); rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size); }
  if (rc != NO_ERROR) return false;
  for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    std::string name = narrow(a->FriendlyName ? a->FriendlyName : L"") + " " + narrow(a->Description ? a->Description : L"");
    if (std::regex_search(name, re)) return true;
  }
  return false;
}

static std::string regString(HKEY root, const wchar_t* sub, const wchar_t* val) {
  wchar_t buf[512];
  DWORD sz = sizeof(buf);
  if (RegGetValueW(root, sub, val, RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS) return narrow(buf);
  return "";
}

std::string systemSummary() {
  std::ostringstream o;
  std::string product = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName");
  std::string disp = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion");
  std::string build = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuild");
  o << "OS: " << product << (disp.empty() ? "" : " " + disp) << " (build " << build << ")\n";
  SYSTEM_INFO si;
  GetNativeSystemInfo(&si);
  o << "CPU: " << trim(regString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString")) << ", " << si.dwNumberOfProcessors << " logical cores\n";
  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof(ms);
  if (GlobalMemoryStatusEx(&ms)) o << "RAM: " << (ms.ullTotalPhys >> 30) << " GB total, " << (ms.ullAvailPhys >> 30) << " GB free\n";
  IDXGIFactory1* f = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) && f) {
    IDXGIAdapter1* ad = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
      DXGI_ADAPTER_DESC1 d;
      ad->GetDesc1(&d);
      if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) o << "GPU: " << narrow(d.Description) << ", " << (d.DedicatedVideoMemory >> 20) << " MB VRAM\n";
      ad->Release();
    }
    f->Release();
  }
  wchar_t user[256], comp[256];
  DWORD un = 256, cn = 256;
  if (GetUserNameW(user, &un)) o << "User: " << narrow(user) << "\n";
  if (GetComputerNameW(comp, &cn)) o << "Computer: " << narrow(comp) << "\n";
  return o.str();
}

std::string nowLocalString() {
  static const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  static const char* months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
  SYSTEMTIME t;
  GetLocalTime(&t);
  TIME_ZONE_INFORMATION tz;
  DWORD mode = GetTimeZoneInformation(&tz);
  long bias = -(tz.Bias + (mode == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightBias : tz.StandardBias));
  char buf[160];
  std::snprintf(buf, sizeof(buf), "%s, %d %s %d, %02d:%02d (UTC%c%02ld:%02ld)", days[t.wDayOfWeek % 7], t.wDay, months[(t.wMonth - 1) % 12], t.wYear, t.wHour,
                t.wMinute, bias < 0 ? '-' : '+', std::labs(bias) / 60, std::labs(bias) % 60);
  return buf;
}

std::string randomHex(size_t bytes) {
  std::vector<unsigned char> b(bytes);
  BCryptGenRandom(nullptr, b.data(), static_cast<ULONG>(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  static const char* hex = "0123456789abcdef";
  std::string s;
  for (auto c : b) { s += hex[c >> 4]; s += hex[c & 15]; }
  return s;
}

bool isTcpPortFree(int port) {
  static bool wsa = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
  if (!wsa) return false;
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return false;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<u_short>(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  BOOL excl = TRUE;
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
  bool ok = bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
  closesocket(s);
  return ok;
}

int findFreePort(int preferred) {
  for (int p = preferred; p < preferred + 100; ++p) if (isTcpPortFree(p)) return p;
  return preferred;
}

std::string tempFilePath(const std::string& ext) {
  wchar_t dir[MAX_PATH + 2];
  DWORD n = GetTempPathW(MAX_PATH + 1, dir);
  return narrow(std::wstring(dir, n)) + "tony_" + randomHex(6) + ext;
}

std::string timestampForFilename() {
  SYSTEMTIME t;
  GetLocalTime(&t);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  return buf;
}

bool whichExe(const std::string& name, std::string* full) {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = SearchPathW(nullptr, widen(name).c_str(), L".exe", static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])), buf, nullptr);
  if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return false;
  if (full) *full = narrow(std::wstring(buf, n));
  return true;
}

}  // namespace tony
