#pragma once

// Windows only. Every way the process ends other than main returning leaves lines on stderr, with the stack of the
// thread it happened on as module+offset: a fatal exception (when it is raised, so also one a library handles or one
// whose unhandled-exception filter a library replaced), an unhandled exception, std::terminate, abort, an invalid CRT
// parameter, a pure virtual call, exit() and ExitProcess called from anywhere. Where a module's PDB lies next to it, the
// stack follows again with function names and lines. A TerminateProcess from outside leaves nothing; only the exit
// code the parent sees tells. With NOCTURNE_CRASH_DUMP_DIR set, an unhandled fatal exception, or an exit() a filter
// makes after one (a test runner's), also writes a minidump there.
namespace azl {

// Once, first thing on the main thread.
void traceExits();
// Main is returning: the exit that follows is the expected one and leaves no trace.
void exitingNormally();

} // namespace azl
