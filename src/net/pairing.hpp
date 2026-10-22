#pragma once

#include <filesystem>
#include <string>

namespace azl {

// The pairing token: a random secret, one per machine, that a protocol client on another machine presents in its
// hello as "token". Clients on this machine (loopback) need none.

// ~/Library/Application Support/Nocturne/State, the supervisor's default --state.
std::filesystem::path defaultStateFolder();
// STATE/token
std::filesystem::path tokenFile(const std::filesystem::path& state);
// The token in `file`, made when missing: 32 random bytes as hex, readable by the owner alone. Throws when it can
// neither read nor make it.
std::string machineToken(const std::filesystem::path& file);
// The token in `file`; empty when there is none.
std::string readToken(const std::filesystem::path& file);
// Both are the same nonempty token; compared in a time that does not depend on where they differ.
bool sameToken(const std::string& presented, const std::string& token);
// Whether a node presents its main engine's token to `host`: only to a main it was told to follow (`named`), or on
// this machine (a loopback address or localhost). A main it found by itself over Bonjour may be anyone's.
bool presentsToken(bool named, const std::string& host);

} // namespace azl
