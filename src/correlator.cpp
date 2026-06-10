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

    auto check = [&](SampleValue v) -> bool {
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
    int best     = 1;
    int run      = 1;
    int8_t prev  = s.events[0].direction;
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
        c.logical_ref_idx     = s.logical_ref_idx;
        c.type                = s.type;
        c.baseline_value      = s.baseline_value;
        c.current_value       = s.current_value;
        c.min_seen            = s.baseline_value;
        c.max_seen            = s.baseline_value;
        auto track = [&](SampleValue v) {
            switch (s.type)
            {
            case RefType::Int:
            case RefType::IntArrayElem:
                if (v.i < c.min_seen.i) c.min_seen.i = v.i;
                if (v.i > c.max_seen.i) c.max_seen.i = v.i;
                break;
            case RefType::Float:
            case RefType::FloatArrayElem:
                if (v.f < c.min_seen.f) c.min_seen.f = v.f;
                if (v.f > c.max_seen.f) c.max_seen.f = v.f;
                break;
            case RefType::Double:
                if (v.d < c.min_seen.d) c.min_seen.d = v.d;
                if (v.d > c.max_seen.d) c.max_seen.d = v.d;
                break;
            }
        };
        for (const auto &e : s.events)
        {
            track(e.from);
            track(e.to);
        }
        track(s.current_value);
        c.is_writable         = m.is_writable;
        c.total_events        = static_cast<int>(s.events.size());

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

        // Scoring formula. Comments document each weighting choice so future
        // tuning against real aircraft has the rationale to refute or keep.
        float score = 0.f;

        if (c.bidirectional)
            score += 100.f; // strongest "this is the cause, not a downstream effect" signal

        if (hints.expected_clicks > 0 && c.staircase_steps == hints.expected_clicks)
            score += 60.f;  // rotary perfect match — near-certain
        else if (c.monotonic_staircase)
            score += 30.f;  // rotary-like (≥3 monotonic steps)

        if (c.has_latency)
        {
            if (c.min_latency_ms < hints.user_click_window_ms)
                score += 25.f;     // first reaction within user-click window
            if (c.median_latency_ms < 250.f)
                score += 10.f;     // overall fast — cause-like
        }

        if (hints.expect_bidirectional)
        {
            int missing = 0;
            if (c.pos_count == 0) ++missing;
            if (c.neg_count == 0) ++missing;
            score -= 15.f * static_cast<float>(missing);
        }

        if (c.asymmetric_decay && !c.bidirectional)
            score -= 10.f;       // looks like a downstream voltage drain — demote

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
            score += 5.f;        // mild bonus — repurposed storage cells are typically writable

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

} // namespace

std::vector<Candidate> rank_commands(const std::vector<CommandEventStream> &streams,
                                     const std::vector<CommandRefMeta>     &metas,
                                     const AnchorList                      &anchors,
                                     const Hints                           &hints)
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
        int   begin_fires = 0;
        float first_begin = -1.f;
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
        float min_lat = -1.f;
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

} // namespace xp_sherlock
