#include "engine/bundle_update.hpp"

namespace azl {

namespace fs = std::filesystem;

namespace {

constexpr const char* kNoSignedBuilds =
    "Update is not available on Windows yet: there are no signed Windows builds to check a pushed one against. "
    "Install a new build by hand";

} // namespace

std::optional<std::string> signerMissing(const fs::path&)
{
    return kNoSignedBuilds;
}

std::optional<std::string> signerRefusal(const fs::path&, const fs::path&)
{
    return kNoSignedBuilds;
}

UpdateSource installedUpdate(const fs::path&)
{
    UpdateSource source;
    source.install = [](const fs::path&) -> std::optional<std::string> { return kNoSignedBuilds; };
    source.refused = kNoSignedBuilds;
    return source;
}

} // namespace azl
