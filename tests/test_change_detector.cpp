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
#include "change_detector.hpp"
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

// Feed `n` identical samples; return the number of confirmed events.
int feed_n(ChangeDetector &d, SampleValue v, int n, int hold)
{
    int events = 0;
    for (int i = 0; i < n; ++i)
    {
        ChangeEvent e;
        if (d.feed(v, static_cast<uint32_t>(i), static_cast<float>(i) / 60.f, hold, e))
            ++events;
    }
    return events;
}

} // namespace

TEST_CASE("Float drift within epsilon → no event", "[change]")
{
    ChangeDetector d;
    d.init(RefType::Float, fv(0.5f));
    REQUIRE(feed_n(d, fv(0.5001f), 10, DEFAULT_HOLD_FRAMES) == 0);
}

TEST_CASE("Float jump and hold → one event", "[change]")
{
    ChangeDetector d;
    d.init(RefType::Float, fv(0.0f));
    int events = 0;
    ChangeEvent e;

    // First sample = jump (no event yet — hold count starts)
    REQUIRE(d.feed(fv(1.0f), 0, 0.f, DEFAULT_HOLD_FRAMES, e) == false);
    // Hold for HOLD_FRAMES additional samples → event fires on the Nth
    for (int i = 1; i < DEFAULT_HOLD_FRAMES; ++i)
    {
        if (d.feed(fv(1.0f), static_cast<uint32_t>(i), static_cast<float>(i) / 60.f, DEFAULT_HOLD_FRAMES, e))
            ++events;
    }
    REQUIRE(events == 1);
    REQUIRE(e.direction > 0);
    REQUIRE(e.to.f == Catch::Approx(1.0f));
}

TEST_CASE("Float jump then bounce back within hold → no event", "[change]")
{
    ChangeDetector d;
    d.init(RefType::Float, fv(0.0f));
    ChangeEvent e;
    // Jump up
    REQUIRE(d.feed(fv(1.0f), 0, 0.f, DEFAULT_HOLD_FRAMES, e) == false);
    // Bounce back to 0 within the hold window → candidate restarts, no event
    REQUIRE(d.feed(fv(0.0f), 1, 0.016f, DEFAULT_HOLD_FRAMES, e) == false);
    REQUIRE(d.feed(fv(0.0f), 2, 0.032f, DEFAULT_HOLD_FRAMES, e) == false);
    // After hold worth of stable-0 samples → confirm an event back to 0.
    // Note: this IS a legitimate event because the candidate snapshot was 0
    // (stable_value never advanced beyond 0). So it must NOT fire.
    REQUIRE(d.feed(fv(0.0f), 3, 0.048f, DEFAULT_HOLD_FRAMES, e) == false);
    REQUIRE(d.feed(fv(0.0f), 4, 0.064f, DEFAULT_HOLD_FRAMES, e) == false);
    REQUIRE(d.feed(fv(0.0f), 5, 0.080f, DEFAULT_HOLD_FRAMES, e) == false);
}

TEST_CASE("Int toggle 0→1→0→1 → 3 alternating events", "[change]")
{
    ChangeDetector d;
    d.init(RefType::Int, iv(0));
    std::vector<ChangeEvent> evs;

    auto run_until_stable = [&](int target) {
        for (int i = 0; i < 10; ++i)
        {
            ChangeEvent e;
            if (d.feed(iv(target), static_cast<uint32_t>(i), static_cast<float>(i) / 60.f, DEFAULT_HOLD_FRAMES, e))
                evs.push_back(e);
        }
    };

    run_until_stable(1);
    run_until_stable(0);
    run_until_stable(1);

    REQUIRE(evs.size() == 3);
    REQUIRE(evs[0].direction == +1);
    REQUIRE(evs[1].direction == -1);
    REQUIRE(evs[2].direction == +1);
}

TEST_CASE("Monotonic staircase 0→1→2→3→4 → 4 same-direction events", "[change]")
{
    ChangeDetector d;
    d.init(RefType::Int, iv(0));
    std::vector<ChangeEvent> evs;
    for (int target = 1; target <= 4; ++target)
    {
        for (int i = 0; i < DEFAULT_HOLD_FRAMES + 1; ++i)
        {
            ChangeEvent e;
            uint32_t    frame = static_cast<uint32_t>(target * 100 + i);
            float       t     = static_cast<float>(frame) / 60.f;
            if (d.feed(iv(target), frame, t, DEFAULT_HOLD_FRAMES, e))
                evs.push_back(e);
        }
    }
    REQUIRE(evs.size() == 4);
    for (const auto &e : evs)
        REQUIRE(e.direction == +1);
}
