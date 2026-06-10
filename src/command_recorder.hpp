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
// shift when the index is rebuilt.

#pragma once

#include "types.hpp"
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

const std::vector<CommandEventStream> &streams();

// Issue a test fire from the UI test panel. mode: 0=Once, 1=Begin, 2=End.
// Returns false if the index is out of range or the command has no handle.
bool test_fire(std::size_t command_idx, int mode);

} // namespace command_recorder
} // namespace xp_sherlock
