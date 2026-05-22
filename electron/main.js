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

const { app, BrowserWindow, shell, Menu, dialog } = require('electron');
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
  app.quit();
} else {
  app.on('second-instance', () => {
    if (mainWindow) {
      if (mainWindow.isMinimized()) mainWindow.restore();
      mainWindow.focus();
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
      // Suppress when we triggered the kill ourselves (app quit) — that
      // path sets `backend = null` BEFORE this handler can fire because
      // we kill synchronously.
      if (!app.isQuitting) {
        dialog.showErrorBox(
          'Symbion backend stopped',
          `The Symbion Python process exited (code ${code}).\n\n` +
          `Check the console log for the cause. You can restart by closing and reopening the app.`);
      }
    }
  });
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
    });
    if (choice.response === 1) openInstallOllamaScript();
    return;
  }
  // Ollama not reachable.
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
  });
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
  // Symbion is a single-window app; closing the last window quits.
  // The 'before-quit' handler below kills the backend.
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
