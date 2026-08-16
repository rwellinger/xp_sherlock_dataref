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

#include "correlator.hpp"
#include "dataref_index.hpp"
#include "types.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xp_sherlock
{

enum class Phase : std::uint8_t
{
    Idle,
    Baseline,
    Record,
    Inspect,
    // Additive noise capture: like Baseline, but instead of resetting the
    // ignore-set it GROWS it. The user performs actions they want excluded
    // (e.g. powering a bus that cascades into many refs) and every ref that
    // moves is added to the ignore-set, so the subsequent Record subtracts
    // that whole cascade. Target-blind: it only needs you to drive the noise.
    NoiseCapture,
    // Short measurement window after a single deliberate action (fire a command
    // or write a DataRef). Records which refs moved as a result, so two actions
    // can be compared by the size of the cascade they trigger. See probe_*().
    Probe,
};

namespace recorder
{

void init();
void stop();

Phase phase();

// Start Phase 1. Asks dataref_index to (re)build if needed, resets ignore_set.
//
// Default raised from 2.5 s: aircraft that chatter on a slow cycle (the AW139
// re-fires sim/autopilot/disconnect every few seconds) can sit out a short
// window entirely and escape the noise filter. On a badly behaved aircraft,
// push this to 10-15 s.
void start_baseline(float baseline_seconds = 5.0f);

// Begin/end additive noise capture (the "Mark as noise" subtract step). While
// active, any ref that moves relative to the baseline is added to the
// ignore-set; it does NOT reset the existing ignore-set, so multiple captures
// accumulate. Open-ended: runs until stop_noise_capture(). No-op if called from
// Baseline/Record. Seeds the baseline arrays if no snapshot was taken yet.
void start_noise_capture();
void stop_noise_capture();

// Begin Phase 2. Allocates ring buffers + EventStreams sized to
// `watched_refs = all - ignore_set`. Returns false (and stays in Idle) if
// the watched-ref count exceeds the safety cap.
//
// Only the switch KIND is passed in. How many times the user actuates is read
// from the anchors they place during the run, so there is no count to configure
// up front and none to get wrong.
bool start_record(bool expect_bidirectional);

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

// Command→DataRef couplings found in the last Record window, tightest first.
// Empty until stop_record() has run. See correlator.hpp for what a link means.
const std::vector<CommandRefLink> &command_ref_links();

// Was this logical ref excluded as baseline noise? A target ref that twitched
// during the baseline is dropped from the whole recording and would otherwise
// be untraceable — the user sees neither the ref nor a reason for its absence.
bool is_ignored_as_noise(std::size_t logical_idx);

// ── Cascade probe ────────────────────────────────────────────────────────────
//
// Answers "should I send the command or write the DataRef?" by measurement
// rather than heuristics.
//
// Firing the command runs the aircraft's own logic, so the whole cascade
// follows: relays, buses, annunciators. Writing the status DataRef directly may
// only move the switch graphic while the systems behind it never notice. Run
// one probe per action and compare how many refs each one actually moved — if
// the command moves substantially more, the DataRef is only the tip and you
// must send the command.
//
// A probe snapshots every watched ref, performs the action, samples for
// PROBE_WINDOW_S, then reports what changed. Requires a completed Record (the
// watched set comes from it).

enum class ProbeAction : std::uint8_t
{
    FireCommand,
    WriteDataRef,
};

struct ProbeResult
{
    bool                     valid        = false;
    ProbeAction              action       = ProbeAction::FireCommand;
    int                      refs_moved   = 0;
    int                      refs_sampled = 0;
    std::string              action_label; // what was fired/written, for the UI
    std::vector<std::size_t> moved_refs;   // logical indices, for the diff view
};

// Start a probe on the given candidate. Returns false if no Record has run, a
// probe/record is already active, or the candidate cannot be actioned (e.g. a
// read-only DataRef). `value` is used only for ProbeAction::WriteDataRef.
bool probe_start(std::size_t candidate_idx, ProbeAction action, SampleValue value);
bool probe_in_progress();

// Results of the two most recent probes, kept separately so they can be shown
// side by side. Invalid until the corresponding probe has completed.
const ProbeResult &probe_command_result();
const ProbeResult &probe_dataref_result();
void               probe_clear();

// Status info for the UI status bar.
struct Status
{
    Phase phase                = Phase::Idle;
    float baseline_elapsed_s   = 0.f;
    float baseline_total_s     = 0.f;
    float record_elapsed_s     = 0.f;
    int   ignored_count        = 0;
    int   watched_count        = 0;
    int   candidate_count      = 0;
    int   total_logical        = 0;
    bool  baseline_in_progress = false;
    bool  auto_stop_armed      = false;
    int   best_events_so_far   = 0; // largest event-count across all streams during Record
    int   anchors_set          = 0; // how many "I Acted Now" stamps the user has placed
    int   muted_command_count  = 0; // commands muted as baseline noise
    // Seconds until auto-stop fires, or -1 when it is not armed (no anchors
    // yet, auto-stop disabled, or still inside the minimum record window).
    // Surfaced so the user can see they have time for another cycle instead of
    // being stopped without warning.
    float auto_stop_in_s = -1.f;
};
Status status();

// Test-mode write helper. Returns the written-back read value via `readback`.
// `ok_write` is false if writability check failed.
bool test_write(std::size_t candidate_idx, SampleValue v, bool &writable, SampleValue &readback);

} // namespace recorder
} // namespace xp_sherlock
