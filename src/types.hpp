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

// Shared, SDK-free types used by recorder, correlator, change_detector, and
// the UI. Kept header-only so they can be linked into the SDK-free test binary
// without dragging in any XPLM symbols.

#pragma once

#include <cstdint>
#include <vector>

namespace xp_sherlock
{

enum class RefType : uint8_t
{
    Int,
    Float,
    Double,
    IntArrayElem,
    FloatArrayElem,
};

inline const char *type_name(RefType t)
{
    switch (t)
    {
    case RefType::Int:
        return "Int";
    case RefType::Float:
        return "Float";
    case RefType::Double:
        return "Double";
    case RefType::IntArrayElem:
        return "I[]";
    case RefType::FloatArrayElem:
        return "F[]";
    }
    return "?";
}

union SampleValue
{
    int32_t i;
    float   f;
    double  d;
};

struct Sample
{
    SampleValue v;
    uint32_t    frame;
    float       t_sec;
};

struct ChangeEvent
{
    SampleValue from;
    SampleValue to;
    uint32_t    frame;
    // Time the change was CONFIRMED, i.e. after the value held steady for
    // HOLD_FRAMES. This is the timestamp used for anchor correlation, where a
    // few frames of lag is irrelevant because it applies to every ref equally.
    float t_sec;
    // Time the value FIRST deviated, before the hold-confirmation delay. Needed
    // for command→DataRef causality: the gap between a command firing and the
    // ref reacting is only a few frames, so the ~HOLD_FRAMES confirmation lag
    // would swamp the very signal being measured.
    float  onset_t_sec;
    int8_t direction; // +1 / -1 / 0
};

struct EventStream
{
    uint32_t                 logical_ref_idx = 0;
    RefType                  type            = RefType::Int;
    std::vector<ChangeEvent> events;
    SampleValue              baseline_value{};
    SampleValue              current_value{};
};

// Kind discriminates DataRef candidates from Command candidates inside the
// shared `Candidate` vector. Both kinds rank against the same score axis so
// they interleave naturally in the UI table.
enum class Kind : uint8_t
{
    DataRef,
    Command,
};

inline const char *kind_name(Kind k) { return (k == Kind::Command) ? "Command" : "DataRef"; }

// A single command fire observed during the Record window. Phase encodes the
// XPLM command lifecycle (Begin=0, Continue=1, End=2) — we keep all three so
// the UI can show users the dominant phase.
struct CommandFireEvent
{
    uint32_t frame = 0;
    float    t_sec = 0.f;
    uint8_t  phase = 0;
};

struct CommandEventStream
{
    uint32_t                      command_idx = 0;
    std::vector<CommandFireEvent> events;
    uint8_t                       last_phase = 0xFF;
    // Begin-fires observed during Baseline / Learn Ambient. A command that
    // fires in a stationary cockpit is ambient noise by definition — some
    // aircraft (e.g. the AW139 with sim/autopilot/disconnect/...) chatter
    // continuously and would otherwise bury the real candidate.
    int baseline_begin_fires = 0;
    // Muted == hidden from the main candidate list. Deliberately NOT
    // "dropped": muted commands keep recording so nothing can be lost, and
    // rank_commands() can promote one back if it behaves purposefully.
    bool muted = false;
};

// Parallel-array companion to CommandEventStream, mirroring how RefMeta
// accompanies EventStream for DataRefs.
struct CommandRefMeta
{
    int path_length = 0; // for tie-breaking (shorter wins slightly)
};

struct Candidate
{
    Kind     kind                = Kind::DataRef;
    uint32_t logical_ref_idx     = 0;
    uint32_t command_idx         = 0; // valid iff kind == Command
    int      pos_count           = 0;
    int      neg_count           = 0;
    bool     bidirectional       = false;
    bool     monotonic_staircase = false;
    int      staircase_steps     = 0;
    float    min_latency_ms      = 0.f; // NaN-safe: 0 means "no anchor available"
    float    median_latency_ms   = 0.f;
    bool     has_latency         = false;
    bool     asymmetric_decay    = false;
    bool     is_writable         = false;
    int      total_events        = 0;
    float    score               = 0.f;
    // For UI display, copied at ranking time:
    RefType     type;
    SampleValue baseline_value{};
    SampleValue current_value{};
    // Min / max value observed across the whole record window. Important for
    // switches that return to their starting state (e.g. ON-OFF-ON ends at
    // the same value it began with, making baseline==current trivially equal
    // even though the ref clearly moved).
    SampleValue min_seen{};
    SampleValue max_seen{};
    // Command-only fields. Populated by rank_commands() for kind==Command rows.
    int     fire_count = 0;
    uint8_t last_phase = 0;
    // Baseline-noise muting. `is_muted` is the ranking-time verdict; the UI
    // additionally honours live user overrides. `auto_unmuted` marks a row the
    // heuristic pulled back out of the muted set, so the UI can explain why.
    bool is_muted       = false;
    bool auto_unmuted   = false;
    int  baseline_fires = 0;
    // Anchor coverage — how cleanly this candidate answered the user's "I Acted
    // Now" stamps. Set for both kinds. anchors_total == 0 means the user placed
    // no anchors, in which case coverage carries no information either way.
    int anchors_hit   = 0;
    int anchors_total = 0;
    int orphan_events = 0;
    // Causal ordering. `onset_lag_ms` is the median time from an actuation to
    // this ref's first movement; the ref that moves FIRST is the cause and
    // everything behind it is the cascade it drives. Only meaningful with
    // anchors, hence the explicit validity flag.
    float onset_lag_ms   = 0.f;
    bool  has_onset_lag  = false;
    bool  is_first_mover = false; // reacted earliest of all candidates
};

} // namespace xp_sherlock
