#pragma once

namespace azl {

// The commit this binary was built from as "YYYYMMDDhhmmss-hash" (commit time, UTC), so a later build compares
// greater.
const char* buildId();

} // namespace azl
