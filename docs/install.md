# Installing the unsigned preview

Nocturne ships an early preview as one `Nocturne.app`: the UI and the engine live in the same bundle, so opening the
app starts the engine in the background and plays a show; quitting it stops the engine again. You do not install or
run anything else.

This preview has **no Apple Developer ID signature** yet. It is ad-hoc signed, which is only enough for Apple silicon
to run the code at all; it is **not notarised**, so macOS Gatekeeper cannot verify the developer and refuses a plain
double-click. The app is safe — it is just not signed by Apple yet. A signed, notarised build comes later
([#32](https://github.com/Azylume/nocturne/issues/32)).

## 1. Download and verify

1. Download `Nocturne-<version>-unsigned.dmg` and its `.sha256` file from the
   [latest release](https://github.com/Azylume/nocturne/releases).
2. Check the download matches the published checksum:

   ```sh
   cd ~/Downloads
   shasum -a 256 -c Nocturne-<version>-unsigned.dmg.sha256
   ```

   It should print `Nocturne-<version>-unsigned.dmg: OK`. If it does not, delete the file and download it again.

3. Open the DMG and drag **Nocturne.app** onto the **Applications** folder shown in the window.

## 2. Open it the first time

macOS blocks the first launch because the app is not notarised. How you get past it once depends on your macOS
version. After the first successful open, it launches normally.

### macOS 15 (Sequoia) and newer

1. Double-click **Nocturne**. macOS shows *"Apple could not verify 'Nocturne' is free of malware ..."*. Click
   **Done** (there is no "Open" button here on macOS 15).
2. Open  > **System Settings** > **Privacy & Security**.
3. Scroll to the **Security** section. You will see *"Nocturne was blocked to protect your Mac."* Click **Open
   Anyway**.
4. Authenticate (Touch ID or password), then click **Open Anyway** again in the confirmation dialog.

### macOS 14 (Sonoma)

1. In **Applications**, **right-click** (or Control-click) **Nocturne** and choose **Open**.
2. In the *"macOS cannot verify the developer"* dialog, click **Open**.

### Terminal alternative

If you would rather not click through the dialogs, remove the download's quarantine flag and then open the app
normally:

```sh
xattr -dr com.apple.quarantine /Applications/Nocturne.app
```

macOS attaches a `com.apple.quarantine` attribute to anything downloaded from a browser; that attribute is what makes
Gatekeeper block an un-notarised app. Removing it tells macOS you vouch for this file, so the app opens without the
"cannot verify" prompt. Only do this for a download you have checked against its checksum.

## 3. First-launch prompts

The first time Nocturne runs it asks for two macOS permissions:

- **Local Network** — the UI talks to its own engine over the loopback interface.
- **Audio / Microphone** — the engine analyses audio so the visuals react to the music. You can decline; the show
  still runs, just without audio-reactive modulation.

## Stopping the engine

Opening Nocturne starts its engine in the background; quitting Nocturne stops it. Quitting any way works — a clean
Quit (Cmd-Q), a crash, or even Force Quit all stop the engine, because its life is tied to the app's. The engine
listens on `127.0.0.1` only, so nothing on your network can reach it.

If an engine is ever left running (for example after a hard power loss), the next launch of Nocturne simply reuses it —
you do not get a second. To stop a leftover engine by hand, run:

```sh
pkill nocturne
```

## Uninstall

Quit Nocturne (and `pkill nocturne` if anything is still running), then drag `Nocturne.app` to the Trash.
The preview app does not install a login item or any background service of its own; it runs the engine only while the
app is open.

## Why signed builds are not here yet

Signing and notarisation need an Apple Developer ID and Apple's notary service. That work is tracked in
[#32](https://github.com/Azylume/nocturne/issues/32). Until then this preview is the way to try Nocturne without
building it from source.

## Windows (preview, port in progress)

The Windows port is still in progress ([#35](https://github.com/Azylume/nocturne/issues/35)). Its installer,
`Nocturne-<version>-windows-x64-unsigned.exe`, comes from the `nocturne-windows-installer` artifact of a CI run or
from `tools\make-installer.ps1` (see `docs/reference.md`); it is not on the releases page yet. It needs Windows 10 version
2004 or later, 64-bit, and admin rights.

### Verify and run the installer

Check the file against the checksum you were given, in PowerShell:

```powershell
Get-FileHash .\Nocturne-<version>-windows-x64-unsigned.exe -Algorithm SHA256
```

The installer and the programs are **not code signed** yet, so Microsoft Defender SmartScreen shows *"Windows
protected your PC"* the first time. Click **More info**, check the file name, then **Run anyway**. Windows then asks
for admin rights (User Account Control): the installer writes to Program Files and adds firewall rules.

### What it installs

- Everything in one folder, `C:\Program Files\Nocturne`: the UI (`Nocturne UI.exe`), the engine supervisor, the
  venue desk, the FFmpeg DLLs and the Visual C++ runtime; the engine (`nocturne.exe`) and `nocturne-ctl.exe` join them
  once they build on Windows. Start menu: **Nocturne** opens the UI.
- Two inbound Windows Firewall rules, "Nocturne engine" and "Nocturne desk", so other machines (the Director, a phone
  on the desk page at port 7480) reach the engine without a firewall prompt. They apply on **Private** networks only:
  Windows marks a new network Public, so set the show network to Private, in **Settings > Network & internet > Wi-Fi**
  (or **Ethernet**) **> the network > Network profile type > Private network**, or as admin in PowerShell:
  `Set-NetConnectionProfile -InterfaceAlias "Wi-Fi" -NetworkCategory Private` (`Get-NetConnectionProfile` lists the
  names).
- Optional, unticked by default: **Start the engine under its supervisor at every logon**, for a show machine. It
  adds the scheduled task `Nocturne`; the supervisor then runs after each logon, with no window. To change it later,
  as admin:

  ```powershell
  powershell -ExecutionPolicy Bypass -File "C:\Program Files\Nocturne\autostart.ps1" install
  powershell -ExecutionPolicy Bypass -File "C:\Program Files\Nocturne\autostart.ps1" uninstall
  ```

Nocturne keeps its own files (state, logs, the pairing token) in `%LOCALAPPDATA%\Nocturne`.

### Uninstall

**Settings > Apps > Installed apps > Nocturne > Uninstall** (or **Uninstall Nocturne** in the Start menu). It stops
the autostart task and the UI, removes the program folder, the firewall rules and the task, and keeps
`%LOCALAPPDATA%\Nocturne`; delete that folder by hand to remove everything.
