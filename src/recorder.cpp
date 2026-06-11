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
constexpr std::size_t MAX_WATCHED       = 200000;
constexpr int         HOLD_FRAMES       = 4;    // change-detector hold window
constexpr float       MIN_RECORD_S      = 5.0f; // auto-stop minimum elapsed
constexpr float       BIDIR_GAP_MIN_S   = 0.2f; // 2nd direction event must be > 200ms after 1st
// Auto-stop minimum events for bool mode. Two events (ON→OFF) is too weak —
// many noise refs happen to flip once each way during a 2-3 second window
// and would tie with the real switch. Three events (ON→OFF→ON, the canonical
// A→B→A→B pattern from the brief) is the discrimination floor.
constexpr int         MIN_BOOL_EVENTS   = 3;

// ── Phase state ──────────────────────────────────────────────────────────────
Phase s_phase = Phase::Idle;

float s_baseline_total = 0.f;
float s_baseline_start = 0.f;
float s_record_start   = 0.f;

bool  s_expect_bidirectional = true;
int   s_expected_clicks      = 0;

std::vector<bool>             s_baseline_changed;    // one flag per logical ref (parallel to dataref_index::all())
std::vector<SampleValue>      s_baseline_value;      // baseline starting value per ref

std::vector<std::size_t>      s_watched;             // indices into dataref_index::all()
std::vector<ChangeDetector>   s_detectors;           // parallel to s_watched
std::vector<EventStream>      s_streams;             // parallel to s_watched
std::vector<RefMeta>          s_metas;               // parallel to s_watched

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

std::vector<Candidate> s_candidates;
Hints                  s_hints;

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

int path_length_of(const std::string &p)
{
    return static_cast<int>(p.size());
}

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
bool stream_meets_auto_stop_bar(const EventStream &s, bool expect_bidirectional, int expected_clicks)
{
    int n = static_cast<int>(s.events.size());

    if (!expect_bidirectional && expected_clicks >= 2)
    {
        if (n < expected_clicks)
            return false;
        // Monotonic check: count longest same-sign run inline (don't pull in correlator here)
        int best = 1, run = 1;
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
        return best >= expected_clicks - 1; // N clicks → N-1 transitions in one direction
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
        XPLMDataRef handle = nullptr;
        bool        is_int = false;
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
        if (lr.array_index < acc->min_idx) acc->min_idx = lr.array_index;
        if (lr.array_index > acc->max_idx) acc->max_idx = lr.array_index;
        acc->entries.emplace_back(static_cast<int>(k), lr.array_index);
    }

    s_array_groups.reserve(accs.size());
    for (auto &a : accs)
    {
        ArrayReadGroup g;
        g.handle  = a.handle;
        g.is_int  = a.is_int;
        g.first   = a.min_idx;
        g.len     = a.max_idx - a.min_idx + 1;
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
        std::size_t  ridx = s_watched[k];
        const auto  &lr   = refs[ridx];
        SampleValue  v0   = (ridx < s_baseline_value.size()) ? s_baseline_value[ridx] : SampleValue{};
        s_detectors[k].init(lr.type, v0);
        s_streams[k].logical_ref_idx = static_cast<uint32_t>(ridx);
        s_streams[k].type            = lr.type;
        s_streams[k].baseline_value  = v0;
        s_streams[k].current_value   = v0;
        // Reserve only a small amount — most refs see zero events. Allocating
        // 64*sizeof(ChangeEvent) for 60k refs would be ~150 MB wasted.
        s_streams[k].events.reserve(4);
        s_metas[k].is_writable     = lr.is_writable;
        s_metas[k].path_length     = path_length_of(lr.display_path);
        s_metas[k].type            = lr.type;
        s_metas[k].baseline_value  = v0;
        s_metas[k].current_value   = v0;
    }

    build_array_groups();
}

void finalize_inspect()
{
    s_hints.expect_bidirectional = s_expect_bidirectional;
    s_hints.expected_clicks      = s_expected_clicks;
    s_hints.user_click_window_ms = 100.f;
    s_candidates                 = rank(s_streams, s_metas, s_anchors, s_hints);

    // Merge in Command candidates. Both lists share the same `score` axis, so a
    // simple concat + sort produces an interleaved ranking. Commands with no
    // Begin fires were filtered out inside rank_commands().
    command_recorder::end_record();
    const auto &cmd_streams = command_recorder::streams();
    const auto &cmd_entries = command_index::all();
    std::vector<CommandRefMeta> cmd_metas(cmd_streams.size());
    for (std::size_t i = 0; i < cmd_streams.size(); ++i)
    {
        if (i < cmd_entries.size())
            cmd_metas[i].path_length = static_cast<int>(cmd_entries[i].name.size());
    }
    auto cmd_candidates = rank_commands(cmd_streams, cmd_metas, s_anchors, s_hints);
    s_candidates.insert(s_candidates.end(), cmd_candidates.begin(), cmd_candidates.end());
    std::sort(s_candidates.begin(), s_candidates.end(),
              [](const Candidate &a, const Candidate &b) { return a.score > b.score; });

    char banner[256];
    snprintf(banner, sizeof(banner),
             "[xp_sherlock] Record stopped: %zu watched, %zu DataRef + %zu Command candidates "
             "(%zu total)\n",
             s_watched.size(), s_candidates.size() - cmd_candidates.size(),
             cmd_candidates.size(), s_candidates.size());
    XPLMDebugString(banner);
}

float baseline_tick(float now)
{
    if (s_baseline_start == 0.f)
        s_baseline_start = now;

    const auto &refs = dataref_index::all();
    if (s_baseline_value.size() != refs.size())
    {
        s_baseline_value.assign(refs.size(), SampleValue{});
        s_baseline_changed.assign(refs.size(), false);
        // initial read
        for (std::size_t i = 0; i < refs.size(); ++i)
            dataref_index::read(refs[i], s_baseline_value[i]);
    }
    else
    {
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
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "[xp_sherlock] Baseline complete: %d of %d refs flagged as ambient noise; "
                 "%d command Begin-fires observed during baseline (expected 0 in a still cockpit)\n",
                 ignored, (int)refs.size(), command_recorder::baseline_fires_observed());
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

    auto feed_one = [&](std::size_t k, SampleValue cur) {
        s_streams[k].current_value = cur;
        ChangeEvent ev;
        if (s_detectors[k].feed(cur, s_frame_counter, t, HOLD_FRAMES, ev))
        {
            s_streams[k].events.push_back(ev);
            if (!any_ready_to_stop &&
                stream_meets_auto_stop_bar(s_streams[k], s_expect_bidirectional, s_expected_clicks))
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
    if (s_auto_stop_enabled && any_ready_to_stop && record_elapsed >= MIN_RECORD_S)
    {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "[xp_sherlock] Auto-stop: a ref met the auto-stop bar (mode=%s, expected_clicks=%d)\n",
                 s_expect_bidirectional ? "bool>=3events" : "rotary", s_expected_clicks);
        XPLMDebugString(msg);
        s_phase = Phase::Inspect;
        finalize_inspect();
        return 0.f; // unregister flight loop
    }
    return -1.f;
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
    command_recorder::clear_baseline_diagnostics();
    command_recorder::set_baseline_phase(true);
    s_phase = Phase::Baseline;
    ensure_flight_loop_registered();
    log_msg("[xp_sherlock] Baseline started\n");
    (void)logf;
}

bool start_record(bool expect_bidirectional, int expected_clicks)
{
    if (s_baseline_changed.empty())
    {
        XPLMDebugString("[xp_sherlock] Record refused: take a baseline snapshot first\n");
        return false;
    }
    s_expect_bidirectional = expect_bidirectional;
    s_expected_clicks      = expected_clicks;

    allocate_record_state();
    if (s_watched.size() > MAX_WATCHED)
    {
        char err[256];
        snprintf(err, sizeof(err),
                 "[xp_sherlock] Record refused: %zu watched refs exceed safety cap %zu.\n",
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
    command_recorder::set_baseline_phase(false);
    command_recorder::reset();
    XPLMDebugString("[xp_sherlock] Reset\n");
}

const std::vector<Candidate> &candidates() { return s_candidates; }

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

bool test_write(std::size_t candidate_idx, SampleValue v, bool &writable, SampleValue &readback)
{
    writable      = false;
    readback      = SampleValue{};
    if (candidate_idx >= s_candidates.size())
        return false;
    const Candidate &c   = s_candidates[candidate_idx];
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
