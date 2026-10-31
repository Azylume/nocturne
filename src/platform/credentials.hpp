#pragma once

#include <optional>
#include <string>

namespace azl {

// A secret the user stored for this account: a generic password in the login Keychain on Apple (read with the security
// tool, which the user's own `security add-generic-password` item trusts), a generic credential named `service` with
// the user name `account` in the Windows Credential Manager (`cmdkey /generic:SERVICE /user:ACCOUNT /pass`). Nothing
// when there is none. Blocks for a short system call; any thread but the render thread.
std::optional<std::string> readCredential(const std::string& service, const std::string& account);

} // namespace azl
