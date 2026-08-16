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

#include "correlator.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace xp_sherlock
{

namespace
{

float abs_delta(RefType type, SampleValue from, SampleValue to)
{
    switch (type)
    {
    case RefType::Int:
    case RefType::IntArrayElem:
        return std::fabs(static_cast<float>(to.i - from.i));
    case RefType::Float:
    case RefType::FloatArrayElem:
        return std::fabs(to.f - from.f);
    case RefType::Double:
        return static_cast<float>(std::fabs(to.d - from.d));
    }
    return 0.f;
}

// Longest plausible gap between an "I Acted Now" click and the actuation it
// announces. The user clicks the button, then travels to the cockpit switch —
// on a large display with mouse travel that is comfortably over a second, so a
// tight window would reject genuine responses. An anchor's window is cut short
// by the next anchor, which keeps consecutive actuations from sharing events.
constexpr float MAX_RESPONSE_S = 3.0f;

// Anchor-coverage weighting, shared by both pipelines.
//
// The weight sits above the latency bonuses (25 + 10) but below the
// bidirectional signal (100): full coverage is strong evidence, yet a ref that
// tracks the switch in both directions is still the better story. The orphan
// penalty is per stray event and intentionally mild — one extra twitch should
// cost a candidate a rank or two, not eliminate it.
constexpr float ANCHOR_COVERAGE_WEIGHT = 40.f;
constexpr float ORPHAN_EVENT_PENALTY   = 6.f;

// Fraction of the user's anchors this candidate answered. Zero anchors yields
// 0.f rather than a division by zero — with nothing announced there is nothing
// to reward, and every candidate is affected equally.
float coverage_ratio(const Candidate &c)
{
    if (c.anchors_total <= 0)
        return 0.f;
    return static_cast<float>(c.anchors_hit) / static_cast<float>(c.anchors_total);
}

// Causal-ordering weighting.
//
// When a switch has no command behind it, every ref in its cascade correlates
// with the click equally well — same coverage, same latency band — so they
// arrive as a cluster of near-identical scores. What still separates them is
// ORDER: the switch's own state ref moves first, and the bus voltages and lamps
// it drives move after it.
//
// Weight sits below bidirectional tracking (100) and coverage (40): being first
// is evidence, not proof. A ref that merely twitched early must not outrank one
// that actually tracked the switch both ways across every actuation.
constexpr float CAUSAL_ORDER_WEIGHT = 30.f;

// How far behind the first mover the credit has fully decayed. Roughly six
// frames at 60 fps — beyond that a ref is reacting to the reaction, not to the
// click. Kept generous because aircraft systems logic often runs at 10-20 Hz.
constexpr float CAUSAL_ORDER_SPREAD_MS = 100.f;

// Grade every candidate by how soon it reacted relative to the earliest one,
// and flag that earliest as the first mover. No-op without anchors — with
// nothing to measure from, inventing an ordering would be pure noise.
void apply_causal_ordering_bonus(std::vector<Candidate> &cands)
{
    float fastest = -1.f;
    for (const auto &c : cands)
        if (c.has_onset_lag && (fastest < 0.f || c.onset_lag_ms < fastest))
            fastest = c.onset_lag_ms;
    if (fastest < 0.f)
        return;

    for (auto &c : cands)
    {
        if (!c.has_onset_lag)
            continue;
        c.is_first_mover   = (c.onset_lag_ms <= fastest);
        const float behind = c.onset_lag_ms - fastest;
        const float credit = 1.f - std::min(1.f, behind / CAUSAL_ORDER_SPREAD_MS);
        c.score += CAUSAL_ORDER_WEIGHT * std::max(0.f, credit);
    }
}

float latency_for_event_ms(const ChangeEvent &e, const AnchorList &anchors)
{
    // Pick the latest anchor that is <= the event time.
    float best = -1.f;
    for (float a : anchors)
    {
        if (a <= e.t_sec && a > best)
            best = a;
    }
    if (best < 0.f)
        return -1.f;
    return (e.t_sec - best) * 1000.f;
}

// Switches typically write integer-like values: bool 0/1, rotary 0..N, or a
// float that takes the discrete values 0.0 / 1.0. Sensors and downstream
// effects (voltages, temperatures, pressures) take continuous floats with
// real fractional parts. If every observed value of a float/double ref is
// within a small epsilon of an integer, treat it as discrete-like.
bool is_discrete_like(const EventStream &s)
{
    if (s.type == RefType::Int || s.type == RefType::IntArrayElem)
        return true;

    auto near_int_d = [](double v) { return std::fabs(v - std::round(v)) < 0.01; };
    auto near_int_f = [](float v) { return std::fabs(v - std::round(v)) < 0.01f; };

    auto check = [&](SampleValue v) -> bool
    {
        switch (s.type)
        {
        case RefType::Float:
        case RefType::FloatArrayElem:
            return near_int_f(v.f);
        case RefType::Double:
            return near_int_d(v.d);
        default:
            return true;
        }
    };

    for (const auto &e : s.events)
    {
        if (!check(e.from) || !check(e.to))
            return false;
    }
    return true;
}

} // namespace

AnchorCoverage anchor_coverage(const std::vector<float> &event_times, const AnchorList &anchors)
{
    AnchorCoverage cov;
    cov.anchors_total = static_cast<int>(anchors.size());
    if (anchors.empty())
    {
        // No anchors means no expectation to measure against. Reporting every
        // event as an orphan would punish candidates for something the user
        // simply did not record.
        return cov;
    }

    // Anchors arrive in chronological order (mark_user_action appends during
    // Record), but sort a local copy so the function is safe to unit-test with
    // arbitrary input.
    AnchorList sorted = anchors;
    std::sort(sorted.begin(), sorted.end());

    std::vector<bool> hit(sorted.size(), false);
    for (float t : event_times)
    {
        // Latest anchor at or before this event — the only one that could have
        // caused it.
        auto it = std::upper_bound(sorted.begin(), sorted.end(), t);
        if (it == sorted.begin())
        {
            ++cov.orphan_events; // fired before the user announced anything
            continue;
        }
        std::size_t idx = static_cast<std::size_t>(std::distance(sorted.begin(), it)) - 1;

        // The window ends at the next anchor, or after MAX_RESPONSE_S — so a
        // late straggler does not get credited to an anchor it cannot belong to.
        float window_end = sorted[idx] + MAX_RESPONSE_S;
        if (idx + 1 < sorted.size())
            window_end = std::min(window_end, sorted[idx + 1]);

        if (t < window_end)
            hit[idx] = true;
        else
            ++cov.orphan_events;
    }

    for (bool h : hit)
        if (h)
            ++cov.anchors_hit;
    return cov;
}

float median_onset_lag_ms(const std::vector<float> &onsets, const AnchorList &anchors)
{
    if (anchors.empty() || onsets.empty())
        return -1.f;

    AnchorList sorted = anchors;
    std::sort(sorted.begin(), sorted.end());

    std::vector<float> lags;
    lags.reserve(sorted.size());

    for (std::size_t i = 0; i < sorted.size(); ++i)
    {
        // Same window rule as anchor_coverage: an anchor owns the time until the
        // next one, capped at MAX_RESPONSE_S. A reaction outside that cannot be
        // attributed to this actuation.
        float window_end = sorted[i] + MAX_RESPONSE_S;
        if (i + 1 < sorted.size())
            window_end = std::min(window_end, sorted[i + 1]);

        for (float o : onsets)
        {
            if (o < sorted[i])
                continue;
            if (o >= window_end)
                break; // onsets are chronological
            lags.push_back((o - sorted[i]) * 1000.f);
            break; // first reaction only — that is the one that ranks causality
        }
    }

    if (lags.empty())
        return -1.f;
    std::sort(lags.begin(), lags.end());
    return lags[lags.size() / 2];
}

bool is_bidirectional(const EventStream &s)
{
    bool pos = false, neg = false;
    for (const auto &e : s.events)
    {
        if (e.direction > 0)
            pos = true;
        else if (e.direction < 0)
            neg = true;
        if (pos && neg)
            return true;
    }
    return false;
}

// Longest run of same-sign deltas (interpreted as a monotonic value walk).
int staircase_steps(const EventStream &s)
{
    if (s.events.empty())
        return 0;
    int    best = 1;
    int    run  = 1;
    int8_t prev = s.events[0].direction;
    for (std::size_t i = 1; i < s.events.size(); ++i)
    {
        int8_t cur = s.events[i].direction;
        if (cur != 0 && cur == prev)
        {
            ++run;
            if (run > best)
                best = run;
        }
        else
        {
            run = 1;
        }
        prev = cur;
    }
    return best;
}

// Heuristic: a stream is "asymmetric decay" when up-deltas are much larger
// than down-deltas (or vice-versa) on average. Captures the "voltage rises
// when switch goes on, drains slowly when it goes off" downstream-effect
// signature.
bool asymmetric_decay(const EventStream &s)
{
    if (s.events.size() < 2)
        return false;
    float up_sum = 0.f, dn_sum = 0.f;
    int   up_n = 0, dn_n = 0;
    for (const auto &e : s.events)
    {
        float d = abs_delta(s.type, e.from, e.to);
        if (e.direction > 0)
        {
            up_sum += d;
            ++up_n;
        }
        else if (e.direction < 0)
        {
            dn_sum += d;
            ++dn_n;
        }
    }
    if (up_n == 0 || dn_n == 0)
        return false;
    float up_avg = up_sum / static_cast<float>(up_n);
    float dn_avg = dn_sum / static_cast<float>(dn_n);
    float ratio  = (up_avg > dn_avg) ? (up_avg / std::max(dn_avg, 1e-6f)) : (dn_avg / std::max(up_avg, 1e-6f));
    return ratio > 3.f;
}

float min_latency_ms(const EventStream &s, const AnchorList &anchors)
{
    if (anchors.empty())
        return -1.f;
    float best = -1.f;
    for (const auto &e : s.events)
    {
        float lat = latency_for_event_ms(e, anchors);
        if (lat < 0.f)
            continue;
        if (best < 0.f || lat < best)
            best = lat;
    }
    return best;
}

float median_latency_ms(const EventStream &s, const AnchorList &anchors)
{
    if (anchors.empty())
        return -1.f;
    std::vector<float> lats;
    lats.reserve(s.events.size());
    for (const auto &e : s.events)
    {
        float lat = latency_for_event_ms(e, anchors);
        if (lat >= 0.f)
            lats.push_back(lat);
    }
    if (lats.empty())
        return -1.f;
    std::sort(lats.begin(), lats.end());
    return lats[lats.size() / 2];
}

std::vector<Candidate> rank(const std::vector<EventStream> &streams, const std::vector<RefMeta> &metas,
                            const AnchorList &anchors, const Hints &hints)
{
    std::vector<Candidate> out;
    out.reserve(streams.size());

    for (std::size_t i = 0; i < streams.size(); ++i)
    {
        const EventStream &s = streams[i];
        if (s.events.empty())
            continue;

        const RefMeta &m = (i < metas.size()) ? metas[i] : RefMeta{};

        Candidate c;
        c.logical_ref_idx = s.logical_ref_idx;
        c.type            = s.type;
        c.baseline_value  = s.baseline_value;
        c.current_value   = s.current_value;
        c.min_seen        = s.baseline_value;
        c.max_seen        = s.baseline_value;
        auto track        = [&](SampleValue v)
        {
            switch (s.type)
            {
            case RefType::Int:
            case RefType::IntArrayElem:
                if (v.i < c.min_seen.i)
                    c.min_seen.i = v.i;
                if (v.i > c.max_seen.i)
                    c.max_seen.i = v.i;
                break;
            case RefType::Float:
            case RefType::FloatArrayElem:
                if (v.f < c.min_seen.f)
                    c.min_seen.f = v.f;
                if (v.f > c.max_seen.f)
                    c.max_seen.f = v.f;
                break;
            case RefType::Double:
                if (v.d < c.min_seen.d)
                    c.min_seen.d = v.d;
                if (v.d > c.max_seen.d)
                    c.max_seen.d = v.d;
                break;
            }
        };
        for (const auto &e : s.events)
        {
            track(e.from);
            track(e.to);
        }
        track(s.current_value);
        c.is_writable  = m.is_writable;
        c.total_events = static_cast<int>(s.events.size());

        for (const auto &e : s.events)
        {
            if (e.direction > 0)
                ++c.pos_count;
            else if (e.direction < 0)
                ++c.neg_count;
        }
        c.bidirectional       = (c.pos_count > 0 && c.neg_count > 0);
        c.staircase_steps     = staircase_steps(s);
        c.monotonic_staircase = (c.staircase_steps >= 3);
        c.asymmetric_decay    = asymmetric_decay(s);

        float min_lat = min_latency_ms(s, anchors);
        float med_lat = median_latency_ms(s, anchors);
        c.has_latency = (min_lat >= 0.f);
        if (c.has_latency)
        {
            c.min_latency_ms    = min_lat;
            c.median_latency_ms = med_lat;
        }

        {
            std::vector<float> ts;
            std::vector<float> onsets;
            ts.reserve(s.events.size());
            onsets.reserve(s.events.size());
            for (const auto &e : s.events)
            {
                ts.push_back(e.t_sec);
                onsets.push_back(e.onset_t_sec);
            }
            AnchorCoverage cov = anchor_coverage(ts, anchors);
            c.anchors_hit      = cov.anchors_hit;
            c.anchors_total    = cov.anchors_total;
            c.orphan_events    = cov.orphan_events;

            const float lag = median_onset_lag_ms(onsets, anchors);
            c.has_onset_lag = (lag >= 0.f);
            c.onset_lag_ms  = c.has_onset_lag ? lag : 0.f;
        }

        // Scoring formula. Comments document each weighting choice so future
        // tuning against real aircraft has the rationale to refute or keep.
        float score = 0.f;

        if (c.bidirectional)
            score += 100.f; // strongest "this is the cause, not a downstream effect" signal

        if (hints.expected_clicks > 0 && c.staircase_steps == hints.expected_clicks)
            score += 60.f; // rotary perfect match — near-certain
        else if (c.monotonic_staircase)
            score += 30.f; // rotary-like (≥3 monotonic steps)

        if (c.has_latency)
        {
            if (c.min_latency_ms < hints.user_click_window_ms)
                score += 25.f; // first reaction within user-click window
            if (c.median_latency_ms < 250.f)
                score += 10.f; // overall fast — cause-like
        }

        // Anchor coverage. Deliberately weighted above the single-latency
        // bonuses: latency only asks "was one reaction quick", coverage asks
        // "did it answer EVERY announced actuation and nothing else". The more
        // times the user works the switch, the harder that is to fake, so this
        // term is what makes extra repetitions pay off.
        score += ANCHOR_COVERAGE_WEIGHT * coverage_ratio(c);
        score -= ORPHAN_EVENT_PENALTY * static_cast<float>(c.orphan_events);

        if (hints.expect_bidirectional)
        {
            int missing = 0;
            if (c.pos_count == 0)
                ++missing;
            if (c.neg_count == 0)
                ++missing;
            score -= 15.f * static_cast<float>(missing);
        }

        if (c.asymmetric_decay && !c.bidirectional)
            score -= 10.f; // looks like a downstream voltage drain — demote

        // Switches set integer-ish values; sensors set fractional floats.
        // Demote non-discrete float refs — likely a downstream effect that
        // happened to swing during the record window.
        if (!is_discrete_like(s))
            score -= 12.f;

        int expected_events_cap = (hints.expected_clicks > 0) ? 3 * hints.expected_clicks : 6;
        int chatty              = c.total_events - expected_events_cap;
        if (chatty > 0)
            score -= 5.f * static_cast<float>(chatty); // chatty refs are usually noise

        if (m.is_writable)
            score += 5.f; // mild bonus — repurposed storage cells are typically writable

        // Tie-breakers (fractional, never overrule strong signal):
        //  - shorter path wins (top-level sim/cockpit2 > deep nested)
        //  - scalars beat arrays
        if (m.path_length > 0)
            score -= 0.01f * static_cast<float>(m.path_length);
        if (s.type == RefType::IntArrayElem || s.type == RefType::FloatArrayElem)
            score -= 0.3f;

        c.score = score;
        if (score > 0.f)
            out.push_back(c);
    }

    // Causal ordering runs across the whole candidate set, so it cannot live in
    // the per-stream loop above: "reacted first" only means something relative
    // to the others.
    apply_causal_ordering_bonus(out);

    std::sort(out.begin(), out.end(), [](const Candidate &a, const Candidate &b) { return a.score > b.score; });
    return out;
}

namespace
{

// Anchor compare for commands: pick the latest anchor <= the fire time.
// Identical contract to latency_for_event_ms() above but operates on the
// command timeline (no ChangeEvent wrapper).
float latency_for_command_ms(float fire_t_sec, const AnchorList &anchors)
{
    float best = -1.f;
    for (float a : anchors)
    {
        if (a <= fire_t_sec && a > best)
            best = a;
    }
    if (best < 0.f)
        return -1.f;
    return (fire_t_sec - best) * 1000.f;
}

// ── Auto-unmute heuristic ────────────────────────────────────────────────────
//
// A command muted as baseline noise is pulled back into the main list only if
// it behaved *purposefully* during Record instead of continuing to chatter.
//
// Both criteria are required, and both exist for a specific reason:
//
//  - MEDIAN latency, not MIN: a constant chatterer eventually lands near an
//    anchor by pure chance, so its minimum latency is meaningless. Its median
//    stays large and randomly distributed. This is exactly the AW139 case
//    (sim/autopilot/disconnect/... firing unprompted throughout the session).
//  - Fire budget: a chatterer accumulates far more fires across the record
//    window than the user made clicks. A genuine hit lands in the order of the
//    expected click count. The budget has a floor so a 3-click bool run does
//    not reject a switch that emitted one extra Begin.
//
// Without any "I Acted Now" anchor there is no evidence to reason from — the
// command then stays muted rather than being promoted on a guess. Reporting a
// wrong DataRef/Command confidently is worse than reporting nothing.
constexpr float AUTO_UNMUTE_MAX_MEDIAN_LATENCY_MS = 250.f;
constexpr int   AUTO_UNMUTE_FIRE_TOLERANCE        = 2;
constexpr int   AUTO_UNMUTE_MIN_FIRE_BUDGET       = 4;

bool qualifies_for_auto_unmute(const Candidate &c, const AnchorList &anchors, const Hints &hints)
{
    if (anchors.empty() || !c.has_latency)
        return false;
    if (c.median_latency_ms > AUTO_UNMUTE_MAX_MEDIAN_LATENCY_MS)
        return false;
    const int budget = std::max(hints.expected_clicks * AUTO_UNMUTE_FIRE_TOLERANCE, AUTO_UNMUTE_MIN_FIRE_BUDGET);
    return c.fire_count <= budget;
}

} // namespace

std::vector<Candidate> rank_commands(const std::vector<CommandEventStream> &streams,
                                     const std::vector<CommandRefMeta> &metas, const AnchorList &anchors,
                                     const Hints &hints)
{
    std::vector<Candidate> out;
    out.reserve(streams.size() / 64); // most commands never fire — start small

    for (std::size_t i = 0; i < streams.size(); ++i)
    {
        const CommandEventStream &s = streams[i];

        // Only Begin events count for presence/latency: Begin is the
        // user-action instant, Continue is "still held" (fires every frame
        // for held keys), End is the release. Scoring against Continue would
        // wildly inflate fire counts for any held-down cockpit binding.
        int                begin_fires = 0;
        float              first_begin = -1.f;
        std::vector<float> begin_times;
        begin_times.reserve(s.events.size());
        for (const auto &e : s.events)
        {
            if (e.phase == 0 /*xplm_CommandBegin*/)
            {
                ++begin_fires;
                begin_times.push_back(e.t_sec);
                if (first_begin < 0.f)
                    first_begin = e.t_sec;
            }
        }
        if (begin_fires == 0)
            continue;

        const CommandRefMeta &m = (i < metas.size()) ? metas[i] : CommandRefMeta{};

        Candidate c;
        c.kind         = Kind::Command;
        c.command_idx  = static_cast<uint32_t>(i);
        c.type         = RefType::Int; // unused for commands, default to something printable
        c.fire_count   = begin_fires;
        c.last_phase   = s.last_phase;
        c.total_events = static_cast<int>(s.events.size());

        // Latency to nearest anchor across all Begin fires.
        float              min_lat = -1.f;
        std::vector<float> lats;
        lats.reserve(begin_times.size());
        for (float t : begin_times)
        {
            float lat = latency_for_command_ms(t, anchors);
            if (lat < 0.f)
                continue;
            lats.push_back(lat);
            if (min_lat < 0.f || lat < min_lat)
                min_lat = lat;
        }
        c.has_latency = (min_lat >= 0.f);
        if (c.has_latency)
        {
            c.min_latency_ms = min_lat;
            std::sort(lats.begin(), lats.end());
            c.median_latency_ms = lats[lats.size() / 2];
        }

        {
            AnchorCoverage cov = anchor_coverage(begin_times, anchors);
            c.anchors_hit      = cov.anchors_hit;
            c.anchors_total    = cov.anchors_total;
            c.orphan_events    = cov.orphan_events;
        }

        // Baseline-noise verdict. Muting only decides where the row is shown —
        // the score below is computed identically either way, so an unmute in
        // the UI reveals a fully-ranked row rather than an empty shell.
        c.baseline_fires = s.baseline_begin_fires;
        c.is_muted       = s.muted;
        if (c.is_muted && qualifies_for_auto_unmute(c, anchors, hints))
        {
            c.is_muted     = false;
            c.auto_unmuted = true;
        }

        // Scoring formula — calibrated to land in roughly the same range as
        // rank() for DataRefs so the two pipelines interleave sanely in the
        // shared candidates table.
        float score = 0.f;

        score += 70.f; // strong presence signal — the command fired at all

        if (c.has_latency)
        {
            if (c.min_latency_ms < hints.user_click_window_ms)
                score += 25.f;
            if (c.median_latency_ms < 250.f)
                score += 10.f;
        }

        // Same coverage term as the DataRef pipeline — a command bound to the
        // switch fires once per actuation and at no other time.
        score += ANCHOR_COVERAGE_WEIGHT * coverage_ratio(c);
        score -= ORPHAN_EVENT_PENALTY * static_cast<float>(c.orphan_events);

        if (begin_fires == 1)
            score += 15.f; // clean one-shot (e.g. battery_1_on)

        if (hints.expected_clicks > 0 && begin_fires == hints.expected_clicks)
            score += 10.f; // rotary match

        // Chatty penalty — commands that fire far more than the expected cap
        // are usually held-key bindings or autopilot internal tickers.
        int expected_cap = (hints.expected_clicks > 0) ? 3 * hints.expected_clicks : 6;
        int chatty       = begin_fires - expected_cap;
        if (chatty > 0)
            score -= 5.f * static_cast<float>(chatty);

        // Tie-breaker: shorter command names win slightly. Prefers top-level
        // cockpit commands over deep aircraft-specific ones at the margin.
        if (m.path_length > 0)
            score -= 0.01f * static_cast<float>(m.path_length);

        c.score = score;
        if (score > 0.f)
            out.push_back(c);
    }

    std::sort(out.begin(), out.end(), [](const Candidate &a, const Candidate &b) { return a.score > b.score; });
    return out;
}

namespace
{

// How long after a command fires a driven DataRef may still be considered a
// consequence of it. A bound switch updates its ref in the same frame or the
// next one; 150 ms covers even a badly stuttering sim (~9 frames at 60 fps)
// while staying far below human reaction time, so an unrelated ref the user
// moved by hand cannot slip in.
constexpr float LINK_WINDOW_MS = 300.f;

// Small backward tolerance. Command timestamps are taken inside the XPLM
// command callback, ref onsets inside the flight loop — depending on which runs
// first within a frame, a genuinely caused onset can land a hair BEFORE the
// fire. Without this, such a pair is silently discarded.
constexpr float LINK_BACKWARD_TOLERANCE_MS = 20.f;

// Fraction of a command's fires that must be answered for a link to count.
//
// Requiring 100% scaled badly: the tool now asks users to actuate five or more
// times, and a single unanswered fire — one press too brief to confirm, one
// stray Begin from a key repeat — killed the entire pair. A majority still
// rules out coincidence while surviving one bad cycle.
constexpr float LINK_MIN_MATCH_RATIO = 0.6f;
constexpr int   LINK_MIN_MATCHES     = 2;

} // namespace

std::vector<CommandRefLink> link_commands_to_refs(const std::vector<CommandEventStream> &cmd_streams,
                                                  const std::vector<EventStream>        &ref_streams)
{
    std::vector<CommandRefLink> out;

    // Pre-extract the Begin times per command. Continue/End are irrelevant here:
    // Begin is the instant the actuation happens.
    struct FiredCommand
    {
        uint32_t           idx = 0;
        std::vector<float> begins;
    };
    std::vector<FiredCommand> fired;
    for (std::size_t i = 0; i < cmd_streams.size(); ++i)
    {
        FiredCommand fc;
        fc.idx = static_cast<uint32_t>(i);
        for (const auto &e : cmd_streams[i].events)
            if (e.phase == 0 /*xplm_CommandBegin*/)
                fc.begins.push_back(e.t_sec);
        if (!fc.begins.empty())
            fired.push_back(std::move(fc));
    }
    if (fired.empty())
        return out;

    const float window_s   = LINK_WINDOW_MS / 1000.f;
    const float back_tol_s = LINK_BACKWARD_TOLERANCE_MS / 1000.f;

    for (const auto &fc : fired)
    {
        const auto required =
            std::max(static_cast<std::size_t>(LINK_MIN_MATCHES),
                     static_cast<std::size_t>(std::ceil(static_cast<float>(fc.begins.size()) * LINK_MIN_MATCH_RATIO)));

        for (const auto &rs : ref_streams)
        {
            if (rs.events.empty())
                continue;

            std::vector<float> delays;
            delays.reserve(fc.begins.size());
            for (float fire_t : fc.begins)
            {
                // Earliest onset at (or fractionally before) this fire. Onset,
                // not confirmation time — the hold delay would otherwise
                // dominate a measurement made in single-frame units.
                float best = -1.f;
                for (const auto &e : rs.events)
                {
                    const float d = e.onset_t_sec - fire_t;
                    if (d < -back_tol_s)
                        continue; // clearly before the fire — cannot be caused by it
                    if (d > window_s)
                        break; // events are chronological — no closer one follows
                    best = std::max(0.f, d);
                    break;
                }
                if (best >= 0.f)
                    delays.push_back(best * 1000.f);
            }

            // Majority rather than unanimity: one missed cycle should cost
            // confidence, not the whole finding. The ratio is reported so the
            // UI can show 4/5 instead of pretending it was perfect.
            if (delays.size() < required)
                continue;

            CommandRefLink link;
            link.command_idx     = fc.idx;
            link.logical_ref_idx = rs.logical_ref_idx;
            link.fires_matched   = static_cast<int>(delays.size());
            link.fires_total     = static_cast<int>(fc.begins.size());
            std::sort(delays.begin(), delays.end());
            link.median_delay_ms = delays[delays.size() / 2];
            out.push_back(link);
        }
    }

    // Tightest coupling first as a provisional order; classify_links() reorders
    // once the ranking scores are available.
    std::sort(out.begin(), out.end(),
              [](const CommandRefLink &a, const CommandRefLink &b) { return a.median_delay_ms < b.median_delay_ms; });
    return out;
}

namespace
{

// Split a DataRef/command path into lowercase tokens on '/' and '_'.
std::vector<std::string> path_tokens(const std::string &p)
{
    std::vector<std::string> out;
    std::string              cur;
    cur.reserve(16);
    for (char ch : p)
    {
        if (ch == '/' || ch == '_')
        {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
            continue;
        }
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch + 32);
        cur.push_back(ch);
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

// Tokens shorter than this carry no discriminating power ("sim", "on", "1")
// and would give every pair a spurious match.
constexpr std::size_t MIN_AFFINITY_TOKEN_LEN = 4;
// Number of leading characters that must agree for two tokens to count as the
// same word. Comparing the full overlap is too strict — "batteries" and
// "battery" diverge at the 7th character yet plainly mean the same thing —
// while a short fixed prefix still keeps "warn" and "water" apart.
constexpr std::size_t AFFINITY_PREFIX_LEN = 5;

} // namespace

int path_affinity(const std::string &a, const std::string &b)
{
    const auto ta = path_tokens(a);
    const auto tb = path_tokens(b);

    int score = 0;
    for (const auto &x : ta)
    {
        if (x.size() < MIN_AFFINITY_TOKEN_LEN)
            continue;
        for (const auto &y : tb)
        {
            if (y.size() < MIN_AFFINITY_TOKEN_LEN)
                continue;
            // Prefix match rather than equality, so "batteries" still matches
            // "battery" — singular/plural drift is the norm in these names.
            const std::size_t n = std::min({x.size(), y.size(), AFFINITY_PREFIX_LEN});
            if (x.compare(0, n, y, 0, n) == 0)
            {
                ++score;
                break; // count each token from `a` at most once
            }
        }
    }
    return score;
}

namespace
{

// Weights for the primary-link decision. Deliberately spread across independent
// signals so no single one can dictate — an aircraft that names everything in
// its own namespace still gets a correct answer from ordering and value shape,
// and an aircraft with tidy sim/* naming gets a boost from affinity on top.
constexpr float W_MATCH_RATIO   = 40.f; // answered the fires consistently
constexpr float W_PROMPTNESS    = 30.f; // reacted first — cause before effect
constexpr float W_DISCRETE      = 15.f; // switch-shaped values, not a sensor curve
constexpr float W_BIDIRECTIONAL = 10.f; // tracked the switch both ways
constexpr float W_NAME_TOKEN    = 8.f;  // per shared path token, capped below
constexpr float W_SCORE_SCALE   = 0.1f; // ranked score, scaled into the same range

// Affinity is capped so a long shared prefix cannot outvote every other signal.
constexpr int MAX_COUNTED_AFFINITY = 3;

// Delay at which promptness credit has fully decayed. A state ref updates in the
// same frame or the next; anything past this is a downstream consequence.
constexpr float PROMPTNESS_FULL_DECAY_MS = 120.f;

float link_plausibility(const CommandRefLink &l)
{
    float s = 0.f;

    if (l.fires_total > 0)
        s += W_MATCH_RATIO * (static_cast<float>(l.fires_matched) / static_cast<float>(l.fires_total));

    // Causal ordering: when a command fires, its own state ref moves first and
    // the cascade follows. Frame resolution is coarse, so this is a gradient
    // rather than a hard rule — it decides ties without overruling everything.
    const float promptness = 1.f - std::min(1.f, l.median_delay_ms / PROMPTNESS_FULL_DECAY_MS);
    s += W_PROMPTNESS * std::max(0.f, promptness);

    if (l.ref_discrete)
        s += W_DISCRETE;
    if (l.ref_bidirectional)
        s += W_BIDIRECTIONAL;

    s += W_NAME_TOKEN * static_cast<float>(std::min(l.name_affinity, MAX_COUNTED_AFFINITY));
    s += W_SCORE_SCALE * l.ref_score;
    return s;
}

} // namespace

void classify_links(std::vector<CommandRefLink> &links, const std::function<LinkRefFacts(uint32_t)> &ref_facts_of,
                    const std::function<std::string(uint32_t)> &command_name_of)
{
    if (links.empty())
        return;

    for (auto &l : links)
    {
        const LinkRefFacts f = ref_facts_of(l.logical_ref_idx);
        l.ref_score          = f.score;
        l.ref_discrete       = f.discrete;
        l.ref_bidirectional  = f.bidirectional;
        l.name_affinity      = path_affinity(command_name_of(l.command_idx), f.path);
        l.plausibility       = link_plausibility(l);
        l.primary            = false;
    }

    auto better = [](const CommandRefLink &a, const CommandRefLink &b)
    {
        if (a.plausibility != b.plausibility)
            return a.plausibility > b.plausibility;
        return a.median_delay_ms < b.median_delay_ms; // stable tie-break
    };

    // One primary per command.
    for (auto &l : links)
    {
        bool best = true;
        for (const auto &other : links)
        {
            if (&other == &l || other.command_idx != l.command_idx)
                continue;
            if (better(other, l))
            {
                best = false;
                break;
            }
        }
        l.primary = best;
    }

    std::sort(links.begin(), links.end(),
              [&](const CommandRefLink &a, const CommandRefLink &b)
              {
                  if (a.primary != b.primary)
                      return a.primary; // primaries first
                  return better(a, b);
              });
}

} // namespace xp_sherlock
