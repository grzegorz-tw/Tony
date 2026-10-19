# Architecture

## Process model

```
tony.exe  (Win32 + D3D11 + ImGui, C++17)                     <- the only thing you launch
 ├─ UI thread ........ draws, owns Settings (draft/live), handles approvals
 ├─ agent thread ..... Agent::runTurn(): model <-> tools loop (one at a time)
 ├─ STT thread ....... audio segments -> whisper-server -> text -> chat
 ├─ TTS thread ....... sentences -> SAPI / Piper -> speakers
 ├─ audio callback ... miniaudio capture, push-to-talk / VAD segmentation
 ├─ status thread .... polls llama-server for model load state (sidebar)
 └─ child processes (each in its own Job Object, KILL_ON_JOB_CLOSE, bound to 127.0.0.1)
      llama-server   router mode, models.ini generated from Settings, --models-max 1|2, random API key
      whisper-server CPU, model kept loaded
      sd.exe         one process per image; exits => VRAM is free again
      aria2c         only when the torrent tool is used
      powershell.exe one per tool call, with timeout and process-tree kill
```

### Why `llama-server` instead of linking `libllama`?

The pinned llama.cpp (b11690) moved its own CLI onto a client/server design and ships a **router mode** that loads/unloads models on demand from a preset file,
with native tool-call parsing for every chat template, vision (`mmproj`), automatic fit-to-VRAM (`--fit`), prompt caching and idle unloading. Re-implementing that
against an internal C API that changes weekly would be more code and less reliable. Using the server's OpenAI-compatible HTTP API still means *llama.cpp, C++, local, no Python*,
and gives crash isolation and VRAM release for free. The boundary is `IChatBackend` (`src/core/types.h`): an in-process `libllama` backend or another server
(`remote` mode: LM Studio, Ollama, vLLM) plugs in without touching the agent.

### Multi-model orchestration on one 16 GB GPU

* `Settings.llama.models[]` -> `buildPresetIni()` -> one router preset per model. `--models-max 1` (policy *swap*) makes the router evict the previous model before loading the next.
* Chat requests name the chat model; `look_at_*` requests name the vision model; the router swaps. Weights are mmap'd, so a swap is a PCIe copy, not a disk read.
* Image jobs first call `/models/unload` for loaded models, then run `sd.exe`; when it exits the VRAM is free and the next chat message reloads the LLM.
* Whisper and TTS run on the CPU so they never compete for VRAM.

## Data flow of one tool call

```
user text ─▶ Agent::runTurn
   ├─ compact history to ~75% of the context window (shrink old tool output, drop oldest turns)
   ├─ system prompt = base + custom instructions + environment (time, PC, folders, network, tools) + memory
   ├─ OpenAiBackend::chat  (SSE stream; reasoning/content deltas -> UI + TTS; tool-call fragments assembled)
   ├─ for each tool call:
   │     parse + validate args ─▶ Tool::assess() ─▶ policy (settings, taint, session grants) ─▶ [approval dialog] ─▶ Tool::run()
   │     result -> sanitize UTF-8 -> truncate -> wrap as EXTERNAL CONTENT if untrusted (sets taint) -> audit line
   └─ repeat until no tool calls, a repeated-call loop is detected, Stop is pressed, or max rounds is reached
```

Invariants enforced by the agent (and covered by `tests/test_agent.cpp`): every assistant tool call gets exactly one tool message (even when stopped),
tools disabled in Settings cannot be called, `alwaysAsk` actions cannot be remembered, tainted chats cannot pre-approve side effects, identical repeated calls are cut off.

## Settings and threading

The UI edits a **draft** copy of `Settings`; *Apply* swaps it into the **live** copy only when nothing is running (agent turn, transcription, model load), so worker threads
never see a half-edited struct. Audio/STT threads read a mutex-protected snapshot. Secrets (remote key, proxy password, Brave key) are DPAPI-encrypted at rest.
Missing keys fall back to defaults, so old settings files keep working after upgrades.

## Unicode and paths

All file I/O goes through `std::filesystem::u8path` / wide Win32 APIs. Engines that are not Unicode-safe (`sd.exe`, Piper) are given short ASCII relative names inside a
private job folder; results are moved to their real location afterwards. Model file paths should still be ASCII.

## Extending Tony

* **New tool:** subclass `Tool` (`src/core/tool.h`), implement `spec()`, `assess()` (what approval it needs) and `run()`, register it in `registerBuiltinTools`. Mark results from outside the machine `untrusted`.
* **No-code tool:** *Settings > Tools > Custom tools* – a PowerShell template with `{placeholders}`; values are inserted as single-quoted literals (including PowerShell's Unicode quote characters).
* **New model backend:** implement `IChatBackend`.
