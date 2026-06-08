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
    std::string name;          // base name, shared across array elements
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
