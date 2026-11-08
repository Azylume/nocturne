#pragma once

// `nocturne-ctl mcp ARGS`: a Model Context Protocol server on standard input and output (ADR 0018). `args` are the
// words after `mcp`. Returns the process exit code.
int runMcp(int argc, char** argv);
