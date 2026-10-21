# C++20 and Metal for the engine

The engine is written in C++20 and renders with Metal through Apple's metal-cpp headers. Apple-only APIs (VideoToolbox, CoreAudio, windows, display links) sit in small Objective-C++ files.

- **Rust** was rejected as hard to read for the owner.
- **Swift** was rejected as unfamiliar and unnecessary.
- **openFrameworks/OpenGL** was rejected because OpenGL on macOS is deprecated, capped at 4.1, and presents single-threaded.

openFrameworks stays a playground for experiments (lasers, calibration).
