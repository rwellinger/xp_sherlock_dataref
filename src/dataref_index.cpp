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

#include "dataref_index.hpp"
#include <XPLM/XPLMUtilities.h>
#include <cstdio>
#include <cstring>

namespace xp_sherlock
{

namespace
{

std::vector<LogicalRef>  s_index;
bool                     s_built = false;
std::vector<std::string> s_user_exclusions;

// Pick a single canonical type for a multi-typed ref. Some refs report
// Int|Float|Double; we pick the most-specific that we support, in this
// priority: Int > Float > Double. Array types are handled separately by the
// caller (after a scalar path is also considered).
RefType pick_scalar_type(XPLMDataTypeID types)
{
    if (types & xplmType_Int)
        return RefType::Int;
    if (types & xplmType_Float)
        return RefType::Float;
    if (types & xplmType_Double)
        return RefType::Double;
    return RefType::Int; // shouldn't happen if caller checked
}

void log_line(const char *fmt, int a = 0, int b = 0, int c = 0)
{
    char buf[256];
    snprintf(buf, sizeof(buf), fmt, a, b, c);
    XPLMDebugString(buf);
}

} // namespace

namespace dataref_index
{

void rebuild()
{
    s_index.clear();
    s_built = false;

    const int total = XPLMCountDataRefs();
    if (total <= 0)
    {
        XPLMDebugString("[xp_sherlock] XPLMCountDataRefs returned 0 — no refs to enumerate.\n");
        s_built = true;
        return;
    }

    std::vector<XPLMDataRef> handles(static_cast<std::size_t>(total));
    XPLMGetDataRefsByIndex(0, total, handles.data());

    s_index.reserve(static_cast<std::size_t>(total));

    int skipped_data          = 0;
    int skipped_unknown       = 0;
    int skipped_private       = 0;
    int skipped_user_excluded = 0;
    int multi_typed           = 0;
    int expanded_arrays       = 0;
    int total_logical         = 0;

    char name_buf[1024];

    // Namespace prefixes that are always noise for switch-finding purposes.
    // `sim/private/...` exposes engine-internal statistics (renderer
    // counters, mouse-tri counts, frame timings, etc.) — they tick every
    // frame, drown out real switches, and never correspond to cockpit
    // controls. Filtering them at enum time keeps the watched-ref count and
    // the baseline noise floor manageable.
    static constexpr const char *PRIVATE_PREFIX     = "sim/private/";
    static constexpr std::size_t PRIVATE_PREFIX_LEN = 12; // strlen("sim/private/")

    for (XPLMDataRef h : handles)
    {
        if (!h)
            continue;

        XPLMDataRefInfo_t info{};
        info.structSize = sizeof(info);
        XPLMGetDataRefInfo(h, &info);

        XPLMDataTypeID types     = info.type;
        const char    *raw_name  = (info.name != nullptr) ? info.name : "";
        bool           writable  = XPLMCanWriteDataRef(h) != 0;

        // Defensively copy the name — SDK does not document ownership of the
        // pointer in XPLMDataRefInfo_t.name. One small allocation per ref at
        // enum time is fine.
        std::strncpy(name_buf, raw_name, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
        std::string name = name_buf;
        if (name.empty())
            continue;

        // Drop engine-internal stats / renderer counters.
        if (name.size() >= PRIVATE_PREFIX_LEN &&
            std::strncmp(name.c_str(), PRIVATE_PREFIX, PRIVATE_PREFIX_LEN) == 0)
        {
            ++skipped_private;
            continue;
        }

        // User-configured prefix exclusions (Snapshot filter checkboxes).
        // Same shape as the sim/private/ check above — substring match anchored
        // at position 0. Kept as a separate pass so the hardcoded filter and
        // the user filter are independently observable in the log banner.
        {
            bool user_drop = false;
            for (const auto &px : s_user_exclusions)
            {
                if (!px.empty() && name.size() >= px.size() &&
                    std::strncmp(name.c_str(), px.c_str(), px.size()) == 0)
                {
                    user_drop = true;
                    break;
                }
            }
            if (user_drop)
            {
                ++skipped_user_excluded;
                continue;
            }
        }

        // Skip byte/string refs in v1 (logged once at the end).
        bool has_data    = (types & xplmType_Data) != 0;
        bool has_scalar  = (types & (xplmType_Int | xplmType_Float | xplmType_Double)) != 0;
        bool has_iarray  = (types & xplmType_IntArray) != 0;
        bool has_farray  = (types & xplmType_FloatArray) != 0;
        bool has_any_we_support = has_scalar || has_iarray || has_farray;

        if (!has_any_we_support)
        {
            if (has_data)
                ++skipped_data;
            else
                ++skipped_unknown;
            continue;
        }

        int populated_types = (has_scalar ? 1 : 0) + (has_iarray ? 1 : 0) + (has_farray ? 1 : 0);
        if (populated_types > 1)
            ++multi_typed;

        // Priority: scalar > IntArray > FloatArray. The "canonical type"
        // dictates how this ref will be sampled. We do NOT emit duplicate
        // logical entries for multi-typed refs — pick one and stick with it,
        // because reading a ref under the wrong type doesn't help and just
        // doubles the noise.
        if (has_scalar)
        {
            LogicalRef lr;
            lr.handle       = h;
            lr.type         = pick_scalar_type(types);
            lr.array_index  = -1;
            lr.is_writable  = writable;
            lr.name         = name;
            lr.display_path = name;
            s_index.push_back(std::move(lr));
            ++total_logical;
            continue;
        }

        // Array refs — query length and expand to per-index logical entries.
        int len = 0;
        if (has_iarray)
            len = XPLMGetDatavi(h, nullptr, 0, 0);
        else // has_farray
            len = XPLMGetDatavf(h, nullptr, 0, 0);

        if (len <= 0 || len > 4096) // hard cap — datarefs with insane lengths get skipped
            continue;

        ++expanded_arrays;
        RefType elem_type = has_iarray ? RefType::IntArrayElem : RefType::FloatArrayElem;
        for (int idx = 0; idx < len; ++idx)
        {
            LogicalRef lr;
            lr.handle      = h;
            lr.type        = elem_type;
            lr.array_index = idx;
            lr.is_writable = writable;
            lr.name        = name;
            char dp[1100];
            snprintf(dp, sizeof(dp), "%s[%d]", name.c_str(), idx);
            lr.display_path = dp;
            s_index.push_back(std::move(lr));
            ++total_logical;
        }
    }

    char banner[256];
    snprintf(banner, sizeof(banner),
             "[xp_sherlock] Enumerated %d datarefs → %d logical (arrays expanded: %d, multi-typed: %d)\n",
             total, total_logical, expanded_arrays, multi_typed);
    XPLMDebugString(banner);

    if (skipped_data || skipped_unknown || skipped_private || skipped_user_excluded)
    {
        char skip_msg[320];
        snprintf(skip_msg, sizeof(skip_msg),
                 "[xp_sherlock] Skipped %d sim/private/* (engine internals), %d byte/string refs (xplmType_Data), "
                 "%d unknown-type refs, %d user-excluded (snapshot filter)\n",
                 skipped_private, skipped_data, skipped_unknown, skipped_user_excluded);
        XPLMDebugString(skip_msg);
    }

    XPLMDebugString("[xp_sherlock] Note: dataref enumeration may miss plugin-registered refs added lazily after this "
                    "point. Use Re-enumerate after aircraft swap.\n");

    s_built = true;
    (void)log_line; // currently unused, kept for future per-step tracing
}

bool is_built() { return s_built; }

const std::vector<LogicalRef> &all() { return s_index; }

void set_user_exclusions(std::vector<std::string> prefixes)
{
    s_user_exclusions = std::move(prefixes);
}

const std::vector<std::string> &user_exclusions() { return s_user_exclusions; }

bool read(const LogicalRef &lr, SampleValue &out)
{
    if (!lr.handle)
        return false;
    switch (lr.type)
    {
    case RefType::Int:
        out.i = XPLMGetDatai(lr.handle);
        return true;
    case RefType::Float:
        out.f = XPLMGetDataf(lr.handle);
        return true;
    case RefType::Double:
        out.d = XPLMGetDatad(lr.handle);
        return true;
    case RefType::IntArrayElem:
    {
        int   v   = 0;
        int   got = XPLMGetDatavi(lr.handle, &v, lr.array_index, 1);
        out.i     = (got == 1) ? v : 0;
        return got == 1;
    }
    case RefType::FloatArrayElem:
    {
        float v   = 0.f;
        int   got = XPLMGetDatavf(lr.handle, &v, lr.array_index, 1);
        out.f     = (got == 1) ? v : 0.f;
        return got == 1;
    }
    }
    return false;
}

bool write(const LogicalRef &lr, SampleValue v)
{
    if (!lr.handle || !lr.is_writable)
        return false;
    switch (lr.type)
    {
    case RefType::Int:
        XPLMSetDatai(lr.handle, v.i);
        return true;
    case RefType::Float:
        XPLMSetDataf(lr.handle, v.f);
        return true;
    case RefType::Double:
        XPLMSetDatad(lr.handle, v.d);
        return true;
    case RefType::IntArrayElem:
        XPLMSetDatavi(lr.handle, &v.i, lr.array_index, 1);
        return true;
    case RefType::FloatArrayElem:
        XPLMSetDatavf(lr.handle, &v.f, lr.array_index, 1);
        return true;
    }
    return false;
}

std::string code_snippet(const LogicalRef &lr)
{
    char buf[1400];
    switch (lr.type)
    {
    case RefType::Int:
        snprintf(buf, sizeof(buf),
                 "XPLMDataRef dr = XPLMFindDataRef(\"%s\");\nint v = XPLMGetDatai(dr);", lr.name.c_str());
        break;
    case RefType::Float:
        snprintf(buf, sizeof(buf),
                 "XPLMDataRef dr = XPLMFindDataRef(\"%s\");\nfloat v = XPLMGetDataf(dr);", lr.name.c_str());
        break;
    case RefType::Double:
        snprintf(buf, sizeof(buf),
                 "XPLMDataRef dr = XPLMFindDataRef(\"%s\");\ndouble v = XPLMGetDatad(dr);", lr.name.c_str());
        break;
    case RefType::IntArrayElem:
        snprintf(buf, sizeof(buf),
                 "XPLMDataRef dr = XPLMFindDataRef(\"%s\");\nint v = 0;\nXPLMGetDatavi(dr, &v, %d, 1);",
                 lr.name.c_str(), lr.array_index);
        break;
    case RefType::FloatArrayElem:
        snprintf(buf, sizeof(buf),
                 "XPLMDataRef dr = XPLMFindDataRef(\"%s\");\nfloat v = 0.f;\nXPLMGetDatavf(dr, &v, %d, 1);",
                 lr.name.c_str(), lr.array_index);
        break;
    }
    return buf;
}

} // namespace dataref_index
} // namespace xp_sherlock
