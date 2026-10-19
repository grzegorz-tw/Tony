// Tony - native Windows entry point (Win32 window + Direct3D 11 + Dear ImGui). No web view, no Electron, no browser.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi.h>
#include <objbase.h>
#include <shellscalingapi.h>
#include <windows.h>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include <string>

#include "app.h"
#include "platform/winutil.h"
#include "theme.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resizeW = 0, g_resizeH = 0;
bool g_occluded = false;

void createRtv() {
  ID3D11Texture2D* back = nullptr;
  g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
  if (back) { g_dev->CreateRenderTargetView(back, nullptr, &g_rtv); back->Release(); }
}
void destroyRtv() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }

bool createDevice(HWND hwnd) {
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferDesc.RefreshRate = {60, 1};
  sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
  if (hr == DXGI_ERROR_UNSUPPORTED)  // no hardware device (e.g. remote desktop): software rasteriser still works for a UI
    hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
  if (FAILED(hr)) return false;
  createRtv();
  return true;
}

void destroyDevice() {
  destroyRtv();
  if (g_swap) { g_swap->Release(); g_swap = nullptr; }
  if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
  if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
  switch (msg) {
    case WM_SIZE:
      if (wp == SIZE_MINIMIZED) return 0;
      g_resizeW = LOWORD(lp);
      g_resizeH = HIWORD(lp);
      return 0;
    case WM_SYSCOMMAND:
      if ((wp & 0xfff0) == SC_KEYMENU) return 0;  // Alt would open the (nonexistent) window menu and steal focus
      break;
    case WM_DPICHANGED: {
      float scale = static_cast<float>(HIWORD(wp)) / 96.0f;
      tony::setUiScale(scale);
      ImGui::GetStyle().FontScaleDpi = scale;
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

std::string windowsFont(const char* file) {
  wchar_t dir[MAX_PATH];
  UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
  return tony::narrow(std::wstring(dir, n)) + "\\Fonts\\" + file;
}

ImFont* loadFonts(float size) {
  ImGuiIO& io = ImGui::GetIO();
  ImFont* ui = nullptr;
  for (const char* f : {"segoeui.ttf", "arial.ttf", "tahoma.ttf"}) {
    std::string p = windowsFont(f);
    if (tony::fileExists(p)) { ui = io.Fonts->AddFontFromFileTTF(p.c_str(), size); if (ui) break; }
  }
  if (!ui) io.Fonts->AddFontDefault();
  // fallbacks merged into the UI font: symbols and CJK, loaded lazily by the dynamic atlas
  for (const char* f : {"seguisym.ttf", "YuGothM.ttc", "msyh.ttc", "malgun.ttf"}) {
    std::string p = windowsFont(f);
    if (!tony::fileExists(p)) continue;
    ImFontConfig cfg;
    cfg.MergeMode = true;
    io.Fonts->AddFontFromFileTTF(p.c_str(), size, &cfg);
  }
  ImFont* mono = nullptr;
  for (const char* f : {"CascadiaMono.ttf", "CascadiaCode.ttf", "consola.ttf", "lucon.ttf", "cour.ttf"}) {
    std::string p = windowsFont(f);
    if (tony::fileExists(p)) { mono = io.Fonts->AddFontFromFileTTF(p.c_str(), size); if (mono) break; }
  }
  return mono;
}
}  // namespace

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // real pixels: screenshots and click coordinates must match
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ImGui_ImplWin32_EnableDpiAwareness();

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = wndProc;
  wc.hInstance = hInst;
  wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  wc.lpszClassName = L"TonyMainWindow";
  if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
  RegisterClassExW(&wc);

  tony::App app;
  // read the saved window size before the window exists
  int w = 1360, h = 860;
  {
    tony::AppPaths p = tony::detectPaths();
    tony::Settings s;
    std::string err;
    if (tony::loadSettings(tony::joinPath(p.dataDir, "settings.json"), s, &err)) { w = s.ui.windowW; h = s.ui.windowH; }
  }
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Tony", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr, hInst, nullptr);
  if (!hwnd) return 1;
  if (!createDevice(hwnd)) { MessageBoxW(nullptr, L"Direct3D 11 could not be initialised.", L"Tony", MB_ICONERROR); return 1; }

  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // window layout is fixed; nothing to persist
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_dev, g_ctx);

  float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
  tony::setUiScale(dpi);
  ImFont* mono = loadFonts(17.0f);
  ImGui::GetStyle().FontScaleDpi = dpi;

  if (!app.init(hwnd, g_dev)) { MessageBoxW(nullptr, L"Tony could not start.", L"Tony", MB_ICONERROR); return 1; }
  app.setMonoFont(mono);
  ImGui::GetStyle().FontSizeBase = app.savedFontSize();
  tony::applyTheme(app.savedTheme());

  ShowWindow(hwnd, SW_SHOWDEFAULT);
  UpdateWindow(hwnd);

  bool done = false;
  while (!done) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) done = true;
    }
    if (done || app.wantsQuit()) break;
    if (IsIconic(hwnd)) { Sleep(30); continue; }
    if (g_occluded && g_swap->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) { Sleep(30); continue; }
    g_occluded = false;
    if (g_resizeW != 0 && g_resizeH != 0) {
      destroyRtv();
      g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
      g_resizeW = g_resizeH = 0;
      createRtv();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    app.frame();
    ImGui::Render();

    const tony::Palette& pal = tony::palette();
    const float clear[4] = {pal.bg.x, pal.bg.y, pal.bg.z, 1.0f};
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (g_swap->Present(1, 0) == DXGI_STATUS_OCCLUDED) g_occluded = true;
  }

  RECT r;
  if (GetClientRect(hwnd, &r) && !IsZoomed(hwnd)) app.rememberWindowSize(r.right - r.left, r.bottom - r.top);
  app.shutdown();
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  destroyDevice();
  DestroyWindow(hwnd);
  UnregisterClassW(wc.lpszClassName, hInst);
  CoUninitialize();
  return 0;
}
