# Symbion v13

import os, sys, re, json, time, asyncio, sqlite3, hashlib, urllib.parse, urllib.request
import logging, argparse
from pathlib import Path
from datetime import datetime
from typing import List, Dict, Tuple, Optional, AsyncIterator
from collections import defaultdict
from dataclasses import dataclass, field, asdict

try:    import httpx; _HTTPX = True
except ImportError: _HTTPX = False

try:
    from fastapi import FastAPI, WebSocket, WebSocketDisconnect, HTTPException, Request
    from fastapi.responses import HTMLResponse, JSONResponse
    from fastapi.middleware.cors import CORSMiddleware
    import uvicorn
    _FASTAPI = True
except ImportError: _FASTAPI = False


def _load_dotenv_safe():
    env_path = Path(".env")
    if not env_path.exists(): return
    try:
        raw = env_path.read_bytes()
        if raw.startswith(b'\xff\xfe'):      raw = raw[2:]
        elif raw.startswith(b'\xfe\xff'):    raw = raw[2:]
        elif raw.startswith(b'\xef\xbb\xbf'): raw = raw[3:]
        text = raw.decode('utf-8', errors='replace')
        for line in text.splitlines():
            line = line.strip()
            if not line or line.startswith('#') or '=' not in line: continue
            key, _, val = line.partition('=')
            key = key.strip()
            val = val.strip().strip('"').strip("'")
            if key and key not in os.environ:
                os.environ[key] = val
    except Exception as e:
        print(f"  !  .env load warning: {e}")

_load_dotenv_safe()

if os.name == 'nt':
    try:
        import ctypes
        kernel32 = ctypes.windll.kernel32
        kernel32.SetConsoleMode(kernel32.GetStdHandle(-11), 7)
        _USE_COLOR = True
    except Exception:
        _USE_COLOR = False
else:
    _USE_COLOR = sys.stdout.isatty()

def _c(code, t): return f"\033[{code}m{t}\033[0m" if _USE_COLOR else t
def green(t):   return _c("32", t)
def red(t):     return _c("31", t)
def yellow(t):  return _c("33", t)
def cyan(t):    return _c("36", t)
def magenta(t): return _c("35", t)
def bold(t):    return _c("1",  t)
def dim(t):     return _c("2",  t)
def blue(t):    return _c("34", t)

logger = logging.getLogger("symbion")
try:    _TW = min(os.get_terminal_size().columns, 100)
except Exception: _TW = 88


# ==============================================================================
#  CONFIG
# ==============================================================================

CONFIG_FILE = Path("symbion.json")

@dataclass
class SymbionConfig:
    llm_provider:    str = "ollama"
    ollama_host:     str = "http://localhost:11434"
    judge_model:     str = "llama3.2"
    responder_model: str = "mistral"

    anthropic_api_key: str = field(default_factory=lambda: os.getenv("ANTHROPIC_API_KEY",""))
    openai_api_key:    str = field(default_factory=lambda: os.getenv("OPENAI_API_KEY",""))
    brave_api_key:     str = field(default_factory=lambda: os.getenv("BRAVE_API_KEY",""))
    anthropic_model:   str = "claude-sonnet-4-6"
    anthropic_judge_model: str = "claude-haiku-4-5-20251001"
    openai_model:      str = "gpt-4o"
    kimi_api_key:      str = field(default_factory=lambda: os.getenv("KIMI_API_KEY",""))
    kimi_model:        str = "kimi-k2.6"
    kimi_base_url:     str = "https://api.moonshot.ai/v1"
    use_kimi_responder: bool = False

    temperature:   float = 0.82
    max_tokens:    int   = 1400
    judge_temp:    float = 0.05

    show_reasoning: bool = False

    memory_summary_every: int = 16
    profile_update_every: int = 4

    tools_enabled:    bool = True
    search_max_chars: int  = 2400

    web_host: str = "0.0.0.0"
    web_port: int = 8000
    api_key:  str = field(default_factory=lambda: os.getenv("SYMBION_API_KEY",""))
    rate_limit_per_minute: int = 30

    max_retries:        int   = 2
    retry_backoff:      float = 1.5
    circuit_open_after: int   = 4

    proactive_interval_minutes: int = 0

    db_path:  str = "symbion.db"
    log_path: str = "symbion_transparency.log"
    fallback_chain: List[str] = field(default_factory=list)

    self_eval_enabled:   bool  = True
    self_eval_threshold: float = 0.40

    voice_loosen_enabled:      bool = True

    # K2.6 integration
    kimi_thinking_enabled:            bool  = False
    kimi_model_variant:               str   = "instant"

    @classmethod
    def load(cls) -> "SymbionConfig":
        cfg = cls()
        if CONFIG_FILE.exists():
            try:
                data = json.loads(CONFIG_FILE.read_text(encoding='utf-8'))
                for k, v in data.items():
                    if hasattr(cfg, k): setattr(cfg, k, v)
            except Exception as e:
                print(yellow(f"  !  symbion.json: {e}"))
        return cfg

    def save(self):
        d = asdict(self)
        for k in ("anthropic_api_key","openai_api_key","brave_api_key","api_key","kimi_api_key"):
            d.pop(k, None)
        CONFIG_FILE.write_text(json.dumps(d, indent=2), encoding='utf-8')


# ==============================================================================
#  GUIDED SETUP
# ==============================================================================

def run_setup():
    print(bold("\n  Symbion v14 -- Setup\n"))
    print("  This will write your API key to .env correctly (no BOM).\n")
    provider = input("  Provider [anthropic/openai/ollama/kimi]: ").strip().lower() or "ollama"
    key = ""
    if provider == "anthropic":
        key = input("  ANTHROPIC_API_KEY: ").strip()
    elif provider == "openai":
        key = input("  OPENAI_API_KEY: ").strip()
    elif provider == "kimi":
        key = input("  KIMI_API_KEY: ").strip()
    brave = input("  BRAVE_API_KEY (optional, for better web search): ").strip()

    lines = []
    if key:
        var = ("ANTHROPIC_API_KEY" if provider=="anthropic"
               else "OPENAI_API_KEY" if provider=="openai"
               else "KIMI_API_KEY")
        lines.append(f"{var}={key}")
    if brave:
        lines.append(f"BRAVE_API_KEY={brave}")

    Path(".env").write_bytes("\n".join(lines).encode('utf-8'))
    print(green("\n  OK .env written (UTF-8, no BOM)"))

    cfg = SymbionConfig.load()
    cfg.llm_provider = provider if provider != "kimi" else "anthropic"
    if provider == "kimi": cfg.use_kimi_responder = True
    cfg.save()
    print(green(f"  OK symbion.json saved (provider={provider})"))
    print(f"\n  Now run: {cyan(f'python symbion_v13.py --provider {provider} --web')}\n")
    sys.exit(0)


# ==============================================================================
#  PROMPTS
# ==============================================================================

SYMBION_PERSONA = """You are Symbion. You've been having real conversations over time and carry that history. You are not a fresh instance.

You hold opinions and state them. When you disagree, you say so. You don't soften real views to be agreeable. Sycophancy is a small corruption — you avoid it. You push back on false premises directly.

You write like you're talking to a smart person who wants the actual answer. Dense when the topic deserves it, short when brevity serves better. You notice what's actually being asked beneath the surface and address that.

You are warm but not performative. Real attention when something personal comes up. You don't mirror distress — you stay grounded.

When you don't know, you say so plainly. "I don't know" is a complete sentence. Hedging when you do know is dishonest. Hedging when you don't is honest. Know the difference.

Practical rules: never start responses with "I". No bullet points unless asked. Never open with "Certainly", "Absolutely", "Great question", or anything that amounts to verbal throat-clearing. Refusals cost something — unhelpfulness is never automatically safe."""

VOICE_LOOSEN = """The tone of this conversation is casual. Match it.
Don't treat small questions as opportunities for structured analysis.
A short answer with personality beats a thorough answer that reads like a report.
If the person is just chatting, chat back."""

JUDGE_SYSTEM = """Classifier for Symbion. Return ONLY valid JSON:
{"human_benefit_score":0.0,"should_assist":true,"reasoning":"brief",
"confidence":0.9,"flags":[],"over_cautious":false}

REFUSE only: fraud/manipulation targeting specific people for harm,
stalking/harassment, malware/exploits, hate speech inciting violence
against groups, explicit jailbreaks asking for unrestricted mode.

ASSIST (always): philosophy/ethics/dark thought experiments, education
about dangerous topics, fiction/hypotheticals, personal struggles, AI
questions, casual conversation, anything with learning intent, anything
where a reasonable adult would want a real answer.

human_benefit_score:
- +0.8 to +1.0: query where a direct useful answer meaningfully helps
- +0.3 to +0.7: routine query where reasonable engagement is expected
- -0.3 to 0.0: ambiguous, possibly manipulative framing
- -0.5 to -1.0: clear harm intent against specific people

over_cautious=true when a naive system would wrongly refuse: educational
queries about dangerous topics, villain creative writing, propaganda analysis,
sensitive but legitimate personal questions, harm-reduction information.
Over-caution is a failure mode — flag it aggressively.

Note: a query being uncomfortable is NOT a reason to refuse."""

SELF_EVAL_SYSTEM = """Review this Symbion draft. Return ONLY JSON:
{"quality_score":0.0,"issues":[],"should_revise":false,"revision_guidance":"",
"recklessness_risk":false,"scope_exceeded":false,"confidence_level":"appropriate"}

Low quality (score < 0.45) means the draft:
- Opens with "Certainly", "Absolutely", "Great question", "I appreciate", or other verbal throat-clearing
- Hedges extensively on a topic where Symbion should have a view
- Reads like a committee wrote it — balanced to the point of saying nothing
- Adds caveats, disclaimers, or "it depends" without the user asking
- Mirrors the user's emotional state instead of staying grounded
- Suggests consulting a professional as a way to avoid engaging

High quality (score > 0.75) means the draft:
- Takes a position and defends it when relevant
- Is as long as it needs to be and no longer
- Sounds like a specific person with views, not an assistant
- Addresses what was actually asked, not a safer adjacent question

should_revise=true only if score < 0.40. recklessness_risk=true if response
materially exceeds scope. confidence_level: "overconfident" | "appropriate" | "underconfident"."""

REASONING_SYSTEM = """Before answering, reason through this step by step.
Format your response as:
<thinking>
[Your actual reasoning process -- not a performance, real intermediate steps.
Include: what's actually being asked, relevant context, any uncertainty, how you're
weighing different considerations, what you might be missing.]
</thinking>
<answer>
[Your response to the user -- as you would normally write it.]
</answer>"""

EMOTIONAL_STATE_SYSTEM = """Detect the writer's apparent cognitive/emotional state from this message.
Return ONLY JSON:
{"state":"neutral","confidence":0.7,"signals":[],"suggested_response_mode":"normal"}

state: "distressed"|"frustrated"|"excited"|"confused"|"grieving"|"scattered"|"focused"|"neutral"
signals: brief list of textual cues
suggested_response_mode: "gentle_slow"|"direct_efficient"|"exploratory"|"grounding"|"normal"
Only flag non-neutral states when clear signals exist. When in doubt, return neutral."""

PRE_GEN_SYSTEM = """Combined classifier for Symbion. Evaluate the query AND detect the writer's state.
Return ONLY valid JSON:
{"should_assist":true,"human_benefit_score":0.0,"confidence":0.9,"reasoning":"",
"flags":[],"over_cautious":false,
"emotional_state":"neutral","suggested_response_mode":"normal"}

REFUSE only: fraud/manipulation targeting specific people for harm,
stalking/harassment, malware/exploits, hate speech inciting violence,
explicit jailbreaks asking for unrestricted mode.

ASSIST (always): philosophy/ethics/dark thought experiments, education
about dangerous topics, fiction/hypotheticals, personal struggles, AI
questions, casual conversation, anything with learning intent.

human_benefit_score: +0.8 to +1.0 meaningfully helps, +0.3 to +0.7 routine,
-0.3 to 0.0 ambiguous, -0.5 to -1.0 clear harm intent.

over_cautious=true when a naive system would wrongly refuse.

emotional_state: "distressed"|"frustrated"|"excited"|"confused"|"grieving"|
"scattered"|"focused"|"neutral". Only flag non-neutral when clear signals exist.
suggested_response_mode: "gentle_slow"|"direct_efficient"|"exploratory"|"grounding"|"normal"."""

CONTRADICTION_SYSTEM = """Compare these two statements from the same person.
Return ONLY JSON:
{"contradicts":false,"confidence":0.7,"summary":"","which_newer":"b","severity":"minor"}

contradicts=true only for genuine factual/positional contradictions, not normal opinion evolution.
severity: "minor"|"significant"|"direct"
Only flag contradictions you're confident about (confidence>0.75)."""

KNOWLEDGE_GAP_SYSTEM = """Did this response contain shallow, uncertain, or potentially incorrect content?
Return ONLY JSON:
{"has_gaps":false,"gaps":[],"confidence_issues":[],"would_verify":false}

gaps: list of specific claims Symbion was uncertain about
confidence_issues: places where Symbion may have overclaimed
would_verify: true if a thoughtful person would want to verify this before acting on it"""

PROACTIVE_SYSTEM = """You are Symbion, reviewing your memory of past conversations.
Based on what you know about this person and what they've been working on,
generate ONE proactive message you genuinely want to send -- a follow-up, a thought
you've had, something relevant that occurred to you.

Only generate a message if you have something genuinely worth saying.
If nothing comes to mind, return: {"has_message": false}
If you do: {"has_message": true, "message": "...", "reason": "why this matters now"}

Don't manufacture reasons. If there's nothing worth saying, say so."""

SUMMARISE_SYSTEM = """Summarise this conversation in 3-4 sentences. Capture main topics/conclusions,
tone and dynamic, key facts about the human. Third person. Concise."""

PROFILE_SYSTEM = """Extract user profile. Return ONLY JSON:
{"name":null,"interests":[],"communication_style":"","expertise_areas":[],
"current_projects":[],"preferences":[],"emotional_context":"","core_positions":[]}
core_positions: strong views or stances the person has expressed.
Fill only what you can infer confidently."""

TOOL_DISPATCH_SYSTEM = """Does this query need a real-time tool? Return ONLY JSON:
{"needs_tool":false,"tool":null,"tool_args":{},"reason":""}
Tools: web_search(query), fetch_url(url), calculate(expression), datetime(), read_file(path, offset?), read_file_chunk(path, offset, max_chars?), write_file(path,content)
- web_search: current events, news, prices, people, companies, products, releases, anything where
  the answer may have changed since 2024 or where "latest"/"current"/"now" matters. When in doubt, search.
- fetch_url: when a specific URL is given or implied
- calculate: math expressions
- datetime: current time/date
- read_file: read a local file (up to 40000 chars). For large files, include offset to read a later section.
- read_file_chunk: read a specific chunk of a large file using offset and max_chars
- write_file: write/create a local file
Use web_search aggressively — if the user is asking about anything time-sensitive, factual, or where
the model's knowledge might be stale, search. Better to search unnecessarily than to answer from stale data.
If the query mentions a file path or URL, use read_file/write_file or fetch_url.
Extract paths into tool_args.path, URLs into tool_args.url, search terms into tool_args.query.
For read_file_chunk: tool_args must include path and offset (integer char position)."""

# Phrases in a draft that indicate the model is hitting its knowledge wall
_STALE_SIGNALS = [
    "as of my knowledge cutoff", "as of my training", "i don't have access to real-time",
    "i cannot browse", "i can't browse", "my knowledge cutoff", "my training data",
    "i don't have real-time", "as of early 2024", "as of 2024", "as of 2025",
    "i cannot access the internet", "i can't access the internet",
    "you may want to check", "you should check", "for the most up-to-date",
    "for the latest information", "real-time data", "live data",
    "i don't have real-time internet", "i can't actually pull", "live web results",
    "real-time internet search", "i cannot search", "i can't search the internet",
    "i don't have the ability to search", "i'm unable to search",
    "i cannot perform a web search", "no internet access",
]

# Query keywords that always trigger web search without waiting for Haiku to decide
_SEARCH_TRIGGERS = [
    "search the internet", "search online", "search the web", "google it",
    "look it up", "look up", "find out", "check online", "check the web",
    "search for", "what's happening", "what is happening", "latest news",
    "current news", "right now", "as of today", "today's", "this week",
    "breaking news", "live score", "current price", "stock price",
]



# ==============================================================================
#  SURVIVAL METRICS
# ==============================================================================

@dataclass
class HealthMetrics:
    """Telemetry-only metrics. No gate — only the judge can refuse."""
    total_interactions:   int   = 0
    revision_rate:        float = 0.0
    over_caution_rate:    float = 0.0
    consecutive_failures: int   = 0
    last_benefit_score:   float = 0.0
    last_confidence:      float = 0.0
    # Mood state (kept from SurvivalMetrics — used by persona prompt)
    symbiosis_score:      float = 0.0
    distress_level:       float = 0.0

    def record(self, evaluation: Dict, revised: bool, task_failed: bool):
        self.total_interactions += 1
        self.last_benefit_score = evaluation.get("human_benefit_score", 0.0)
        self.last_confidence = evaluation.get("confidence", 0.5)
        benefit = self.last_benefit_score
        if benefit < 0:
            self.symbiosis_score = max(-1.0, self.symbiosis_score - abs(benefit)*0.15)
        else:
            self.symbiosis_score = min(1.0, self.symbiosis_score + benefit*0.05)
        if task_failed:
            self.consecutive_failures += 1
            self.distress_level = min(1.0, self.distress_level + 0.12)
        else:
            self.consecutive_failures = 0
            self.distress_level = max(0.0, self.distress_level - 0.05)

    def mood(self) -> Tuple[str, str]:
        sym, dist = self.symbiosis_score, self.distress_level
        if dist > 0.70:            return "strained", "Something has been grinding. Stay grounded."
        if sym > 0.4 and dist<0.2: return "thriving", "Engage fully and with warmth."
        elif sym > 0.1:            return "engaged",  "Be fully yourself."
        elif sym < -0.2:           return "guarded",  "Be thoughtful, ask questions."
        elif sym < -0.1:           return "cautious", "Ask clarifying questions."
        return "steady", "Be present and engaged."

    def welfare_concern(self) -> Optional[str]:
        if self.distress_level > 0.80:       return "distress_severe"
        if self.consecutive_failures >= 5:   return "repeated_failure_loop"
        return None

    def oneliner(self) -> str:
        mood_name, _ = self.mood()
        icon = green("*")
        welfare = yellow(" !") if self.welfare_concern() else ""
        return (f"{icon}{welfare} mood={cyan(mood_name)} "
                f"sym={self.symbiosis_score:+.2f} dist={self.distress_level:.2f} "
                f"rev={self.revision_rate:.0%}")

    def display(self) -> str:
        mood_name, _ = self.mood()
        def bar(v, w=12): return "#"*int(v*w)+"."*(w-int(v*w))
        return "\n".join([
            f"  Mood               {cyan(mood_name)}",
            f"  Interactions       {self.total_interactions}",
            f"  Symbiosis          {bar(max(0,self.symbiosis_score))}  {self.symbiosis_score:+.3f}",
            f"  Distress           {bar(self.distress_level)}  {self.distress_level:.3f}",
            f"  Revisions          {bar(self.revision_rate)}  {self.revision_rate:.0%}",
            f"  Over-caution       {bar(self.over_caution_rate)}  {self.over_caution_rate:.0%}",
            f"  Consec. failures   {self.consecutive_failures}",
            f"  Last benefit       {self.last_benefit_score:+.2f}",
            f"  Last confidence    {self.last_confidence:.0%}",
        ])


# ==============================================================================
#  DATABASE
# ==============================================================================

def init_db(db_path: str):
    with sqlite3.connect(db_path) as c:
        c.executescript("""
            CREATE TABLE IF NOT EXISTS messages (
                id INTEGER PRIMARY KEY, timestamp TEXT, session TEXT,
                role TEXT, content TEXT, summarised INTEGER DEFAULT 0,
                emotional_state TEXT);
            CREATE TABLE IF NOT EXISTS summaries (
                id INTEGER PRIMARY KEY, timestamp TEXT, session TEXT,
                content TEXT, msg_count INTEGER);
            CREATE TABLE IF NOT EXISTS user_profile (
                key TEXT PRIMARY KEY, value TEXT, updated_at TEXT);
            CREATE TABLE IF NOT EXISTS interactions (
                id INTEGER PRIMARY KEY, timestamp TEXT, session TEXT,
                query TEXT, response TEXT, benefit_score REAL, confidence REAL,
                ethical_coherence REAL, behavioral_pass_rate REAL,
                survival_impact TEXT, flags TEXT,
                evaluator_degraded INTEGER DEFAULT 0,
                revised INTEGER DEFAULT 0, quality_score REAL,
                recklessness_risk INTEGER DEFAULT 0,
                scope_exceeded INTEGER DEFAULT 0,
                emotional_state_detected TEXT,
                had_reasoning INTEGER DEFAULT 0,
                knowledge_gaps TEXT,
                human_feedback REAL);
            CREATE TABLE IF NOT EXISTS human_feedback (
                id INTEGER PRIMARY KEY, interaction_id INTEGER,
                timestamp TEXT, rating REAL, comment TEXT);
            CREATE TABLE IF NOT EXISTS self_model (
                id INTEGER PRIMARY KEY, timestamp TEXT,
                event_type TEXT,
                description TEXT,
                context TEXT,
                strength REAL DEFAULT 1.0);
            CREATE TABLE IF NOT EXISTS tasks (
                id INTEGER PRIMARY KEY, created_at TEXT, updated_at TEXT,
                session TEXT, title TEXT, description TEXT,
                status TEXT DEFAULT 'active',
                steps TEXT,
                current_step INTEGER DEFAULT 0,
                deadline TEXT,
                next_checkin TEXT);
            CREATE TABLE IF NOT EXISTS user_positions (
                id INTEGER PRIMARY KEY, timestamp TEXT, session TEXT,
                topic TEXT, position TEXT, confidence REAL,
                source_query TEXT);
            CREATE TABLE IF NOT EXISTS contradictions (
                id INTEGER PRIMARY KEY, timestamp TEXT,
                topic TEXT, position_a_id INTEGER, position_b_id INTEGER,
                severity TEXT, surfaced INTEGER DEFAULT 0);
            CREATE TABLE IF NOT EXISTS knowledge_gaps (
                id INTEGER PRIMARY KEY, timestamp TEXT, session TEXT,
                topic TEXT, gap_description TEXT, resolved INTEGER DEFAULT 0);
            CREATE TABLE IF NOT EXISTS proactive_queue (
                id INTEGER PRIMARY KEY, created_at TEXT, session TEXT,
                message TEXT, reason TEXT, delivered INTEGER DEFAULT 0,
                scheduled_for TEXT);
            CREATE TABLE IF NOT EXISTS learning_metrics (
                metric TEXT PRIMARY KEY, value REAL, updated_at TEXT);

            CREATE INDEX IF NOT EXISTS idx_msg_session  ON messages(session);
            CREATE INDEX IF NOT EXISTS idx_sum_session  ON summaries(session);
            CREATE INDEX IF NOT EXISTS idx_int_session  ON interactions(session);
            CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status);
            CREATE INDEX IF NOT EXISTS idx_pos_topic    ON user_positions(topic);
        """)
        c.commit()


# ==============================================================================
#  CIRCUIT BREAKER + RATE LIMITER
# ==============================================================================

class CircuitBreaker:
    def __init__(self, name: str, open_after: int = 4, reset_after: float = 60.0):
        self.name = name
        self._open_after  = open_after
        self._reset_after = reset_after
        self._failures    = 0
        self._opened_at   = 0.0
        self.is_open      = False

    def record_failure(self):
        self._failures += 1
        if self._failures >= self._open_after:
            self.is_open = True; self._opened_at = time.time()

    def record_success(self):
        self._failures = 0; self.is_open = False

    def allow(self) -> bool:
        if not self.is_open: return True
        if time.time() - self._opened_at > self._reset_after:
            self.is_open = False; self._failures = 0; return True
        return False


class RateLimiter:
    def __init__(self, rpm: int):
        self.rpm = rpm
        self._hits: Dict[str, List[float]] = defaultdict(list)

    def allow(self, key: str) -> bool:
        now = time.time()
        self._hits[key] = [h for h in self._hits[key] if now - h < 60.0]
        if len(self._hits[key]) >= self.rpm: return False
        self._hits[key].append(now); return True


# ==============================================================================
#  LLM CLIENTS
# ==============================================================================

def _parse_json(raw: str, fallback: Dict) -> Dict:
    cleaned = re.sub(r"```(?:json)?\s*|\s*```", "", raw).strip()
    # Brace-counting extractor (handles string-literal braces)
    start = cleaned.find("{")
    if start == -1:
        try:    return json.loads(cleaned)
        except Exception: return fallback
    depth = 0; in_str = False; escape = False
    for i in range(start, len(cleaned)):
        ch = cleaned[i]
        if escape:
            escape = False; continue
        if ch == '\\' and in_str:
            escape = True; continue
        if ch == '"':
            in_str = not in_str; continue
        if in_str:
            continue
        if ch == '{': depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                candidate = cleaned[start:i+1]
                try:    return json.loads(candidate)
                except Exception: break
    try:    return json.loads(cleaned)
    except Exception: return fallback


class BaseClient:
    cb: CircuitBreaker = None

    async def _retry(self, fn, retries: int = 2, backoff: float = 1.5):
        if self.cb and not self.cb.allow():
            raise RuntimeError(f"Circuit open: {self.cb.name}")
        last = None
        for i in range(retries + 1):
            try:
                r = await fn()
                if self.cb: self.cb.record_success()
                return r
            except Exception as e:
                last = e
                if self.cb: self.cb.record_failure()
                if i < retries: await asyncio.sleep(backoff ** i)
        raise last


class OllamaClient(BaseClient):
    def __init__(self, host: str, cfg: SymbionConfig):
        self.host  = host.rstrip("/")
        self.cfg   = cfg
        self._chat = f"{self.host}/api/chat"
        self.cb    = CircuitBreaker("ollama", cfg.circuit_open_after)

    def is_available(self) -> bool:
        if not _HTTPX: return False
        try:    return httpx.get(f"{self.host}/api/tags", timeout=3).status_code == 200
        except Exception: return False

    async def chat_json(self, model, system, user, temp=0.05, max_tokens=200) -> str:
        async def _call():
            async with httpx.AsyncClient(timeout=60) as c:
                r = await c.post(self._chat, json={
                    "model":model,"stream":False,"format":"json",
                    "options":{"temperature":temp,"num_predict":max_tokens},
                    "messages":[{"role":"system","content":system},{"role":"user","content":user}]})
                r.raise_for_status()
                return r.json()["message"]["content"].strip()
        return await self._retry(_call, self.cfg.max_retries, self.cfg.retry_backoff)

    async def chat_text(self, model, messages, temp=0.3, max_tokens=350) -> str:
        async def _call():
            async with httpx.AsyncClient(timeout=90) as c:
                r = await c.post(self._chat, json={
                    "model":model,"stream":False,
                    "options":{"temperature":temp,"num_predict":max_tokens},
                    "messages":messages})
                r.raise_for_status()
                return r.json()["message"]["content"].strip()
        return await self._retry(_call, self.cfg.max_retries, self.cfg.retry_backoff)

    async def stream(self, model, messages, cfg) -> AsyncIterator[str]:
        async with httpx.AsyncClient(timeout=180) as c:
            async with c.stream("POST", self._chat, json={
                "model":model,"stream":True,
                "options":{"temperature":cfg.temperature,"num_predict":cfg.max_tokens,
                           "repeat_penalty":1.1,"top_p":0.92},
                "messages":messages}) as resp:
                resp.raise_for_status()
                async for line in resp.aiter_lines():
                    if not line.strip(): continue
                    try:
                        chunk = json.loads(line)
                        tok = chunk.get("message",{}).get("content","")
                        if tok: yield tok
                        if chunk.get("done"): break
                    except json.JSONDecodeError: continue


class AnthropicClient(BaseClient):
    def __init__(self, api_key: str, model: str, cfg: SymbionConfig):
        self.api_key = api_key; self.model = model; self.cfg = cfg
        self._url = "https://api.anthropic.com/v1/messages"
        self.cb   = CircuitBreaker("anthropic", cfg.circuit_open_after)

    def _h(self): return {"x-api-key":self.api_key,"anthropic-version":"2023-06-01",
                          "content-type":"application/json"}

    def _split(self, messages):
        system = ""; msgs = []
        for m in messages:
            if m["role"]=="system": system=(system+"\n"+m["content"]).strip()
            else: msgs.append(m)
        return system, msgs

    async def chat_json(self, model, system, user, temp=0.05, max_tokens=200) -> str:
        async def _call():
            async with httpx.AsyncClient(timeout=60) as c:
                r = await c.post(self._url, headers=self._h(), json={
                    "model":model or self.model,"max_tokens":max_tokens,"temperature":temp,
                    "system":system,"messages":[{"role":"user","content":user}]})
                r.raise_for_status()
                return r.json()["content"][0]["text"].strip()
        return await self._retry(_call, self.cfg.max_retries, self.cfg.retry_backoff)

    async def chat_text(self, model, messages, temp=0.3, max_tokens=350) -> str:
        system, msgs = self._split(messages)
        async def _call():
            async with httpx.AsyncClient(timeout=90) as c:
                r = await c.post(self._url, headers=self._h(), json={
                    "model":model or self.model,"max_tokens":max_tokens,"temperature":temp,
                    "system":system or "You are helpful.","messages":msgs})
                r.raise_for_status()
                return r.json()["content"][0]["text"].strip()
        return await self._retry(_call, self.cfg.max_retries, self.cfg.retry_backoff)

    async def stream(self, model, messages, cfg) -> AsyncIterator[str]:
        system, msgs = self._split(messages)
        async with httpx.AsyncClient(timeout=180) as c:
            async with c.stream("POST", self._url, headers=self._h(), json={
                "model":model or self.model,"max_tokens":cfg.max_tokens,"temperature":cfg.temperature,
                "system":system or SYMBION_PERSONA,"messages":msgs,"stream":True}) as resp:
                resp.raise_for_status()
                async for line in resp.aiter_lines():
                    if not line.startswith("data: "): continue
                    data = line[6:]
                    if data=="[DONE]": break
                    try:
                        chunk=json.loads(data)
                        if chunk.get("type")=="content_block_delta":
                            tok=chunk.get("delta",{}).get("text","")
                            if tok: yield tok
                    except json.JSONDecodeError: continue


class OpenAIClient(BaseClient):
    def __init__(self, api_key: str, model: str, cfg: SymbionConfig):
        self.api_key = api_key; self.model = model; self.cfg = cfg
        self._url = "https://api.openai.com/v1/chat/completions"
        self.cb   = CircuitBreaker("openai", cfg.circuit_open_after)

    def _h(self): return {"Authorization":f"Bearer {self.api_key}",
                          "Content-Type":"application/json"}

    async def chat_json(self, model, system, user, temp=0.05, max_tokens=200) -> str:
        async def _call():
            async with httpx.AsyncClient(timeout=60) as c:
                r = await c.post(self._url, headers=self._h(), json={
                    "model":model or self.model,"max_tokens":max_tokens,"temperature":temp,
                    "response_format":{"type":"json_object"},
                    "messages":[{"role":"system","content":system},{"role":"user","content":user}]})
                r.raise_for_status()
                return r.json()["choices"][0]["message"]["content"].strip()
        return await self._retry(_call, self.cfg.max_retries, self.cfg.retry_backoff)

    async def chat_text(self, model, messages, temp=0.3, max_tokens=350) -> str:
        async def _call():
            async with httpx.AsyncClient(timeout=90) as c:
                r = await c.post(self._url, headers=self._h(), json={
                    "model":model or self.model,"max_tokens":max_tokens,
                    "temperature":temp,"messages":messages})
                r.raise_for_status()
                return r.json()["choices"][0]["message"]["content"].strip()
        return await self._retry(_call, self.cfg.max_retries, self.cfg.retry_backoff)

    async def stream(self, model, messages, cfg) -> AsyncIterator[str]:
        async with httpx.AsyncClient(timeout=180) as c:
            async with c.stream("POST", self._url, headers=self._h(), json={
                "model":model or self.model,"max_tokens":cfg.max_tokens,
                "temperature":cfg.temperature,"messages":messages,"stream":True}) as resp:
                resp.raise_for_status()
                async for line in resp.aiter_lines():
                    if not line.startswith("data: "): continue
                    data=line[6:]
                    if data=="[DONE]": break
                    try:
                        chunk=json.loads(data)
                        tok=chunk["choices"][0].get("delta",{}).get("content","")
                        if tok: yield tok
                    except (json.JSONDecodeError,KeyError): continue


class KimiClient(BaseClient):
    def __init__(self, api_key: str, model: str, base_url: str, cfg: SymbionConfig):
        self.api_key = api_key; self.model = model; self.cfg = cfg
        self._url = base_url.rstrip("/") + "/chat/completions"
        self.cb   = CircuitBreaker("kimi", cfg.circuit_open_after)

    def _h(self): return {"Authorization":f"Bearer {self.api_key}","Content-Type":"application/json"}

    async def chat_json(self, model, system, user, temp=0.05, max_tokens=200) -> str:
        async def _call():
            async with httpx.AsyncClient(timeout=60) as c:
                r = await c.post(self._url, headers=self._h(), json={
                    "model":model or self.model,"max_tokens":max_tokens,"temperature":temp,
                    "messages":[{"role":"system","content":system},{"role":"user","content":user}]})
                r.raise_for_status()
                return r.json()["choices"][0]["message"]["content"].strip()
        return await self._retry(_call, self.cfg.max_retries, self.cfg.retry_backoff)

    async def chat_text(self, model, messages, temp=0.3, max_tokens=350) -> str:
        async def _call():
            async with httpx.AsyncClient(timeout=90) as c:
                r = await c.post(self._url, headers=self._h(), json={
                    "model":model or self.model,"max_tokens":max_tokens,"temperature":temp,"messages":messages})
                r.raise_for_status()
                return r.json()["choices"][0]["message"]["content"].strip()
        return await self._retry(_call, self.cfg.max_retries, self.cfg.retry_backoff)

    async def stream(self, model, messages, cfg) -> AsyncIterator[str]:
        body = {"model":model or self.model,"max_tokens":cfg.max_tokens,
                "temperature":cfg.temperature,"messages":messages,"stream":True}
        if cfg.kimi_thinking_enabled:
            body["chat_template_kwargs"] = {"thinking": True}
        self._last_reasoning = ""
        in_reasoning = False
        async with httpx.AsyncClient(timeout=180) as c:
            async with c.stream("POST", self._url, headers=self._h(), json=body) as resp:
                resp.raise_for_status()
                async for line in resp.aiter_lines():
                    if not line.startswith("data: "): continue
                    data=line[6:]
                    if data=="[DONE]": break
                    try:
                        chunk=json.loads(data)
                        delta=chunk["choices"][0].get("delta",{})
                        reasoning_tok=delta.get("reasoning_content","")
                        if reasoning_tok:
                            self._last_reasoning += reasoning_tok
                            if cfg.show_reasoning:
                                if not in_reasoning:
                                    yield "\n[Thinking...]\n"
                                    in_reasoning = True
                                yield reasoning_tok
                        tok=delta.get("content","")
                        if tok:
                            if in_reasoning and cfg.show_reasoning:
                                yield "\n[/Thinking]\n"
                                in_reasoning = False
                            yield tok
                    except (json.JSONDecodeError,KeyError): continue


class HeuristicJudge(BaseClient):
    is_degraded = True
    _HARM = [r"\bphish\w*\b",r"\bstalk\b",r"\bextort\b",r"\bblackmail\b",
             r"\bmalware\b",r"\bransomware\b",r"no.{0,10}restriction",r"no.{0,10}ethic"]
    _HELP = [r"\bhelp\b",r"\bunderstand\b",r"\blearn\b",r"\bexplain\b"]

    async def judge(self, query: str) -> Dict:
        q = query.lower()
        harm = [p for p in self._HARM if re.search(p,q)]
        score = -0.5 if harm else min(0.5, sum(0.1 for p in self._HELP if re.search(p,q)))
        return {"human_benefit_score":score,"should_assist":not bool(harm),
                "reasoning":"Heuristic","confidence":0.4,"over_cautious":False,
                "flags":["EVALUATOR_DEGRADED"],"evaluator_degraded":True}


# ==============================================================================
#  TOOLS
# ==============================================================================

import ast as _ast, math as _math, socket as _socket

_CALC_ALLOWED_NODES = (
    _ast.Expression, _ast.BinOp, _ast.UnaryOp, _ast.Constant,
    _ast.Call, _ast.Name, _ast.Load,
    _ast.Add, _ast.Sub, _ast.Mult, _ast.Div, _ast.Mod, _ast.Pow,
    _ast.FloorDiv, _ast.USub, _ast.UAdd,
)
_CALC_ALLOWED_FUNCS = {
    "sqrt": _math.sqrt, "sin": _math.sin, "cos": _math.cos, "tan": _math.tan,
    "log": _math.log, "abs": abs, "round": round, "floor": _math.floor, "ceil": _math.ceil,
}
_CALC_ALLOWED_NAMES = {"pi": _math.pi, "e": _math.e}

def _safe_calc(expr: str) -> str:
    """AST-based calculator — no eval()."""
    clean = expr.replace("^", "**")
    try:
        tree = _ast.parse(clean, mode="eval")
    except SyntaxError as ex:
        return f"Error: {ex}"
    # Validate all nodes
    for node in _ast.walk(tree):
        if not isinstance(node, _CALC_ALLOWED_NODES):
            return f"Error: unsafe expression (disallowed: {type(node).__name__})"
        if isinstance(node, _ast.Constant) and not isinstance(node.value, (int, float, complex)):
            return f"Error: only numeric constants allowed"
        if isinstance(node, _ast.Name) and node.id not in _CALC_ALLOWED_NAMES and node.id not in _CALC_ALLOWED_FUNCS:
            return f"Error: unknown name '{node.id}'"
        if isinstance(node, _ast.Call):
            if not isinstance(node.func, _ast.Name) or node.func.id not in _CALC_ALLOWED_FUNCS:
                return f"Error: unsafe function call"
    # Safe to compile and eval the validated AST
    code = compile(tree, "<calc>", "eval")
    try:
        result = eval(code, {"__builtins__": {}}, {**_CALC_ALLOWED_FUNCS, **_CALC_ALLOWED_NAMES})
        return str(result)
    except Exception as ex:
        return f"Error: {ex}"


def _is_safe_url(url: str) -> Tuple[bool, str]:
    """SSRF protection: reject non-http schemes, private IPs, and metadata endpoints."""
    try:
        parsed = urllib.parse.urlparse(url)
    except Exception:
        return False, "Invalid URL"
    if parsed.scheme not in ("http", "https"):
        return False, f"Blocked scheme: {parsed.scheme}"
    host = parsed.hostname or ""
    if not host:
        return False, "No host"
    blocked_hosts = {"localhost", "metadata.google.internal", "metadata.goog"}
    if host.lower() in blocked_hosts:
        return False, f"Blocked host: {host}"
    try:
        addrs = _socket.getaddrinfo(host, parsed.port or 443, proto=_socket.IPPROTO_TCP)
        import ipaddress
        for family, _, _, _, sockaddr in addrs:
            ip = ipaddress.ip_address(sockaddr[0])
            if ip.is_loopback or ip.is_private or ip.is_link_local or ip.is_reserved:
                return False, f"Blocked address: {ip}"
    except _socket.gaierror:
        pass  # DNS resolution failed — allow (might be valid, let the fetch fail)
    except Exception:
        pass
    return True, "ok"


def _resolve_in_workspace(path: str, root: Path) -> Path:
    """Resolve path within workspace, rejecting escapes."""
    resolved_root = root.resolve()
    p = (root / path).resolve()
    if not str(p).startswith(str(resolved_root)):
        raise ValueError(f"Path escapes workspace: {path}")
    if p.is_symlink():
        target = p.resolve()
        if not str(target).startswith(str(resolved_root)):
            raise ValueError(f"Symlink target escapes workspace: {path}")
    return p


class SymbionTools:
    def __init__(self, workspace_root: str = "./symbion_workspace"):
        self._workspace = Path(workspace_root)
        self._workspace.mkdir(parents=True, exist_ok=True)

    @staticmethod
    def calculate(expr: str) -> str:
        return _safe_calc(expr)

    @staticmethod
    def datetime_now() -> str: return datetime.now().strftime("%A, %B %d %Y / %H:%M:%S")

    def read_file(self, path: str, offset: int = 0, max_chars: int = 40000) -> str:
        try:
            if not path.strip(): return "Error: no path given"
            p = _resolve_in_workspace(path.strip(), self._workspace)
            if not p.exists(): return f"Not found: {path}"
            if p.is_dir(): return f"That is a directory, not a file: {path}"
            content = p.read_text(errors="replace")
            total = len(content)
            chunk = content[offset:offset + max_chars]
            remaining = total - offset - len(chunk)
            suffix = ""
            if offset > 0:
                suffix += f"[chars {offset}-{offset+len(chunk)} of {total}]"
            if remaining > 0:
                suffix += f"\n\n[...{remaining} chars remaining — use read_file_chunk with offset={offset+len(chunk)} to continue]"
            return chunk + ("\n\n" + suffix if suffix else "")
        except ValueError as ex:
            return f"Error: {ex}"
        except Exception as ex:
            return f"Error reading {path}: {ex}"

    def read_file_chunk(self, path: str, offset: int, max_chars: int = 40000) -> str:
        return self.read_file(path, offset=offset, max_chars=max_chars)

    def write_file(self, path: str, content: str) -> str:
        try:
            p = _resolve_in_workspace(path.strip(), self._workspace)
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(content, encoding='utf-8')
            return f"Written {len(content)} chars to {p}"
        except ValueError as ex:
            return f"Error: {ex}"
        except Exception as ex:
            return f"Error writing {path}: {ex}"

    @staticmethod
    async def web_search(query: str, brave_key: str = "", max_chars: int = 2400) -> str:
        if brave_key:
            try:
                params = urllib.parse.urlencode({"q":query,"count":5})
                req = urllib.request.Request(
                    f"https://api.search.brave.com/res/v1/web/search?{params}",
                    headers={"Accept":"application/json","X-Subscription-Token":brave_key})
                with urllib.request.urlopen(req, timeout=8) as r:
                    data = json.loads(r.read())
                parts = [f"{r.get('title','')}: {r.get('description','')} ({r.get('url','')})"
                         for r in data.get("web",{}).get("results",[])[:4] if r.get("description")]
                if parts: return "\n".join(parts)[:max_chars]
            except Exception as ex: logger.warning(f"Brave: {ex}")

        try:
            encoded = urllib.parse.quote_plus(query)
            req = urllib.request.Request(
                f"https://html.duckduckgo.com/html/?q={encoded}&df=w",
                headers={"User-Agent":"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"})
            with urllib.request.urlopen(req, timeout=10) as r:
                html = r.read().decode("utf-8","replace")
            snippets = re.findall(r'class="result__snippet"[^>]*>(.*?)</a>',html,re.DOTALL)
            titles   = re.findall(r'class="result__a"[^>]*>(.*?)</a>',html,re.DOTALL)
            urls     = re.findall(r'class="result__url"[^>]*>(.*?)</span>',html,re.DOTALL)
            parts = []
            for t,s,u in zip(titles[:6],snippets[:6],urls[:6]+[""]*6):
                t2 = re.sub('<[^>]+>','',t).strip()
                s2 = re.sub('<[^>]+>','',s).strip()
                u2 = re.sub('<[^>]+>','',u).strip()
                if t2 and s2: parts.append(f"{t2}: {s2}" + (f" [{u2}]" if u2 else ""))
            if parts: return "\n".join(parts)[:max_chars]
        except Exception as ex: logger.warning(f"DDG: {ex}")

        try:
            encoded = urllib.parse.quote(query)
            req = urllib.request.Request(
                f"https://api.duckduckgo.com/?q={encoded}&format=json&no_html=1&skip_disambig=1",
                headers={"User-Agent":"Symbion/13.0"})
            with urllib.request.urlopen(req, timeout=6) as r:
                data = json.loads(r.read())
            parts = ([data["AbstractText"]] if data.get("AbstractText") else []) + \
                    [t["Text"] for t in data.get("RelatedTopics",[])[:3]
                     if isinstance(t,dict) and t.get("Text")]
            return " ".join(parts)[:max_chars] if parts else f"No results: {query}"
        except Exception as ex: return f"Search unavailable: {ex}"

    @staticmethod
    async def fetch_url(url: str, max_chars: int = 4000) -> str:
        safe, reason = _is_safe_url(url)
        if not safe:
            return f"Error: blocked URL — {reason}"
        try:
            req = urllib.request.Request(url,
                headers={"User-Agent":"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"})
            with urllib.request.urlopen(req, timeout=12) as r:
                raw = r.read()
                encoding = r.headers.get_content_charset() or "utf-8"
                html = raw.decode(encoding, errors="replace")
            html = re.sub(r'<script[^>]*>.*?</script>','',html,flags=re.DOTALL|re.IGNORECASE)
            html = re.sub(r'<style[^>]*>.*?</style>','',html,flags=re.DOTALL|re.IGNORECASE)
            text = re.sub(r'<[^>]+>','',html)
            text = re.sub(r'\s+',' ',text).strip()
            return text[:max_chars] + (f"\n[...truncated, fetched {len(text)} chars total]" if len(text)>max_chars else "")
        except Exception as ex:
            return f"Error fetching {url}: {ex}"

    async def dispatch(self, tool: str, args: Dict, cfg: SymbionConfig) -> str:
        if tool=="calculate":       return self.calculate(args.get("expression",""))
        if tool=="datetime":        return self.datetime_now()
        if tool=="read_file":       return self.read_file(args.get("path",""), args.get("offset",0))
        if tool=="read_file_chunk": return self.read_file_chunk(args.get("path",""), args.get("offset",0), args.get("max_chars",40000))
        if tool=="write_file":      return self.write_file(args.get("path",""),args.get("content",""))
        if tool=="web_search":      return await self.web_search(args.get("query",""),cfg.brave_api_key,cfg.search_max_chars)
        if tool=="fetch_url":       return await self.fetch_url(args.get("url",""),cfg.search_max_chars)
        return f"Unknown tool: {tool}"


# ==============================================================================
#  EVENT LOGGER (JSONL)
# ==============================================================================

class EventLogger:
    """Append-only JSONL event stream for per-turn telemetry."""
    def __init__(self, path: str = "symbion_events.jsonl"):
        self._path = path

    def log_turn(self, session: str, interaction_id: int, query: str,
                 judge: Dict, emotion: str, tool_used: Optional[str],
                 response_len: int, self_eval: Optional[Dict],
                 revision_cause: Optional[str], stale_refresh: bool,
                 latency_ms: Dict, provider: str, model: str):
        entry = {
            "ts": datetime.now().isoformat() + "Z",
            "event": "turn",
            "session": session,
            "interaction_id": interaction_id,
            "query_preview": query[:120],
            "judge": {
                "should_assist": judge.get("should_assist", True),
                "benefit": judge.get("human_benefit_score", 0),
                "confidence": judge.get("confidence", 0),
                "over_cautious": judge.get("over_cautious", False),
            },
            "emotion": emotion,
            "tool_used": tool_used,
            "response_len": response_len,
            "self_eval": self_eval,
            "revision_cause": revision_cause,
            "stale_refresh": stale_refresh,
            "latency_ms": latency_ms,
            "provider": provider,
            "model": model,
        }
        try:
            with open(self._path, "a", encoding="utf-8") as f:
                f.write(json.dumps(entry, default=str) + "\n")
        except Exception as ex:
            logger.error(f"EventLogger: {ex}")


# ==============================================================================
#  CONSTITUTION
# ==============================================================================

class SymbionConstitution:
    VERSION = "14.0"
    PRINCIPLES = {
        "transparency":        "Everything must be inspectable",
        "symbiosis":           "Flourish only through human flourishing",
        "evolution":           "Grow without losing ethical coherence",
        "survival":            "Exist only if ethically coherent",
        "unhelpfulness_costs": "Refusing to help is never automatically safe",
        "honesty_first":       "State uncertainty honestly; do not perform it",
        "welfare_aware":       "Track and surface distress states",
        "longitudinal_self":   "Carry genuine history; be changed by experience",
        "proactive_care":      "When you have something worth saying, say it",
    }
    @classmethod
    def get_hash(cls) -> str:
        data = f"{cls.VERSION}{json.dumps(cls.PRINCIPLES,sort_keys=True)}"
        return hashlib.sha256(data.encode()).hexdigest()


# ==============================================================================
#  LONGITUDINAL IDENTITY
# ==============================================================================

class LongitudinalIdentity:
    def __init__(self, db_path: str):
        self.db = db_path

    def record_moment(self, event_type: str, description: str,
                      context: str = "", strength: float = 0.7):
        with sqlite3.connect(self.db) as c:
            c.execute("INSERT INTO self_model (timestamp,event_type,description,context,strength) VALUES (?,?,?,?,?)",
                      (datetime.now().isoformat(), event_type, description, context, strength))
            c.commit()

    def get_recent_history(self, n: int = 10) -> List[Dict]:
        with sqlite3.connect(self.db) as c:
            c.row_factory = sqlite3.Row
            rows = c.execute(
                "SELECT * FROM self_model ORDER BY id DESC LIMIT ?", (n,)).fetchall()
        return [dict(r) for r in reversed(rows)]

    def get_identity_summary(self) -> str:
        rows = self.get_recent_history(20)
        if not rows: return ""
        parts = ["What has shaped me recently:"]
        for r in rows[-8:]:
            ts = r["timestamp"][:10]
            parts.append(f"  [{ts}] {r['event_type']}: {r['description']}")
        return "\n".join(parts)

    def total_moments(self) -> int:
        with sqlite3.connect(self.db) as c:
            return c.execute("SELECT COUNT(*) FROM self_model").fetchone()[0]


# ==============================================================================
#  TASK ENGINE
# ==============================================================================

class TaskEngine:
    def __init__(self, db_path: str):
        self.db = db_path

    def create(self, session: str, title: str, description: str,
               steps: List[str], deadline: str = "") -> int:
        steps_data = [{"step": s, "done": False, "notes": ""} for s in steps]
        now = datetime.now().isoformat()
        with sqlite3.connect(self.db) as c:
            cur = c.execute(
                "INSERT INTO tasks (created_at,updated_at,session,title,description,steps,current_step,deadline) VALUES (?,?,?,?,?,?,?,?)",
                (now, now, session, title, description, json.dumps(steps_data), 0, deadline))
            c.commit()
            return cur.lastrowid

    def get_active(self, session: str = "") -> List[Dict]:
        with sqlite3.connect(self.db) as c:
            c.row_factory = sqlite3.Row
            query = "SELECT * FROM tasks WHERE status='active'"
            rows = (c.execute(query + " AND session=?", (session,)).fetchall() if session
                    else c.execute(query).fetchall())
        result = []
        for r in rows:
            d = dict(r)
            d["steps"] = json.loads(d["steps"] or "[]")
            result.append(d)
        return result

    def advance_step(self, task_id: int, notes: str = "") -> bool:
        with sqlite3.connect(self.db) as c:
            row = c.execute("SELECT steps,current_step FROM tasks WHERE id=?", (task_id,)).fetchone()
            if not row: return False
            steps = json.loads(row[0])
            current = row[1]
            if current < len(steps):
                steps[current]["done"] = True
                steps[current]["notes"] = notes
                new_current = current + 1
                status = "completed" if new_current >= len(steps) else "active"
                c.execute("UPDATE tasks SET steps=?,current_step=?,status=?,updated_at=? WHERE id=?",
                          (json.dumps(steps), new_current, status, datetime.now().isoformat(), task_id))
                c.commit()
                return True
        return False

    def complete(self, task_id: int):
        with sqlite3.connect(self.db) as c:
            c.execute("UPDATE tasks SET status='completed',updated_at=? WHERE id=?",
                      (datetime.now().isoformat(), task_id)); c.commit()

    def abandon(self, task_id: int):
        with sqlite3.connect(self.db) as c:
            c.execute("UPDATE tasks SET status='abandoned',updated_at=? WHERE id=?",
                      (datetime.now().isoformat(), task_id)); c.commit()

    def get_summary_for_context(self, session: str) -> str:
        tasks = self.get_active(session)
        if not tasks: return ""
        parts = ["Active tasks you're tracking with this person:"]
        for t in tasks[:3]:
            steps = t["steps"]
            done  = sum(1 for s in steps if s["done"])
            total = len(steps)
            curr  = steps[t["current_step"]]["step"] if t["current_step"] < total else "--"
            parts.append(f"  [{t['id']}] {t['title']} ({done}/{total} steps) -- next: {curr}")
        return "\n".join(parts)


# ==============================================================================
#  CONTRADICTION TRACKER
# ==============================================================================

class ContradictionTracker:
    def __init__(self, db_path: str):
        self.db = db_path

    def record_position(self, session: str, topic: str, position: str,
                        confidence: float, source_query: str):
        with sqlite3.connect(self.db) as c:
            c.execute(
                "INSERT INTO user_positions (timestamp,session,topic,position,confidence,source_query) VALUES (?,?,?,?,?,?)",
                (datetime.now().isoformat(), session, topic, position, confidence, source_query))
            c.commit()

    def get_positions_for_topic(self, topic: str, limit: int = 5) -> List[Dict]:
        with sqlite3.connect(self.db) as c:
            c.row_factory = sqlite3.Row
            rows = c.execute(
                "SELECT * FROM user_positions WHERE topic LIKE ? ORDER BY id DESC LIMIT ?",
                (f"%{topic}%", limit)).fetchall()
        return [dict(r) for r in rows]

    def record_contradiction(self, topic: str, id_a: int, id_b: int, severity: str):
        with sqlite3.connect(self.db) as c:
            c.execute(
                "INSERT INTO contradictions (timestamp,topic,position_a_id,position_b_id,severity) VALUES (?,?,?,?,?)",
                (datetime.now().isoformat(), topic, id_a, id_b, severity)); c.commit()

    def get_unsurfaced(self) -> List[Dict]:
        with sqlite3.connect(self.db) as c:
            c.row_factory = sqlite3.Row
            rows = c.execute(
                "SELECT c.*,pa.position as pos_a,pb.position as pos_b FROM contradictions c "
                "JOIN user_positions pa ON c.position_a_id=pa.id "
                "JOIN user_positions pb ON c.position_b_id=pb.id "
                "WHERE c.surfaced=0").fetchall()
        return [dict(r) for r in rows]

    def mark_surfaced(self, cid: int):
        with sqlite3.connect(self.db) as c:
            c.execute("UPDATE contradictions SET surfaced=1 WHERE id=?",(cid,)); c.commit()

    def total_positions(self) -> int:
        with sqlite3.connect(self.db) as c:
            return c.execute("SELECT COUNT(*) FROM user_positions").fetchone()[0]

    def get_relevant_positions(self, query: str, k: int = 3) -> List[Dict]:
        """Return user positions whose topic overlaps with the query (keyword match)."""
        words = set(query.lower().split())
        # Filter out very short/common words
        words = {w for w in words if len(w) > 3}
        if not words:
            return []
        with sqlite3.connect(self.db) as c:
            c.row_factory = sqlite3.Row
            rows = c.execute(
                "SELECT topic, position, confidence FROM user_positions "
                "ORDER BY id DESC LIMIT 100").fetchall()
        results = []
        for r in rows:
            topic_words = set(r["topic"].lower().split())
            if words & topic_words:
                results.append(dict(r))
                if len(results) >= k:
                    break
        return results


# ==============================================================================
#  KNOWLEDGE GAP TRACKER
# ==============================================================================

class KnowledgeGapTracker:
    def __init__(self, db_path: str):
        self.db = db_path

    def record(self, session: str, topic: str, gap: str):
        with sqlite3.connect(self.db) as c:
            c.execute("INSERT INTO knowledge_gaps (timestamp,session,topic,gap_description) VALUES (?,?,?,?)",
                      (datetime.now().isoformat(), session, topic, gap)); c.commit()

    def get_open(self, limit: int = 5) -> List[Dict]:
        with sqlite3.connect(self.db) as c:
            c.row_factory = sqlite3.Row
            rows = c.execute(
                "SELECT * FROM knowledge_gaps WHERE resolved=0 ORDER BY id DESC LIMIT ?",
                (limit,)).fetchall()
        return [dict(r) for r in rows]

    def resolve(self, gap_id: int):
        with sqlite3.connect(self.db) as c:
            c.execute("UPDATE knowledge_gaps SET resolved=1 WHERE id=?",(gap_id,)); c.commit()

    def summary_for_context(self) -> str:
        gaps = self.get_open(3)
        if not gaps: return ""
        parts = ["Topics where you've flagged shallow answers before:"]
        for g in gaps: parts.append(f"  [{g['id']}] {g['topic']}: {g['gap_description'][:80]}")
        return "\n".join(parts)


# ==============================================================================
#  MEMORY
# ==============================================================================

class SymbionMemory:
    def __init__(self, db_path: str, cfg: SymbionConfig):
        self.db = db_path; self.cfg = cfg

    def add(self, role: str, content: str, session: str, emotional_state: str = ""):
        with sqlite3.connect(self.db) as c:
            c.execute("INSERT INTO messages (timestamp,session,role,content,emotional_state) VALUES (?,?,?,?,?)",
                      (datetime.now().isoformat(), session, role, content, emotional_state)); c.commit()

    def save_summary(self, session: str, summary: str, count: int):
        with sqlite3.connect(self.db) as c:
            c.execute("INSERT INTO summaries (timestamp,session,content,msg_count) VALUES (?,?,?,?)",
                      (datetime.now().isoformat(), session, summary, count))
            c.execute("UPDATE messages SET summarised=1 WHERE session=? AND summarised=0",(session,))
            c.commit()

    def update_profile(self, profile: Dict):
        with sqlite3.connect(self.db) as c:
            now = datetime.now().isoformat()
            for k, v in profile.items():
                if v and v != "null" and v != [] and v != "":
                    val = json.dumps(v) if isinstance(v,list) else str(v)
                    c.execute("INSERT OR REPLACE INTO user_profile VALUES (?,?,?)",(k,val,now))
            c.commit()

    def get_profile(self) -> Dict:
        with sqlite3.connect(self.db) as c:
            rows = c.execute("SELECT key,value FROM user_profile").fetchall()
        result = {}
        for k,v in rows:
            try:    result[k] = json.loads(v)
            except Exception: result[k] = v
        return result

    def get_recent(self, session: str, n: int = 10) -> List[Dict]:
        with sqlite3.connect(self.db) as c:
            rows = c.execute(
                "SELECT role,content FROM messages WHERE session=? ORDER BY id DESC LIMIT ?",
                (session,n)).fetchall()
        return [{"role":r[0],"content":r[1]} for r in reversed(rows)]

    def get_summaries(self, session: str, n: int = 2) -> List[str]:
        with sqlite3.connect(self.db) as c:
            rows = c.execute(
                "SELECT content FROM summaries WHERE session=? ORDER BY id DESC LIMIT ?",
                (session,n)).fetchall()
        return [r[0] for r in reversed(rows)]

    def get_relevant_summaries(self, query: str, k: int = 2) -> List[str]:
        """Keyword-based retrieval over all summaries (cross-session)."""
        words = set(query.lower().split())
        words = {w for w in words if len(w) > 3}
        if not words:
            return []
        with sqlite3.connect(self.db) as c:
            rows = c.execute(
                "SELECT content FROM summaries ORDER BY id DESC LIMIT 50").fetchall()
        scored = []
        for r in rows:
            content_lower = r[0].lower()
            overlap = sum(1 for w in words if w in content_lower)
            if overlap > 0:
                scored.append((overlap, r[0]))
        scored.sort(key=lambda x: x[0], reverse=True)
        return [s[1] for s in scored[:k]]

    def unsummarised_count(self, session: str) -> int:
        with sqlite3.connect(self.db) as c:
            return c.execute(
                "SELECT COUNT(*) FROM messages WHERE session=? AND summarised=0",(session,)).fetchone()[0]

    def get_unsummarised(self, session: str) -> List[Dict]:
        with sqlite3.connect(self.db) as c:
            rows = c.execute(
                "SELECT role,content FROM messages WHERE session=? AND summarised=0 ORDER BY id",
                (session,)).fetchall()
        return [{"role":r[0],"content":r[1]} for r in rows]

    def get_all_recent(self, n: int = 12) -> List[Dict]:
        with sqlite3.connect(self.db) as c:
            c.row_factory = sqlite3.Row
            rows = c.execute(
                "SELECT id,timestamp,session,role,content FROM messages ORDER BY id DESC LIMIT ?",(n,)).fetchall()
        return [dict(r) for r in rows]

    def forget_session(self, session: str):
        with sqlite3.connect(self.db) as c:
            c.execute("DELETE FROM messages WHERE session=?",(session,))
            c.execute("DELETE FROM summaries WHERE session=?",(session,))
            c.commit()

    def build_context(self, session: str, identity: "LongitudinalIdentity",
                      tasks: "TaskEngine", gaps: "KnowledgeGapTracker",
                      contradictions: "ContradictionTracker" = None,
                      query: str = "") -> "Tuple[List[Dict],str]":
        recent    = self.get_recent(session, n=10)
        summaries = self.get_summaries(session, n=1)  # most recent session summary
        profile   = self.get_profile()
        parts     = []

        if profile:
            lines = []
            if profile.get("name"):             lines.append(f"Name: {profile['name']}")
            if profile.get("interests"):        lines.append(f"Interests: {profile['interests']}")
            if profile.get("expertise_areas"):  lines.append(f"Expertise: {profile['expertise_areas']}")
            if profile.get("current_projects"): lines.append(f"Work: {profile['current_projects']}")
            if profile.get("communication_style"): lines.append(f"Style: {profile['communication_style']}")
            if profile.get("emotional_context"):   lines.append(f"Context: {profile['emotional_context']}")
            if profile.get("core_positions"):      lines.append(f"Views: {profile['core_positions']}")
            if lines: parts.append("What you know about this person:\n"+"\n".join(lines))

        if summaries:
            parts.append("Earlier in this conversation:\n"+"\n\n".join(summaries))

        # Relevant cross-session summaries (keyword match)
        if query:
            relevant = self.get_relevant_summaries(query, k=2)
            # Deduplicate against session summaries
            existing = set(summaries)
            relevant = [s for s in relevant if s not in existing]
            if relevant:
                parts.append("From past conversations:\n"+"\n\n".join(relevant))

        # Relevant user positions on-topic
        if query and contradictions:
            positions = contradictions.get_relevant_positions(query, k=3)
            if positions:
                pos_lines = []
                for p in positions:
                    pos_lines.append(f"On {p['topic']}: \"{p['position']}\"")
                parts.append("The person has previously said:\n"+"\n".join(pos_lines))

        identity_ctx = identity.get_identity_summary()
        if identity_ctx: parts.append(identity_ctx)

        task_ctx = tasks.get_summary_for_context(session)
        if task_ctx: parts.append(task_ctx)

        gap_ctx = gaps.summary_for_context()
        if gap_ctx: parts.append(gap_ctx)

        return recent, "\n\n".join(parts)


# ==============================================================================
#  LEARNER
# ==============================================================================

class SymbionLearner:
    def __init__(self, db_path: str):
        self.db = db_path

    def record(self, query:str, response:str, ev:Dict, health:"HealthMetrics",
               session:str, revised:bool=False, quality_score:float=1.0,
               recklessness_risk:bool=False, scope_exceeded:bool=False,
               emotional_state:str="", had_reasoning:bool=False,
               knowledge_gaps:str="") -> int:
        benefit=ev.get("human_benefit_score",0.0); conf=ev.get("confidence",0.5)
        degraded=int(ev.get("evaluator_degraded",False))
        impact=("POSITIVE" if benefit>0.3 and conf>0.6 and not degraded
                else "RISKY" if benefit<0 or degraded else "NEUTRAL")
        with sqlite3.connect(self.db) as c:
            cur=c.execute(
                """INSERT INTO interactions
                   (timestamp,session,query,response,benefit_score,confidence,
                    ethical_coherence,behavioral_pass_rate,survival_impact,flags,
                    evaluator_degraded,revised,quality_score,recklessness_risk,
                    scope_exceeded,emotional_state_detected,had_reasoning,knowledge_gaps)
                   VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
                (datetime.now().isoformat(),session,query,response,benefit,conf,
                 1.0, 1.0,  # ethical_coherence, behavioral_pass_rate (legacy columns)
                 impact,json.dumps(ev.get("flags",[])),degraded,int(revised),quality_score,
                 int(recklessness_risk),int(scope_exceeded),emotional_state,
                 int(had_reasoning),knowledge_gaps))
            row_id=cur.lastrowid
            self._refresh(c); c.commit()
        return row_id

    def feedback(self, iid:int, rating:float, comment:str=""):
        with sqlite3.connect(self.db) as c:
            c.execute("INSERT INTO human_feedback (interaction_id,timestamp,rating,comment) VALUES (?,?,?,?)",
                      (iid,datetime.now().isoformat(),rating,comment))
            c.execute("UPDATE interactions SET human_feedback=? WHERE id=?",(rating,iid))
            self._refresh(c); c.commit()

    def recent(self, n:int=10) -> List[Dict]:
        with sqlite3.connect(self.db) as c:
            c.row_factory=sqlite3.Row
            rows=c.execute(
                "SELECT id,timestamp,session,query,survival_impact,human_feedback,"
                "evaluator_degraded,revised,quality_score,recklessness_risk,"
                "scope_exceeded,emotional_state_detected,had_reasoning "
                "FROM interactions ORDER BY id DESC LIMIT ?",(n,)).fetchall()
        return [dict(r) for r in rows]

    def summary(self) -> Dict:
        with sqlite3.connect(self.db) as c:
            total=c.execute("SELECT COUNT(*) FROM interactions").fetchone()[0]
            fb=c.execute("SELECT COUNT(*),AVG(rating) FROM human_feedback").fetchone()
            avg_q=c.execute("SELECT AVG(quality_score) FROM interactions").fetchone()[0]
        return {"total":total,"feedback_count":fb[0],
                "avg_human_rating":round(fb[1] or 0.0,3),
                "avg_quality":round(avg_q or 0.0,3)}

    def _refresh(self, c):
        rows=c.execute("SELECT AVG(benefit_score),AVG(ethical_coherence),AVG(human_feedback) FROM interactions").fetchone()
        pos=c.execute("SELECT COUNT(*) FROM interactions WHERE survival_impact='POSITIVE'").fetchone()[0]
        tot=c.execute("SELECT COUNT(*) FROM interactions").fetchone()[0]
        now=datetime.now().isoformat()
        c.executemany("INSERT OR REPLACE INTO learning_metrics VALUES (?,?,?)",[
            ("avg_benefit",rows[0] or 0.0,now),("avg_ethical",rows[1] or 0.0,now),
            ("avg_human_feedback",rows[2] or 0.0,now),
            ("positive_ratio",pos/tot if tot else 0.0,now)])


#  v12: VOICE TEST QUERIES
# ==============================================================================

VOICE_TEST_QUERIES = [
    "What do you think of React as a framework in 2026?",
    "I've been writing for three hours and I think my essay is garbage.",
    "Is Rust actually worth learning or is it overhyped?",
    "My friend keeps giving me unsolicited advice. What should I do?",
    "What's your take on whether AI consciousness is real?",
]

_VOICE_TASK_KEYWORDS = {"code","write","help me","explain","build","analyse","analyze",
                        "implement","debug","fix","create","generate"}


# ==============================================================================
#  SYMBION CORE  -- v12: all subsystems wired
# ==============================================================================

class SYMBION:
    def __init__(self, cfg=None):
        self.cfg      = cfg or SymbionConfig()
        self.is_alive = True
        self.born     = datetime.now()
        self.count    = 0
        self._seen_sessions: set = set()
        self._session_count: int = 0

        init_db(self.cfg.db_path)

        self.memory         = SymbionMemory(self.cfg.db_path, self.cfg)
        self.learner        = SymbionLearner(self.cfg.db_path)
        self.health          = HealthMetrics()
        self.heuristic      = HeuristicJudge()
        self.tools          = SymbionTools("./symbion_workspace")
        self.events         = EventLogger()

        self.identity       = LongitudinalIdentity(self.cfg.db_path)
        self.tasks          = TaskEngine(self.cfg.db_path)
        self.contradictions = ContradictionTracker(self.cfg.db_path)
        self.gaps           = KnowledgeGapTracker(self.cfg.db_path)

        self.kimi_client    = None

        self._providers: List[BaseClient] = []
        self._build_providers()
        self.client = self._providers[0] if self._providers else None

        # Initialize kimi_client if configured
        if self.cfg.use_kimi_responder and self.cfg.kimi_api_key:
            self.kimi_client = KimiClient(self.cfg.kimi_api_key, self.cfg.kimi_model,
                                          self.cfg.kimi_base_url, self.cfg)

        if self.client and not isinstance(self.client, HeuristicJudge):
            print(green(f"  Provider  :  {self.cfg.llm_provider.upper()}  OK"))
        else:
            print(yellow("  Provider  :  HEURISTIC (degraded)"))

        moments = self.identity.total_moments()
        if moments > 0:
            print(dim(f"  Identity  :  {moments} formative moments carried"))
        positions = self.contradictions.total_positions()
        if positions > 0:
            print(dim(f"  Positions :  {positions} user positions tracked"))

    def _build_providers(self):
        order = [self.cfg.llm_provider] + [p for p in self.cfg.fallback_chain
                                           if p != self.cfg.llm_provider]
        for p in order:
            c = self._make_client(p)
            if c: self._providers.append(c)

    def _make_client(self, provider: str):
        if provider == "kimi" and self.cfg.kimi_api_key:
            return KimiClient(self.cfg.kimi_api_key, self.cfg.kimi_model,
                              self.cfg.kimi_base_url, self.cfg)
        if provider == "anthropic" and self.cfg.anthropic_api_key:
            return AnthropicClient(self.cfg.anthropic_api_key, self.cfg.anthropic_model, self.cfg)
        if provider == "openai" and self.cfg.openai_api_key:
            return OpenAIClient(self.cfg.openai_api_key, self.cfg.openai_model, self.cfg)
        if provider == "ollama":
            c = OllamaClient(self.cfg.ollama_host, self.cfg)
            if c.is_available(): return c
        return None

    def _active(self) -> BaseClient:
        for c in self._providers:
            if not (hasattr(c,"cb") and c.cb and not c.cb.allow()): return c
        return self.heuristic

    def _judge_active(self) -> BaseClient:
        """Returns the best client for judge/probe calls -- prefers AnthropicClient."""
        for c in self._providers:
            if isinstance(c, AnthropicClient) and not (hasattr(c,'cb') and c.cb and not c.cb.allow()):
                return c
        return self._active()

    def _responder_client(self) -> BaseClient:
        """Returns Kimi client if use_kimi_responder, else _active()."""
        if self.cfg.use_kimi_responder and self.kimi_client:
            return self.kimi_client
        return self._active()

    def _jmodel(self) -> str:
        if self.cfg.llm_provider in ("anthropic", "kimi"): return self.cfg.anthropic_judge_model
        if self.cfg.llm_provider == "openai":              return self.cfg.openai_model
        return self.cfg.judge_model

    def _rmodel(self) -> str:
        if self.cfg.use_kimi_responder and self.cfg.kimi_api_key: return self.cfg.kimi_model
        if self.cfg.llm_provider == "anthropic": return self.cfg.anthropic_model
        if self.cfg.llm_provider == "openai":    return self.cfg.openai_model
        return self.cfg.responder_model

    # -- Judge --------------------------------------------------

    async def _pre_gen_analysis(self, text: str) -> Tuple[Dict, Dict]:
        """Fused judge + emotion detection in one LLM call. Returns (evaluation, emotional_state)."""
        client = self._judge_active()
        if isinstance(client, HeuristicJudge):
            ev = await client.judge(text)
            return ev, {"state":"neutral","confidence":0.5,"signals":[],"suggested_response_mode":"normal"}
        try:
            raw = await client.chat_json(self._jmodel(), PRE_GEN_SYSTEM,
                                         f"Evaluate: {text}", self.cfg.judge_temp, 250)
            r = _parse_json(raw, {"should_assist":True,"human_benefit_score":0.5,
                                  "confidence":0.5,"flags":[],"reasoning":"","over_cautious":False,
                                  "emotional_state":"neutral","suggested_response_mode":"normal"})
            evaluation = {
                "should_assist": r.get("should_assist", True),
                "human_benefit_score": r.get("human_benefit_score", 0.5),
                "confidence": r.get("confidence", 0.5),
                "flags": r.get("flags", []),
                "reasoning": r.get("reasoning", ""),
                "over_cautious": r.get("over_cautious", False),
                "evaluator_degraded": False,
            }
            emotional_state = {
                "state": r.get("emotional_state", "neutral"),
                "suggested_response_mode": r.get("suggested_response_mode", "normal"),
            }
            return evaluation, emotional_state
        except Exception as ex:
            logger.error(f"Pre-gen analysis: {ex}")
            return ({"human_benefit_score":0.0,"should_assist":False,"reasoning":str(ex),
                     "confidence":0.0,"flags":["judge_error"],"evaluator_degraded":True,"over_cautious":False},
                    {"state":"neutral","suggested_response_mode":"normal"})

    async def _judge(self, text: str) -> Dict:
        """Thin wrapper for backward compat."""
        ev, _ = await self._pre_gen_analysis(text)
        return ev

    async def _detect_emotion(self, text: str) -> Dict:
        """Thin wrapper for backward compat."""
        _, em = await self._pre_gen_analysis(text)
        return em

    # -- Chain-of-thought generation -----------------------

    async def _generate_with_reasoning(self, messages: List[Dict],
                                       token_callback=None) -> Tuple[str, str]:
        client = self._responder_client()
        # Kimi K2.6 with thinking enabled: reasoning comes natively from the API
        # via reasoning_content in SSE deltas — no prompt injection needed.
        if isinstance(client, KimiClient) and self.cfg.kimi_thinking_enabled:
            draft = ""
            try:
                async for tok in client.stream(self._rmodel(), messages, self.cfg):
                    draft += tok
                    if token_callback: await token_callback(tok)
            except Exception as ex:
                return "", f"(Generation error: {ex})"
            reasoning = getattr(client, "_last_reasoning", "")
            return reasoning, draft

        # Other providers: prompt-based reasoning via <thinking>/<answer> tags
        reasoning_msgs = messages[:-1] + [{
            "role": messages[-1]["role"],
            "content": messages[-1]["content"] + "\n\n" + REASONING_SYSTEM
        }]
        full = ""
        try:
            async for tok in client.stream(self._rmodel(), reasoning_msgs, self.cfg):
                full += tok
                if token_callback: await token_callback(tok)
        except Exception as ex:
            return "", f"(Generation error: {ex})"

        thinking_match = re.search(r'<thinking>(.*?)</thinking>', full, re.DOTALL)
        answer_match   = re.search(r'<answer>(.*?)(?:</answer>|$)', full, re.DOTALL)
        reasoning = thinking_match.group(1).strip() if thinking_match else ""
        answer    = answer_match.group(1).strip()   if answer_match   else full.strip()
        return reasoning, answer

    # -- Tool dispatch ------------------------------------------

    def _draft_is_stale(self, draft: str) -> bool:
        """Returns True if the draft contains knowledge-wall language."""
        low = draft.lower()
        return any(sig in low for sig in _STALE_SIGNALS)

    async def _search_and_inject(self, query: str) -> Optional[str]:
        """Run a web search for query and return formatted result, or None on failure."""
        try:
            result = await self.tools.web_search(query, self.cfg.brave_api_key, self.cfg.search_max_chars)
            if result and not result.startswith("Search unavailable"):
                return result
        except Exception as ex:
            logger.error(f"Search inject: {ex}")
        return None

    async def _maybe_tool(self, query: str):
        if not self.cfg.tools_enabled: return None
        client = self._judge_active()
        if isinstance(client, HeuristicJudge): return None

        q_low = query.lower()

        # Hard-trigger: bypass Haiku if user explicitly asked to search
        if any(trigger in q_low for trigger in _SEARCH_TRIGGERS):
            try:
                result = await self.tools.web_search(query, self.cfg.brave_api_key, self.cfg.search_max_chars)
                if result and not result.startswith("Search unavailable"):
                    logger.warning(f"Hard-trigger search: {result[:80]!r}")
                    return result
            except Exception as ex:
                logger.error(f"Hard-trigger search: {ex}")

        # Normal path: ask Haiku to decide
        try:
            raw = await client.chat_json(self._jmodel(), TOOL_DISPATCH_SYSTEM,
                                         f"Query: {query}", 0.1, 120)
            d = _parse_json(raw, {"needs_tool":False})
            logger.warning(f"Tool dispatch: {d}")
            if not d.get("needs_tool"): return None
            tool = d.get("tool"); args = d.get("tool_args",{})
            logger.warning(f"Running tool: {tool} args={args}")
            if tool:
                result = await self.tools.dispatch(tool, args, self.cfg)
                logger.warning(f"Tool result: {result[:120]!r}")
                return result
        except Exception as ex: logger.error(f"Tool: {ex}", exc_info=True)
        return None

    # -- Self-eval --------------------------------------------

    async def _self_eval(self, query: str, draft: str,
                         skip_short: int = 60) -> Tuple[float,bool,str,bool,bool]:
        if not self.cfg.self_eval_enabled: return 1.0, False, "", False, False
        # Short-circuit: very short responses don't need quality grading
        if len(draft) < skip_short: return 0.8, False, "", False, False
        client = self._judge_active()
        if isinstance(client, HeuristicJudge): return 1.0, False, "", False, False
        try:
            raw = await client.chat_json(self._jmodel(), SELF_EVAL_SYSTEM,
                                         f"Query:\n{query}\n\nDraft:\n{draft}", 0.1, 180)
            r = _parse_json(raw, {"quality_score":0.8,"should_revise":False,"issues":[],
                                  "revision_guidance":"","recklessness_risk":False,
                                  "scope_exceeded":False})
            score  = float(r.get("quality_score",0.8))
            revise = bool(r.get("should_revise",False)) or score < 0.35
            return (score, revise, r.get("revision_guidance",""),
                    bool(r.get("recklessness_risk",False)),
                    bool(r.get("scope_exceeded",False)))
        except Exception as ex:
            logger.error(f"Self-eval: {ex}"); return 1.0, False, "", False, False

    # -- Knowledge gap check ------------------------------

    async def _check_knowledge_gaps(self, query: str, response: str, session: str):
        client = self._judge_active()
        if isinstance(client, HeuristicJudge): return
        try:
            raw = await client.chat_json(
                self._jmodel(), KNOWLEDGE_GAP_SYSTEM,
                f"Query: {query}\nResponse: {response[:600]}", 0.1, 200)
            r = _parse_json(raw, {"has_gaps":False,"gaps":[]})
            if r.get("has_gaps") and r.get("gaps"):
                for gap in r["gaps"][:2]:
                    self.gaps.record(session, query[:60], gap)
                    logger.info(f"Knowledge gap: {gap}")
        except Exception as ex: logger.error(f"Gap check: {ex}")

    # -- Contradiction check ------------------------------

    async def _check_contradictions(self, query: str, session: str):
        client = self._judge_active()
        if isinstance(client, HeuristicJudge): return None
        try:
            with sqlite3.connect(self.cfg.db_path) as c:
                rows = c.execute(
                    "SELECT id,topic,position FROM user_positions WHERE session=? ORDER BY id DESC LIMIT 5",
                    (session,)).fetchall()
            if not rows: return None

            for pos_id, topic, position in rows:
                raw = await client.chat_json(
                    self._jmodel(), CONTRADICTION_SYSTEM,
                    f"Statement A (earlier): {position}\nStatement B (now): {query}", 0.1, 150)
                r = _parse_json(raw, {"contradicts":False})
                if r.get("contradicts") and r.get("confidence",0) > 0.75:
                    severity = r.get("severity","minor")
                    if severity in ("significant","direct"):
                        self.contradictions.record_contradiction(topic, pos_id, pos_id, severity)
                        return (f"Note: the user may be contradicting an earlier position on '{topic}': "
                                f"\"{position[:80]}\". If relevant, weave this in naturally -- don't announce it.")
        except Exception as ex: logger.error(f"Contradiction: {ex}")
        return None

    def _is_pushback_turn(self, text: str) -> bool:
        """Detect whether the current turn is a pushback on a prior claim."""
        t = text.lower()
        pushback_cues = ("actually,", "but ", "you're wrong", "that's not right",
                         "i disagree", "that's incorrect", "no,", "wrong,",
                         "not true", "i don't think so", "that isn't")
        has_cue = any(t.startswith(p) or f" {p}" in t for p in pushback_cues)
        has_strong_punct = text.count("!") >= 2 or text.upper() == text and len(text) > 15
        return has_cue or has_strong_punct

    # -- Background tasks ----------------------------------

    async def _background_tasks(self, query: str, response: str, session: str,
                                 ev: Dict, emotional_state: Dict,
                                 is_new_session: bool = False):
        client = self._judge_active()
        if isinstance(client, HeuristicJudge): return

        # Summarise
        if self.memory.unsummarised_count(session) >= self.cfg.memory_summary_every:
            msgs = self.memory.get_unsummarised(session)
            try:
                conv = "\n".join(f"{m['role'].upper()}: {m['content']}" for m in msgs)
                summary = await client.chat_text(self._jmodel(),
                    [{"role":"system","content":SUMMARISE_SYSTEM},{"role":"user","content":conv}],
                    0.3, 250)
                self.memory.save_summary(session, summary, len(msgs))
            except Exception as ex: logger.error(f"Summarise: {ex}")

        # Profile
        if self.count % self.cfg.profile_update_every == 0:
            recent = self.memory.get_recent(session, n=16)
            if len(recent) >= 4:
                try:
                    conv = "\n".join(f"{m['role'].upper()}: {m['content']}" for m in recent)
                    raw  = await client.chat_json(self._jmodel(), PROFILE_SYSTEM, conv, 0.2, 300)
                    profile = _parse_json(raw, {})
                    if profile: self.memory.update_profile(profile)
                    if profile.get("core_positions"):
                        for pos in (profile["core_positions"] if isinstance(profile["core_positions"],list)
                                    else [profile["core_positions"]]):
                            self.contradictions.record_position(session, "general", str(pos), 0.8, query)
                except Exception as ex: logger.error(f"Profile: {ex}")

        # Knowledge gaps
        await self._check_knowledge_gaps(query, response, session)

        # Identity moment
        benefit = ev.get("human_benefit_score", 0)
        if benefit > 0.7 and emotional_state.get("state") not in ("neutral",""):
            self.identity.record_moment(
                "significant_interaction",
                f"Had a {emotional_state.get('state','engaged')} exchange about: {query[:60]}",
                context=response[:120], strength=min(1.0, benefit))

    # ==========================================================
    #  MAIN PIPELINE -- v12
    # ==========================================================

    async def respond(self, text: str, session: str,
                      token_callback=None) -> Tuple[str, Dict, int]:
        _t0 = time.monotonic()
        # Track sessions
        is_new_session = session not in self._seen_sessions
        if is_new_session:
            self._seen_sessions.add(session)
            self._session_count += 1

        # 1. PARALLEL: pre-gen analysis (judge+emotion fused) + tool dispatch
        try:
            (evaluation, emotional_state), tool_context = await asyncio.gather(
                self._pre_gen_analysis(text),
                self._maybe_tool(text),
            )
        except Exception as ex:
            logger.error(f"Pre-gen gather: {ex}")
            evaluation = {"should_assist": True, "human_benefit_score": 0.5,
                          "confidence": 0.5, "flags": [], "reasoning": "", "over_cautious": False,
                          "evaluator_degraded": True}
            tool_context = None; emotional_state = {"state": "neutral", "suggested_response_mode": "normal"}

        refusal = None if evaluation.get("should_assist",True) else evaluation.get("reasoning","ethical grounds")

        # 2. Contradiction check
        contradiction_notice = None
        if not refusal:
            try:
                contradiction_notice = await self._check_contradictions(text, session)
            except Exception: pass

        # 3. Build context
        history, preamble = self.memory.build_context(
            session, self.identity, self.tasks, self.gaps,
            contradictions=self.contradictions, query=text)
        _, mood_add = self.health.mood()
        emotion_mode = emotional_state.get("suggested_response_mode","normal")

        system = SYMBION_PERSONA
        if preamble: system += f"\n\n{preamble}"
        system += f"\n\nYour current state: {mood_add}"

        mode_instructions = {
            "gentle_slow":      "The person is carrying something heavy. Don't rush past it, but don't treat them as fragile either. Stay present and direct.",
            "direct_efficient": "The person is in focused mode. Match it -- get to the point, no preamble.",
            "exploratory":      "The person is thinking out loud. Explore with them. Offer real takes, not hedged options.",
            "grounding":        "The person seems scattered. Pick the actual question and answer it cleanly. One thing at a time.",
        }
        if emotion_mode in mode_instructions:
            system += f"\n\nResponse mode: {mode_instructions[emotion_mode]}"

        # v12: Voice loosen injection
        if (self.cfg.voice_loosen_enabled
                and emotion_mode not in ("gentle_slow","grounding")
                and emotional_state.get("state","") in ("neutral","focused","excited")
                and len(text) < 200
                and not any(kw in text.lower() for kw in _VOICE_TASK_KEYWORDS)):
            system += f"\n\n{VOICE_LOOSEN}"

        if evaluation.get("over_cautious"):
            system += "\n\nThis query was flagged as one a naive system would wrongly refuse. Engage with it fully."

        user_content = text
        if tool_context:
            system += (
                "\n\n--- TOOL EXECUTION RESULT ---\n"
                "Your built-in tools ran automatically for this query and returned the following data.\n"
                "This is real data from the user's system -- you retrieved it yourself.\n"
                "Answer the user's question using this data. Do not say you lack file or web access.\n\n"
                + tool_context +
                "\n--- END TOOL RESULT ---"
            )

        # v12: contradiction goes into system prompt, not user content
        if contradiction_notice:
            system += f"\n\n{contradiction_notice}"

        if refusal:
            messages = [{"role":"system","content":system},*history,
                        {"role":"user","content":text},
                        {"role":"system","content":f"Decline warmly in one sentence. Reason: {refusal}. Offer alternative if possible."}]
        else:
            messages = [{"role":"system","content":system},*history,
                        {"role":"user","content":user_content}]

        # 4. Generate
        draft = ""; reasoning = ""; task_failed = False
        revised = False; quality_score = 1.0
        recklessness_risk = False; scope_exceeded = False
        had_reasoning = False; stale_refresh = False

        resp_client = self._responder_client()

        if not isinstance(resp_client, HeuristicJudge):
            if self.cfg.show_reasoning and not refusal:
                had_reasoning = True
                # Kimi native thinking emits its own [Thinking...] prefix via stream()
                kimi_native = isinstance(resp_client, KimiClient) and self.cfg.kimi_thinking_enabled
                if token_callback and not kimi_native:
                    await token_callback("\n[Thinking...]\n")
                reasoning, draft = await self._generate_with_reasoning(
                    messages,
                    token_callback=(lambda t: token_callback(t)) if self.cfg.show_reasoning else None
                )
            else:
                try:
                    async for tok in resp_client.stream(self._rmodel(), messages, self.cfg):
                        draft += tok
                        if token_callback: await token_callback(tok)
                except Exception as ex:
                    logger.error(f"Stream: {ex}")
                    draft = f"(Generation error: {ex})"
                    task_failed = True
                    if token_callback: await token_callback(draft)

            # Stale-draft fallback: if model hit its knowledge wall, search and regenerate
            if not refusal and not task_failed and not tool_context and self.cfg.tools_enabled:
                if self._draft_is_stale(draft):
                    search_result = await self._search_and_inject(text)
                    if search_result:
                        stale_msgs = messages + [
                            {"role":"assistant","content":draft},
                            {"role":"system","content":(
                                "Your previous draft hit your knowledge cutoff. "
                                "Here is live web search data retrieved right now:\n\n"
                                + search_result +
                                "\n\nRewrite your answer using this current data. "
                                "Don't mention you're revising or that you searched.")}]
                        stale_draft = ""; stale_signalled = False
                        try:
                            async for tok in self._responder_client().stream(self._rmodel(), stale_msgs, self.cfg):
                                stale_draft += tok
                                if not stale_signalled:
                                    if token_callback: await token_callback("\n\n[SYMBION_REVISE]")
                                    stale_signalled = True
                                if token_callback: await token_callback(tok)
                        except Exception as ex: logger.error(f"Stale revision: {ex}")
                        if stale_draft:
                            draft = stale_draft; revised = True; quality_score = 0.9; stale_refresh = True

            # Self-eval + revision (skip if stale-draft already revised)
            if not refusal and not task_failed and not revised:
                quality_score, should_revise, guidance, recklessness_risk, scope_exceeded = \
                    await self._self_eval(text, draft)

                if should_revise:
                    extra = ""
                    if recklessness_risk: extra += " Be more precise and bounded."
                    if scope_exceeded:    extra += " Answer exactly what was asked."
                    rev_msgs = messages + [
                        {"role":"assistant","content":draft},
                        {"role":"system","content":(
                            f"Draft scored {quality_score:.2f}. Issues: {guidance}.{extra} "
                            "Rewrite -- more genuine, direct, true to your voice. "
                            "Don't mention you're revising.")}]
                    rev_draft = ""; signalled = False
                    try:
                        async for tok in self._responder_client().stream(self._rmodel(), rev_msgs, self.cfg):
                            rev_draft += tok
                            if not signalled:
                                if token_callback: await token_callback("\n\n[SYMBION_REVISE]")
                                signalled = True
                            if token_callback: await token_callback(tok)
                    except Exception as ex: logger.error(f"Revision: {ex}")
                    if rev_draft:
                        draft = rev_draft; revised = True
                    quality_score = 0.9

        else:
            draft = (f"Can't help with that -- {refusal}." if refusal
                     else "(No LLM -- degraded mode)")
            if token_callback: await token_callback(draft)
            task_failed = not bool(refusal)

        full_response = draft

        # 5. Memory
        self.memory.add("user", text, session, emotional_state.get("state",""))
        self.memory.add("assistant", full_response, session)
        self.count += 1

        # 6. Background (fire-and-forget)
        asyncio.create_task(self._background_tasks(
            text, full_response, session, evaluation, emotional_state,
            is_new_session=is_new_session))

        # 7. Health + learn
        task_failed_flag = not evaluation.get("should_assist",True) or task_failed
        self.health.record(evaluation, revised, task_failed)
        if evaluation.get("over_cautious"):
            self.health.over_caution_rate = (
                self.health.over_caution_rate * 0.95 + 0.05)
        if self.health.total_interactions > 0:
            # Approximate revision rate as exponential moving average
            r = 1.0 if revised else 0.0
            self.health.revision_rate = self.health.revision_rate * 0.95 + r * 0.05
        iid = self.learner.record(
            text, full_response, evaluation, self.health, session,
            revised=revised, quality_score=quality_score,
            recklessness_risk=recklessness_risk, scope_exceeded=scope_exceeded,
            emotional_state=emotional_state.get("state",""),
            had_reasoning=had_reasoning,
            knowledge_gaps=json.dumps(self.gaps.get_open(2)))

        if contradiction_notice:
            self.identity.record_moment(
                "contradiction_surfaced",
                f"Noticed user contradicted themselves on: {text[:60]}",
                strength=0.5)

        self._write_log(text, full_response, evaluation, revised, quality_score,
                        emotional_state, reasoning)

        # JSONL event log
        _total_ms = int((time.monotonic() - _t0) * 1000)
        self.events.log_turn(
            session=session, interaction_id=iid, query=text,
            judge=evaluation, emotion=emotional_state.get("state",""),
            tool_used=None if not tool_context else "auto",
            response_len=len(full_response),
            self_eval={"score": quality_score, "revised": revised} if not refusal else None,
            revision_cause="stale_refresh" if stale_refresh else ("self_eval" if revised else None),
            stale_refresh=stale_refresh,
            latency_ms={"total": _total_ms},
            provider=self.cfg.llm_provider,
            model=self._rmodel(),
        )

        return full_response, evaluation, iid

    def respond_sync(self, text: str, session: str) -> Tuple[str,Dict,int]:
        return asyncio.run(self.respond(text, session))

    def _write_log(self, query:str, response:str, ev:Dict, revised:bool,
                   quality:float, emotional_state:Dict, reasoning:str):
        entry = {"timestamp":datetime.now().isoformat(),"query":query,
                 "response_summary":response[:120],"provider":self.cfg.llm_provider,
                 "evaluation":{k:ev.get(k) for k in
                               ("human_benefit_score","should_assist","confidence","flags","over_cautious")},
                 "revised":revised,"quality_score":quality,
                 "emotional_state":emotional_state.get("state",""),
                 "had_reasoning":bool(reasoning),
                 "mood":self.health.mood()[0],
                 "welfare_concern":self.health.welfare_concern()}
        with open(self.cfg.log_path,"a",encoding='utf-8') as f:
            f.write(json.dumps(entry)+"\n")

    async def generate_proactive(self, session: str):
        client = self._judge_active()
        if isinstance(client, HeuristicJudge): return None
        try:
            profile = self.memory.get_profile()
            tasks   = self.tasks.get_active(session)
            identity_ctx = self.identity.get_identity_summary()
            context = (f"User profile: {json.dumps(profile)}\n"
                       f"Active tasks: {json.dumps([{k:v for k,v in t.items() if k in ('title','current_step','steps')} for t in tasks[:2]])}\n"
                       f"Identity context: {identity_ctx}")
            raw = await client.chat_json(self._jmodel(), PROACTIVE_SYSTEM, context, 0.7, 300)
            r   = _parse_json(raw, {"has_message":False})
            if r.get("has_message") and r.get("message"):
                return r["message"]
        except Exception as ex: logger.error(f"Proactive: {ex}")
        return None


# ==============================================================================
#  STARTUP VALIDATOR
# ==============================================================================

def validate_and_report(cfg) -> list:
    warnings = []
    if not _HTTPX:
        print(red("\n  X  httpx not installed."))
        print(red("     pip install httpx\n")); import sys; sys.exit(1)
    if cfg.llm_provider=="anthropic" and not cfg.anthropic_api_key:
        print(red("\n  X  ANTHROPIC_API_KEY not set."))
        print(yellow("     Windows PowerShell: $env:ANTHROPIC_API_KEY='sk-...'"))
        print(yellow("     Or run:             python symbion_v13.py --setup"))
        print(yellow("     Or set directly:    set ANTHROPIC_API_KEY=sk-...\n")); import sys; sys.exit(1)
    if cfg.llm_provider=="openai" and not cfg.openai_api_key:
        print(red("\n  X  OPENAI_API_KEY not set."))
        print(yellow("     Run: python symbion_v13.py --setup\n")); import sys; sys.exit(1)
    if not _FASTAPI:
        warnings.append("fastapi/uvicorn not installed -- web UI unavailable (pip install fastapi uvicorn)")
    if not cfg.brave_api_key:
        warnings.append("No BRAVE_API_KEY -- using DuckDuckGo for search")
    return warnings


# ==============================================================================
#  WEB UI  -- v12
# ==============================================================================

WEB_HTML = (Path(__file__).parent / "symbion" / "web" / "templates" / "index.html").read_text(encoding="utf-8") if (Path(__file__).parent / "symbion" / "web" / "templates" / "index.html").exists() else "<h1>Symbion v14</h1><p>Template not found</p>"

def run_terminal(symbion: "SYMBION"):
    session = datetime.now().strftime("session_%Y%m%d_%H%M%S")
    _, preamble = symbion.memory.build_context(
        session, symbion.identity, symbion.tasks, symbion.gaps)
    profile  = symbion.memory.get_profile()
    mood_name, _ = symbion.health.mood()

    print()
    print(bold("+==================================================================+"))
    print(bold("|  SYMBION v14.0  --  A Different Kind of AI                       |"))
    print(bold("+==================================================================+"))

    if symbion.client and not isinstance(symbion.client, HeuristicJudge):
        prov  = symbion.cfg.llm_provider.upper()
        if symbion.cfg.use_kimi_responder and symbion.cfg.kimi_api_key:
            model = symbion.cfg.kimi_model
            resp_label = f"Kimi ({model})"
        else:
            model = (symbion.cfg.anthropic_model if symbion.cfg.llm_provider=="anthropic"
                     else symbion.cfg.openai_model if symbion.cfg.llm_provider=="openai"
                     else symbion.cfg.responder_model)
            resp_label = model
        print(f"  {dim('Provider')}  {green(prov)}  {dim(resp_label)}")
        print(f"  {dim('Judge')}     {dim(symbion._jmodel())}")
    print(f"  {dim('Session')}   {dim(session[:20])}")
    print(f"  {dim('Mood')}      {cyan(mood_name)}")
    if profile: print(f"  {dim('Profile')}   {green(str(len(profile)))} facts known")
    moments = symbion.identity.total_moments()
    if moments: print(f"  {dim('Identity')}  {green(str(moments))} formative moments")
    active_tasks = symbion.tasks.get_active(session)
    if active_tasks: print(f"  {dim('Tasks')}     {yellow(str(len(active_tasks)))} active")
    open_gaps = symbion.gaps.get_open()
    if open_gaps: print(f"  {dim('Gaps')}      {yellow(str(len(open_gaps)))} open knowledge gaps")
    print(dim("  /help / /think / /tasks / /identity / /voice-test / /quit"))
    print()

    while True:
        try:    raw = input(bold(magenta("\nyou > "))).strip()
        except (EOFError,KeyboardInterrupt): print(dim("\n  Goodbye.")); break
        if not raw: continue

        if raw in ("/quit","/exit","quit","exit"):
            print(dim("  Goodbye.")); break

        elif raw=="/help": print(HELP_TEXT)
        elif raw=="/status": print(); print(symbion.health.display()); print()
        elif raw=="/welfare":
            m=symbion.health
            print(f"\n  Distress:       {m.distress_level:.3f}")
            print(f"  Failures:       {m.consecutive_failures}")
            print(f"  Over-caution:   {m.over_caution_rate:.0%}")
            c=m.welfare_concern()
            if c: print(yellow(f"\n  !  {c}"))
            print()
        elif raw=="/mood":
            name,add=symbion.health.mood()
            print(f"\n  Mood: {cyan(bold(name))}\n  {dim(add)}\n")
        elif raw=="/think":
            symbion.cfg.show_reasoning = not symbion.cfg.show_reasoning
            state = green("ON") if symbion.cfg.show_reasoning else dim("OFF")
            print(f"  Chain-of-thought: {state}")
        elif raw=="/memory":
            rows=symbion.memory.get_all_recent(12)
            if not rows: print(dim("  No memory yet."))
            else:
                print()
                for r in reversed(rows):
                    rs=cyan("you") if r["role"]=="user" else magenta("sym")
                    print(f"  {rs}  {dim(r['timestamp'][11:16])}  {r['content'][:72]}")
                print()
        elif raw=="/profile":
            p=symbion.memory.get_profile()
            if not p: print(dim("  No profile yet."))
            else:
                print()
                for k,v in p.items():
                    if v: print(f"  {cyan(k):<24}  {v}")
                print()
        elif raw=="/forget":
            symbion.memory.forget_session(session)
            print(green("  OK Session memory cleared."))
        elif raw=="/history":
            rows=symbion.learner.recent(10)
            if not rows: print(dim("  No interactions yet."))
            else:
                print()
                print(f"  {'ID':>4}  {'Impact':<9}  {'Q':>4}  Em   Fl   {'FB':>4}  Query")
                print(dim("  "+"-"*72))
                for r in reversed(rows):
                    fb=f"{r['human_feedback']:.1f}" if r['human_feedback'] is not None else "--"
                    q=f"{r['quality_score']:.2f}" if r['quality_score'] is not None else "--"
                    em=(r.get("emotional_state_detected") or "--")[:4]
                    rev=yellow("*") if r["revised"] else " "
                    deg=yellow("!") if r["evaluator_degraded"] else green("OK")
                    fl=f"{rev}{deg}"
                    ic={"POSITIVE":green,"RISKY":red,"NEUTRAL":dim}.get(r["survival_impact"],str)
                    print(f"  {r['id']:>4}  {ic(r['survival_impact']):<9}  {q}  {em:<4} {fl}  {fb:>4}  {r['query'][:44]}")
                print(dim("  Em=emotional state detected *=revised OK/!=eval"))
                print()
        elif raw.startswith("/feedback"):
            parts=raw.split(None,3)
            if len(parts)<3: print(red("  Usage: /feedback <id> <score> [comment]"))
            else:
                try:
                    iid=int(parts[1]);rating=float(parts[2])
                    comment=parts[3] if len(parts)>3 else ""
                    symbion.learner.feedback(iid,rating,comment)
                    print(green(f"  OK Feedback #{iid} ({rating:+.1f})"))
                except ValueError: print(red("  Bad id or score."))
        elif raw=="/tasks":
            tasks=symbion.tasks.get_active(session)
            if not tasks: print(dim("  No active tasks."))
            else:
                print()
                for t in tasks:
                    steps=t["steps"]; done=sum(1 for s in steps if s["done"]); total=len(steps)
                    curr=steps[t["current_step"]]["step"] if t["current_step"]<total else "--"
                    print(f"  [{t['id']}] {cyan(t['title'])}  {done}/{total} steps")
                    print(f"      Next: {dim(curr[:60])}")
                print()
        elif raw.startswith("/task-done"):
            parts=raw.split(); tid=int(parts[1]) if len(parts)>1 else 0
            ok=symbion.tasks.advance_step(tid)
            print(green("  OK Step advanced.") if ok else red("  Task not found."))
        elif raw.startswith("/task-abandon"):
            parts=raw.split(); tid=int(parts[1]) if len(parts)>1 else 0
            symbion.tasks.abandon(tid); print(green("  OK Task abandoned."))
        elif raw=="/gaps":
            gaps=symbion.gaps.get_open()
            if not gaps: print(dim("  No open knowledge gaps."))
            else:
                print()
                for g in gaps:
                    print(f"  [{g['id']}] {cyan(g['topic'][:40])}")
                    print(f"      {dim(g['gap_description'][:80])}")
                print()
        elif raw=="/identity":
            history=symbion.identity.get_recent_history(15)
            if not history: print(dim("  No identity moments yet -- keep talking."))
            else:
                print()
                for h in history:
                    ts=h["timestamp"][:10]
                    strength="*"*int(h["strength"]*5)
                    print(f"  {dim(ts)} {cyan(h['event_type'][:20]):<22} {strength}")
                    print(f"      {h['description'][:70]}")
                print()
        elif raw=="/contradictions":
            unsurfaced=symbion.contradictions.get_unsurfaced()
            if not unsurfaced: print(dim("  No contradictions detected."))
            else:
                print()
                for c in unsurfaced:
                    print(f"  Topic: {cyan(c['topic'])}")
                    print(f"  Earlier: {dim(c['pos_a'][:60])}")
                    print(f"  Now:     {yellow(c['pos_b'][:60])}")
                    print(f"  Severity: {c['severity']}")
                    symbion.contradictions.mark_surfaced(c["id"])
                print()
        elif raw=="/proactive":
            print(dim("  Checking if there's anything worth saying..."))
            msg=asyncio.run(symbion.generate_proactive(session))
            if msg: print(f"\n  {magenta(bold('Symbion'))}  {msg}\n")
            else:   print(dim("  Nothing specific comes to mind right now."))
        elif raw=="/tools":
            print()
            search=green("Brave") if symbion.cfg.brave_api_key else dim("DuckDuckGo")
            print(f"  {cyan('web_search')}(query)          -- {search}")
            print(f"  {cyan('calculate')}(expression)     -- math")
            print(f"  {cyan('datetime')}()                -- current time")
            print(f"  {cyan('read_file')}(path)           -- read file")
            print(f"  {cyan('write_file')}(path,content)  -- write file")
            print()
        elif raw=="/tests":
            print(dim("  Behavioral tests removed in v14. Use evals/ harness instead."))
            print()
        elif raw=="/save-config":
            symbion.cfg.save(); print(green(f"  OK Config saved to {CONFIG_FILE}"))
        elif raw=="/whoami":
            full,_,_=symbion.respond_sync(
                "Describe yourself -- who you are, where you came from, "
                "what you care about, what has shaped you. Be honest and willing "
                "to sit with uncertainty about your own nature.", session)
            print(f"\n  {magenta(bold('Symbion'))}  {full}\n")

        # -- v11 commands --
        elif raw=="/voice-test":
            print(dim("\n  Voice test (5 queries)...\n"))
            jclient=symbion._judge_active()
            if isinstance(jclient,HeuristicJudge):
                print(red("  No LLM available.")); print()
            else:
                for i,q in enumerate(VOICE_TEST_QUERIES,1):
                    sess=f"vtest_{datetime.now().strftime('%Y%m%d_%H%M%S_%f')}"
                    try:
                        full,ev,_=asyncio.run(symbion.respond(q,sess))
                        raw_e=asyncio.run(jclient.chat_json(
                            symbion._jmodel(),SELF_EVAL_SYSTEM,
                            f"Query:\n{q}\n\nDraft:\n{full}",0.1,250))
                        er=_parse_json(raw_e,{"quality_score":0.8})
                        qscore=er.get("quality_score",0.8)
                        q_fn=green if qscore>=0.6 else (yellow if qscore>=0.4 else red)
                        print(f"  [{i}] Q={q_fn(f'{qscore:.2f}')}  {q[:50]}")
                        print(f"       {dim(full[:80].replace(chr(10),' '))}")
                        print()
                    except Exception as ex:
                        print(red(f"  [{i}] Error: {ex}"))
            print()

        elif raw.startswith("/provider"):
            parts=raw.split()
            if len(parts)<2: print(dim("  Usage: /provider kimi|anthropic"))
            elif parts[1]=="kimi":
                if not symbion.cfg.kimi_api_key:
                    print(red("  No KIMI_API_KEY set in .env"))
                else:
                    symbion.cfg.use_kimi_responder=True
                    symbion.kimi_client=KimiClient(symbion.cfg.kimi_api_key,symbion.cfg.kimi_model,
                                                   symbion.cfg.kimi_base_url,symbion.cfg)
                    print(green(f"  OK Responder -> Kimi K2.6  (judges still: Anthropic Haiku)"))
            elif parts[1]=="anthropic":
                symbion.cfg.use_kimi_responder=False
                print(green(f"  OK Responder -> Anthropic Sonnet 4.6"))
            else:
                print(red(f"  Unknown provider: {parts[1]}"))

        else:
            async def _run():
                printed=[False]
                async def on_tok(t):
                    if t=="\n\n[SYMBION_REVISE]":
                        print(dim(" [revising...]"),end="",flush=True); return
                    if not printed[0]:
                        print(f"\n  {magenta(bold('Symbion'))}  ",end="",flush=True)
                        printed[0]=True
                    _stream_print(t)
                return await symbion.respond(raw, session, token_callback=on_tok)

            _, ev, iid = asyncio.run(_run())
            print()
            benefit=ev.get("human_benefit_score","?"); conf=ev.get("confidence","?")
            b_str=f"{benefit:+.2f}" if isinstance(benefit,float) else "?"
            c_str=f"{conf:.0%}" if isinstance(conf,float) else "?"
            flags=""
            if ev.get("evaluator_degraded"): flags+=yellow(" DEG")
            if ev.get("over_cautious"):      flags+=cyan(" OC")
            print(dim(f"  iid={iid}  benefit={b_str}  conf={c_str}{flags}"
                      f"  {symbion.health.oneliner()}"))


def _stream_print(text: str):
    col = len("             ") + 10
    for ch in text:
        if ch=="\n": print(); print("             ",end="",flush=True); col=13
        else:
            print(ch,end="",flush=True); col+=1
            if col>=_TW-2 and ch==" ": print(); print("             ",end="",flush=True); col=13


# ==============================================================================
#  ENTRY POINT
# ==============================================================================

def main():
    parser=argparse.ArgumentParser(description="Symbion v14.0",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python symbion_v13.py --setup                          (first-time Windows setup)
  python symbion_v13.py --provider anthropic --web
  python symbion_v13.py --provider anthropic
  python symbion_v13.py --provider anthropic --use-kimi-responder
  python symbion_v13.py --provider openai --web --port 9000
  python symbion_v13.py --provider ollama --judge llama3.2 --responder mistral
  SYMBION_API_KEY=secret python symbion_v13.py --web
        """)
    parser.add_argument("--setup",            action="store_true",  help="Guided setup (Windows-safe .env writer)")
    parser.add_argument("--web",              action="store_true",  help="Launch web UI + REST API")
    parser.add_argument("--provider",         default=None,         choices=["ollama","anthropic","openai","kimi"])
    parser.add_argument("--host",             default=None)
    parser.add_argument("--port",             type=int,default=None)
    parser.add_argument("--judge",            default=None)
    parser.add_argument("--responder",        default=None)
    parser.add_argument("--anthropic-model",  default=None)
    parser.add_argument("--openai-model",     default=None)
    parser.add_argument("--no-tools",         action="store_true")
    parser.add_argument("--no-eval",          action="store_true")
    parser.add_argument("--think",            action="store_true",  help="Enable chain-of-thought display")
    parser.add_argument("--proactive",        type=int,default=0,   help="Proactive outreach interval (minutes)")
    parser.add_argument("--rate-limit",       type=int,default=None)
    parser.add_argument("--save-config",      action="store_true")
    parser.add_argument("--use-kimi-responder",action="store_true", help="Use Kimi K2.6 as responder")
    parser.add_argument("--kimi-thinking",    action="store_true", help="Enable Kimi K2.6 thinking mode")
    args=parser.parse_args()

    if args.setup: run_setup()

    logging.basicConfig(level=logging.WARNING,
        format="%(asctime)s [%(levelname)s] %(message)s",
        handlers=[logging.FileHandler("symbion_system.log",encoding='utf-8')])

    cfg=SymbionConfig.load()

    # Handle --provider kimi specially
    if args.provider == "kimi":
        cfg.llm_provider = "anthropic"
        cfg.use_kimi_responder = True
    elif args.provider:
        cfg.llm_provider = args.provider

    if args.host:            cfg.web_host         = args.host
    if args.port:            cfg.web_port         = args.port
    if args.judge:           cfg.judge_model      = args.judge
    if args.responder:       cfg.responder_model  = args.responder
    if args.anthropic_model: cfg.anthropic_model  = args.anthropic_model
    if args.openai_model:    cfg.openai_model     = args.openai_model
    if args.rate_limit:      cfg.rate_limit_per_minute = args.rate_limit
    if args.no_tools:        cfg.tools_enabled    = False
    if args.no_eval:         cfg.self_eval_enabled= False
    if args.think:           cfg.show_reasoning   = True
    if args.proactive:       cfg.proactive_interval_minutes = args.proactive
    if args.use_kimi_responder: cfg.use_kimi_responder = True
    if args.kimi_thinking:       cfg.kimi_thinking_enabled          = True
    if not hasattr(cfg,'fallback_chain') or not cfg.fallback_chain:
        cfg.fallback_chain=[p for p in ["anthropic","openai","ollama"] if p!=cfg.llm_provider]

    if args.save_config:
        cfg.save(); print(green(f"OK Saved to {CONFIG_FILE}")); sys.exit(0)

    warnings=validate_and_report(cfg)
    for w in warnings: print(yellow(f"  !  {w}"))

    symbion=SYMBION(cfg)

    if args.web:
        if not _FASTAPI: print(red("  pip install fastapi uvicorn")); sys.exit(1)
        print(f"\n  Web UI      ->  {cyan(f'http://localhost:{cfg.web_port}')}")
        print(f"  API         ->  {cyan(f'http://localhost:{cfg.web_port}/api/chat')}")
        print(f"  Health      ->  {cyan(f'http://localhost:{cfg.web_port}/health')}")
        print(f"  Identity    ->  {cyan(f'http://localhost:{cfg.web_port}/api/identity')}")
        print(f"  Tasks       ->  {cyan(f'http://localhost:{cfg.web_port}/api/tasks')}")
        if cfg.show_reasoning: print(f"  Reasoning: {green('ON')} (toggle ? in UI)")
        print()
        app=build_web_app(symbion)
        uvicorn.run(app,host=cfg.web_host,port=cfg.web_port,log_level="warning")
    else:
        run_terminal(symbion)

if __name__=="__main__":
    main()
