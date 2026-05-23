// Electron main process for Symbion.
//
// Lifecycle:
//   1. Single-instance lock — second launch focuses the existing window
//   2. Spawn `python -m symbion --web` as a child process (portable Python
//      at ../.python/python.exe when present, else system python)
//   3. Poll /health until the backend is responding
//   4. Open a BrowserWindow at http://localhost:<port>
//   5. On app quit, kill the backend's process tree (Windows: taskkill /F /T)
//
// No IPC bridge yet — the renderer just loads the existing web UI as-is.
// preload.js exposes app version / platform for future use.

const { app, BrowserWindow, shell, Menu, dialog, Tray, Notification, nativeImage } = require('electron');
const { spawn, execFile } = require('child_process');
const fs   = require('fs');
const http = require('http');
const path = require('path');

// Resolve the Symbion repo root. Tried in priority order:
//   1. $env:SYMBION_REPO override
//   2. Dev-mode: electron/ is inside the repo, so `..` works
//   3. Installed-mode: %USERPROFILE%\symbion (canonical install location
//      from install.ps1) — this is the case for the NSIS-installed app,
//      where __dirname lives under %LOCALAPPDATA%\Programs\Symbion\ and
//      `..` would point at Programs\, not at the repo.
//   4. %USERPROFILE%\SourceCode\symbion (alt user layout)
// Returns null when none of these have a symbion_v14.py — the user has
// no Symbion repo on the machine and we surface a clear error dialog.
function resolveRepoRoot() {
  const home = process.env.USERPROFILE || process.env.HOME || '';
  const candidates = [
    process.env.SYMBION_REPO,
    path.resolve(__dirname, '..'),
    home ? path.join(home, 'symbion')            : null,
    home ? path.join(home, 'SourceCode', 'symbion') : null,
  ].filter(Boolean);
  for (const c of candidates) {
    if (fs.existsSync(path.join(c, 'symbion_v14.py'))) return c;
  }
  return null;
}
const REPO_ROOT = resolveRepoRoot();

// Config — load symbion.json so we use the same web_port the user has
// configured. Falls back to 8000 (SymbionConfig default).
function loadConfig() {
  if (!REPO_ROOT) return {};
  const p = path.join(REPO_ROOT, 'symbion.json');
  try {
    const raw = fs.readFileSync(p, 'utf8');
    return JSON.parse(raw);
  } catch (e) {
    return {};
  }
}
const cfg = loadConfig();
const PORT     = cfg.web_port  || 8000;
const HOST     = '127.0.0.1';
const HEALTH_URL = `http://${HOST}:${PORT}/health`;
const UI_URL     = `http://${HOST}:${PORT}/`;

// Pick the Python interpreter. Priority order:
//   1. Bundled Python shipped with the Electron installer (packaged mode):
//      extraResources puts .python/ at process.resourcesPath/.python/.
//      Lets the desktop installer work standalone without install.ps1
//      having pre-staged a portable Python.
//   2. Repo's portable Python at $REPO_ROOT/.python/python.exe (dev mode,
//      or installed-mode where install.ps1 already ran bootstrap-portable).
//   3. `python` on PATH (last resort — system Python may not have deps).
// Returns "python" as a final fallback so the spawn at least tries; the
// startBackend → waitForBackend chain will surface a clear error if it
// fails. Never returns null — falling back to PATH-python lets the user
// recover by setting up their own env without reinstalling.
function resolvePython() {
  // Packaged mode: extraResources lives at process.resourcesPath. In dev
  // this points at Electron's own resources dir which doesn't have .python,
  // so the check falls through to REPO_ROOT/.python below.
  if (app.isPackaged) {
    const bundled = path.join(process.resourcesPath, '.python', 'python.exe');
    if (fs.existsSync(bundled)) return bundled;
  }
  if (REPO_ROOT) {
    const portable = path.join(REPO_ROOT, '.python', 'python.exe');
    if (fs.existsSync(portable)) return portable;
  }
  return 'python';
}

let backend     = null;   // child_process handle for `python -m symbion --web`
let mainWindow  = null;
let backendDied = false;  // gets set when the child exits unexpectedly

// ---- Single-instance lock ----
// Without this, double-clicking the shortcut or relaunching from a
// taskbar pin spawns a second Electron + a second backend, the second
// backend then fails to bind to PORT and Symbion appears "broken".
const gotLock = app.requestSingleInstanceLock();
if (!gotLock) {
  // Lock is held by another Symbion / installed Symbion.exe — exit
  // silently. The other instance's 'second-instance' handler will
  // focus its window if it has one. Stale lockfiles in %APPDATA%\
  // symbion-app\ from a crashed prior run can cause this too;
  // delete %APPDATA%\symbion-app\lockfile to recover.
  app.quit();
} else {
  // Second-instance handler: another Symbion.exe / electron . launch
  // hit the singleton lock and bounced. Surface the main window.
  // When mainWindow is null (e.g. the user closed the chat window while
  // the tray kept the app alive), create a fresh window — same fallback
  // pattern the tray menu's 'Open Symbion' uses. Without this, clicking
  // a pinned taskbar / Start menu shortcut after closing the window
  // does nothing visible.
  app.on('second-instance', () => {
    if (mainWindow && !mainWindow.isDestroyed()) {
      if (mainWindow.isMinimized()) mainWindow.restore();
      mainWindow.show();
      mainWindow.focus();
    } else {
      createWindow();
    }
  });
}

// ---- Backend spawn ----
function startBackend() {
  const py = resolvePython();
  // -u disables stdout buffering so log lines show up in the Electron
  // console in real time, not in 8KB chunks.
  const args = ['-u', '-m', 'symbion', '--web'];
  console.log(`[symbion] spawning: ${py} ${args.join(' ')}`);
  backend = spawn(py, args, {
    cwd: REPO_ROOT,
    env: { ...process.env, PYTHONUNBUFFERED: '1' },
    stdio: ['ignore', 'pipe', 'pipe'],
    // Windows: detached + no shell so the child has its own process group.
    // We need this so taskkill /F /T can take down the whole tree when
    // the app quits (Symbion spawns its own subprocesses for MCP / ollama
    // checks). On macOS/Linux the spawned child IS the process group leader
    // by default, so detached:true doesn't hurt either.
    windowsHide: true,
    detached: false,
  });

  backend.stdout.on('data', (b) => process.stdout.write(`[symbion] ${b}`));
  backend.stderr.on('data', (b) => process.stderr.write(`[symbion] ${b}`));

  backend.on('exit', (code, signal) => {
    backendDied = true;
    console.log(`[symbion] backend exited code=${code} signal=${signal}`);
    if (mainWindow && !mainWindow.isDestroyed()) {
      // Surface unexpected exits to the user so they know what happened.
      // Suppress when we triggered the kill ourselves (app quit or
      // tray-driven provider switch) — those set the matching flag before
      // killing so this handler knows the exit was intentional.
      if (!app.isQuitting && !app.isRestartingBackend) {
        dialog.showErrorBox(
          'Symbion backend stopped',
          `The Symbion Python process exited (code ${code}).\n\n` +
          `Check the console log for the cause. You can restart by closing and reopening the app.`);
      }
    }
  });
}

// Kill the spawned backend and wait for the OS to fully reap it before
// resolving. Used by the tray-driven provider switcher so the replacement
// spawn doesn't race the old child for port 8000. Resolves immediately if
// nothing to kill; resolves after an 8s hard timeout if 'exit' never fires
// (rare Windows case where detached grandchildren keep the group alive).
function killBackendTree() {
  return new Promise((resolve) => {
    if (!backend || backend.killed) return resolve();
    const child = backend;
    let done = false;
    const finish = () => { if (!done) { done = true; resolve(); } };
    child.once('exit', finish);
    if (process.platform === 'win32') {
      try {
        execFile('taskkill', ['/PID', String(child.pid), '/T', '/F'], () => {});
      } catch (e) { finish(); }
    } else {
      try { child.kill('SIGTERM'); } catch (e) { finish(); }
    }
    setTimeout(finish, 8000);
  });
}

// Restart sequence used by the provider switcher. Marks the restart so
// the exit handler doesn't fire its error dialog, kills the old child,
// resets state, spawns a fresh one, and waits for /health.
async function restartBackend() {
  app.isRestartingBackend = true;
  try {
    await killBackendTree();
    backend = null;
    backendDied = false;
    startBackend();
    const ready = await waitForBackend();
    if (!ready.ok) {
      dialog.showErrorBox(
        'Backend restart failed',
        `Couldn't reach ${HEALTH_URL} after restart (${ready.reason}).\n\n` +
        `The provider change was written to symbion.json. Check the console for the cause.`);
    }
    return ready.ok;
  } finally {
    app.isRestartingBackend = false;
  }
}

// Poll /health until the backend responds, then resolve. Times out after
// ~30s (Symbion's startup typically lands in 2-5s but the first run after
// dependency install can be slower). On timeout we still open the window
// so the user can see the error rather than staring at a blank screen.
function waitForBackend(timeoutMs = 30_000) {
  const deadline = Date.now() + timeoutMs;
  return new Promise((resolve) => {
    const tick = () => {
      if (backendDied) return resolve({ ok: false, reason: 'backend exited before ready' });
      const req = http.get(HEALTH_URL, (res) => {
        // 200 from /health is the canonical ready signal.
        if (res.statusCode === 200) {
          res.resume();
          resolve({ ok: true });
        } else {
          res.resume();
          retry();
        }
      });
      req.on('error', retry);
      req.setTimeout(2000, () => { req.destroy(); retry(); });
    };
    const retry = () => {
      if (Date.now() > deadline) {
        return resolve({ ok: false, reason: 'health check timeout' });
      }
      setTimeout(tick, 300);
    };
    tick();
  });
}

// ---- Window ----
function createWindow() {
  // Per-platform icon. The installer (electron-builder) bakes
  // assets/symbion.ico into the EXE itself, but the BrowserWindow icon
  // is what shows in the taskbar / alt-tab / window title bar — Electron
  // doesn't pick that up from the EXE on Windows, so we set it
  // explicitly. macOS reads icon from .icns in the bundle and ignores
  // this; Linux uses the PNG.
  const iconPath = path.join(__dirname, 'assets',
    process.platform === 'win32'  ? 'symbion.ico'  :
    process.platform === 'darwin' ? 'symbion.icns' :
                                    'symbion-512.png');

  mainWindow = new BrowserWindow({
    width:  1280,
    height: 800,
    minWidth:  720,
    minHeight: 480,
    backgroundColor: '#0c0a08',  // matches Symbion's web UI --bg
    title: 'Symbion',
    icon: iconPath,
    webPreferences: {
      contextIsolation: true,
      nodeIntegration:  false,
      preload: path.join(__dirname, 'preload.js'),
    },
    autoHideMenuBar: false,
  });

  // Open external links in the system browser, not in-app.
  mainWindow.webContents.setWindowOpenHandler(({ url }) => {
    if (url.startsWith('http://') || url.startsWith('https://')) {
      shell.openExternal(url);
      return { action: 'deny' };
    }
    return { action: 'allow' };
  });

  mainWindow.loadURL(UI_URL);

  mainWindow.on('closed', () => { mainWindow = null; });
}

// Native menu — File / Edit / View / Window / Help. The Electron defaults
// drop View entirely on production builds, which hides reload + devtools
// for debugging. Custom menu keeps those reachable.
function buildMenu() {
  const isMac = process.platform === 'darwin';
  const template = [
    ...(isMac ? [{ role: 'appMenu' }] : []),
    { role: 'fileMenu' },
    { role: 'editMenu' },
    {
      label: 'View',
      submenu: [
        { role: 'reload' },
        { role: 'forceReload' },
        { role: 'toggleDevTools' },
        { type: 'separator' },
        { role: 'resetZoom' },
        { role: 'zoomIn' },
        { role: 'zoomOut' },
        { type: 'separator' },
        { role: 'togglefullscreen' },
      ],
    },
    { role: 'windowMenu' },
    {
      role: 'help',
      submenu: [
        {
          label: 'Symbion on GitHub',
          click: () => shell.openExternal('https://github.com/symbion0130/symbion'),
        },
      ],
    },
  ];
  Menu.setApplicationMenu(Menu.buildFromTemplate(template));
}

// Probe /health once with a short timeout. Used at startup to decide
// whether to spawn our own backend or attach to an already-running one.
function probeHealth(timeoutMs = 1500) {
  return new Promise((resolve) => {
    const req = http.get(HEALTH_URL, (res) => {
      const ok = res.statusCode === 200;
      res.resume();
      resolve(ok);
    });
    req.on('error', () => resolve(false));
    req.setTimeout(timeoutMs, () => { req.destroy(); resolve(false); });
  });
}

// Fetch the full /health JSON for the tray widget. Same shape as
// probeHealth but returns the parsed payload. null on any failure.
function fetchHealthJson(timeoutMs = 2000) {
  return new Promise((resolve) => {
    const req = http.get(HEALTH_URL, (res) => {
      if (res.statusCode !== 200) { res.resume(); return resolve(null); }
      let buf = '';
      res.setEncoding('utf8');
      res.on('data', (c) => { buf += c; });
      res.on('end', () => {
        try { resolve(JSON.parse(buf)); } catch (e) { resolve(null); }
      });
    });
    req.on('error', () => resolve(null));
    req.setTimeout(timeoutMs, () => { req.destroy(); resolve(null); });
  });
}

// ---- Tray widget (analytics rollout 2026-05-23) ----
// System tray icon with tooltip-style health summary, right-click menu
// for quick navigation, and native OS notification when Symbion's health
// crosses into 'something's wrong' territory (consecutive_failures > 0).
// Polls /health every TRAY_POLL_MS; debounces failure notifications so a
// sustained outage doesn't spam the OS notification center.
let tray = null;
let lastTrayFailures = 0;   // for change-detection on consecutive_failures
let lastNotifyAt    = 0;    // debounce timestamp
const TRAY_POLL_MS  = 30_000;
const NOTIFY_DEBOUNCE_MS = 5 * 60 * 1000;

function trayIconPath() {
  // Reuse the existing assets baked for the BrowserWindow icon. On
  // Windows the .ico is multi-resolution so it scales to tray size.
  // On macOS the .icns includes all the sizes the menu bar wants.
  return path.join(__dirname, 'assets',
    process.platform === 'win32'  ? 'symbion.ico' :
    process.platform === 'darwin' ? 'symbion.icns' :
                                    'symbion-512.png');
}

// Provider switcher (tray submenu). Each entry rewrites cfg.llm_provider
// in symbion.json and restarts the Python backend in place. The four
// supported providers correspond to the four responder/judge pairings
// documented in CLAUDE.md ("Provider conventions").
const SUPPORTED_PROVIDERS = [
  { id: 'anthropic', label: 'Anthropic — Sonnet 4.6 + Haiku 4.5' },
  { id: 'groq',      label: 'Groq — Llama 3.3 70b + 3.1 8b' },
  { id: 'kimi',      label: 'Moonshot — K2.6 + v1-8k' },
  { id: 'ollama',    label: 'Ollama — Qwen 2.5 14b + 3b (local)' },
];

function readActiveProvider() {
  // Re-read symbion.json each call so external edits (or our own write
  // from switchProvider) are reflected without restarting Electron.
  try {
    const fresh = loadConfig();
    return String(fresh.llm_provider || 'anthropic').toLowerCase();
  } catch (e) {
    return 'anthropic';
  }
}

function buildProviderSubmenu() {
  const active = readActiveProvider();
  return SUPPORTED_PROVIDERS.map((p) => ({
    label:   p.label,
    type:    'radio',
    checked: p.id === active,
    click:   () => { switchProvider(p.id); },
  }));
}

async function switchProvider(newProvider) {
  const current = readActiveProvider();
  if (newProvider === current) return;
  if (!REPO_ROOT) {
    dialog.showErrorBox('Cannot switch provider',
      'No Symbion repo found — symbion.json location is unknown.');
    return;
  }

  // Confirm the switch — restart drops any in-flight turn and the new
  // provider may need an API key the user hasn't set in .env yet.
  const confirm = await dialog.showMessageBox(mainWindow || null, {
    type: 'question',
    title: 'Switch LLM provider',
    message: `Switch from "${current}" to "${newProvider}"?`,
    detail:
      `Symbion will set llm_provider="${newProvider}" in symbion.json and ` +
      `restart the Python backend (~5s). Any in-flight turn will be dropped.\n\n` +
      `If the new provider needs an API key, make sure it's set in .env first.`,
    buttons: ['Switch + restart', 'Cancel'],
    defaultId: 0,
    cancelId:  1,
  });
  if (confirm.response !== 0) {
    // User backed out — refresh the menu so the radio dot snaps back to
    // the still-current provider (Electron's radio menus check eagerly).
    if (tray) tray.setContextMenu(buildTrayMenu(null));
    return;
  }

  // Rewrite symbion.json. Preserve every other key (we just patch the
  // single field) and pretty-print so the file stays human-editable.
  const cfgPath = path.join(REPO_ROOT, 'symbion.json');
  let obj;
  try {
    const raw = fs.readFileSync(cfgPath, 'utf8');
    obj = JSON.parse(raw);
    obj.llm_provider = newProvider;
    fs.writeFileSync(cfgPath, JSON.stringify(obj, null, 2) + '\n');
  } catch (e) {
    dialog.showErrorBox('Failed to write symbion.json', String(e && e.message || e));
    return;
  }

  // Fallback chain prompt. When the user changes primary, the existing
  // fallback_chain may no longer make sense — e.g. switching FROM anthropic
  // TO groq with chain=["anthropic"] is fine, but chain=["groq"] when groq
  // is the primary is nonsensical (would try to fall back to itself). Offer
  // three resolutions; default is "fall back to previous provider" since
  // that's the most common intent ("I want to try X, but if it dies, give
  // me back the old one").
  const existingChain = Array.isArray(obj.fallback_chain) ? obj.fallback_chain : [];
  const chainLabel = existingChain.length ? existingChain.join(' → ') : '(empty — no fallback)';
  const fb = await dialog.showMessageBox(mainWindow || null, {
    type: 'question',
    title: 'Update fallback chain?',
    message: `Primary is now "${newProvider}". Update fallback_chain?`,
    detail:
      `Current chain: ${chainLabel}\n\n` +
      `When the primary trips its circuit breaker (529 / timeout), Symbion ` +
      `walks fallback_chain looking for a healthy provider. Empty chain = ` +
      `return the heuristic stub on failure (no auto-failover).`,
    buttons: [
      `Fall back to "${current}"`,
      'Leave chain unchanged',
      'Clear chain (no fallback)',
    ],
    defaultId: 0,
    cancelId:  1,
  });
  try {
    if (fb.response === 0) {
      obj.fallback_chain = [current];
      fs.writeFileSync(cfgPath, JSON.stringify(obj, null, 2) + '\n');
    } else if (fb.response === 2) {
      obj.fallback_chain = [];
      fs.writeFileSync(cfgPath, JSON.stringify(obj, null, 2) + '\n');
    }
    // response === 1 leaves the chain as it was; no rewrite needed.
  } catch (e) {
    // Chain update failed but the primary switch already landed on disk.
    // Surface the error but don't abort the restart — the primary change
    // is still valid even if the chain stayed stale.
    dialog.showErrorBox('Failed to update fallback_chain',
      `Primary set to "${newProvider}" but fallback_chain update failed:\n${String(e && e.message || e)}\n\n` +
      `Edit symbion.json by hand if needed.`);
  }

  // Keep the module-level cfg snapshot in sync so analytics window key
  // resolution and similar reads see the new value without re-launching.
  Object.assign(cfg, loadConfig());

  // Attach mode (existing backend detected at startup) — we don't own the
  // Python process, so we can't restart it. The file write succeeded;
  // tell the user to bounce it manually.
  if (!backend) {
    if (tray) tray.setContextMenu(buildTrayMenu(null));
    dialog.showMessageBox(mainWindow || null, {
      type: 'info',
      title: 'Provider written',
      message: `llm_provider set to "${newProvider}".`,
      detail:
        `Symbion's backend was launched outside this app (attach mode), ` +
        `so Electron can't restart it. Stop the running backend manually ` +
        `and relaunch for the change to take effect.`,
    });
    return;
  }

  const ok = await restartBackend();
  if (tray) tray.setContextMenu(buildTrayMenu(null));
  if (ok && mainWindow && !mainWindow.isDestroyed()) {
    // Reload the chat window so the WebSocket reconnects to the new
    // backend — stale sockets from the old process would otherwise sit
    // disconnected until the user manually refreshes.
    mainWindow.webContents.reload();
  }
}

function buildTrayMenu(health) {
  // Right-click menu. Status item is disabled (display-only); the action
  // items open the main window, the analytics page, or quit the app.
  const status = health
    ? `Mood: ${health.mood || '?'} | turns: ${health.interactions ?? '?'} | fails: ${health.consecutive_failures ?? '?'}`
    : 'Backend unreachable';
  return Menu.buildFromTemplate([
    { label: status, enabled: false },
    { type: 'separator' },
    { label: 'Open Symbion',  click: () => { if (mainWindow) { mainWindow.show(); mainWindow.focus(); } else { createWindow(); } } },
    { label: 'Open analytics', click: () => openAnalyticsWindow() },
    { type: 'separator' },
    { label: 'LLM provider', submenu: buildProviderSubmenu() },
    { type: 'separator' },
    { label: 'Quit Symbion',  click: () => { app.isQuitting = true; app.quit(); } },
  ]);
}

function updateTrayFromHealth(health) {
  if (!tray) return;
  if (!health) {
    tray.setToolTip('Symbion — backend unreachable');
    tray.setContextMenu(buildTrayMenu(null));
    return;
  }
  const mood = health.mood || '?';
  const turns = health.interactions ?? 0;
  const fails = health.consecutive_failures ?? 0;
  tray.setToolTip(`Symbion — ${mood} | ${turns} turns | ${fails} failures`);
  tray.setContextMenu(buildTrayMenu(health));

  // Notify on transition into failing state. Debounced — a sustained
  // outage shouldn't fire repeatedly. The in-process Slack watcher
  // already handles breaker-trip notifications; this is the local-OS
  // counterpart for when you're not watching Slack.
  const now = Date.now();
  if (fails > 0 && lastTrayFailures === 0 && (now - lastNotifyAt) > NOTIFY_DEBOUNCE_MS) {
    try {
      new Notification({
        title: 'Symbion — failures detected',
        body: `${fails} consecutive failure${fails === 1 ? '' : 's'}. Last error visible in the analytics view.`,
        urgency: 'normal',
      }).show();
      lastNotifyAt = now;
    } catch (e) {
      console.warn('[symbion] tray notification failed:', e);
    }
  }
  lastTrayFailures = fails;
}

// Open the /analytics route in a managed BrowserWindow. The route is
// gated by X-API-Key auth (same as /api/chat), and a plain
// shell.openExternal() opens the default browser which doesn't send
// the header — that's the "invalid api key" symptom users hit when
// clicking 'Open analytics' from the tray menu. Inject the key via
// webRequest.onBeforeSendHeaders so the URL stays clean (no key in
// query params, no key in browser history, no key in server access
// logs). Reads symbion.json directly each call so a rotated key
// takes effect without restarting Electron.
let analyticsWindow = null;

function openAnalyticsWindow() {
  // Focus the existing window if it's already open instead of stacking.
  if (analyticsWindow && !analyticsWindow.isDestroyed()) {
    if (analyticsWindow.isMinimized()) analyticsWindow.restore();
    analyticsWindow.focus();
    return;
  }
  // Re-read symbion.json (and .env fallback) per click so key rotations
  // don't require an Electron restart. cfg is module-level and stale
  // after the user rotates a key in symbion.json.
  let apiKey = '';
  try {
    const freshCfg = loadConfig();
    apiKey = (freshCfg.api_key || '').trim();
  } catch (e) {}
  if (!apiKey) {
    // Fallback: pull from .env (SYMBION_API_KEY=...)
    try {
      const envPath = path.join(REPO_ROOT || '', '.env');
      if (envPath && fs.existsSync(envPath)) {
        const raw = fs.readFileSync(envPath, 'utf8');
        const m = raw.match(/^SYMBION_API_KEY\s*=\s*(.+?)\s*$/m);
        if (m) apiKey = m[1].replace(/^["']|["']$/g, '').trim();
      }
    } catch (e) {}
  }

  analyticsWindow = new BrowserWindow({
    width: 1024,
    height: 720,
    minWidth: 600,
    minHeight: 400,
    backgroundColor: '#0c0a08',
    title: 'Symbion analytics',
    icon: path.join(__dirname, 'assets',
      process.platform === 'win32' ? 'symbion.ico' :
      process.platform === 'darwin' ? 'symbion.icns' : 'symbion-512.png'),
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
    },
    autoHideMenuBar: true,
  });

  // Inject X-API-Key on every request from this window's session. Scope
  // to JUST this window's session (defaultSession would affect every
  // BrowserWindow) — analytics auth shouldn't leak into the main chat
  // window's network requests.
  if (apiKey) {
    analyticsWindow.webContents.session.webRequest.onBeforeSendHeaders(
      { urls: [`${UI_URL}*`] },
      (details, callback) => {
        details.requestHeaders['X-API-Key'] = apiKey;
        callback({ requestHeaders: details.requestHeaders });
      });
  } else {
    // No key resolvable — show a clear in-page error rather than the
    // server's 401 page (which is opaque about why).
    analyticsWindow.loadURL(`data:text/html;charset=utf-8,${encodeURIComponent(
      `<html><body style="background:#0c0a08;color:#eee;font-family:sans-serif;padding:2rem">
       <h1 style="color:#ffd089">Symbion analytics — no API key</h1>
       <p>Couldn't resolve SYMBION_API_KEY from symbion.json or .env.</p>
       <p>Add <code>api_key</code> to symbion.json or <code>SYMBION_API_KEY=...</code> to .env, then restart Symbion.</p>
       </body></html>`)}`);
    analyticsWindow.on('closed', () => { analyticsWindow = null; });
    return;
  }

  analyticsWindow.loadURL(`${UI_URL}analytics?suggest=1`);
  analyticsWindow.on('closed', () => { analyticsWindow = null; });
}

function startTray() {
  if (process.platform === 'linux' && !process.env.DISPLAY) {
    console.log('[symbion] no DISPLAY — skipping tray');
    return;
  }
  try {
    const iconPath = trayIconPath();
    // On Windows .ico already scales for tray. nativeImage handles
    // platform-specific resampling.
    const icon = nativeImage.createFromPath(iconPath);
    tray = new Tray(icon);
    tray.setToolTip('Symbion — connecting…');
    tray.setContextMenu(buildTrayMenu(null));

    // Initial fetch + recurring poll. Fail silently — tray going stale
    // is better than the app crashing on a transient network error.
    const poll = async () => {
      try { updateTrayFromHealth(await fetchHealthJson()); }
      catch (e) { console.warn('[symbion] tray poll error:', e); }
    };
    poll();
    setInterval(poll, TRAY_POLL_MS);

    // Left-click on the tray icon focuses (or opens) the main window —
    // standard macOS behavior; harmless on Windows where right-click is
    // the menu and left-click triggers this.
    tray.on('click', () => {
      if (mainWindow) { mainWindow.show(); mainWindow.focus(); }
      else { createWindow(); }
    });
  } catch (e) {
    console.warn('[symbion] tray setup failed:', e);
  }
}

// Probe Ollama's /api/tags to see (a) whether Ollama is running, (b) which
// models are pulled. Resolves to { ok, models[] } or { ok: false }. Short
// timeout because this is a non-blocking startup check; if Ollama is slow
// we'd rather skip the check than hold up the window.
function probeOllamaTags(host, timeoutMs = 1500) {
  return new Promise((resolve) => {
    let urlBase;
    try { urlBase = new URL(host); }
    catch (e) { return resolve({ ok: false }); }
    const tagsUrl = `${urlBase.protocol}//${urlBase.host}/api/tags`;
    const req = http.get(tagsUrl, (res) => {
      if (res.statusCode !== 200) {
        res.resume();
        return resolve({ ok: false });
      }
      let buf = '';
      res.setEncoding('utf8');
      res.on('data', (chunk) => { buf += chunk; });
      res.on('end', () => {
        try {
          const data = JSON.parse(buf);
          const models = (data.models || []).map((m) => String(m.name || ''));
          resolve({ ok: true, models });
        } catch (e) {
          resolve({ ok: false });
        }
      });
    });
    req.on('error', () => resolve({ ok: false }));
    req.setTimeout(timeoutMs, () => { req.destroy(); resolve({ ok: false }); });
  });
}

// Async first-run check: surface a one-time dialog if Ollama is missing or
// the embedding model isn't pulled. Non-blocking — dialog fires after the
// main window is up so it doesn't gate startup. Skips entirely when
// embeddings are disabled in symbion.json (the user has opted out of
// semantic retrieval and doesn't need Ollama for that path).
//
// Why a dialog instead of auto-pulling: mxbai-embed-large is ~670 MB.
// Triggering a download of that size without user consent is bad UX,
// especially on metered connections. install-ollama.ps1 is the canonical
// installer and already handles idempotent pulls — point users there
// rather than duplicating its logic in Electron.
// Persistence for the "Don't ask again" choice on the Ollama dialog.
// A marker file in app.getPath('userData') suppresses re-prompting on
// every launch. Two markers — one per dialog flavor — so dismissing
// the 'model missing' nag doesn't also suppress the more meaningful
// 'Ollama not detected' one (different situations, different decisions).
// User can re-enable both prompts by deleting these files:
//   %APPDATA%\symbion-app\ollama-model-missing-dismissed
//   %APPDATA%\symbion-app\ollama-not-detected-dismissed
function ollamaDismissedPath(kind) {
  return path.join(app.getPath('userData'),
                    `ollama-${kind}-dismissed`);
}

function isOllamaDismissed(kind) {
  try { return fs.existsSync(ollamaDismissedPath(kind)); }
  catch (e) { return false; }
}

function setOllamaDismissed(kind) {
  try {
    fs.writeFileSync(ollamaDismissedPath(kind),
      `dismissed at ${new Date().toISOString()}\n` +
      `delete this file to re-enable the prompt.\n`);
  } catch (e) {
    console.warn(`[symbion] failed to persist ollama dismissal: ${e}`);
  }
}

async function maybeShowOllamaDialog(parentWindow) {
  // Honor user opt-out from symbion.json.
  if (cfg.embedding_enabled === false) return;
  // Default ollama host comes from SymbionConfig (http://localhost:11434).
  const ollamaHost = cfg.ollama_host || 'http://localhost:11434';
  const embedModel = cfg.embedding_model || 'mxbai-embed-large';

  const probe = await probeOllamaTags(ollamaHost);
  if (probe.ok) {
    // Ollama reachable. Match by base name — Ollama tags include `:latest`
    // (e.g. "mxbai-embed-large:latest") so a substring match is the right
    // semantics for "is this model pulled".
    const hasModel = probe.models.some((n) => n.startsWith(embedModel));
    if (hasModel) return;
    if (isOllamaDismissed('model-missing')) return;
    const choice = await dialog.showMessageBox(parentWindow, {
      type: 'info',
      title: 'Symbion: embedding model missing',
      message: `Ollama is running but the "${embedModel}" model isn't pulled.`,
      detail:
        `Symbion uses ${embedModel} for semantic memory retrieval (~670 MB ` +
        `download). Without it, retrieval falls back to BM25 (keyword-only) — ` +
        `Symbion will still work, just less effective at recalling paraphrased ` +
        `past context.\n\n` +
        `Run install-ollama.ps1 or "ollama pull ${embedModel}" to enable it.`,
      buttons: ['OK', 'Open install script'],
      defaultId: 0,
      cancelId: 0,
      checkboxLabel: "Don't ask again",
      checkboxChecked: false,
    });
    if (choice.checkboxChecked) setOllamaDismissed('model-missing');
    if (choice.response === 1) openInstallOllamaScript();
    return;
  }
  // Ollama not reachable.
  if (isOllamaDismissed('not-detected')) return;
  const choice = await dialog.showMessageBox(parentWindow, {
    type: 'info',
    title: 'Symbion: Ollama not detected',
    message: `Ollama isn't running at ${ollamaHost}.`,
    detail:
      `Symbion works fine with cloud LLM providers (Anthropic, OpenAI) — ` +
      `you don't need Ollama for chat. But local LLM mode and semantic ` +
      `memory retrieval require Ollama.\n\n` +
      `Run install-ollama.ps1 in PowerShell to install Ollama + pull the ` +
      `models Symbion uses.`,
    buttons: ['OK', 'Open install script'],
    defaultId: 0,
    cancelId: 0,
    checkboxLabel: "Don't ask again",
    checkboxChecked: false,
  });
  if (choice.checkboxChecked) setOllamaDismissed('not-detected');
  if (choice.response === 1) openInstallOllamaScript();
}

// Spawn install-ollama.ps1 in a visible PowerShell window. Uses
// ExecutionPolicy Bypass so the script runs even when the user's
// machine-wide policy is Restricted (the default on locked-down Windows).
// Detached so closing the PowerShell window doesn't kill Symbion.
function openInstallOllamaScript() {
  if (!REPO_ROOT) {
    dialog.showErrorBox(
      'Install script not found',
      'Could not locate the Symbion repo. Run install.ps1 first to set up ' +
      'the repo at %USERPROFILE%\\symbion, then re-launch Symbion.');
    return;
  }
  const script = path.join(REPO_ROOT, 'scripts', 'install-ollama.ps1');
  if (!fs.existsSync(script)) {
    dialog.showErrorBox(
      'Install script not found',
      `Expected install-ollama.ps1 at:\n${script}\n\n` +
      `The Symbion repo may be partially installed — re-run install.ps1 ` +
      `or clone a fresh copy.`);
    return;
  }
  if (process.platform === 'win32') {
    try {
      const child = spawn(
        'powershell.exe',
        ['-NoExit', '-ExecutionPolicy', 'Bypass', '-File', script],
        { detached: true, stdio: 'ignore' });
      child.unref();
    } catch (e) {
      dialog.showErrorBox('Failed to launch installer',
        `Could not start PowerShell:\n${e.message}\n\n` +
        `Open a PowerShell window manually and run:\n  ${script}`);
    }
  } else {
    // Non-Windows: install-ollama.ps1 is Windows-specific. Surface the path
    // so the user can run the equivalent steps manually.
    dialog.showMessageBox(mainWindow || null, {
      type: 'info',
      title: 'Manual install required',
      message: 'install-ollama.ps1 is Windows-only.',
      detail:
        `On macOS/Linux, install Ollama from https://ollama.com and run:\n` +
        `  ollama pull ${cfg.embedding_model || 'mxbai-embed-large'}\n` +
        `  ollama pull mistral\n` +
        `  ollama pull llama3.2`,
    });
  }
}

// ---- App lifecycle ----
app.whenReady().then(async () => {
  buildMenu();
  // Attach-vs-spawn: if a Symbion is already serving on PORT (user has
  // `python -m symbion --web` running in a terminal, or this app was
  // relaunched after an unclean exit left the previous backend alive),
  // skip spawning a duplicate. The duplicate would fail to bind and
  // the user would see a confusing "backend not ready" dialog.
  const existing = await probeHealth();
  if (existing) {
    console.log('[symbion] existing backend detected — attaching');
    createWindow();
    if (cfg.electron_tray_enabled !== false) startTray();
    maybeShowOllamaDialog(mainWindow).catch((e) => {
      console.warn('[symbion] Ollama check failed:', e);
    });
    return;
  }
  // No backend running and no repo found — we can't spawn. Surface a
  // clear error instead of trying to spawn `python` and letting it fail
  // silently. Common cause: user installed the desktop app but skipped
  // install.ps1 so there's no Symbion repo at %USERPROFILE%\symbion.
  if (!REPO_ROOT) {
    createWindow();
    dialog.showErrorBox(
      'Symbion repo not found',
      'The Symbion desktop app needs the Symbion Python repo on the same machine to spawn its own backend.\n\n' +
      'Tried these locations:\n' +
      '  $env:SYMBION_REPO\n' +
      '  ' + path.resolve(__dirname, '..') + '\n' +
      '  %USERPROFILE%\\symbion\n' +
      '  %USERPROFILE%\\SourceCode\\symbion\n\n' +
      'Either install Symbion via install.ps1 (default lands at %USERPROFILE%\\symbion) or set the SYMBION_REPO env var to your existing repo, then relaunch.');
    return;
  }
  startBackend();
  const ready = await waitForBackend();
  createWindow();
  // Tray widget — opt-out via cfg.electron_tray_enabled=false in
  // symbion.json. Default-on because the Electron app itself is opt-in;
  // anyone running the desktop shell probably wants the menu bar widget.
  if (cfg.electron_tray_enabled !== false) {
    startTray();
  }
  if (!ready.ok) {
    // Window opens to localhost:8000 anyway; on connection-refused the
    // user sees Chromium's error page. Surface the actual cause in a
    // dialog so they know it's the backend.
    dialog.showErrorBox(
      'Symbion backend not ready',
      `Couldn't reach ${HEALTH_URL} (${ready.reason}).\n\n` +
      `Possible causes: port ${PORT} already in use by another process; Python deps missing;\n` +
      `bootstrap script never ran. Check the console output in View > Toggle DevTools.`);
    return;
  }
  // Backend is up. Run the Ollama / embedding-model check non-blockingly:
  // we don't await here because the result drives a dialog the user can
  // dismiss; the main window is fully usable while it's open. Errors in
  // the check itself swallow silently — better to skip the dialog than
  // to block on a flaky probe.
  maybeShowOllamaDialog(mainWindow).catch((e) => {
    console.warn('[symbion] Ollama check failed:', e);
  });
});

app.on('window-all-closed', () => {
  // When the tray widget is active, the app outlives the main window —
  // user can close the chat window but keep the tray's "Open Symbion"
  // shortcut, the health tooltip, and the failure-transition
  // notifications. That's the whole point of a tray app. The user
  // quits explicitly via the tray menu's Quit item.
  //
  // When the tray is disabled (cfg.electron_tray_enabled === false),
  // fall back to the original behavior: closing the only window quits
  // the app (otherwise the Electron process would leak with no way for
  // the user to bring it back).
  //
  // macOS convention is also to keep the app alive after window close;
  // honored here regardless of the tray flag.
  if (tray || process.platform === 'darwin') {
    return;
  }
  app.quit();
});

app.on('activate', () => {
  // macOS dock click re-opens the window if it was closed.
  if (BrowserWindow.getAllWindows().length === 0 && !backendDied) {
    createWindow();
  }
});

app.on('before-quit', () => {
  app.isQuitting = true;
  // Tear down the tray icon so it doesn't leave a phantom in the menu
  // bar after quit. Tray.destroy() is the canonical Electron disposal.
  if (tray) {
    try { tray.destroy(); } catch (e) {}
    tray = null;
  }
  if (!backend || backend.killed) return;
  console.log('[symbion] terminating backend...');
  if (process.platform === 'win32') {
    // SIGTERM doesn't propagate to grandchildren on Windows. taskkill /T
    // walks the process tree; /F forces, /PID targets a specific root.
    // We deliberately spawn taskkill async (no await) — by the time it
    // runs, Electron is already on its way out and we don't want to
    // block quit on it.
    try {
      execFile('taskkill', ['/PID', String(backend.pid), '/T', '/F']);
    } catch (e) { console.warn('[symbion] taskkill failed', e); }
  } else {
    try { backend.kill('SIGTERM'); } catch (e) {}
  }
});
