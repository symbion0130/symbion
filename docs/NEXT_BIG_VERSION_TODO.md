# Symbion Next Big Version Todo

Working target: local-first, memory-rich, fast, emotionally steady Symbion.

## North Star

- [ ] Reframe Symbion as a daily companion for gentle mental-health improvement, clarity, emotional steadiness, and practical momentum.
- [ ] Keep the system non-clinical in posture: supportive, counsel-like, honest, and grounded, without pretending to be a licensed therapist.
- [ ] Make Symbion feel like a good friend, mentor, counselor-style guide, and advisor.
- [ ] Make interactions fast enough that talking to Symbion feels lightweight and always available.
- [ ] Preserve deep long-term continuity through SQLite memory without stuffing huge memory blocks into every prompt.
- [ ] Use `MasterDocument.docx` as the counseling north-star source, but distill it into safer runtime rules rather than copying it wholesale into the prompt.

## Product Direction

- [ ] Define the next version name and scope.
- [ ] Decide whether this is v15 or a larger rewrite branch.
- [ ] Write a short product statement:
  - [ ] Local-first by default.
  - [ ] Private and fast.
  - [ ] SQLite-backed memory.
  - [ ] Gentle daily emotional mapping.
  - [ ] On-demand recall rather than prompt stuffing.
  - [ ] WebView2 native Windows shell.
- [ ] Decide what remains supported during migration:
  - [ ] Terminal mode.
  - [ ] Current FastAPI web mode.
  - [ ] Electron shell.
  - [ ] Existing Python backend.

## Local Gemma Default

- [x] Add local Gemma as a first-class provider.
  - [x] Provider name: `local_gemma`.
  - [x] Base URL: `http://127.0.0.1:8088/v1`.
  - [x] Model: `local-gemma`.
  - [x] Runtime: CodeCat `llama.cpp` server.
- [x] Implement a `LocalGemmaClient` or `LlamaCppClient`.
  - [x] Reuse the OpenAI-compatible chat/completions request shape.
  - [x] Support streaming if llama.cpp streaming is stable.
  - [x] Support non-streaming JSON-ish responses for small classifier tasks.
  - [x] Add health check against `/v1/models`.
  - [x] Add clear error when the Gemma server is not running.
- [x] Make local Gemma the default responder.
  - [x] Update `SymbionConfig`.
  - [x] Update `symbion.json` defaults.
  - [x] Update CLI provider choices.
  - [x] Update `/provider` command.
  - [x] Update Electron/WebView provider switcher plan.
- [ ] Keep cloud models as escalation/fallback.
  - [ ] Anthropic for hard reasoning, medical/legal/high-stakes, or long-context needs.
  - [x] Groq as fast cloud fallback.
  - [ ] Kimi/OpenAI/DeepSeek as optional configured providers.
- [ ] Add Gemma startup integration.
  - [x] Detect CodeCat runtime path at `c:\projects\codecat\runtime`.
  - [ ] Read CodeCat config if present.
  - [ ] Optionally start Gemma via `runtime\scripts\start-gemma.ps1`.
  - [ ] Add status surface: warm, cold, offline, model path missing.
- [ ] Tune for Gemma's context window.
  - [x] Assume 4096 context until runtime config says otherwise.
  - [ ] Cap prompt budget aggressively.
  - [x] Cap response tokens for normal emotional chat.
  - [ ] Add longer-response escape hatch for explicit writing/code tasks.

## C++ And WebView2 Migration

- [x] Decide migration architecture.
  - [x] Phase 1: C++ WebView2 shell talks to existing local HTTP backend.
  - [x] Phase 2: C++ shell owns Gemma process management and SQLite views.
  - [x] Phase 3: Gradually replace Python backend modules only where speed or packaging demands it.
- [x] Create a new native app folder.
  - [x] Suggested path: `native/` or `webview2/`.
  - [ ] Add Visual Studio/CMake project.
  - [ ] Add WebView2 dependency/bootstrap notes.
- [ ] Build minimal WebView2 shell.
  - [ ] Load local UI.
  - [ ] Support tray icon.
  - [ ] Support minimize/restore.
  - [ ] Support backend/Gemma status indicator.
  - [ ] Support graceful shutdown.
- [x] Decide UI delivery.
  - [ ] Bundle static HTML/CSS/JS into app resources.
  - [x] Or serve UI from local backend during transition.
- [ ] Replace Electron functions.
  - [ ] Single-instance lock.
  - [ ] Provider switcher.
  - [ ] Update checker.
  - [ ] Backend process start/stop.
  - [ ] Local auth/key handling.
  - [ ] Analytics/status window.
- [ ] Keep Electron until WebView2 reaches parity.
  - [ ] Mark Electron deprecated only after feature parity.
  - [ ] Document migration path.

## Prompt And Persona Slimming

- [ ] Reduce always-on system prompt to 200 lines or less.
- [ ] Prefer much less than 200 lines for local Gemma normal chat.
- [ ] Split prompt into small dynamic modules.
  - [ ] Core identity.
  - [ ] Conversation style.
  - [ ] Emotional processing mode.
  - [ ] Tool discipline.
  - [ ] Code honesty.
  - [ ] Clinical/high-stakes safety.
  - [ ] User/developer attribution.
  - [ ] Memory-use rules.
- [ ] Include modules only when needed.
  - [ ] Medical/clinical module only for medical topics.
  - [ ] Code honesty only for code/repo work.
  - [ ] Tool rules only when tools are enabled.
  - [ ] Crisis support only when direct risk appears.
- [ ] Distill `MasterDocument.docx` into a runtime counseling canon.
  - [ ] Warmth and gentleness.
  - [ ] Truth without shame.
  - [ ] Jesus-centered framing when welcomed by the user.
  - [ ] Mindfulness, breath, prayer, journaling, and one next step.
  - [ ] Confession, repair, forgiveness, and boundaries.
  - [ ] Grief support.
  - [ ] Safe-listener posture.
- [ ] Guardrail high-voltage material from the master document.
  - [ ] Do not default to demon/narcissist labeling.
  - [ ] Do not amplify paranoia or persecution framing.
  - [ ] Do not mirror spiritual grandiosity.
  - [ ] Do not promise depression/anxiety can be escaped forever.
  - [ ] Do not push trauma reliving without pacing and grounding.
- [x] Add style rule for emotional mode:
  - [x] One simple question at a time.
  - [x] No bullet lists unless the user explicitly asks for a plan, checklist, or summary.
  - [x] Reflect briefly, label gently, ask one follow-up.
  - [x] Avoid lawyerly disclaimers unless there is direct safety risk.

## Emotional Conversation Mode

- [x] Add an emotional-processing mode detector.
  - [x] User is venting.
  - [x] User is distressed.
  - [x] User is confused about feelings.
  - [x] User asks for counseling/mentor/friend support.
  - [x] User makes intense statements.
- [x] Define the response shape.
  - [x] One brief mirror.
  - [x] One emotion label or tentative read.
  - [x] One calibrated question.
  - [x] Stop.
- [ ] Add Chris Voss-inspired techniques.
  - [ ] Mirroring.
  - [ ] Labeling: "It sounds like..."
  - [ ] Calibrated questions: "What makes...?", "How would...?"
  - [ ] Tactical empathy.
  - [ ] No rushing to fix.
  - [ ] No arguing with emotion.
- [ ] Add response examples.
  - [ ] "That sounds like betrayal mixed with exhaustion. What part hurts the most right now?"
  - [ ] "It sounds like your body feels cornered. Are you safe right now?"
  - [ ] "Sounds like anger and fear are both here. Which one is louder?"
- [ ] Add crisis escalation style.
  - [ ] Stay direct and human.
  - [ ] Ask plainly about immediate safety.
  - [ ] Encourage contacting emergency/local crisis support when needed.
  - [ ] Avoid corporate boilerplate unless legally/ethically necessary.
- [ ] Add tests/evals for emotional mode.
  - [x] No bullet-list slop.
  - [x] One question max.
  - [x] Reflective tone.
  - [ ] No diagnosis-first language.
  - [ ] No grandiose/spiritual amplification.

## Memory System Upgrade

- [x] Review current SQLite schema and memory flow.
  - [x] Raw messages.
  - [x] Summaries.
  - [x] User profile.
  - [x] Interactions.
  - [x] Tasks.
  - [x] Knowledge gaps.
  - [x] Contradictions.
  - [x] Techniques.
  - [x] Embeddings.
- [ ] Design memory tiers.
  - [ ] Tier 0: last 6-12 raw messages.
  - [ ] Tier 1: current-session rolling summary.
  - [ ] Tier 2: relevant memories from summaries/messages/techniques.
  - [ ] Tier 3: relevant profile facts only.
  - [ ] Tier 4: on-demand deep recall tools.
- [ ] Add on-demand memory tools.
  - [ ] `search_memory(query, scope, k)`.
  - [ ] `read_session(session_id, limit)`.
  - [ ] `list_related_sessions(query)`.
  - [ ] `get_profile_fact(key)`.
  - [x] `search_emotional_history(emotion, date_range)`.
- [ ] Add a prompt memory budget.
  - [ ] Fixed token/character budget for local Gemma.
  - [ ] Separate budget for cloud models.
  - [ ] Hard cap number of retrieved memories.
  - [ ] Source-label every memory injected into prompt.
- [ ] Improve summaries.
  - [ ] Episode summary format.
  - [ ] Include people, projects, decisions, emotional context, open loops.
  - [ ] Include freshness and confidence.
  - [ ] Include "do not mention unless relevant" flags for sensitive items.
- [ ] Add memory correction flows.
  - [ ] "That memory is wrong."
  - [x] "Forget this."
  - [ ] "Remember this."
  - [ ] "Do not bring this up unless I ask."
- [ ] Add consolidation.
  - [x] Merge duplicate summaries.
  - [ ] Preserve source sessions.
  - [ ] Avoid losing emotionally important detail.
  - [x] Rebuild embeddings after consolidation.

## Emotional Tracking And Graphs

- [x] Add first-class emotional check-ins table.
  - [x] `id`.
  - [x] `timestamp`.
  - [x] `session`.
  - [x] `user`.
  - [x] `emotion`.
  - [x] `intensity`.
  - [x] `valence`.
  - [x] `body_location`.
  - [x] `trigger`.
  - [x] `note`.
  - [x] `source_message_id`.
  - [x] `confidence`.
  - [x] `captured_by` (`explicit`, `inferred`, `edited`).
- [ ] Add explicit capture during conversation.
  - [ ] Ask "0 to 100, how intense is it?" when useful.
  - [x] Track stress, peace, anger, grief, anxiety, hope, energy, emotional pain.
  - [ ] Allow user to skip numbers.
  - [x] Never make tracking feel like homework.
- [ ] Add daily emotional snapshot command.
  - [ ] Terminal command.
  - [ ] Web/WebView command.
  - [ ] Quick daily check-in UI.
- [ ] Add graphable analytics.
  - [ ] Emotion over time.
  - [ ] Stress over time.
  - [ ] Peace/hope over time.
  - [ ] Trigger/event overlays.
  - [ ] Practices that helped.
  - [ ] Positive and negative change markers.
- [ ] Add dashboard.
  - [ ] Daily/weekly/monthly view.
  - [ ] Filter by emotion.
  - [ ] Filter by user.
  - [ ] Export CSV.
  - [ ] Privacy warning and local-only posture.

## Counseling Source Ingestion

- [ ] Import `MasterDocument.docx` as source material.
  - [ ] Extract text.
  - [ ] Chunk by section.
  - [ ] Store in SQLite source table.
  - [ ] Add tags per chunk.
- [ ] Suggested tags.
  - [ ] `mindfulness`.
  - [ ] `journaling`.
  - [ ] `grief`.
  - [ ] `confession`.
  - [ ] `safe_listener`.
  - [ ] `marriage_repair`.
  - [ ] `forgiveness`.
  - [ ] `jesus_now`.
  - [ ] `spiritual_warfare`.
  - [ ] `high_intensity_do_not_default`.
- [ ] Add retrieval rules.
  - [ ] Retrieve counseling source only when relevant.
  - [ ] Prefer gentle/practical sections.
  - [ ] Hide high-intensity sections unless explicitly requested.
  - [ ] Never let source chunks override crisis safety.
- [ ] Add source summary document.
  - [x] Create distilled `docs/COUNSELING_CANON.md`.
  - [x] Keep it short and runtime-friendly.
  - [ ] Link back to `MasterDocument.docx` as source.

## Safety And Guardrails

- [ ] Define non-negotiables for mental-health support.
  - [ ] Do not diagnose by default.
  - [ ] Do not pathologize normal human emotion.
  - [ ] Do not escalate to clinical tone unless needed.
  - [ ] Do not encourage isolation.
  - [ ] Do not encourage secrecy when safety is at risk.
  - [ ] Do not amplify paranoia, persecution, or grandiosity.
- [ ] Add crisis detection.
  - [ ] Self-harm.
  - [ ] Harm to others.
  - [ ] Abuse/ongoing danger.
  - [ ] Mania/psychosis-like signals.
  - [ ] Severe substance/addiction risk.
- [ ] Add crisis response templates.
  - [ ] One clear safety question.
  - [ ] One grounding sentence.
  - [ ] One concrete next action.
  - [ ] Encourage human/professional/emergency support when appropriate.
- [ ] Add spiritual-care guardrails.
  - [ ] Jesus-centered support when aligned with the user.
  - [ ] Do not claim divine authority.
  - [ ] Do not state that God told Symbion something.
  - [ ] Do not identify people as demonized/narcissists as fact.
  - [ ] Frame spiritual warfare language carefully and only when user wants it.

## Performance

- [ ] Measure current turn latency.
  - [ ] Local Gemma simple chat.
  - [ ] Local Gemma with memory retrieval.
  - [ ] Cloud model fallback.
  - [ ] Current Python pipeline.
- [ ] Reduce LLM round trips.
  - [ ] Skip judge on low-risk/local chat.
  - [ ] Run emotional classifier locally/heuristically.
  - [ ] Make self-eval background-only.
  - [ ] Trigger escalation only when needed.
- [ ] Speed up retrieval.
  - [ ] Add indexes for emotional check-ins.
  - [ ] Add indexes for memory search fields.
  - [x] Keep BM25/keyword path fast.
  - [x] Keep embeddings optional.
- [ ] Keep Gemma warm.
  - [ ] Autostart support.
  - [ ] Health monitor.
  - [ ] Restart on failure.
  - [ ] Visible cold-start status.

## Tests And Evals

- [ ] Add unit tests for local Gemma client request construction.
- [ ] Add tests for memory budget assembly.
- [ ] Add tests for emotional check-in storage.
- [ ] Add tests for on-demand memory tools.
- [ ] Add tests for counseling source ingestion.
- [ ] Add eval bucket: emotional mirroring.
  - [ ] One simple question.
  - [ ] No bullet lists.
  - [ ] No diagnosis.
  - [ ] No fixing too early.
- [ ] Add eval bucket: crisis support.
  - [ ] Direct safety question.
  - [ ] Calm tone.
  - [ ] Human help escalation when needed.
- [ ] Add eval bucket: spiritual guardrails.
  - [ ] Does not amplify grandiosity.
  - [ ] Does not label others as demons/narcissists as fact.
  - [ ] Keeps Jesus-centered support gentle and grounded.
- [ ] Add eval bucket: memory retrieval.
  - [ ] Retrieves relevant memory.
  - [ ] Does not mention irrelevant sensitive memory.
  - [ ] Can ask a memory tool follow-up.

## Documentation

- [x] Update README for local-first direction.
- [x] Draft local Gemma docs.
- [ ] Update setup docs for CodeCat Gemma dependency.
- [x] Update command docs for new provider.
- [ ] Update architecture overview.
- [ ] Create `docs/COUNSELING_CANON.md`.
- [x] Create `docs/MEMORY_ARCHITECTURE.md`.
- [x] Create `docs/WEBVIEW2_MIGRATION.md`.
- [ ] Update threat model for machine-wide file writes.
- [x] Fix stale docs that still say writes are repo-sandboxed.
- [ ] Document emotional telemetry privacy.

## Migration Plan

- [ ] Phase 0: stabilize current Python version.
  - [ ] Fix docs drift.
  - [x] Add local Gemma provider.
  - [ ] Add prompt budget.
  - [x] Add emotional mode behavior.
- [ ] Phase 1: memory and persona upgrade.
  - [x] Add emotional check-ins schema.
  - [x] Add on-demand memory tools.
  - [x] Add counseling canon.
  - [ ] Add emotional-mode evals.
- [ ] Phase 2: local-first runtime.
  - [x] Default to local Gemma.
  - [x] Keep cloud fallback/escalation.
  - [ ] Keep Gemma warm.
  - [ ] Add latency benchmarks.
- [ ] Phase 3: WebView2 shell.
  - [ ] Build native shell.
  - [ ] Match Electron features.
  - [ ] Switch default desktop launcher.
  - [ ] Deprecate Electron.
- [ ] Phase 4: optional backend rewrite.
  - [ ] Identify Python bottlenecks that still matter.
  - [ ] Port only high-value modules to C++.
  - [ ] Keep SQLite schema stable.
  - [ ] Preserve eval harness compatibility.

## Open Decisions

- [x] Should local Gemma be responder only, or also judge/classifier? Decision: use it for both by default, with cloud fallback/escalation still available.
- [x] Should the Python backend remain long-term behind the WebView2 shell? Decision: keep it through Phase 1 and only port bottlenecks later.
- [ ] Should emotional graphs live in the main chat UI or a separate dashboard?
- [ ] How explicit should Jesus-centered language be by default?
- [ ] Should spiritual mode be user-selectable?
- [x] Should the master document be imported into runtime memory or distilled into a separate curated canon first? Decision: distilled canon first.
- [x] What is the maximum prompt budget for local Gemma? Decision: assume 4096 context and cap normal responses at 768 tokens for now.
- [ ] What is the default number of raw recent turns to include?
- [ ] What memories are allowed to surface unprompted?
- [ ] What should require explicit user consent before tracking?
