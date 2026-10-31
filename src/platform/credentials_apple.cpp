#include "platform/credentials.hpp"

#include "platform/process.hpp"

namespace azl {

std::optional<std::string> readCredential(const std::string& service, const std::string& account)
{
    const std::optional<PipeEnds> pipe = makePipe();
    if (!pipe)
        return std::nullopt;
    std::optional<Process> security =
        spawnProcess({.program = "/usr/bin/security",
                      .args = {"find-generic-password", "-s", service, "-a", account, "-w"},
                      .output = Stdio::to(pipe->write),
                      .error = Stdio::null()});
    closeHandle(pipe->write);
    std::string secret;
    char buffer[256];
    for (long n; security && (n = readHandle(pipe->read, buffer, sizeof buffer)) > 0;)
        secret.append(buffer, size_t(n));
    closeHandle(pipe->read);
    if (!security || security->wait() != 0)
        return std::nullopt;
    while (!secret.empty() && (secret.back() == '\n' || secret.back() == '\r'))
        secret.pop_back();
    return secret;
}

} // namespace azl
