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

#include "recorder.hpp"
#include "change_detector.hpp"
#include "command_index.hpp"
#include "command_recorder.hpp"
#include "correlator.hpp"
#include "dataref_index.hpp"
#include <XPLM/XPLMProcessing.h>
#include <XPLM/XPLMUtilities.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace xp_sherlock
{

namespace
{

// ── Tuning constants ─────────────────────────────────────────────────────────
// MAX_WATCHED is intentionally generous — payware aircraft (e.g. AOA T-6A)
// can expose ~66k logical refs after array expansion, of which only a few
// thousand twitch during baseline. The remaining ~60k all become watched
// refs. The cost per watched ref is now low (no ring buffer; just a small
// FSM + empty event vector), so we can afford it.
constexpr std::size_t MAX_WATCHED     = 200000;
constexpr int         HOLD_FRAMES     = 4;    // change-detector hold window
constexpr float       MIN_RECORD_S    = 5.0f; // auto-stop minimum elapsed
constexpr float       BIDIR_GAP_MIN_S = 0.2f; // 2nd direction event must be > 200ms after 1st
// Auto-stop minimum events for bool mode. Two events (ON→OFF) is too weak —
// many noise refs happen to flip once each way during a 2-3 second window
// and would tie with the real switch. Three events (ON→OFF→ON, the canonical
// A→B→A→B pattern from the brief) is the discrimination floor.
constexpr int MIN_BOOL_EVENTS = 3;
// Quiet period after the last "I Acted Now" before auto-stop concludes the user
// is finished.
//
// A fixed threshold cannot work: mouse travel between the button and a cockpit
// switch on a large display is easily longer than a short timeout, and cutting
// someone off mid-sequence destroys exactly the anchor coverage this tool now
// depends on. So the threshold adapts to the pace actually observed — if your
// cycles take 6 s, waiting 4 s for "quiet" would stop the run every single time.
constexpr float AUTO_STOP_QUIET_MIN_S = 8.0f;
// Grace period before the no-anchor fallback may fire. Cockpit clicks anchor
// themselves now, so "no anchors" means the user is still moving the mouse from
// the Record button to the switch. Stopping during that trip ends the recording
// before the user has done anything — the failure mode this replaces.
constexpr float NO_ANCHOR_GRACE_S = 25.0f;
// Multiple of the user's own slowest observed gap between anchors. >1 so a
// cycle slightly slower than the previous ones still does not trip the stop.
constexpr float AUTO_STOP_QUIET_FACTOR = 1.8f;

// Quiet threshold for the current run, derived from the anchors placed so far.
float auto_stop_quiet_threshold(const AnchorList &anchors)
{
    if (anchors.size() < 2)
        return AUTO_STOP_QUIET_MIN_S;
    float widest = 0.f;
    for (std::size_t i = 1; i < anchors.size(); ++i)
        widest = std::max(widest, anchors[i] - anchors[i - 1]);
    return std::max(AUTO_STOP_QUIET_MIN_S, widest * AUTO_STOP_QUIET_FACTOR);
}

// ── Phase state ──────────────────────────────────────────────────────────────
Phase s_phase = Phase::Idle;

float s_baseline_total = 0.f;
float s_baseline_start = 0.f;
float s_record_start   = 0.f;

bool s_expect_bidirectional = true;

std::vector<bool>        s_baseline_changed; // one flag per logical ref (parallel to dataref_index::all())
std::vector<SampleValue> s_baseline_value;   // baseline starting value per ref

std::vector<std::size_t>    s_watched;   // indices into dataref_index::all()
std::vector<ChangeDetector> s_detectors; // parallel to s_watched
std::vector<EventStream>    s_streams;   // parallel to s_watched
std::vector<RefMeta>        s_metas;     // parallel to s_watched

// Pre-computed array read groups: one entry per unique base handle that has at
// least one watched array element. Each entry tells us "read elements
// [first..first+len) into a scratch buffer, then dispatch them to the
// s_watched indices in `targets`". This collapses the per-frame SDK call
// count for a large array (often hundreds of elements) into ONE
// XPLMGetDatav{i,f} regardless of how many of its indices we watch.
struct ArrayReadGroup
{
    XPLMDataRef handle = nullptr;
    bool        is_int = false;
    int         first  = 0;
    int         len    = 0;
    // Per element in the read range, the index into s_watched that should
    // receive the value, or -1 if we don't watch that element. Size == len.
    std::vector<int> targets;
};
std::vector<ArrayReadGroup> s_array_groups;

AnchorList s_anchors;
uint32_t   s_frame_counter = 0;

// How many actuations the user made, derived from the anchors they placed
// rather than configured up front. 0 means "no anchors, count unknown" — the
// same sentinel the scoring code already treats as "no expectation".
int actuations_observed() { return static_cast<int>(s_anchors.size()); }

std::vector<Candidate>      s_candidates;
std::vector<CommandRefLink> s_links;

// ── Cascade probe state ──────────────────────────────────────────────────────
// Long enough for a cascade to propagate through the aircraft's systems (relays
// energising, buses coming up, annunciators latching), short enough that the
// user is not tempted to touch anything mid-probe.
constexpr float PROBE_WINDOW_S = 1.5f;

float                    s_probe_start = 0.f;
recorder::ProbeAction    s_probe_action{recorder::ProbeAction::FireCommand};
std::string              s_probe_label;
std::vector<SampleValue> s_probe_before; // parallel to s_watched
recorder::ProbeResult    s_probe_cmd_result;
recorder::ProbeResult    s_probe_ref_result;
Hints                    s_hints;

bool s_flight_loop_registered = false;
bool s_auto_stop_enabled      = true;

XPLMDataRef s_dr_time = nullptr; // sim/time/total_running_time_sec — for absolute timing

// ── Helpers ──────────────────────────────────────────────────────────────────
float now_sec()
{
    if (!s_dr_time)
        s_dr_time = XPLMFindDataRef("sim/time/total_running_time_sec");
    return s_dr_time ? XPLMGetDataf(s_dr_time) : 0.f;
}

int path_length_of(const std::string &p) { return static_cast<int>(p.size()); }

void log_msg(const char *msg) { XPLMDebugString(msg); }
void logf(const char *fmt, int a = 0, int b = 0, int c = 0)
{
    char buf[256];
    snprintf(buf, sizeof(buf), fmt, a, b, c);
    XPLMDebugString(buf);
}

bool bidirectional_with_gap(const EventStream &s, float min_gap_s)
{
    float first_pos_t = -1.f, first_neg_t = -1.f;
    for (const auto &e : s.events)
    {
        if (e.direction > 0 && first_pos_t < 0.f)
            first_pos_t = e.t_sec;
        if (e.direction < 0 && first_neg_t < 0.f)
            first_neg_t = e.t_sec;
        if (first_pos_t >= 0.f && first_neg_t >= 0.f)
        {
            float gap = std::abs(first_pos_t - first_neg_t);
            return gap >= min_gap_s;
        }
    }
    return false;
}

// Does this stream meet the auto-stop bar? The bar is intentionally higher
// than "any one bidirectional pair":
//   - Bool mode: need >= MIN_BOOL_EVENTS (3) confirmed events AND both
//     directions seen with a real gap. Two events is not enough to
//     discriminate the real switch from a noise ref that flipped once each
//     way during the recording window.
//   - Rotary mode (one-shot N clicks): need >= expected_clicks events with
//     a monotonic staircase pattern.
//
// `expected_clicks` now comes from the anchor count, so it is 0 when the user
// placed none. A rotary switch never reverses direction, so falling through to
// the bidirectional bar would mean it never auto-stops at all — hence the
// explicit floor below.
constexpr int ROTARY_FALLBACK_STEPS = 3;

bool stream_meets_auto_stop_bar(const EventStream &s, bool expect_bidirectional, int expected_clicks)
{
    int n = static_cast<int>(s.events.size());

    if (!expect_bidirectional)
    {
        const int need = (expected_clicks >= 2) ? expected_clicks : ROTARY_FALLBACK_STEPS;
        if (n < need)
            return false;
        // Monotonic check: count longest same-sign run inline (don't pull in correlator here)
        int    best = 1, run = 1;
        int8_t prev = s.events.front().direction;
        for (std::size_t i = 1; i < s.events.size(); ++i)
        {
            int8_t cur = s.events[i].direction;
            if (cur != 0 && cur == prev)
            {
                if (++run > best)
                    best = run;
            }
            else
            {
                run = 1;
            }
            prev = cur;
        }
        return best >= need - 1; // N clicks → N-1 transitions in one direction
    }

    if (n < MIN_BOOL_EVENTS)
        return false;
    return bidirectional_with_gap(s, BIDIR_GAP_MIN_S);
}

// Build read-groups: collect watched logical refs of array-element type by
// their base handle and array-index range. Each group reads its entire span
// with ONE SDK call per frame and dispatches into the per-element targets.
// Scalar refs are not grouped — they're individually read in record_tick.
void build_array_groups()
{
    s_array_groups.clear();

    struct Acc
    {
        XPLMDataRef handle  = nullptr;
        bool        is_int  = false;
        int         min_idx = INT32_MAX;
        int         max_idx = INT32_MIN;
        // logical-watch-index → array-element-index pairs
        std::vector<std::pair<int, int>> entries;
    };

    // Map base handle → accumulator. Linear scan with a small std::vector is
    // faster than std::unordered_map for the typical N≈1000 buckets.
    std::vector<Acc> accs;
    accs.reserve(256);

    const auto &refs = dataref_index::all();
    for (std::size_t k = 0; k < s_watched.size(); ++k)
    {
        const auto &lr = refs[s_watched[k]];
        if (lr.type != RefType::IntArrayElem && lr.type != RefType::FloatArrayElem)
            continue;
        bool is_int = (lr.type == RefType::IntArrayElem);
        Acc *acc    = nullptr;
        for (auto &a : accs)
        {
            if (a.handle == lr.handle)
            {
                acc = &a;
                break;
            }
        }
        if (!acc)
        {
            accs.push_back(Acc{});
            acc         = &accs.back();
            acc->handle = lr.handle;
            acc->is_int = is_int;
        }
        if (lr.array_index < acc->min_idx)
            acc->min_idx = lr.array_index;
        if (lr.array_index > acc->max_idx)
            acc->max_idx = lr.array_index;
        acc->entries.emplace_back(static_cast<int>(k), lr.array_index);
    }

    s_array_groups.reserve(accs.size());
    for (auto &a : accs)
    {
        ArrayReadGroup g;
        g.handle = a.handle;
        g.is_int = a.is_int;
        g.first  = a.min_idx;
        g.len    = a.max_idx - a.min_idx + 1;
        g.targets.assign(g.len, -1);
        for (auto &p : a.entries)
            g.targets[p.second - g.first] = p.first;
        s_array_groups.push_back(std::move(g));
    }
}

void allocate_record_state()
{
    const auto &refs = dataref_index::all();
    s_watched.clear();
    s_watched.reserve(refs.size());
    for (std::size_t i = 0; i < refs.size(); ++i)
    {
        if (i < s_baseline_changed.size() && s_baseline_changed[i])
            continue; // noise — ignore
        s_watched.push_back(i);
    }

    s_detectors.assign(s_watched.size(), ChangeDetector{});
    s_streams.assign(s_watched.size(), EventStream{});
    s_metas.assign(s_watched.size(), RefMeta{});

    for (std::size_t k = 0; k < s_watched.size(); ++k)
    {
        std::size_t ridx = s_watched[k];
        const auto &lr   = refs[ridx];
        SampleValue v0   = (ridx < s_baseline_value.size()) ? s_baseline_value[ridx] : SampleValue{};
        s_detectors[k].init(lr.type, v0);
        s_streams[k].logical_ref_idx = static_cast<uint32_t>(ridx);
        s_streams[k].type            = lr.type;
        s_streams[k].baseline_value  = v0;
        s_streams[k].current_value   = v0;
        // Reserve only a small amount — most refs see zero events. Allocating
        // 64*sizeof(ChangeEvent) for 60k refs would be ~150 MB wasted.
        s_streams[k].events.reserve(4);
        s_metas[k].is_writable    = lr.is_writable;
        s_metas[k].path_length    = path_length_of(lr.display_path);
        s_metas[k].type           = lr.type;
        s_metas[k].baseline_value = v0;
        s_metas[k].current_value  = v0;
    }

    build_array_groups();
}

void finalize_inspect()
{
    s_hints.expect_bidirectional = s_expect_bidirectional;
    s_hints.expected_clicks      = actuations_observed();
    s_hints.user_click_window_ms = 100.f;
    s_candidates                 = rank(s_streams, s_metas, s_anchors, s_hints);

    // Merge in Command candidates. Both lists share the same `score` axis, so a
    // simple concat + sort produces an interleaved ranking. Commands with no
    // Begin fires were filtered out inside rank_commands().
    command_recorder::end_record();
    const auto                 &cmd_streams = command_recorder::streams();
    const auto                 &cmd_entries = command_index::all();
    std::vector<CommandRefMeta> cmd_metas(cmd_streams.size());
    for (std::size_t i = 0; i < cmd_streams.size(); ++i)
    {
        if (i < cmd_entries.size())
            cmd_metas[i].path_length = static_cast<int>(cmd_entries[i].name.size());
    }
    // Command→DataRef coupling. Runs on the raw streams (not the ranked
    // candidates) so a link survives even when one side scored too low to make
    // the table — the coupling itself is often the more useful answer.
    s_links = link_commands_to_refs(cmd_streams, s_streams);

    auto cmd_candidates = rank_commands(cmd_streams, cmd_metas, s_anchors, s_hints);
    // Push the ranking's auto-unmute verdicts back into command_recorder so it
    // remains the single source of truth for "is this row hidden". The UI reads
    // that live state, which is what lets a manual Unmute take effect
    // immediately without re-running the ranking.
    for (const auto &c : cmd_candidates)
        if (c.auto_unmuted)
            command_recorder::set_muted(c.command_idx, false);
    s_candidates.insert(s_candidates.end(), cmd_candidates.begin(), cmd_candidates.end());
    std::sort(s_candidates.begin(), s_candidates.end(),
              [](const Candidate &a, const Candidate &b) { return a.score > b.score; });

    // Now that scores exist, work out which link per command is the actual
    // state ref and which are just the cascade riding along behind it.
    classify_links(
        s_links,
        [](uint32_t ref_idx) -> LinkRefFacts
        {
            LinkRefFacts f;
            const auto  &refs = dataref_index::all();
            if (ref_idx < refs.size())
                f.path = refs[ref_idx].display_path;
            for (const auto &c : s_candidates)
            {
                if (c.kind != Kind::DataRef || c.logical_ref_idx != ref_idx)
                    continue;
                f.score         = c.score;
                f.bidirectional = c.bidirectional;
                // A latching switch's state ref holds integer-like values; a
                // downstream voltage or temperature does not. min==max would
                // mean it never actually moved, so require a real span.
                f.discrete = (c.type == RefType::Int || c.type == RefType::IntArrayElem);
                break;
            }
            return f;
        },
        [](uint32_t cmd_idx) -> std::string
        {
            const auto &cmds = command_index::all();
            return (cmd_idx < cmds.size()) ? cmds[cmd_idx].name : std::string{};
        });

    int primary_links = 0;
    for (const auto &l : s_links)
        if (l.primary)
            ++primary_links;

    char banner[256];
    snprintf(banner, sizeof(banner),
             "[xp_sherlock] Record stopped: %zu watched, %zu DataRef + %zu Command candidates "
             "(%zu total), %d primary command->dataref pairs (%zu links incl. cascade)\n",
             s_watched.size(), s_candidates.size() - cmd_candidates.size(), cmd_candidates.size(), s_candidates.size(),
             primary_links, s_links.size());
    XPLMDebugString(banner);
}

// Seed the baseline reference arrays from current ref values, unless already
// sized to the live index. Returns true iff it seeded on this call (so the
// caller can skip the mover-flagging pass for the very first tick). Shared by
// Baseline (Phase 1) and additive Noise-capture.
bool ensure_baseline_arrays_seeded()
{
    const auto &refs = dataref_index::all();
    if (s_baseline_value.size() == refs.size())
        return false;
    s_baseline_value.assign(refs.size(), SampleValue{});
    s_baseline_changed.assign(refs.size(), false);
    for (std::size_t i = 0; i < refs.size(); ++i)
        dataref_index::read(refs[i], s_baseline_value[i]);
    return true;
}

// Flag every ref whose current value deviates from its baseline value, adding
// it to the ignore-set. Idempotent per ref (already-flagged refs are skipped).
// Shared by the Baseline tick and the Noise-capture tick — the only difference
// between the two phases is that Baseline resets the set first and Noise-capture
// does not.
void flag_movers_against_baseline()
{
    const auto &refs = dataref_index::all();
    // Guard against a mid-capture re-enumeration that resized the index out from
    // under our parallel arrays — skip this pass rather than index out of bounds.
    if (s_baseline_changed.size() != refs.size() || s_baseline_value.size() != refs.size())
        return;
    for (std::size_t i = 0; i < refs.size(); ++i)
    {
        if (s_baseline_changed[i])
            continue;
        SampleValue cur;
        if (!dataref_index::read(refs[i], cur))
            continue;
        if (exceeds_epsilon(refs[i].type, s_baseline_value[i], cur))
            s_baseline_changed[i] = true;
    }
}

float baseline_tick(float now)
{
    if (s_baseline_start == 0.f)
        s_baseline_start = now;

    const auto &refs = dataref_index::all();
    // First tick seeds the reference values; subsequent ticks flag movers.
    if (!ensure_baseline_arrays_seeded())
        flag_movers_against_baseline();

    float elapsed = now - s_baseline_start;
    if (elapsed >= s_baseline_total)
    {
        int ignored = 0;
        for (bool b : s_baseline_changed)
            if (b)
                ++ignored;
        s_phase = Phase::Idle; // briefly — start_record will move us forward
        // Log + return; we now wait for the user to click "Record" which calls
        // start_record() and triggers state-allocation + transition to Record.
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "[xp_sherlock] Baseline complete: %d of %d refs flagged as ambient noise; "
                 "%d command Begin-fires observed during baseline (expected 0 in a still cockpit), "
                 "%d commands muted as noise\n",
                 ignored, (int)refs.size(), command_recorder::baseline_fires_observed(),
                 command_recorder::muted_count());
        XPLMDebugString(msg);
        command_recorder::set_baseline_phase(false);
        s_phase = Phase::Idle;
        // Set a sentinel so UI knows baseline is finished and Record is allowed.
        // We use the elapsed >= total state as the "baseline done" signal.
        return 0.f; // stop flight loop until Record starts
    }
    return -1.f;
}

float record_tick(float now)
{
    ++s_frame_counter;
    const auto &refs = dataref_index::all();
    float       t    = now - s_record_start;

    bool any_ready_to_stop = false;

    auto feed_one = [&](std::size_t k, SampleValue cur)
    {
        s_streams[k].current_value = cur;
        ChangeEvent ev;
        if (s_detectors[k].feed(cur, s_frame_counter, t, HOLD_FRAMES, ev))
        {
            s_streams[k].events.push_back(ev);
            if (!any_ready_to_stop &&
                stream_meets_auto_stop_bar(s_streams[k], s_expect_bidirectional, actuations_observed()))
                any_ready_to_stop = true;
        }
    };

    // Pass 1: scalar refs (one SDK call per ref).
    for (std::size_t k = 0; k < s_watched.size(); ++k)
    {
        const auto &lr = refs[s_watched[k]];
        if (lr.type == RefType::IntArrayElem || lr.type == RefType::FloatArrayElem)
            continue;
        SampleValue cur;
        if (!dataref_index::read(lr, cur))
            continue;
        feed_one(k, cur);
    }

    // Pass 2: array refs — one SDK call per base handle covers many watched
    // elements. Scratch buffer reused across groups (sized to largest group).
    static std::vector<int>   scratch_i;
    static std::vector<float> scratch_f;
    for (const auto &g : s_array_groups)
    {
        if (g.is_int)
        {
            if (static_cast<int>(scratch_i.size()) < g.len)
                scratch_i.resize(static_cast<std::size_t>(g.len));
            int got = XPLMGetDatavi(g.handle, scratch_i.data(), g.first, g.len);
            int n   = std::min(got, g.len);
            for (int i = 0; i < n; ++i)
            {
                int target = g.targets[i];
                if (target < 0)
                    continue;
                SampleValue v{};
                v.i = scratch_i[i];
                feed_one(static_cast<std::size_t>(target), v);
            }
        }
        else
        {
            if (static_cast<int>(scratch_f.size()) < g.len)
                scratch_f.resize(static_cast<std::size_t>(g.len));
            int got = XPLMGetDatavf(g.handle, scratch_f.data(), g.first, g.len);
            int n   = std::min(got, g.len);
            for (int i = 0; i < n; ++i)
            {
                int target = g.targets[i];
                if (target < 0)
                    continue;
                SampleValue v{};
                v.f = scratch_f[i];
                feed_one(static_cast<std::size_t>(target), v);
            }
        }
    }

    float record_elapsed = now - s_record_start;
    if (!s_auto_stop_enabled || record_elapsed < MIN_RECORD_S)
        return -1.f;

    // Anchored stop: once the user has placed anchors, THEY decide how many
    // cycles to run — stopping on a fixed event count would cut a five-flip
    // sequence off after the third, which is exactly what anchor coverage needs
    // more of. So we wait for quiet instead: no new anchor for a few seconds
    // means the user is done.
    if (!s_anchors.empty())
    {
        float since_last_anchor = record_elapsed - s_anchors.back();
        float quiet_needed      = auto_stop_quiet_threshold(s_anchors);
        if (since_last_anchor >= quiet_needed)
        {
            char msg[224];
            snprintf(msg, sizeof(msg),
                     "[xp_sherlock] Auto-stop: %.1f s of quiet (threshold %.1f s) after the last of %zu anchors.\n",
                     static_cast<double>(since_last_anchor), static_cast<double>(quiet_needed), s_anchors.size());
            XPLMDebugString(msg);
            s_phase = Phase::Inspect;
            finalize_inspect();
            return 0.f;
        }
        // Anchors present but the user is still working — never stop on the
        // event-count bar here, or we would defeat the whole point.
        return -1.f;
    }

    // No anchors yet. Since cockpit clicks now anchor themselves, this means the
    // user has not reached the switch — they are still travelling there with the
    // mouse. Stopping on a pattern match now would end the run before it began,
    // which is exactly what used to happen. Give them room to arrive first.
    if (record_elapsed < NO_ANCHOR_GRACE_S)
        return -1.f;

    // Past the grace period with still no action registered: fall back to the
    // original pattern bar, since there is no "user is done" signal to wait for.
    if (any_ready_to_stop)
    {
        char msg[160];
        snprintf(msg, sizeof(msg), "[xp_sherlock] Auto-stop: a ref met the auto-stop bar (mode=%s, no anchors set)\n",
                 s_expect_bidirectional ? "bool>=3events" : "rotary");
        XPLMDebugString(msg);
        s_phase = Phase::Inspect;
        finalize_inspect();
        return 0.f; // unregister flight loop
    }
    return -1.f;
}

// Compare every watched ref against the pre-action snapshot and record which
// ones moved. Runs once, when the probe window closes.
void finalize_probe()
{
    const auto &refs = dataref_index::all();

    recorder::ProbeResult r;
    r.valid        = true;
    r.action       = s_probe_action;
    r.action_label = s_probe_label;
    r.refs_sampled = static_cast<int>(s_watched.size());
    r.moved_refs.reserve(32);

    for (std::size_t k = 0; k < s_watched.size() && k < s_probe_before.size(); ++k)
    {
        std::size_t ridx = s_watched[k];
        if (ridx >= refs.size())
            continue;
        SampleValue cur;
        if (!dataref_index::read(refs[ridx], cur))
            continue;
        if (exceeds_epsilon(refs[ridx].type, s_probe_before[k], cur))
            r.moved_refs.push_back(ridx);
    }
    r.refs_moved = static_cast<int>(r.moved_refs.size());

    const bool is_cmd = (s_probe_action == recorder::ProbeAction::FireCommand);
    const int  moved  = r.refs_moved;
    if (is_cmd)
        s_probe_cmd_result = std::move(r);
    else
        s_probe_ref_result = std::move(r);

    char msg[256];
    snprintf(msg, sizeof(msg), "[xp_sherlock] Probe (%s) finished: %d of %zu watched refs moved.\n",
             is_cmd ? "fire command" : "write dataref", moved, s_watched.size());
    XPLMDebugString(msg);
}

float flight_loop_cb(float, float, int, void *)
{
    float now = now_sec();
    switch (s_phase)
    {
    case Phase::Baseline:
    {
        float next = baseline_tick(now);
        if (next == 0.f)
        {
            // Baseline finished — stop flight loop, wait for user to start Record.
            return 0.f;
        }
        return -1.f;
    }
    case Phase::Record:
        return record_tick(now);
    case Phase::NoiseCapture:
        // Open-ended: keep flagging movers every frame until the user stops.
        // Arrays are guaranteed seeded by start_noise_capture().
        flag_movers_against_baseline();
        return -1.f;
    case Phase::Probe:
        // Fixed-length window. We only need the end state, so nothing is
        // sampled per frame — we just wait out the cascade.
        if (now - s_probe_start >= PROBE_WINDOW_S)
        {
            finalize_probe();
            s_phase = Phase::Inspect;
            return 0.f;
        }
        return -1.f;
    case Phase::Idle:
    case Phase::Inspect:
    default:
        return 0.f;
    }
}

void ensure_flight_loop_registered()
{
    if (!s_flight_loop_registered)
    {
        XPLMRegisterFlightLoopCallback(flight_loop_cb, -1.f, nullptr);
        s_flight_loop_registered = true;
    }
    else
    {
        XPLMSetFlightLoopCallbackInterval(flight_loop_cb, -1.f, 1, nullptr);
    }
}

void unregister_flight_loop_if_needed()
{
    if (s_flight_loop_registered)
    {
        XPLMUnregisterFlightLoopCallback(flight_loop_cb, nullptr);
        s_flight_loop_registered = false;
    }
}

} // namespace

namespace recorder
{

void init() { /* lazy: nothing to do until user clicks a button */ }

void stop()
{
    unregister_flight_loop_if_needed();
    s_phase = Phase::Idle;
}

Phase phase() { return s_phase; }

void start_baseline(float baseline_seconds)
{
    if (!dataref_index::is_built())
        dataref_index::rebuild();
    // Mirror the dataref lazy-build behaviour for commands. If the command
    // index hasn't been built yet (e.g. user clicked Take Snapshot before
    // ever pressing Re-enumerate), build it now and refresh the handler
    // registrations so command_recorder hooks the newly-found commands.
    if (!command_index::is_built())
    {
        command_recorder::disable();
        command_index::rebuild();
        command_recorder::enable();
    }

    s_baseline_total = std::max(0.5f, baseline_seconds);
    s_baseline_start = 0.f;
    s_baseline_value.clear();
    s_baseline_changed.clear();
    s_anchors.clear();
    s_candidates.clear();
    s_links.clear();
    command_recorder::clear_baseline_diagnostics();
    // Baseline is the resetting phase for both noise sets: it clears the DataRef
    // ignore-set above, so it must clear the command mute-set too. Learn Ambient
    // is the additive counterpart and deliberately keeps both.
    command_recorder::clear_mutes();
    command_recorder::set_baseline_phase(true);
    s_phase = Phase::Baseline;
    ensure_flight_loop_registered();
    log_msg("[xp_sherlock] Baseline started\n");
    (void)logf;
}

void start_noise_capture()
{
    // Not meaningful mid-baseline or mid-record.
    if (s_phase == Phase::Baseline || s_phase == Phase::Record)
        return;
    if (!dataref_index::is_built())
        dataref_index::rebuild();
    // Seed the reference arrays if the user hasn't taken a baseline yet, so we
    // have something to diff against. The existing ignore-set (ambient noise +
    // any prior captures) is preserved — capture is additive by design.
    ensure_baseline_arrays_seeded();
    // Commands are collected symmetrically to DataRefs: anything that fires
    // while the capture runs joins the mute-set. Additive — no clear_mutes().
    command_recorder::set_baseline_phase(true);
    s_phase = Phase::NoiseCapture;
    ensure_flight_loop_registered();
    XPLMDebugString("[xp_sherlock] Noise capture started - drive the actions you want excluded, then Stop.\n");
}

void stop_noise_capture()
{
    if (s_phase != Phase::NoiseCapture)
        return;
    s_phase = Phase::Idle;
    command_recorder::set_baseline_phase(false);
    unregister_flight_loop_if_needed();
    int ignored = 0;
    for (bool b : s_baseline_changed)
        if (b)
            ++ignored;
    char msg[192];
    snprintf(msg, sizeof(msg), "[xp_sherlock] Noise capture stopped: %d refs and %d commands now excluded as noise.\n",
             ignored, command_recorder::muted_count());
    XPLMDebugString(msg);
}

bool start_record(bool expect_bidirectional)
{
    if (s_baseline_changed.empty())
    {
        XPLMDebugString("[xp_sherlock] Record refused: take a baseline snapshot first\n");
        return false;
    }
    s_expect_bidirectional = expect_bidirectional;

    allocate_record_state();
    if (s_watched.size() > MAX_WATCHED)
    {
        char err[256];
        snprintf(err, sizeof(err), "[xp_sherlock] Record refused: %zu watched refs exceed safety cap %zu.\n",
                 s_watched.size(), MAX_WATCHED);
        XPLMDebugString(err);
        s_watched.clear();
        s_streams.clear();
        s_detectors.clear();
        s_array_groups.clear();
        return false;
    }
    s_frame_counter = 0;
    s_record_start  = now_sec();
    s_anchors.clear();
    command_recorder::set_baseline_phase(false);
    command_recorder::begin_record(s_record_start, s_frame_counter);
    s_phase = Phase::Record;
    ensure_flight_loop_registered();

    // Count scalars vs. array elements for the diagnostic log — useful for
    // confirming "yes, we're actually watching scalars too, not just arrays".
    const auto &refs       = dataref_index::all();
    std::size_t n_scalars  = 0;
    std::size_t n_arr_elem = 0;
    for (std::size_t idx : s_watched)
    {
        if (idx >= refs.size())
            continue;
        RefType t = refs[idx].type;
        if (t == RefType::IntArrayElem || t == RefType::FloatArrayElem)
            ++n_arr_elem;
        else
            ++n_scalars;
    }
    char msg[256];
    snprintf(msg, sizeof(msg),
             "[xp_sherlock] Record started: %zu watched (%zu scalars, %zu array elements in %zu base arrays)\n",
             s_watched.size(), n_scalars, n_arr_elem, s_array_groups.size());
    XPLMDebugString(msg);
    return true;
}

void set_auto_stop_enabled(bool on) { s_auto_stop_enabled = on; }
bool auto_stop_enabled() { return s_auto_stop_enabled; }

void mark_user_action()
{
    if (s_phase != Phase::Record)
        return;
    float t = now_sec() - s_record_start;
    s_anchors.push_back(t);
    char msg[120];
    snprintf(msg, sizeof(msg), "[xp_sherlock] User action stamped at t=%.3f s\n", static_cast<double>(t));
    XPLMDebugString(msg);
}

void stop_record()
{
    if (s_phase != Phase::Record)
        return;
    s_phase = Phase::Inspect;
    finalize_inspect();
    unregister_flight_loop_if_needed();
}

void reset()
{
    unregister_flight_loop_if_needed();
    s_phase = Phase::Idle;
    s_baseline_value.clear();
    s_baseline_changed.clear();
    s_watched.clear();
    s_detectors.clear();
    s_streams.clear();
    s_metas.clear();
    s_array_groups.clear();
    s_anchors.clear();
    s_candidates.clear();
    s_links.clear();
    s_probe_before.clear();
    probe_clear();
    command_recorder::set_baseline_phase(false);
    command_recorder::reset();
    XPLMDebugString("[xp_sherlock] Reset\n");
}

const std::vector<Candidate> &candidates() { return s_candidates; }

const std::vector<CommandRefLink> &command_ref_links() { return s_links; }

bool is_ignored_as_noise(std::size_t logical_idx)
{
    return logical_idx < s_baseline_changed.size() && s_baseline_changed[logical_idx];
}

const LogicalRef *logical_ref_at(std::size_t logical_idx)
{
    const auto &refs = dataref_index::all();
    if (logical_idx >= refs.size())
        return nullptr;
    return &refs[logical_idx];
}

Status status()
{
    Status st;
    st.phase = s_phase;
    if (s_phase == Phase::Baseline && s_baseline_start > 0.f)
    {
        st.baseline_elapsed_s = now_sec() - s_baseline_start;
        st.baseline_total_s   = s_baseline_total;
    }
    if (s_phase == Phase::Record)
        st.record_elapsed_s = now_sec() - s_record_start;

    int ignored = 0;
    for (bool b : s_baseline_changed)
        if (b)
            ++ignored;
    st.ignored_count        = ignored;
    st.watched_count        = static_cast<int>(s_watched.size());
    st.candidate_count      = static_cast<int>(s_candidates.size());
    st.total_logical        = static_cast<int>(dataref_index::all().size());
    st.baseline_in_progress = (s_phase == Phase::Baseline);
    st.auto_stop_armed      = (s_phase == Phase::Record && (now_sec() - s_record_start) >= MIN_RECORD_S);
    st.anchors_set          = static_cast<int>(s_anchors.size());
    st.muted_command_count  = command_recorder::muted_count();
    if (s_phase == Phase::Record && s_auto_stop_enabled && !s_anchors.empty())
    {
        float elapsed = now_sec() - s_record_start;
        if (elapsed >= MIN_RECORD_S)
        {
            float remaining   = auto_stop_quiet_threshold(s_anchors) - (elapsed - s_anchors.back());
            st.auto_stop_in_s = std::max(0.f, remaining);
        }
    }
    if (s_phase == Phase::Record)
    {
        int best = 0;
        for (const auto &stream : s_streams)
        {
            int n = static_cast<int>(stream.events.size());
            if (n > best)
                best = n;
        }
        st.best_events_so_far = best;
    }
    return st;
}

bool probe_start(std::size_t candidate_idx, ProbeAction action, SampleValue value)
{
    // A probe measures against the watched set, which only exists after a
    // Record. Refusing here is better than silently probing zero refs.
    if (s_phase != Phase::Inspect || s_watched.empty())
    {
        XPLMDebugString("[xp_sherlock] Probe refused: run a Record first.\n");
        return false;
    }
    if (candidate_idx >= s_candidates.size())
        return false;

    const Candidate &c    = s_candidates[candidate_idx];
    const auto      &refs = dataref_index::all();

    // Snapshot BEFORE acting — this is the reference the cascade is measured
    // against, so it has to be taken while nothing has moved yet.
    s_probe_before.assign(s_watched.size(), SampleValue{});
    for (std::size_t k = 0; k < s_watched.size(); ++k)
    {
        std::size_t ridx = s_watched[k];
        if (ridx < refs.size())
            dataref_index::read(refs[ridx], s_probe_before[k]);
    }

    if (action == ProbeAction::FireCommand)
    {
        if (c.kind != Kind::Command)
            return false;
        const auto &cmds = command_index::all();
        if (c.command_idx >= cmds.size())
            return false;
        s_probe_label = cmds[c.command_idx].name;
        if (!command_recorder::test_fire(c.command_idx, /*mode=*/0))
            return false;
    }
    else
    {
        if (c.kind != Kind::DataRef)
            return false;
        const LogicalRef *lr = logical_ref_at(c.logical_ref_idx);
        if (!lr)
            return false;
        if (!lr->is_writable)
        {
            XPLMDebugString("[xp_sherlock] Probe refused: DataRef is read-only.\n");
            return false;
        }
        s_probe_label = lr->display_path;
        dataref_index::write(*lr, value);
    }

    s_probe_action = action;
    s_probe_start  = now_sec();
    s_phase        = Phase::Probe;
    ensure_flight_loop_registered();
    return true;
}

bool probe_in_progress() { return s_phase == Phase::Probe; }

const ProbeResult &probe_command_result() { return s_probe_cmd_result; }
const ProbeResult &probe_dataref_result() { return s_probe_ref_result; }

void probe_clear()
{
    s_probe_cmd_result = ProbeResult{};
    s_probe_ref_result = ProbeResult{};
}

bool test_write(std::size_t candidate_idx, SampleValue v, bool &writable, SampleValue &readback)
{
    writable = false;
    readback = SampleValue{};
    if (candidate_idx >= s_candidates.size())
        return false;
    const Candidate  &c  = s_candidates[candidate_idx];
    const LogicalRef *lr = logical_ref_at(c.logical_ref_idx);
    if (!lr)
        return false;
    writable = lr->is_writable;
    if (!writable)
        return false;
    dataref_index::write(*lr, v);
    dataref_index::read(*lr, readback);
    return true;
}

} // namespace recorder
} // namespace xp_sherlock
