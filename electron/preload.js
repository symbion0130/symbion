// Preload bridge between Electron main and the Symbion web UI renderer.
//
// Currently a no-op — the renderer is the existing HTML/JS UI that talks
// to the FastAPI backend over HTTP/WebSocket and doesn't need anything
// from Electron's APIs. Kept as a stub so future native integrations
// (system tray notifications, native file picker for attachments,
// auto-start at login) have a place to land without changing the main
// process's webPreferences.
//
// contextBridge + contextIsolation:true in main.js means anything we
// expose here lives on a SEPARATE world from the page's own scripts,
// so the symbion web UI can't reach into Node APIs even if compromised.

const { contextBridge } = require('electron');

contextBridge.exposeInMainWorld('symbion', {
  platform: process.platform,
  versions: process.versions,
});
