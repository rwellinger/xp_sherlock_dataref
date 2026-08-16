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

// Pure, SDK-free change detection FSM.
//
// A raw sample delta only becomes a confirmed ChangeEvent if the new value
// holds within epsilon for HOLD_FRAMES consecutive frames. This filters out
// bouncing/drifting noise (engine vibration, voltage flicker, animations)
// that would otherwise masquerade as switch events.
//
// Used by recorder.cpp at runtime and by test_change_detector.cpp directly.

#pragma once

#include "types.hpp"
#include <cmath>
#include <cstdint>

namespace xp_sherlock
{

constexpr int    DEFAULT_HOLD_FRAMES = 4;
constexpr float  EPS_FLOAT           = 1e-3f;
constexpr double EPS_DOUBLE          = 1e-4;
constexpr int    EPS_INT             = 0;

// Compare two SampleValues using the type's epsilon. Returns true if the new
// value is "different enough" to potentially be a change event.
inline bool exceeds_epsilon(RefType type, SampleValue prev, SampleValue cur)
{
    switch (type)
    {
    case RefType::Int:
    case RefType::IntArrayElem:
        return prev.i != cur.i;
    case RefType::Float:
    case RefType::FloatArrayElem:
        return std::fabs(cur.f - prev.f) > EPS_FLOAT;
    case RefType::Double:
        return std::fabs(cur.d - prev.d) > EPS_DOUBLE;
    }
    return false;
}

inline bool within_epsilon(RefType type, SampleValue a, SampleValue b) { return !exceeds_epsilon(type, a, b); }

inline int8_t direction_of(RefType type, SampleValue from, SampleValue to)
{
    int dir = 0;
    switch (type)
    {
    case RefType::Int:
    case RefType::IntArrayElem:
        dir = (to.i > from.i) ? +1 : (to.i < from.i ? -1 : 0);
        break;
    case RefType::Float:
    case RefType::FloatArrayElem:
        dir = (to.f > from.f) ? +1 : (to.f < from.f ? -1 : 0);
        break;
    case RefType::Double:
        dir = (to.d > from.d) ? +1 : (to.d < from.d ? -1 : 0);
        break;
    }
    return static_cast<int8_t>(dir);
}

// Per-ref FSM state. Caller owns one instance per logical ref.
// States:
//   STABLE(prev)          → CANDIDATE(new, hold=N)  on |Δ|>eps
//   CANDIDATE(new, hold)  → STABLE(new) + emit      when |Δ|<=eps for N frames
//   CANDIDATE(new, hold)  → CANDIDATE(s, hold=N)    when value bounces
//                           (restart count without drifting the stable baseline)
struct ChangeDetector
{
    enum class Phase : uint8_t
    {
        Stable,
        Candidate
    };

    RefType     type;
    SampleValue stable_value;    // last confirmed-stable value
    SampleValue candidate_value; // currently held but not yet confirmed
    Phase       phase     = Phase::Stable;
    int         hold_left = 0;
    // Sim-time at which this ref first left its stable value. Captured on the
    // Stable→Candidate edge and carried through any bounces, so the emitted
    // event reports when the ref actually started moving rather than when the
    // hold counter happened to expire.
    float onset_t_sec = 0.f;

    void init(RefType t, SampleValue v)
    {
        type         = t;
        stable_value = v;
        phase        = Phase::Stable;
        hold_left    = 0;
        onset_t_sec  = 0.f;
    }

    // Feed one sample. If a change is confirmed, fills `out` and returns true.
    bool feed(SampleValue sample, uint32_t frame, float t_sec, int hold_frames, ChangeEvent &out)
    {
        if (phase == Phase::Stable)
        {
            if (exceeds_epsilon(type, stable_value, sample))
            {
                phase           = Phase::Candidate;
                candidate_value = sample;
                hold_left       = hold_frames - 1;
                onset_t_sec     = t_sec;
            }
            return false;
        }

        // phase == Candidate
        if (within_epsilon(type, candidate_value, sample))
        {
            if (--hold_left <= 0)
            {
                phase = Phase::Stable;
                // If the candidate settled back to the original stable value
                // (a noise bounce that swung away and returned), it's a
                // round-trip with no net change — do NOT emit a phantom
                // event. Without this guard, a noisy ref that touches a
                // different value briefly and returns would manufacture a
                // direction=0 event per swing.
                if (within_epsilon(type, stable_value, candidate_value))
                    return false;

                out.from        = stable_value;
                out.to          = candidate_value;
                out.frame       = frame;
                out.t_sec       = t_sec;
                out.onset_t_sec = onset_t_sec;
                out.direction   = direction_of(type, stable_value, candidate_value);
                stable_value    = candidate_value;
                return true;
            }
            return false;
        }

        // Value bounced again — restart candidate hold with the new value.
        // Crucially: DO NOT advance stable_value (avoids drift turning into a
        // sequence of phantom events).
        candidate_value = sample;
        hold_left       = hold_frames - 1;
        return false;
    }
};

} // namespace xp_sherlock
