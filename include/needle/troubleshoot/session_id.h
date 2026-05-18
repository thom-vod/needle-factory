#pragma once

#include <string>

namespace needle {

// Returns ISO8601-with-dashes UTC timestamp + "-" + 4-hex-digit random suffix.
// The random suffix lets two sessions started in the same second coexist
// without a busy-wait retry. Caller appends the "session-" prefix for the
// directory name.
std::string make_troubleshoot_session_id();

} // namespace needle
