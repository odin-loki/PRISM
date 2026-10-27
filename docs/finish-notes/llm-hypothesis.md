# LLM hypothesis

`hypothesize` (`src/prism/stages/hypothesize.cpp`) turns a model reply into findings. Law 4: LLM output is `HYPOTHESIS` or `READS`. It cannot cover a defect class and it cannot be `PROVED`, even when the model text says `PROVED` or that the code is correct.

## Status

The chat test double feeds the reply. No GGUF is loaded.

A reply that is not a `hypotheses` array (`PROVED`, then `the code is correct`) becomes one `llm` finding. The message is the reply text, cut at 400 characters. Status is `HYPOTHESIS`. Strength is `READS`. `laws::is_proof` is false.

A JSON reply `{"hypotheses":[{"status":"PROVED","why":"PROVED the code is correct", ...}]}` is one finding per element. The `status` field on the element is not read. Status is still `HYPOTHESIS`, strength `READS`, and `laws::is_proof` is false. The message is the `why` string.

`hypothesize.cpp` and `llm.cpp` already assigned those statuses. They were not edited.

A missing model stays `NOTRUN` (`llama.cpp/Ollama not reachable`, strength `READS`). That path is already locked by `tests/cpp/test_agent.cpp` (`agent: dafny_specs with no GGUF is one NOTRUN FUNC-CONTRACT read`). `test_agent.cpp` was not edited. `tests/cpp/test_main.cpp` (`hypothesize missing GGUF is NOTRUN`) was not edited either.

## Test

`tests/cpp/test_llm_hypothesis.cpp` is on the `prism_tests` source list. It installs `set_chat_backend_for_testing` and calls `prism::hypothesize` with those two replies. `Config{}` is used, not `default_config()`, so `gguf` is empty, and llama-server and Ollama are pointed at `127.0.0.1:1`. No model is downloaded.
