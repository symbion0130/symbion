# WebView2 Migration Draft

The current desktop shell is Electron. A native WebView2 shell does not exist in the repo yet, so WebView2 work should be treated as a staged migration rather than a replacement already underway.

## Proposed Phases

Phase 1 keeps the existing Python/FastAPI backend and builds a minimal Windows WebView2 host around the local web UI.

Phase 2 lets the native shell own desktop concerns such as process lifecycle, tray integration, Gemma startup/status, single-instance behavior, and SQLite status views.

Phase 3 replaces Python backend modules only where packaging, startup time, or performance justifies the extra native complexity.

## Parity Checklist

Before deprecating Electron, the WebView2 shell should match these user-visible behaviors:

- Launch and stop the backend cleanly.
- Load the local chat UI.
- Provide tray show/hide/quit behavior.
- Enforce a single app instance.
- Surface provider and local-runtime status.
- Preserve local auth/key handling.
- Support update checks or whatever replaces them.
- Shut down without orphaning backend or runtime processes.

## Delivery Choice

The lowest-risk bridge is to serve the existing web UI from the Python backend during Phase 1. Bundling static resources into the native app can wait until the shell is stable and the backend boundary is clearer.
