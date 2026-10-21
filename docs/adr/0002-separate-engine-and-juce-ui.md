# Engine and UI are separate processes; the UI is JUCE

The engine runs without any UI. The JUCE-based UI is a separate process that only sends commands and shows state and previews.

- **Why separate:** a UI crash must never stop the show, which was a real pain with Resolume. The same headless engine binary also serves as a render node on other machines.
- **Why JUCE:** it is what Resolume's own UI is built on, and it gives a dense custom-drawn interface in C++. React/web UI was explicitly rejected.
- **Reconsidered 2026-10-13, kept:** the working HTML prototype made a web UI tempting. JUCE stays because live previews share the engine's IOSurfaces with no copy, which a browser cannot do on the Mac mini without encoding streams, and an earlier web-shell (Tauri) app did not hold up. The HTML prototype remains the interaction reference; the JUCE UI reproduces it.
