#pragma once

namespace azl {

// The commit this binary was built from as "YYYYMMDDhhmmss-hash" (commit time, UTC), so a later build compares
// greater.
const char* buildId();

// The project version and the commit the build came from, as "0.1.1 (abc1234)": "-dirty" after the commit when
// tracked files differed from it at configure time, "unknown" in place of it without git.
const char* versionLabel();

} // namespace azl
