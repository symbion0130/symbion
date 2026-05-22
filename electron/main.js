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

// Resolve the repo root (one level above this electron/ directory). Used
// to find the portable Python interpreter, the symbion package, and the
// symbion.json config.
const REPO_ROOT = path.resolve(__dirname, '..');

// Config — load symbion.json so we use the same web_port the user has
// configured. Falls back to 8000 (SymbionConfig default).
function loadConfig() {
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

// Pick the Python interpreter. Prefer the portable copy that ships with
// Symbion (scripts/bootstrap-portable.bat puts it at .python/python.exe);
// fall back to `python` on PATH.
function resolvePython() {
  const portable = path.join(REPO_ROOT, '.python', 'python.exe');
  if (fs.existsSync(portable)) return portable;
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
  }
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
