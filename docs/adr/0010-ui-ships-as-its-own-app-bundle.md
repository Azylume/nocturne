# The UI ships as its own app bundle, signed like the engine

The JUCE UI is `Nocturne UI.app` (`com.azylume.nocturne.ui`), a second bundle next to `Nocturne.app`, signed with the same "Nocturne Dev" identity (`tools/sign-bundle.sh --ui`).

- **The show bundle stays headless and unchanged.** The Mini runs `Nocturne.app` from one path with one microphone grant. Putting the UI inside it would ship JUCE to every node and re-sign the engine's bundle on every UI change, which is the step that has cost permission grants before.
- **Each bundle has its own privacy grants.** The UI only needs Local Network, to reach engines on other machines; it never asks for the microphone. macOS keys the grant on the bundle id plus the certificate, so it survives rebuilds the same way the engine's does.
- **A crash stays in its own process and bundle** (ADR 0002). A helper executable inside `Nocturne.app` would share the engine's bundle id and Info.plist, so its Dock icon, menu bar name and TCC attribution would all be the engine's.
- **Considered:** a helper inside `Nocturne.app/Contents/MacOS`. It was rejected because a GUI app needs its own Info.plist, and for the reasons above.
