// Minimal cross-platform "copy to clipboard". Per-platform impl in clipboard.cpp.

#pragma once

#include <string>

namespace xp_sherlock
{
namespace clipboard
{

// Best-effort. Returns true if the platform-specific call succeeded.
// On failure, logs to XPLMDebugString but does not throw.
bool copy(const std::string &text);

} // namespace clipboard
} // namespace xp_sherlock
