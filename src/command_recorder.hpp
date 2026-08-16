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

// Owns the lifecycle of XPLM command-handler hooks and the per-command
// fire-event log. Conceptually parallel to recorder.cpp's DataRef-sampling
// pipeline, but event-driven (commands fire callbacks) rather than poll-based.
//
// Lifecycle:
//   enable()       — registers a handler on every command in command_index.
//                    Call ONCE after command_index::rebuild() has populated
//                    the index. Handlers stay registered for the whole
//                    plugin session (always-on); the body short-circuits
//                    unless the recorder is in Record phase.
//   disable()      — symmetric counterpart for plugin shutdown / re-enumeration.
//   begin_record() — allocates per-command CommandEventStream buffers and
//                    arms the gate so subsequent fires get logged.
//   end_record()   — closes the gate; streams() remain accessible until reset().
//   reset()        — drops all event data and clears the streams.
//
// Re-enumeration during a session: caller MUST disable() before
// command_index::rebuild() and re-enable() afterwards, because handle indices
// shift when the index is rebuilt. Consequence: disable() drops the whole
// stream vector, so the baseline mute-set is lost across a re-enumeration.
// That is unavoidable — mutes are keyed by index, and every index moves — so
// a re-enumeration must be followed by a fresh baseline.

#pragma once

#include "types.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace xp_sherlock
{
namespace command_recorder
{

void enable();
void disable();

void begin_record(float record_start_t_sec, uint32_t frame_counter_at_start);
void end_record();
void reset();

bool is_enabled();

// Number of fires seen during the most recent baseline window — for diagnostics.
// (Commands should NOT fire during baseline in a stationary cockpit.)
int  baseline_fires_observed();
void clear_baseline_diagnostics();
// Set true while baseline is running so the handler counts fires even
// though it does NOT push them into streams (baseline is observation-only).
void set_baseline_phase(bool on);

// ── Baseline noise muting ────────────────────────────────────────────────────
//
// Any command that fires (Begin) while set_baseline_phase(true) is active gets
// muted: hidden from the main candidate list, but still fully recorded during
// Record. Some aircraft chatter constantly (the AW139 fires
// sim/autopilot/disconnect/... unprompted), which otherwise drowns the list.
//
// Muting never discards data — streams() keeps every event either way, so an
// unmute in the UI reveals the complete fire history retroactively.

// Master switch (UI, default true). Off restores pre-1.1.4 behaviour.
void set_mute_enabled(bool on);
bool mute_enabled();

// Manual override from the UI ("Unmute" / "Mute again"). Out-of-range indices
// are ignored.
void set_muted(std::size_t command_idx, bool on);
bool is_muted(std::size_t command_idx);
int  muted_count();

// Drop every mute. Baseline is a resetting phase and calls this; Learn Ambient
// is additive by design and deliberately does not.
void clear_mutes();

const std::vector<CommandEventStream> &streams();

// Issue a test fire from the UI test panel. mode: 0=Once, 1=Begin, 2=End.
// Returns false if the index is out of range or the command has no handle.
bool test_fire(std::size_t command_idx, int mode);

} // namespace command_recorder
} // namespace xp_sherlock
