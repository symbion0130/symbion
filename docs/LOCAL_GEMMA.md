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

The current implementation health-checks `GET /v1/models` and gives a clear warning when the server is offline. Full lifecycle ownership remains future work: detect `c:\projects\codecat\runtime`, read CodeCat configuration when present, and optionally start Gemma with `runtime\scripts\start-gemma.ps1`. User-visible status should distinguish warm, cold, offline, and model-path-missing states.

## Prompt Budget

Assume a 4096-token context until the runtime config proves otherwise. Normal responses are capped with `local_gemma_max_tokens` and JSON/classifier calls with `local_gemma_json_max_tokens`. Deeper prompt-budget work remains open.
