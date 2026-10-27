# LLM limits

Slice on `src/prism/stages/llm.cpp`. No llama doctest file exists under `tests/cpp/`, so none was edited. `docs/AI.md` was left as it stands.

## GGUF size

`gguf_is_model` requires a regular file of at least 1 MiB (`kMinRealGguf`, `1 << 20`, 1048576 bytes). `LlamaEngine::bind` and `native_llama_complete` both use it.

A missing path, a non-file, an unreadable size, or a file smaller than 1 MiB does not select backend `llama.cpp`. `bind` leaves `backend` as `"none"` (unless llama-server, Ollama, or the test double already won). `hypothesize` then returns one `NOTRUN` row with `LLM_UNAVAILABLE_MSG`, the same row it already returns when the GGUF path is absent. The loader's own miss string stays `missing GGUF (set PRISM_GGUF)`.

The previous comparison was `> 1'000'000` (a decimal megabyte). `docs/AI.md` still says "more than 1 MB".

## Context and reply length

`docs/AI.md` (Grammars) names an 8192-token context and says the reply may use whatever the prompt leaves. The code already matches that:

- `kNativeCtx` is 8192 (`ctx_params.n_ctx` and `ctx_params.n_batch`).
- There is no `kMaxNew`. `max_new` is `llama_n_ctx(ctx) - n_prompt`.

The port-plan audit's 2048 / 256 figures are older than this tree (`72f07bcce`). They were not restored, and the context was not raised above 8192.

## `PRISM_N_GPU_LAYERS`

The include already used by this file is `llama.h`. `llama_context_params` in that header has no `n_gpu_layers`, so the variable is not read where the context params are filled. `llama_model_params.n_gpu_layers` does exist. `NativeLlama::get` already assigns `native_gpu_layers()` there:

- unset, or a value `std::stoi` cannot parse: `-1`
- Windows when `cl` is not on `PATH`: `0` (the env var is not consulted)

That assignment is unchanged. Unset therefore keeps the previous behavior. A GPU is not required: the Windows fallback is 0 layers, and a CPU-only llama.cpp build has an empty device list so offload stays on the CPU.

Vendored `llama_model_default_params()` initializes `n_gpu_layers` to 999. Passing `-1` straight into this llama.cpp is not the llama-cpp-python "every layer" switch (`common_model_params_to_llama` treats `-1` as "leave the default"). This slice does not remap `-1` to 999.
