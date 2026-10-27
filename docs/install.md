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

Opening Nocturne starts its engine in the background; quitting Nocturne (Cmd-Q) stops it. The engine listens on
`127.0.0.1` only, so nothing on your network can reach it.

If Nocturne ever quits or crashes without stopping the engine, the next launch simply reuses the one still running —
you do not get a second. To stop a leftover engine by hand, run:

```sh
pkill nocturne-supervisor
```

The supervisor stops the engine when it goes, so that one command stops both.

## Uninstall

Quit Nocturne (and `pkill nocturne-supervisor` if anything is still running), then drag `Nocturne.app` to the Trash.
The preview app does not install a login item or any background service of its own; it runs the engine only while the
app is open.

## Why signed builds are not here yet

Signing and notarisation need an Apple Developer ID and Apple's notary service. That work is tracked in
[#32](https://github.com/Azylume/nocturne/issues/32). Until then this preview is the way to try Nocturne without
building it from source.
