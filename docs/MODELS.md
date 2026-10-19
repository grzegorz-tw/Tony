# Choosing models for 16 GB VRAM + 128 GB DDR5

Model names move quickly; treat these as starting points and check Hugging Face for newer GGUF releases. Download inside Tony (*Settings > Models > Download*) or with
`scripts\setup.ps1 -ChatModelRepo <owner/repo>`.

## Chat / agent brain (the model that decides which tools to call)

* **Mixture-of-Experts models are the sweet spot.** With 128 GB of fast RAM you can hold very large MoE models; only a few billion parameters are active per token, so
  memory bandwidth - not VRAM - sets the speed. Dense models of 70B+ parameters are slow on this hardware.
* Example (verified to exist): `ggml-org/gpt-oss-20b-GGUF` (`gpt-oss-20b-MXFP4.gguf`) fits entirely in 16 GB and supports tool calling. Larger MoE models (e.g. the 100B+ class) fit in RAM;
  set *MoE experts on CPU* high enough that the attention weights + KV cache fit in VRAM.
* For programming, prefer a coder-tuned MoE of 30B-class size (Qwen3-Coder-30B-A3B family or successors) in Q4/Q5.
* Context: 32k-64k is a good start. Set KV cache types to `q8_0` to roughly halve cache memory.
* Tool calling needs a model whose chat template supports tools; llama.cpp handles the template (`--jinja`) and parses the calls.

### Recipe: large MoE on a 16 GB card

1. Add the model, leave *GPU layers* at **-1 (auto)**.
2. Set *MoE experts on CPU* to a high value (e.g. all layers) and load it (*Load chat model now*). Watch the sidebar chip and `Settings > About > Show llama-server log`.
3. Lower the number until VRAM is nearly full but not overflowing; each step moves expert weights to the GPU and speeds generation.
4. Turn on *Flash attention: on* and *KV cache: q8_0* if you need more context.

## Vision (screen reading)

A 7-12B vision-language model with its `mmproj` file (same repository). Add it as a second model, set *Role: vision*, put the mmproj path in, and choose it with
*Use as vision model*. With the default *swap* policy it replaces the chat model on the GPU only while it looks at something.
Some chat models are multimodal themselves: give *that* entry an mmproj and no separate vision model is needed.

## Speech recognition

`ggml-large-v3-turbo-q5_0.bin` (multilingual, best accuracy/speed trade-off, runs fine on the CPU). Smaller: `ggml-small.bin`, `ggml-base.bin`. Set the language explicitly if you only use one.

## Text to speech

* **Windows SAPI** voices work immediately (install more under Windows *Settings > Time & language > Speech*).
* **Piper** voices sound more natural and run offline on the CPU; Tony can download several from `rhasspy/piper-voices` (pick one in *Settings > Voice*; each needs its `.onnx` **and** `.onnx.json`).

## Images

* SDXL-class single-file checkpoints run comfortably in 16 GB.
* FLUX-class models: use GGUF-quantised diffusion models with separate VAE / CLIP-L / T5-XXL files, and keep *stream weights from RAM* on.
* Editing by instruction: a FLUX Kontext-class model (preset template *FLUX Kontext (edit)*); pass reference images via `edit_image`.
* Check each model's license before use.
