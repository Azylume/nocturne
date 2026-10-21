#pragma once

#include "protocol/messages.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace azl {

// Starts the installer on an unpacked Nocturne.app; the reason it could not, or none.
using InstallBundle = std::function<std::optional<std::string>(const std::filesystem::path& app)>;

// Where Update keeps a pushed bundle and how it installs it; unset (no `install`), Update is not served.
struct UpdateSource {
    std::filesystem::path incoming;
    InstallBundle install;
    // The installer's last result (tools/update-bundle.sh writes it).
    std::filesystem::path result;
    // Why Update is refused whatever the time, e.g. the running bundle names no signer to check a pushed one against.
    std::optional<std::string> refused;
};

// Why `running` gives Update no signer to check a pushed bundle against: it is not signed, or signed ad hoc. None when
// it is signed with an identity. Reads the signature without validating it, which would hash the whole bundle.
std::optional<std::string> signerMissing(const std::filesystem::path& running);
// Why `candidate` may not replace `running`: `running` names no signer, or `candidate` is not validly signed (nested
// code included) by one satisfying `running`'s designated requirement: its identifier and signing certificate.
std::optional<std::string> signerRefusal(const std::filesystem::path& running, const std::filesystem::path& candidate);

// Runs tools/update-bundle.sh (`script`) on the bundle with `environment` added, in its own session: outside the
// autostart job's process group, so it outlives this engine, which it stops.
InstallBundle bundleInstaller(std::filesystem::path script, std::map<std::string, std::string> environment);
// Update for the engine run from an installed bundle (VENUE/Nocturne.app/Contents/MacOS/nocturne): pushed bundles go to
// VENUE/incoming and the running bundle's own Contents/Resources/update-bundle.sh installs those signerRefusal lets
// through. Unset when the engine is not in an .app; refused when the bundle names no signer.
UpdateSource installedUpdate(const std::filesystem::path& state);

// Receives a new Nocturne.app a client pushes over the protocol (protocol/bundle_push.hpp) and installs it:
//
//   {"op": "update", "size", "build"} starts a transfer of a gzipped tar of Nocturne.app, replacing one in progress
//   {"op": "update", "offset", "data": <binary>} the next part, at most kBundleChunk bytes
//
// Each is answered {"op": "update", "state": "receiving", "received", "size"}. After the last part the archive is
// unpacked into `incoming`, the answer is {"op": "update", "state": "installing", "build"}, and the installer starts:
// it stops this engine, swaps the bundles, checks the new one and rolls back when it is not healthy. Errors are
// {"type": "error", "op": "update"}, and drop the transfer. Every message has "type": "machine".
class BundleUpdate {
public:
    explicit BundleUpdate(UpdateSource source);

    // `refusal` is asked at the start and before the install: why Update is refused now, or none.
    Message receive(const Message& message, const std::function<std::optional<std::string>()>& refusal);
    // The installer's last result as it wrote it, null before the first.
    Message lastResult() const;
    bool served() const { return bool(source_.install); }
    const std::optional<std::string>& refused() const { return source_.refused; }

private:
    Message start(const Message& message);
    Message part(const Message& message, const std::function<std::optional<std::string>()>& refusal);
    Message install(const std::function<std::optional<std::string>()>& refusal);
    void drop();

    UpdateSource source_;
    std::mutex mutex_;
    std::ofstream out_;
    uint64_t size_ = 0;
    uint64_t received_ = 0;
    std::string build_;
};

} // namespace azl
