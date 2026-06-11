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

// Three-phase state machine, flight-loop sampling, change-event production,
// and ring-buffer storage. Owns the connection between the SDK (via
// dataref_index) and the pure correlator.
//
// States:  Idle → Baseline → Record → Inspect → Idle
//
// Phase 1 (Baseline, ~2.5 s): sample every ref each frame; any ref that
//   moves goes to the ignore set (ambient noise).
// Phase 2 (Record): sample only non-ignored refs; feed ChangeDetector FSMs;
//   confirmed events flow into per-ref EventStreams + ring buffers.
// Phase 3 (Inspect): correlator.rank() has produced Candidates; the UI shows
//   the ranked list and optionally lets the user write to a selected ref.

#pragma once

#include "dataref_index.hpp"
#include "types.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace xp_sherlock
{

enum class Phase : std::uint8_t
{
    Idle,
    Baseline,
    Record,
    Inspect,
};

namespace recorder
{

void init();
void stop();

Phase phase();

// Start Phase 1. Asks dataref_index to (re)build if needed, resets ignore_set.
void start_baseline(float baseline_seconds = 2.5f);

// Begin Phase 2. Allocates ring buffers + EventStreams sized to
// `watched_refs = all - ignore_set`. Returns false (and stays in Idle) if
// the watched-ref count exceeds the safety cap.
bool start_record(bool expect_bidirectional, int expected_clicks);

// Auto-stop preference (default: enabled). When disabled, Record runs until
// the user clicks Stop — useful on large displays where mouse travel between
// cockpit switch and the "I Acted Now" button is long enough that an
// over-eager auto-stop would cut you off mid-sequence.
void set_auto_stop_enabled(bool on);
bool auto_stop_enabled();

// User signals "I just performed the action". Latency is measured from
// these anchor timestamps.
void mark_user_action();

// User-driven stop.
void stop_record();

void reset();

// Inspect-mode accessors.
const std::vector<Candidate> &candidates();
const LogicalRef             *logical_ref_at(std::size_t logical_idx);

// Status info for the UI status bar.
struct Status
{
    Phase phase                 = Phase::Idle;
    float baseline_elapsed_s    = 0.f;
    float baseline_total_s      = 0.f;
    float record_elapsed_s      = 0.f;
    int   ignored_count         = 0;
    int   watched_count         = 0;
    int   candidate_count       = 0;
    int   total_logical         = 0;
    bool  baseline_in_progress  = false;
    bool  auto_stop_armed       = false;
    int   best_events_so_far    = 0; // largest event-count across all streams during Record
    int   anchors_set           = 0; // how many "I Acted Now" stamps the user has placed
};
Status status();

// Test-mode write helper. Returns the written-back read value via `readback`.
// `ok_write` is false if writability check failed.
bool test_write(std::size_t candidate_idx, SampleValue v, bool &writable, SampleValue &readback);

} // namespace recorder
} // namespace xp_sherlock
