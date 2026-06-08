// Pure ranking of behavioural-correlation candidates. ZERO SDK includes —
// fully unit-testable via Catch2 against synthetic EventStream inputs.

#pragma once

#include "types.hpp"
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

} // namespace xp_sherlock
