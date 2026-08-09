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

// DataRef enumeration, handle cache, type info, array expansion, and
// type-erased read/write. This is the only module that talks to the XPLM
// data-access API.

#pragma once

#include "types.hpp"
#include <XPLM/XPLMDataAccess.h>
#include <cstddef>
#include <string>
#include <vector>

namespace xp_sherlock
{

struct LogicalRef
{
    XPLMDataRef handle      = nullptr;
    RefType     type        = RefType::Int;
    int         array_index = -1; // -1 for scalars
    bool        is_writable = false;
    std::string name;         // base name, shared across array elements
    std::string display_path; // name or name[i]
};

namespace dataref_index
{

// Build the index. Idempotent — subsequent calls re-enumerate (useful after
// aircraft swap, or to discover plugin-registered refs that came up late).
// Logs counts of enumerated, skipped (string/byte), and expanded (array elem)
// refs via XPLMDebugString.
void rebuild();

bool                           is_built();
const std::vector<LogicalRef> &all();

// User-configurable exclusion prefixes, applied at the next rebuild().
// Any DataRef whose name starts with one of these strings is dropped during
// enumeration — same mechanism as the hardcoded sim/private/ filter, but
// driven by the UI. The hardcoded filter remains in force independently.
void                            set_user_exclusions(std::vector<std::string> prefixes);
const std::vector<std::string> &user_exclusions();

// Read a single logical ref's current value. Returns false on null handle.
bool read(const LogicalRef &lr, SampleValue &out);

// Typed write. Returns false if not writable (or handle is null). For arrays,
// writes only the indexed element.
bool write(const LogicalRef &lr, SampleValue v);

// Helper: produce a paste-ready C-snippet for a ref (raw path or
// XPLMGetDatav{i,f} call for array elements).
std::string code_snippet(const LogicalRef &lr);

} // namespace dataref_index

} // namespace xp_sherlock
