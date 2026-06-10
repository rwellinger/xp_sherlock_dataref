// Unit tests for the SDK-free rank_commands() scoring function. The XPLM
// command callback path is not covered here — that requires a running sim.
// What we DO cover is the scoring formula, anchor-latency math, and the
// "no Begin fires → no candidate" early-exit.

#include "catch_amalgamated.hpp"
#include "correlator.hpp"
#include "types.hpp"

using namespace xp_sherlock;

namespace
{

CommandFireEvent fire(uint8_t phase, float t)
{
    CommandFireEvent e{};
    e.phase = phase;
    e.t_sec = t;
    return e;
}

CommandEventStream stream(uint32_t idx, std::initializer_list<CommandFireEvent> evs)
{
    CommandEventStream s;
    s.command_idx = idx;
    s.events.assign(evs.begin(), evs.end());
    if (!s.events.empty())
        s.last_phase = s.events.back().phase;
    return s;
}

CommandRefMeta meta(int path_len)
{
    CommandRefMeta m;
    m.path_length = path_len;
    return m;
}

} // namespace

TEST_CASE("rank_commands: zero Begin fires produces no candidate", "[correlator][commands]")
{
    // A stream with only Continue/End events (no Begin) should be ignored —
    // Begin is the user-action instant and the only phase that counts.
    std::vector<CommandEventStream> streams = {
        stream(0, { fire(1 /*Continue*/, 0.1f), fire(2 /*End*/, 0.5f) }),
    };
    std::vector<CommandRefMeta>     metas   = { meta(20) };
    AnchorList                      anchors = { 0.0f };
    Hints                           hints;

    auto out = rank_commands(streams, metas, anchors, hints);
    REQUIRE(out.empty());
}

TEST_CASE("rank_commands: empty stream produces no candidate", "[correlator][commands]")
{
    std::vector<CommandEventStream> streams = { stream(0, {}) };
    std::vector<CommandRefMeta>     metas   = { meta(20) };
    AnchorList                      anchors;
    Hints                           hints;

    auto out = rank_commands(streams, metas, anchors, hints);
    REQUIRE(out.empty());
}

TEST_CASE("rank_commands: single clean fire at low latency scores high", "[correlator][commands]")
{
    // Battery toggle case: one Begin fire 50ms after the user-action anchor.
    // Expect: presence(+70) + low-latency(+25) + median<250(+10) + one-shot(+15)
    // - tiebreaker(0.01*20) = 120 - 0.2 = 119.8
    std::vector<CommandEventStream> streams = {
        stream(0, { fire(0 /*Begin*/, 0.05f) }),
    };
    std::vector<CommandRefMeta>     metas   = { meta(20) };
    AnchorList                      anchors = { 0.0f };
    Hints                           hints;
    hints.user_click_window_ms = 100.f;

    auto out = rank_commands(streams, metas, anchors, hints);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].kind == Kind::Command);
    REQUIRE(out[0].command_idx == 0);
    REQUIRE(out[0].fire_count == 1);
    REQUIRE(out[0].has_latency);
    REQUIRE(out[0].min_latency_ms == Catch::Approx(50.f));
    REQUIRE(out[0].score == Catch::Approx(119.8f));
}

TEST_CASE("rank_commands: chatty fires (held key spam) get demoted", "[correlator][commands]")
{
    // A command that fires 20 times within the record window — likely a
    // held-down key binding or autopilot tick, not a switch press. Should
    // still produce a candidate but well below a clean single-fire case.
    CommandEventStream s;
    s.command_idx = 0;
    for (int i = 0; i < 20; ++i)
        s.events.push_back(fire(0 /*Begin*/, 0.05f + 0.01f * static_cast<float>(i)));
    s.last_phase = 0;

    std::vector<CommandEventStream> streams = { s };
    std::vector<CommandRefMeta>     metas   = { meta(20) };
    AnchorList                      anchors = { 0.0f };
    Hints                           hints;
    hints.user_click_window_ms = 100.f;
    hints.expected_clicks      = 0; // bool mode → expected_cap = 6

    auto out = rank_commands(streams, metas, anchors, hints);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].fire_count == 20);
    // Penalty: 5 * (20 - 6) = 70. Bonuses: presence(70) + low-lat(25) + median<250(10) = 105.
    // Net = 105 - 70 = 35. minus tiebreaker(0.2) = 34.8.
    REQUIRE(out[0].score == Catch::Approx(34.8f));
}

TEST_CASE("rank_commands: rotary expected_clicks match adds bonus", "[correlator][commands]")
{
    // 4 Begin fires when expected_clicks=4 → rotary perfect match bonus(+10).
    // Note: 4 fires also exceeds the one-shot bonus check (which is fire_count==1).
    std::vector<CommandEventStream> streams = {
        stream(0, { fire(0, 0.05f), fire(0, 0.30f), fire(0, 0.55f), fire(0, 0.80f) }),
    };
    std::vector<CommandRefMeta>     metas   = { meta(30) };
    AnchorList                      anchors = { 0.0f, 0.25f, 0.50f, 0.75f };
    Hints                           hints;
    hints.user_click_window_ms = 100.f;
    hints.expected_clicks      = 4;

    auto out = rank_commands(streams, metas, anchors, hints);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].fire_count == 4);
    // presence(70) + low-lat(25) + median<250(10) + rotary-match(10) - tiebreaker(0.3) = 114.7
    REQUIRE(out[0].score == Catch::Approx(114.7f));
}

TEST_CASE("rank_commands: ranked output sorted by score descending", "[correlator][commands]")
{
    // Three streams: chatty (low score), clean one-shot (high score), zero fires (dropped).
    CommandEventStream chatty;
    chatty.command_idx = 0;
    for (int i = 0; i < 20; ++i)
        chatty.events.push_back(fire(0, 0.05f + 0.01f * static_cast<float>(i)));

    CommandEventStream clean;
    clean.command_idx = 1;
    clean.events      = { fire(0, 0.05f) };

    CommandEventStream silent;
    silent.command_idx = 2;
    // no events

    std::vector<CommandEventStream> streams = { chatty, clean, silent };
    std::vector<CommandRefMeta>     metas   = { meta(20), meta(20), meta(20) };
    AnchorList                      anchors = { 0.0f };
    Hints                           hints;
    hints.user_click_window_ms = 100.f;

    auto out = rank_commands(streams, metas, anchors, hints);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].command_idx == 1);          // clean one-shot wins
    REQUIRE(out[1].command_idx == 0);          // chatty trails
    REQUIRE(out[0].score > out[1].score);
}
