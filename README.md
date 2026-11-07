# Tony

A **local, voice-capable computer assistant for Windows**. A native C++ desktop app (no browser, no Electron) that orchestrates several specialised
models running entirely on your own PC:

| Job | Engine | Where it runs |
|---|---|---|
| Reasoning, coding, tool use | **llama.cpp** (`llama-server`, router mode) | GPU + system RAM (MoE experts in RAM) |
| Seeing your screen / images | llama.cpp with a vision model (+ `mmproj`) | GPU, swapped in on demand |
| Speech → text | **whisper.cpp** (`whisper-server`) | CPU (keeps VRAM free) |
| Text → speech | Windows **SAPI** voices, or **Piper** neural voices | CPU |
| Image generation & editing | **stable-diffusion.cpp** (`sd.exe`) | GPU, one process per image |
| Torrents (optional) | **aria2** over RPC | network |

Tony itself (`tony.exe`) is plain C++17: Win32 + Direct3D 11 + [Dear ImGui](https://github.com/ocornut/imgui), libcurl, miniaudio.
It owns the agent loop, the tools, the safety layer, the voice pipeline and the settings; the heavy models are separate processes it starts,
supervises and kills with it (Windows Job Objects), so a crashed model never takes the UI down and VRAM is released the moment a job ends.

## What it can do

* **Chat with tools.** Tony calls tools in a loop (read → act → verify) until the task is done. Everything with side effects asks first.
* **PowerShell** – run commands/scripts (Windows PowerShell 5.1, UTF-8, timeouts, process-tree kill).
* **Files** – read, search, edit (exact-match patches), write, copy/move, delete to Recycle Bin. Overwritten files are backed up automatically. Restricted to folders you choose.
* **Programming help** – edit-and-run workflow: `read_file` → `edit_file` → `powershell` (build/test/git). Coding tips are in the example custom instructions.
* **See the screen** – `look_at_screen` captures any monitor/window and asks the vision model; `look_at_image` for files; coordinates are converted to real screen pixels.
* **Control the PC** (off by default) – `ui_input` types, presses keys, clicks, scrolls – every action needs your approval.
* **Web** – `web_search` (DuckDuckGo, SearXNG or Brave), `fetch_url` (readable text + links), `download_file`; **Tor** (`.onion`) and **I2P** (`.i2p`) through your local Tor/i2pd; proxy, **VPN** control and a VPN kill-switch.
* **Torrents** (off by default) – add/list/remove through aria2, bindable to your VPN adapter, blocked while the VPN is down.
* **Images** – `generate_image`, `edit_image` (instruction editing, e.g. FLUX Kontext) and inpainting with a mask; results appear in the chat.
* **Voice** – hold-to-talk or hands-free with optional wake word ("Hey Tony"), barge-in, sentence-by-sentence spoken replies.
* **Memory, clipboard, system info, open items, custom tools** – and you can add your own PowerShell-template tools in the UI.
* **Settings window** – light/dark theme, proxy/VPN/Tor/I2P, system prompt, custom instructions, models (add/choose/tune/download), image presets, voices, tools (enable + per-tool approval + custom tools), file-access restrictions, shell policy.

## Requirements

* Windows 10 (1809+) or 11, x64
* A GPU with a Vulkan driver (Intel Arc, AMD or NVIDIA). Intel Arc: current Arc driver.
* To build: **Visual Studio 2022** with *Desktop development with C++* (includes CMake). Nothing else – dependencies are fetched by CMake.
* Disk: engines ≈ 0.3 GB, Whisper model ≈ 0.6 GB, chat models 10–70 GB, image models 2–25 GB.

## Quick start

```powershell
git clone <your Tony repo> C:\Tony ; cd C:\Tony      # keep the path ASCII-only (some engines cannot open other paths)
.\scripts\build.ps1 -Package                         # builds dist\Tony\tony.exe (first build downloads and compiles curl + ImGui)
cd dist\Tony
.\scripts\setup.ps1                                  # downloads llama.cpp, whisper.cpp, sd.exe, Piper, aria2 + a Whisper model + a voice
.\tony.exe                                           # Settings > Models > "Download models" or choose a .gguf you already have
```

`setup.ps1` can also fetch a chat model: `.\scripts\setup.ps1 -ChatModelRepo ggml-org/gpt-oss-20b-GGUF -ChatModelPattern MXFP4`.
Run it **before** the first launch, or restart Tony afterwards (it edits `%APPDATA%\Tony\settings.json`).

Prefer compiling the engines yourself (e.g. for a SYCL build)? `.\scripts\build.ps1 -Engines` builds llama.cpp, whisper.cpp and stable-diffusion.cpp at the
pinned versions (needs the Vulkan SDK, or Intel oneAPI with `-DTONY_GPU=sycl`). This path is optional and slower.

## Tuning for 16 GB VRAM + 128 GB DDR5 + Ryzen 9

* **Use a Mixture-of-Experts model for the chat brain.** Only a few billion parameters are active per token, so the big weights can live in your fast RAM while attention
  runs on the GPU. In *Settings > Models*, leave *GPU layers* on **auto** (llama.cpp fits what it can into free VRAM) and raise *MoE experts on CPU* until the model loads
  with some VRAM to spare for the context. See [docs/MODELS.md](docs/MODELS.md).
* **GPU memory policy = swap** (default): chat, vision and image models take turns on the card. Switching costs seconds because the weights are in the page cache.
* **Whisper runs on the CPU** by default – a Ryzen 9 transcribes a sentence in about a second and the GPU stays free.
* **Backend choice for Arc:** Vulkan is the default and the easiest. `setup.ps1 -Gpu sycl` / `-Gpu openvino` fetch the other official Windows builds; benchmark with
  `engines\llama\llama-bench.exe` and keep whichever is faster for your model.
* Quantize the KV cache (`q8_0`) in the model settings to fit longer contexts.

## Using Tony

* **Enter** sends, **Ctrl+Enter** adds a new line. **Stop** cancels generation and any running tool. **Esc** silences speech.
* **Mic button** cycles *off → hold-to-talk (F6 by default) → hands-free*. **Voice button** cycles *when I talk → always → silent*.
* Approval dialogs show the exact command, path or URL. *Allow for this chat* is offered only for low-risk, untainted actions.
* The sidebar shows engine status, voice, network, file restrictions, and a warning when external content is in the chat (see Security).
* Right-click a chat to rename, export as Markdown, or delete.

## Safety model (short version – details in [docs/SECURITY.md](docs/SECURITY.md))

1. Tools declare what they need; the agent decides: **auto**, **ask**, or **deny**. Dangerous PowerShell is detected and always asks.
2. **Taint tracking**: once web pages, files, clipboard or screenshots are in the conversation, side-effect actions can no longer be pre-approved.
3. File tools run through a **path guard** (allow-list of folders, protected locations always blocked, symlink/8.3/ADS/device-path tricks rejected).
4. Everything is **audited** (`%APPDATA%\Tony\audit.log`); overwritten files are **backed up**; deletes go to the **Recycle Bin**.
5. Engines listen on **127.0.0.1 only** with a random API key per start and are **never** routed through your proxy/VPN settings.

## Repository layout

```
src/core        portable logic: agent loop, settings, path guard, command risk, chat store   (unit-tested on any OS)
src/net         libcurl client, network policy (proxy/Tor/I2P/VPN kill-switch)               (tested)
src/engines     llama-server / whisper-server / sd.exe wrappers, OpenAI-compatible backend     (parsers + backend tested)
src/tools       the tools the model can call
src/platform    Windows layer: processes, audio, screen capture, input, dialogs, DPAPI
src/ui          Win32 + D3D11 + ImGui application
scripts         setup.ps1 (download engines/models), build.ps1
tests           unit tests + a streaming integration test against a mock llama-server
cmake, resources  build files, manifest, icon
```

## Tests and what has (and has not) been verified

`ctest` runs four suites on any OS: core/path-guard/risk/VAD/SSE/HTML (194 checks), agent policy with a scripted fake model (107), engine argument builders + parsers (44),
and a **real streaming integration test** (libcurl → SSE → tool-call assembly → cancel) against `tests/mock_llama.py`.

The Windows application was **cross-compiled and linked with MinGW-w64 from Linux** against the real pinned sources (ImGui 1.93, miniaudio 0.11.25, curl 8.23 with the
Windows TLS stack, stb, nlohmann/json): the result is a valid 64-bit Windows GUI executable that imports only system DLLs, with zero compiler warnings in Tony's own code
and its manifest (per-monitor DPI, UTF-8, long paths), version info and icon embedded. Visual Studio / MSVC is the intended toolchain; that build, and the program itself,
**have not been run on Windows hardware by the author**. Expect rough edges on first launch (layout, audio devices, driver quirks) and please report them with the files from
`%LOCALAPPDATA%\Tony\logs`.

`scripts\setup.ps1` was parse-checked and its release-asset resolution (llama.cpp b11690 Vulkan/SYCL/OpenVINO/CPU, whisper.cpp, stable-diffusion.cpp, newest-release lookup) was
verified against live GitHub using PowerShell 7; it has not been run on Windows PowerShell 5.1, and the Hugging Face downloads (Whisper model, Piper voices) were not exercised.
The llama.cpp router/preset syntax, server flags, whisper-server and Piper CLI options and the sd.cpp command line were each checked against the pinned upstream sources and release
contents. `-LlamaTag latest` picks the newest release but newer builds may change flags: the default pinned tag is the one this code was written against.

## Known limitations / roadmap

* Screen reading is screenshot + vision model; a UI-Automation (accessibility tree) reader would be faster and exact for text.
* No MCP client yet (custom PowerShell tools cover most needs); no embeddings/RAG over your documents yet.
* DuckDuckGo scraping can hit a human check – use SearXNG or Brave for reliability.
* A public hostname that resolves to a LAN address (DNS rebinding) is not detected by the "local network" confirmation.
* Context trimming drops/shrinks old messages; it does not yet summarise them.

## Third-party software

llama.cpp, whisper.cpp, stable-diffusion.cpp, ggml (MIT) · Dear ImGui (MIT) · miniaudio (public domain / MIT-0) · stb (public domain / MIT) · nlohmann/json (MIT) ·
libcurl (curl license) · Piper (MIT; voices have their own licenses) · aria2 (GPL-2.0, used as a separate program). Model licenses vary: check each model card.

# Generated using Claude Sonnet 5.5
