#pragma once

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unistd.h>

namespace rocos {

// Used only by startup/reporting code. Redirected logs and NO_COLOR stay plain.
inline bool consoleColorEnabled(const std::ostream &output) noexcept {
    const char *term = std::getenv("TERM");
    if (std::getenv("NO_COLOR") || (term && std::strcmp(term, "dumb") == 0)) return false;
    if (&output == &std::cout) return isatty(STDOUT_FILENO) == 1;
    if (&output == &std::cerr) return isatty(STDERR_FILENO) == 1;
    return false;
}

struct ConsoleStyle {
    bool enabled{false};
    const char *heading() const noexcept { return enabled ? "\033[1;36m" : ""; }
    const char *ok() const noexcept { return enabled ? "\033[1;32m" : ""; }
    const char *warning() const noexcept { return enabled ? "\033[1;33m" : ""; }
    const char *error() const noexcept { return enabled ? "\033[1;31m" : ""; }
    const char *reset() const noexcept { return enabled ? "\033[0m" : ""; }
};

} // namespace rocos
