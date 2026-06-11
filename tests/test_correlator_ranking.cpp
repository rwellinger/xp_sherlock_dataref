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

#include "catch_amalgamated.hpp"
#include "correlator.hpp"
#include "types.hpp"

using namespace xp_sherlock;

namespace
{

SampleValue iv(int v)
{
    SampleValue s{};
    s.i = v;
    return s;
}

SampleValue fv(float v)
{
    SampleValue s{};
    s.f = v;
    return s;
}

ChangeEvent ev_int(int from, int to, float t)
{
    ChangeEvent e{};
    e.from      = iv(from);
    e.to        = iv(to);
    e.t_sec     = t;
    e.direction = (to > from) ? 1 : (to < from ? -1 : 0);
    return e;
}

ChangeEvent ev_float(float from, float to, float t)
{
    ChangeEvent e{};
    e.from      = fv(from);
    e.to        = fv(to);
    e.t_sec     = t;
    e.direction = (to > from) ? 1 : (to < from ? -1 : 0);
    return e;
}

EventStream stream_int(uint32_t idx, std::initializer_list<ChangeEvent> evs)
{
    EventStream s;
    s.logical_ref_idx = idx;
    s.type            = RefType::Int;
    s.events.assign(evs.begin(), evs.end());
    if (!s.events.empty())
    {
        s.baseline_value = s.events.front().from;
        s.current_value  = s.events.back().to;
    }
    return s;
}

EventStream stream_float(uint32_t idx, std::initializer_list<ChangeEvent> evs)
{
    EventStream s;
    s.logical_ref_idx = idx;
    s.type            = RefType::Float;
    s.events.assign(evs.begin(), evs.end());
    if (!s.events.empty())
    {
        s.baseline_value = s.events.front().from;
        s.current_value  = s.events.back().to;
    }
    return s;
}

} // namespace

TEST_CASE("Bidirectional float beats one-shot int (Fuel Cut signature)", "[correlator]")
{
    // Mimics the T-6A Fuel Cut case: mixture_ratio_all float goes 0→1→0→1 in
    // sync with switch clicks; a different ref makes only a single one-shot
    // change with no return.
    EventStream fuel_cut = stream_float(/*idx=*/42, {
        ev_float(0.f, 1.f, 0.05f),
        ev_float(1.f, 0.f, 0.50f),
        ev_float(0.f, 1.f, 1.00f),
    });
    EventStream one_shot = stream_int(/*idx=*/7, {
        ev_int(0, 1, 0.20f),
    });

    std::vector<RefMeta> metas(2);
    metas[0].is_writable = true;
    metas[0].path_length = 40;
    metas[0].type        = RefType::Float;
    metas[1].is_writable = false;
    metas[1].path_length = 30;
    metas[1].type        = RefType::Int;

    AnchorList anchors{0.0f, 0.45f, 0.95f};

    Hints h;
    h.expect_bidirectional = true;
    h.expected_clicks      = 3;

    auto out = rank({fuel_cut, one_shot}, metas, anchors, h);
    REQUIRE(out.size() >= 1);
    REQUIRE(out.front().logical_ref_idx == 42);
    REQUIRE(out.front().bidirectional);
    REQUIRE(out.front().score >= 100.f);
}

TEST_CASE("Staircase matching expected_clicks → top score", "[correlator]")
{
    EventStream rotary = stream_int(/*idx=*/9, {
        ev_int(0, 1, 0.10f),
        ev_int(1, 2, 0.30f),
        ev_int(2, 3, 0.50f),
        ev_int(3, 4, 0.70f),
    });

    std::vector<RefMeta> metas(1);
    metas[0].type        = RefType::Int;
    metas[0].is_writable = true;
    metas[0].path_length = 30;

    AnchorList anchors{0.05f, 0.25f, 0.45f, 0.65f};
    Hints h;
    h.expect_bidirectional = false;
    h.expected_clicks      = 4;

    auto out = rank({rotary}, metas, anchors, h);
    REQUIRE(out.size() == 1);
    REQUIRE(out.front().staircase_steps == 4);
    REQUIRE(out.front().score >= 60.f);
}

TEST_CASE("Low-latency event ranks above same-pattern delayed event", "[correlator]")
{
    EventStream fast  = stream_int(/*idx=*/1, {ev_int(0, 1, 0.05f), ev_int(1, 0, 0.50f)});
    EventStream slow  = stream_int(/*idx=*/2, {ev_int(0, 1, 0.60f), ev_int(1, 0, 1.30f)});

    std::vector<RefMeta> metas(2);
    for (auto &m : metas)
    {
        m.is_writable = true;
        m.path_length = 30;
        m.type        = RefType::Int;
    }

    AnchorList anchors{0.00f, 0.50f}; // user clicks at 0.0 and 0.5
    Hints      h;
    h.expect_bidirectional = true;
    h.expected_clicks      = 0;
    h.user_click_window_ms = 100.f;

    auto out = rank({fast, slow}, metas, anchors, h);
    REQUIRE(out.size() >= 2);
    REQUIRE(out.front().logical_ref_idx == 1); // fast wins
    REQUIRE(out.front().score > out.back().score);
}

TEST_CASE("Empty streams → no candidates (UI shows 'likely Command-based' path)", "[correlator]")
{
    std::vector<EventStream> streams; // empty
    std::vector<RefMeta>     metas;
    AnchorList               anchors;
    Hints                    h;
    auto out = rank(streams, metas, anchors, h);
    REQUIRE(out.empty());
}

TEST_CASE("Streams that yielded no confirmed events → dropped (score==0)", "[correlator]")
{
    EventStream empty_stream;
    empty_stream.logical_ref_idx = 5;
    empty_stream.type            = RefType::Int;
    // No events.

    std::vector<RefMeta> metas(1);
    metas[0].is_writable = true;
    metas[0].path_length = 30;
    metas[0].type        = RefType::Int;

    auto out = rank({empty_stream}, metas, {}, Hints{});
    REQUIRE(out.empty());
}

TEST_CASE("Tie-break: shorter path wins over longer when scores are otherwise equal", "[correlator]")
{
    EventStream a = stream_int(/*idx=*/1, {ev_int(0, 1, 0.10f), ev_int(1, 0, 0.50f), ev_int(0, 1, 0.90f)});
    EventStream b = stream_int(/*idx=*/2, {ev_int(0, 1, 0.10f), ev_int(1, 0, 0.50f), ev_int(0, 1, 0.90f)});

    std::vector<RefMeta> metas(2);
    metas[0].is_writable = true;
    metas[0].path_length = 80; // longer
    metas[0].type        = RefType::Int;
    metas[1].is_writable = true;
    metas[1].path_length = 30; // shorter — should rank higher on tie
    metas[1].type        = RefType::Int;

    AnchorList anchors{0.05f, 0.45f, 0.85f};
    Hints      h;
    h.expect_bidirectional = true;
    h.expected_clicks      = 3;

    auto out = rank({a, b}, metas, anchors, h);
    REQUIRE(out.size() == 2);
    REQUIRE(out.front().logical_ref_idx == 2);
}

TEST_CASE("is_bidirectional helper exposed for direct testing", "[correlator]")
{
    EventStream up_only = stream_int(/*idx=*/1, {ev_int(0, 1, 0.1f), ev_int(1, 2, 0.2f)});
    EventStream both    = stream_int(/*idx=*/2, {ev_int(0, 1, 0.1f), ev_int(1, 0, 0.2f)});
    REQUIRE_FALSE(is_bidirectional(up_only));
    REQUIRE(is_bidirectional(both));
}

TEST_CASE("staircase_steps helper counts longest monotonic run", "[correlator]")
{
    EventStream s = stream_int(/*idx=*/1, {
        ev_int(0, 1, 0.1f),
        ev_int(1, 2, 0.2f),
        ev_int(2, 3, 0.3f),
        ev_int(3, 2, 0.4f), // breaks the run
    });
    REQUIRE(staircase_steps(s) == 3);
}

TEST_CASE("Discrete-like float (0.0/1.0 switch) beats fractional float (sensor)", "[correlator]")
{
    // Switch-like: float that toggles between exactly 0.0 and 1.0, three flips.
    EventStream switch_like = stream_float(/*idx=*/1, {
        ev_float(0.0f, 1.0f, 0.10f),
        ev_float(1.0f, 0.0f, 0.45f),
        ev_float(0.0f, 1.0f, 0.80f),
    });
    // Sensor-like: fractional voltage that wobbles in the same direction
    // pattern at similar latencies (e.g. bus voltage rising/falling in
    // response to the switch — a downstream effect).
    EventStream sensor_like = stream_float(/*idx=*/2, {
        ev_float(24.13f, 27.41f, 0.12f),
        ev_float(27.41f, 24.27f, 0.47f),
        ev_float(24.27f, 27.55f, 0.82f),
    });

    std::vector<RefMeta> metas(2);
    for (auto &m : metas)
    {
        m.is_writable = true;
        m.path_length = 40;
        m.type        = RefType::Float;
    }

    AnchorList anchors{0.05f, 0.40f, 0.75f};
    Hints      h;
    h.expect_bidirectional = true;
    h.expected_clicks      = 3;

    auto out = rank({switch_like, sensor_like}, metas, anchors, h);
    REQUIRE(out.size() == 2);
    REQUIRE(out.front().logical_ref_idx == 1); // discrete-like switch wins
    REQUIRE(out.front().score > out.back().score + 5.f); // meaningful margin
}
