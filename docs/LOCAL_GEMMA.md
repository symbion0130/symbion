# Local Gemma

Status: implemented as the default provider in `symbion_v14.py` and `symbion.json`.

## Target Runtime

- Provider id: `local_gemma`
- Base URL: `http://127.0.0.1:8088/v1`
- Model id: `local-gemma`
- Runtime: CodeCat `llama.cpp` server under `c:\projects\codecat\runtime`
- Expected health probe: `GET /v1/models`

## Current Behavior

`local_gemma` uses the OpenAI-compatible chat/completions shape so the implementation stays close to the existing OpenAI-style clients. Normal chat streams from llama.cpp. Small classifier and routing tasks use non-streaming responses without `response_format`, which keeps the path compatible with llama.cpp's OpenAI shim.

Gemma is now the default local-first responder. Cloud providers remain configured as escalation or fallback paths for difficult reasoning, high-stakes questions, long context, or local runtime failure.

## Startup And Status

The current implementation health-checks `GET /v1/models`, reads `c:\projects\codecat\runtime\config\codecat.server.json`, checks whether the configured model path exists, and exposes `/api/local-gemma/status`. If `local_gemma_autostart` is enabled, Symbion can launch `runtime\scripts\start-gemma.ps1` and wait briefly for the server to warm. Status distinguishes warm, cold, offline/unhealthy, and model-path-missing states.

## Prompt Budget

Assume a 4096-token context until the runtime config proves otherwise. Normal responses are capped with `local_gemma_max_tokens`, JSON/classifier calls with `local_gemma_json_max_tokens`, recent raw turns with `local_gemma_recent_turns`, and ambient preamble memory with `local_gemma_context_char_budget`. Deep recall should use memory tools rather than preloading everything.
