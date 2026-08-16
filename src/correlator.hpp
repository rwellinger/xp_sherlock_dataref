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

// Pure ranking of behavioural-correlation candidates. ZERO SDK includes —
// fully unit-testable via Catch2 against synthetic EventStream inputs.

#pragma once

#include "types.hpp"
#include <functional>
#include <string>
#include <vector>

namespace xp_sherlock
{

struct Hints
{
    int   expected_clicks      = 0;     // 0 → unknown
    bool  expect_bidirectional = true;  // false for one-shot or rotary modes
    float user_click_window_ms = 100.f; // "low latency" threshold for cause-likely
};

struct RefMeta
{
    bool        is_writable = false;
    int         path_length = 0; // for tie-breaking (shorter wins slightly)
    RefType     type;
    SampleValue baseline_value{};
    SampleValue current_value{};
};

// Latency anchors: each entry is a sim-time (in seconds, relative to record
// start) at which the user clicked "I Acted Now".
using AnchorList = std::vector<float>;

// How well a candidate's activity lines up 1:1 with the user's anchors.
//
// This is the strongest signal the tool has, and it strengthens with every
// extra actuation: a real target produces exactly one response per anchor,
// while noise produces too few, too many, or responses nowhere near an anchor.
// A single flip cannot distinguish the two — five flips can.
struct AnchorCoverage
{
    int anchors_hit   = 0; // anchors answered by at least one event
    int anchors_total = 0;
    int orphan_events = 0; // events belonging to no anchor at all
};

// Each anchor owns the window [t_i, min(t_i+1, t_i + MAX_RESPONSE_S)); events
// before the first anchor or past a window's end are orphans. `event_times`
// must be sorted ascending. Shared by the DataRef and Command pipelines.
AnchorCoverage anchor_coverage(const std::vector<float> &event_times, const AnchorList &anchors);

// Median time from each anchor to the first reaction that followed it, in ms;
// negative when no anchor was answered.
//
// This is the cause-vs-effect signal for DataRefs that have no command behind
// them. Clicking a switch starts a chain: the switch's own state ref moves
// first, then whatever it drives — bus voltages, lamps, annunciators. All of
// them correlate with the click equally well, which is why they arrive as a
// cluster of similarly-scored candidates. Arrival ORDER is what separates them.
//
// Feed ONSET times (ChangeEvent::onset_t_sec), not confirmation times: the
// hold-frame delay is constant across refs and would erase the few-frame gaps
// this measurement depends on.
float median_onset_lag_ms(const std::vector<float> &onsets, const AnchorList &anchors);

// Pure analysis of a single stream. Exposed for tests + reuse by rank().
bool  is_bidirectional(const EventStream &s);
int   staircase_steps(const EventStream &s);
bool  asymmetric_decay(const EventStream &s);
float min_latency_ms(const EventStream &s, const AnchorList &anchors);
float median_latency_ms(const EventStream &s, const AnchorList &anchors);

// Build and score candidates. `metas` MUST be parallel to `streams`
// (metas[i] describes streams[i].logical_ref_idx). Returns candidates with
// score > 0, sorted descending by score.
std::vector<Candidate> rank(const std::vector<EventStream> &streams, const std::vector<RefMeta> &metas,
                            const AnchorList &anchors, const Hints &hints);

// Score Command-fire streams against the recording window. Commands have no
// value/delta — scoring is presence + anchor latency + fire-count fit.
// `metas` MUST be parallel to `streams`. Returns candidates with score > 0,
// sorted descending by score. The returned Candidates have kind=Command.
std::vector<Candidate> rank_commands(const std::vector<CommandEventStream> &streams,
                                     const std::vector<CommandRefMeta> &metas, const AnchorList &anchors,
                                     const Hints &hints);

// A command that appears to drive a DataRef.
//
// This is what separates the actuator from the indicator. When a cockpit switch
// is bound to a command, clicking it fires the command FIRST and the DataRef
// follows a frame or two later — the DataRef is the aircraft logic's status
// output, not its input. That ordering is the evidence: it says "bind the
// command to write, read the DataRef for state" instead of leaving the user to
// guess between two unrelated-looking rows.
struct CommandRefLink
{
    uint32_t command_idx     = 0;
    uint32_t logical_ref_idx = 0;
    int      fires_matched   = 0; // command fires followed by this ref reacting
    int      fires_total     = 0;
    float    median_delay_ms = 0.f;
    // Filled in by classify_links(), once the ref side can be judged.
    float ref_score         = 0.f;
    int   name_affinity     = 0;     // shared path tokens between command and ref
    bool  ref_discrete      = false; // switch-shaped values rather than a sensor curve
    bool  ref_bidirectional = false; // tracked the switch in both directions
    float plausibility      = 0.f;   // combined score deciding the primary
    // The single most plausible ref for this command. Everything else that
    // reacted is cascade: firing the battery master also lights every warning
    // lamp on the panel within the same frame, and those are consequences of
    // the state ref, not the state ref itself.
    bool primary = false;
};

// Count path tokens (split on '/' and '_') that appear in both names, matching
// on prefixes so "batteries" and "battery" still count. This is what separates
// sim/electrical/batteries_toggle → sim/cockpit/electrical/battery_on from the
// dozen unrelated lamps that happened to react in the same frame.
int path_affinity(const std::string &a, const std::string &b);

// Everything classify_links() needs to judge one candidate ref, supplied by the
// caller so the correlator stays free of SDK and index lookups.
struct LinkRefFacts
{
    float       score = 0.f;           // ranked candidate score, 0 if it did not rank
    std::string path;                  // display path, for name affinity
    bool        discrete      = false; // integer-like values (a switch state, not a sensor)
    bool        bidirectional = false; // moved both ways — a latching switch signature
};

// Mark one primary link per command and sort the list. Call after rank() so the
// ref facts are available.
//
// Selection combines several independent signals rather than letting any one of
// them dictate. Name affinity alone is not enough: it works when a command and
// its ref share a namespace, and does nothing when an aircraft uses its own
// naming throughout — which is common. Causal ordering and value shape carry the
// decision in that case.
void classify_links(std::vector<CommandRefLink> &links, const std::function<LinkRefFacts(uint32_t)> &ref_facts_of,
                    const std::function<std::string(uint32_t)> &command_name_of);

// Correlate command fires against DataRef onsets. A link needs a MAJORITY of a
// command's Begin fires to be followed by a reaction from that ref within a few
// frames — enough to rule out coincidence, while surviving one bad cycle out of
// five. `fires_matched`/`fires_total` carry the actual ratio so the UI can show
// 4/5 rather than implying a perfect match. Each stream carries its own
// logical_ref_idx.
//
// Cost is O(commands_that_fired x refs_that_moved); both sets are small after
// the baseline filter, so this runs once at Inspect time, never per frame.
std::vector<CommandRefLink> link_commands_to_refs(const std::vector<CommandEventStream> &cmd_streams,
                                                  const std::vector<EventStream>        &ref_streams);

} // namespace xp_sherlock
