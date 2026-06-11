/*
 * xp_sherlock_dataref - X-Plane 12 plugin for behavioural
 *   DataRef correlation (DataRef Detective)
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

// X-Plane Command enumeration and handle cache. Mirrors dataref_index in
// shape (rebuild / all / set_user_exclusions / code_snippet) but uses the
// XPLM command API.
//
// IMPORTANT XPLM limitation: there is no XPLMCountCommands / xx_GetByIndex —
// commands cannot be enumerated by the SDK. We therefore read X-Plane's
// installed `Resources/plugins/Commands.txt` at runtime (via XPLMGetSystemPath)
// and call XPLMFindCommand for every entry. If Commands.txt is missing or
// unreadable we fall back to a small embedded list of well-known sim/* commands
// so the feature isn't silently dead.

#pragma once

#include <XPLM/XPLMUtilities.h>
#include <cstddef>
#include <string>
#include <vector>

namespace xp_sherlock
{

struct CommandEntry
{
    XPLMCommandRef handle = nullptr;
    std::string    name;        // e.g. "sim/electrical/battery_1_on"
    std::string    description; // human-readable, from Commands.txt column 2+
};

namespace command_index
{

// (Re)build the index. Idempotent. Logs counts via XPLMDebugString. Honours
// `set_user_exclusions()` and the hardcoded sim/private/ filter.
void rebuild();

bool                            is_built();
const std::vector<CommandEntry> &all();
std::size_t                      size();

// Apply the same UI exclusion-prefix list that drives dataref_index — so the
// snapshot filter checkboxes affect both indexes consistently.
void                            set_user_exclusions(std::vector<std::string> prefixes);

// Paste-ready C snippet for a single command. Used by UI's "Copy code snippet".
std::string                     code_snippet(const CommandEntry &cmd);

} // namespace command_index
} // namespace xp_sherlock
