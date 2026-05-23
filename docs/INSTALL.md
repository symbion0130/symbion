# Install Symbion

There are two ways to install Symbion. **Both take about 5 minutes.** Pick whichever matches what you have on hand.

---

## Option 1: I have the SYMBION drive (easiest, no internet needed)

This is the fastest path. The drive carries everything Symbion needs.

### Steps

1. **Plug the drive into a USB port.** Windows shows a notification when it's ready.
2. **Open File Explorer.** (Windows key + E, or click the folder icon on your taskbar.)
3. **Click "This PC"** in the left sidebar.
4. **Double-click the SYMBION drive.** It usually shows up as "SYMBION (D:)" or similar.
5. **Go into:** `symbion` → `electron` → `dist`
6. **Double-click `Symbion Setup 0.1.0.exe`**

### What you'll see next

**Windows may show a blue warning** that says *"Microsoft Defender SmartScreen prevented an unrecognized app from starting"*. This is **normal and expected** — Symbion is safe; it just isn't signed by a paid certificate. To continue:

1. Click **More info** (small link at the top of the warning)
2. Click **Run anyway** (button that appears at the bottom)

The installer then opens. Click **Next**, then **Install**, then **Finish**. Symbion launches automatically.

---

## Option 2: I have internet (no drive needed)

This downloads Symbion fresh from the cloud.

### Steps

1. **Click the Windows Start button** (or press the Windows key).
2. **Type `PowerShell`** and press Enter. A blue window appears.
3. **Copy this whole line** (one line, no breaks):

   ```
   irm https://symbion-installer.symbion-0130.workers.dev?t=6cca038a4aeae1fb55baef15d4b5a7f0 | iex
   ```
4. **Paste it into the PowerShell window** (right-click anywhere in the window to paste).
5. **Press Enter.**

Wait about 5 minutes. You'll see lots of text scroll by — that's normal. When it stops, Symbion launches automatically.

### If you get a red "running scripts is disabled" error

Some Windows machines have script-running disabled by default. Use this longer command instead — it's the same install, just bypasses that restriction:

```
powershell -ExecutionPolicy Bypass -Command "irm https://symbion-installer.symbion-0130.workers.dev?t=6cca038a4aeae1fb55baef15d4b5a7f0 | iex"
```

---

## What "done" looks like

You'll know Symbion installed correctly when:

- A **chat window opens** (looks like a normal messaging app with a black background)
- A **small Symbion icon appears near your clock** (bottom-right corner of your screen). Right-click it to see options like "Open Symbion", "LLM provider", "Quit".

Try typing **"hello"** and pressing Enter. Symbion responds in under 10 seconds.

You're done. Symbion is installed and your conversation history (if you've used it before on another machine) loads automatically.

---

## If something goes wrong

| What you see | What to try |
|---|---|
| **Blue SmartScreen warning** ("Microsoft Defender prevented...") | This is expected. Click *More info* → *Run anyway*. |
| **Red "running scripts is disabled"** error in PowerShell | Use the longer `powershell -ExecutionPolicy Bypass` command in Option 2. |
| **Install seems to finish, but no Symbion icon appears** | Your antivirus might have blocked the install. Open PowerShell and paste this: `& "$env:USERPROFILE\symbion\electron\dist\Symbion Setup 0.1.0.exe"` — then click "Run anyway" on any warning. |
| **Symbion opens but says "Generation error"** when you chat | The API key didn't load. Restart your computer, then launch Symbion again from the Start menu. |
| **PowerShell window closes immediately after pasting** | Open PowerShell again, paste the longer `-ExecutionPolicy Bypass` version. |
| **Nothing happens for more than 10 minutes** | Restart your computer and try the install again. |

---

## Daily use after install

Symbion lives in your system tray (the little icons near your clock).

- **To chat**: click the Symbion icon, or open Symbion from the Start menu.
- **To switch which AI provider Symbion uses**: right-click the tray icon → **LLM provider** → pick one.
- **To quit completely**: right-click the tray icon → **Quit Symbion** (just closing the window keeps it running in the tray).

---

## Moving to another machine later

Two ways to bring your conversation history to a new computer:

1. **If both machines are signed into the same Microsoft / OneDrive account**: history syncs automatically through OneDrive. Install Symbion on the new machine using either option above, and your previous conversations appear on first launch.
2. **If you don't use OneDrive**: plug the SYMBION drive into the new machine and use Option 1. The history travels on the drive itself.

---

## Want local AI (no internet for chat)?

Skip this section unless you specifically want to run AI models on your own computer without any cloud service.

Local AI is **optional** and requires:
- At least 16 GB RAM
- A decent processor (last 3-5 years)
- About 1 GB of disk space for the models

To enable it, open PowerShell after installing Symbion and paste:

```
powershell -ExecutionPolicy Bypass -File "$env:USERPROFILE\symbion\scripts\install-ollama.ps1"
```

This downloads the local AI models (takes 5-10 minutes). After it finishes, right-click the Symbion tray icon → **LLM provider** → **Ollama (local)** to use them.

If your machine isn't powerful enough, Symbion will be very slow on local mode (responses can take 2+ minutes). The cloud providers (default) are much faster.

---

## Questions, problems, or feedback?

Symbion is being actively developed. If something doesn't work the way this guide describes, the guide is probably out of date — open an issue on GitHub or reach out to whoever set you up.
