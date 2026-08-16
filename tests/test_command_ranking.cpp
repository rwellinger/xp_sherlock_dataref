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

// Mark a stream as baseline noise, as command_recorder would after seeing it
// fire while the cockpit was idle.
CommandEventStream &mute(CommandEventStream &s, int baseline_fires)
{
    s.muted                = true;
    s.baseline_begin_fires = baseline_fires;
    return s;
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
    // Expect: presence(+70) + coverage 1/1(+40) + low-latency(+25)
    // + median<250(+10) + one-shot(+15) - tiebreaker(0.01*20) = 160 - 0.2 = 159.8
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
    REQUIRE(out[0].anchors_hit == 1);
    REQUIRE(out[0].anchors_total == 1);
    REQUIRE(out[0].orphan_events == 0);
    REQUIRE(out[0].score == Catch::Approx(159.8f));
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
    // All 20 fires fall inside the single anchor's 3 s window, so coverage is
    // 1/1 with no orphans — coverage alone cannot separate a chatterer from a
    // clean hit when the user only placed one anchor. That is precisely why
    // more actuations sharpen the ranking: the chatty penalty still demotes it.
    REQUIRE(out[0].anchors_hit == 1);
    REQUIRE(out[0].orphan_events == 0);
    // Penalty: 5 * (20 - 6) = 70. Bonuses: presence(70) + coverage(40)
    // + low-lat(25) + median<250(10) = 145. Net = 145 - 70 - tiebreaker(0.2) = 74.8.
    REQUIRE(out[0].score == Catch::Approx(74.8f));
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
    // One fire per anchor, each inside its own window — the ideal signature.
    REQUIRE(out[0].anchors_hit == 4);
    REQUIRE(out[0].anchors_total == 4);
    REQUIRE(out[0].orphan_events == 0);
    // presence(70) + coverage 4/4(40) + low-lat(25) + median<250(10)
    // + rotary-match(10) - tiebreaker(0.3) = 154.7
    REQUIRE(out[0].score == Catch::Approx(154.7f));
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

// ── Baseline noise muting / auto-unmute ─────────────────────────────────────

TEST_CASE("rank_commands: unmuted streams are unaffected", "[correlator][commands][mute]")
{
    // Regression guard: a stream that never fired during baseline must rank
    // exactly as it did before muting existed.
    std::vector<CommandEventStream> streams = { stream(0, { fire(0, 0.05f) }) };
    std::vector<CommandRefMeta>     metas   = { meta(20) };
    AnchorList                      anchors = { 0.0f };
    Hints                           hints;
    hints.user_click_window_ms = 100.f;

    auto out = rank_commands(streams, metas, anchors, hints);
    REQUIRE(out.size() == 1);
    REQUIRE_FALSE(out[0].is_muted);
    REQUIRE_FALSE(out[0].auto_unmuted);
    REQUIRE(out[0].baseline_fires == 0);
    REQUIRE(out[0].score == Catch::Approx(159.8f));
}

TEST_CASE("rank_commands: constant chatterer stays muted", "[correlator][commands][mute]")
{
    // The AW139 case: sim/autopilot/disconnect/... fired throughout baseline and
    // keeps firing across the whole record window. Some of those fires land near
    // an anchor by chance (min latency is tiny), but the MEDIAN stays large — and
    // the fire count blows past the click budget. Must stay muted.
    CommandEventStream s;
    s.command_idx = 0;
    for (int i = 0; i < 20; ++i)
        s.events.push_back(fire(0, 0.05f + 0.25f * static_cast<float>(i)));
    mute(s, 14);

    std::vector<CommandEventStream> streams = { s };
    std::vector<CommandRefMeta>     metas   = { meta(20) };
    AnchorList                      anchors = { 0.0f };
    Hints                           hints;
    hints.user_click_window_ms = 100.f;
    hints.expected_clicks      = 3;

    auto out = rank_commands(streams, metas, anchors, hints);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].is_muted);
    REQUIRE_FALSE(out[0].auto_unmuted);
    REQUIRE(out[0].baseline_fires == 14);
    // Muting must not touch the score — an unmute in the UI has to reveal a
    // fully ranked row, not an empty shell.
    REQUIRE(out[0].fire_count == 20);
}

TEST_CASE("rank_commands: muted command firing on the anchors is brought back", "[correlator][commands][mute]")
{
    // The switch the user is hunting happened to fire once during baseline, so
    // it got muted. During Record it fires exactly three times, each ~50 ms
    // after an "I Acted Now" anchor. That is purposeful behaviour, not noise.
    CommandEventStream s = stream(0, { fire(0, 0.05f), fire(0, 1.05f), fire(0, 2.05f) });
    mute(s, 1);

    std::vector<CommandEventStream> streams = { s };
    std::vector<CommandRefMeta>     metas   = { meta(20) };
    AnchorList                      anchors = { 0.0f, 1.0f, 2.0f };
    Hints                           hints;
    hints.user_click_window_ms = 100.f;
    hints.expected_clicks      = 3;

    auto out = rank_commands(streams, metas, anchors, hints);
    REQUIRE(out.size() == 1);
    REQUIRE_FALSE(out[0].is_muted);
    REQUIRE(out[0].auto_unmuted);
    REQUIRE(out[0].baseline_fires == 1);
    REQUIRE(out[0].median_latency_ms == Catch::Approx(50.f));
}

TEST_CASE("rank_commands: no anchors means no auto-unmute", "[correlator][commands][mute]")
{
    // Without an "I Acted Now" stamp there is no evidence that the fires line up
    // with anything the user did. Staying muted is the honest verdict — the user
    // can always unmute manually.
    CommandEventStream s = stream(0, { fire(0, 0.05f), fire(0, 1.05f), fire(0, 2.05f) });
    mute(s, 1);

    std::vector<CommandEventStream> streams = { s };
    std::vector<CommandRefMeta>     metas   = { meta(20) };
    AnchorList                      anchors; // user never clicked "I Acted Now"
    Hints                           hints;
    hints.user_click_window_ms = 100.f;
    hints.expected_clicks      = 3;

    auto out = rank_commands(streams, metas, anchors, hints);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].is_muted);
    REQUIRE_FALSE(out[0].auto_unmuted);
}

TEST_CASE("rank_commands: fire budget blocks auto-unmute", "[correlator][commands][mute]")
{
    // Every fire sits right on an anchor, so the median latency is perfect —
    // but 7 fires for 3 expected clicks exceeds the budget (max(3*2, 4) = 6).
    // This is the guard that keeps a fast chatterer from sneaking back in.
    CommandEventStream s;
    s.command_idx = 0;
    AnchorList anchors;
    for (int i = 0; i < 7; ++i)
    {
        anchors.push_back(1.0f * static_cast<float>(i));
        s.events.push_back(fire(0, 1.0f * static_cast<float>(i) + 0.05f));
    }
    mute(s, 5);

    std::vector<CommandEventStream> streams = { s };
    std::vector<CommandRefMeta>     metas   = { meta(20) };
    Hints                           hints;
    hints.user_click_window_ms = 100.f;
    hints.expected_clicks      = 3;

    auto out = rank_commands(streams, metas, anchors, hints);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].fire_count == 7);
    REQUIRE(out[0].median_latency_ms == Catch::Approx(50.f));
    REQUIRE(out[0].is_muted);
    REQUIRE_FALSE(out[0].auto_unmuted);
}

// ── Anchor coverage ─────────────────────────────────────────────────────────

TEST_CASE("anchor_coverage: one response per anchor is full coverage", "[correlator][coverage]")
{
    AnchorList anchors = { 0.0f, 1.0f, 2.0f };
    // One event shortly after each anchor — the ideal target signature.
    auto cov = anchor_coverage({ 0.05f, 1.05f, 2.05f }, anchors);
    REQUIRE(cov.anchors_total == 3);
    REQUIRE(cov.anchors_hit == 3);
    REQUIRE(cov.orphan_events == 0);
}

TEST_CASE("anchor_coverage: events before the first anchor are orphans", "[correlator][coverage]")
{
    AnchorList anchors = { 1.0f };
    auto       cov     = anchor_coverage({ 0.2f, 0.5f, 1.05f }, anchors);
    REQUIRE(cov.anchors_hit == 1);
    REQUIRE(cov.orphan_events == 2); // nothing was announced yet when they fired
}

TEST_CASE("anchor_coverage: a later anchor closes the previous window", "[correlator][coverage]")
{
    // Anchors 0.5 s apart: an event at 0.6 belongs to anchor 0.5, not to
    // anchor 0.0 — otherwise consecutive actuations would share credit.
    AnchorList anchors = { 0.0f, 0.5f };
    auto       cov     = anchor_coverage({ 0.1f, 0.6f }, anchors);
    REQUIRE(cov.anchors_hit == 2);
    REQUIRE(cov.orphan_events == 0);
}

TEST_CASE("anchor_coverage: a straggler past the response window is an orphan", "[correlator][coverage]")
{
    // Single anchor, so the window runs the full MAX_RESPONSE_S (3 s). An event
    // 4 s later cannot plausibly be the response to that click.
    AnchorList anchors = { 0.0f };
    auto       cov     = anchor_coverage({ 0.1f, 4.0f }, anchors);
    REQUIRE(cov.anchors_hit == 1);
    REQUIRE(cov.orphan_events == 1);
}

TEST_CASE("anchor_coverage: no anchors yields no signal either way", "[correlator][coverage]")
{
    // Without anchors there is nothing to measure against. Counting every event
    // as an orphan would punish candidates for what the user did not record.
    AnchorList anchors;
    auto       cov = anchor_coverage({ 0.1f, 0.2f, 0.3f }, anchors);
    REQUIRE(cov.anchors_total == 0);
    REQUIRE(cov.anchors_hit == 0);
    REQUIRE(cov.orphan_events == 0);
}

TEST_CASE("rank_commands: partial coverage ranks below full coverage", "[correlator][coverage]")
{
    // This is the case that repetition buys you. Both commands fire, both are
    // close to *an* anchor — but only one answers every actuation. With a single
    // anchor these would be indistinguishable.
    AnchorList anchors = { 0.0f, 1.0f, 2.0f, 3.0f };

    CommandEventStream full = stream(0, { fire(0, 0.05f), fire(0, 1.05f), fire(0, 2.05f), fire(0, 3.05f) });
    CommandEventStream partial = stream(1, { fire(0, 0.05f), fire(0, 2.05f) });

    std::vector<CommandEventStream> streams = { full, partial };
    std::vector<CommandRefMeta>     metas   = { meta(20), meta(20) };
    Hints                           hints;
    hints.user_click_window_ms = 100.f;
    hints.expected_clicks      = 4;

    auto out = rank_commands(streams, metas, anchors, hints);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].command_idx == 0); // full coverage wins
    REQUIRE(out[0].anchors_hit == 4);
    REQUIRE(out[1].anchors_hit == 2);
    REQUIRE(out[0].score > out[1].score);
}

// ── Command → DataRef coupling ──────────────────────────────────────────────

namespace
{

// A confirmed change whose onset (what the link analysis reads) is distinct
// from its confirmation time, mirroring the HOLD_FRAMES delay in the real
// detector. Using the same value for both would hide onset/confirm mix-ups.
ChangeEvent change(float onset_t, int8_t dir)
{
    ChangeEvent e{};
    e.onset_t_sec = onset_t;
    e.t_sec       = onset_t + 0.067f; // ~4 frames at 60 fps
    e.direction   = dir;
    return e;
}

EventStream ref_stream(uint32_t logical_idx, std::initializer_list<ChangeEvent> evs)
{
    EventStream s;
    s.logical_ref_idx = logical_idx;
    s.type            = RefType::Int;
    s.events.assign(evs.begin(), evs.end());
    return s;
}

} // namespace

TEST_CASE("link_commands_to_refs: ref reacting to every fire is linked", "[correlator][link]")
{
    // The battery case: batteries_toggle fires three times, battery_on flips
    // ~20 ms after each one. That ordering says the command is the actuator and
    // the ref is the status output.
    std::vector<CommandEventStream> cmds = {
        stream(0, { fire(0, 1.00f), fire(0, 2.00f), fire(0, 3.00f) }),
    };
    std::vector<EventStream> refs = {
        ref_stream(42, { change(1.02f, +1), change(2.02f, -1), change(3.02f, +1) }),
    };

    auto links = link_commands_to_refs(cmds, refs);
    REQUIRE(links.size() == 1);
    REQUIRE(links[0].command_idx == 0);
    REQUIRE(links[0].logical_ref_idx == 42);
    REQUIRE(links[0].fires_matched == 3);
    REQUIRE(links[0].fires_total == 3);
    REQUIRE(links[0].median_delay_ms == Catch::Approx(20.f).margin(1.0));
}

TEST_CASE("link_commands_to_refs: a majority of fires still links, and says so", "[correlator][link]")
{
    // 2 of 3 answered. Demanding unanimity scaled badly once users were asked to
    // actuate five or more times: one brief press or one stray Begin destroyed
    // the whole finding. A majority is reported as a link, but honestly — the
    // ratio is preserved so the UI shows 2/3 rather than implying perfection.
    std::vector<CommandEventStream> cmds = {
        stream(0, { fire(0, 1.00f), fire(0, 2.00f), fire(0, 3.00f) }),
    };
    std::vector<EventStream> refs = {
        ref_stream(42, { change(1.02f, +1), change(3.02f, +1) }),
    };

    auto links = link_commands_to_refs(cmds, refs);
    REQUIRE(links.size() == 1);
    REQUIRE(links[0].fires_matched == 2);
    REQUIRE(links[0].fires_total == 3);
}

TEST_CASE("link_commands_to_refs: a small minority of fires is still rejected", "[correlator][link]")
{
    // 1 of 4 is coincidence, not causation — something else moved this ref and
    // happened to overlap one fire. The majority rule must not become a
    // free pass for anything that twitched once.
    std::vector<CommandEventStream> cmds = {
        stream(0, { fire(0, 1.00f), fire(0, 2.00f), fire(0, 3.00f), fire(0, 4.00f) }),
    };
    std::vector<EventStream> refs = { ref_stream(42, { change(1.02f, +1) }) };

    auto links = link_commands_to_refs(cmds, refs);
    REQUIRE(links.empty());
}

TEST_CASE("link_commands_to_refs: an onset a hair before the fire still counts", "[correlator][link]")
{
    // Command timestamps come from the command callback, ref onsets from the
    // flight loop. Depending on their order within a frame, a genuinely caused
    // onset can land marginally BEFORE the fire. Rejecting that would drop real
    // pairs for a clock artefact.
    std::vector<CommandEventStream> cmds = { stream(0, { fire(0, 1.000f), fire(0, 2.000f) }) };
    std::vector<EventStream>        refs = { ref_stream(42, { change(0.995f, +1), change(1.995f, -1) }) };

    auto links = link_commands_to_refs(cmds, refs);
    REQUIRE(links.size() == 1);
    REQUIRE(links[0].fires_matched == 2);
    REQUIRE(links[0].median_delay_ms == Catch::Approx(0.f).margin(0.001));
}

TEST_CASE("link_commands_to_refs: a slow follower is not linked", "[correlator][link]")
{
    // 400 ms after the fire is far outside LINK_WINDOW_MS — that is a downstream
    // effect settling (or an unrelated action), not the switch's own state ref.
    std::vector<CommandEventStream> cmds = { stream(0, { fire(0, 1.00f) }) };
    std::vector<EventStream>        refs = { ref_stream(42, { change(1.40f, +1) }) };

    auto links = link_commands_to_refs(cmds, refs);
    REQUIRE(links.empty());
}

TEST_CASE("link_commands_to_refs: a ref moving before the fire is not linked", "[correlator][link]")
{
    // Causality has a direction: something that already moved cannot have been
    // caused by a command that fires afterwards.
    std::vector<CommandEventStream> cmds = { stream(0, { fire(0, 1.00f) }) };
    std::vector<EventStream>        refs = { ref_stream(42, { change(0.90f, +1) }) };

    auto links = link_commands_to_refs(cmds, refs);
    REQUIRE(links.empty());
}

TEST_CASE("link_commands_to_refs: tighter coupling ranks first", "[correlator][link]")
{
    // Both refs follow every fire, but one reacts in the next frame and the
    // other 100 ms later — the immediate one is the switch's own state ref, the
    // laggard is a downstream consequence of it.
    std::vector<CommandEventStream> cmds = { stream(0, { fire(0, 1.00f), fire(0, 2.00f) }) };
    std::vector<EventStream>        refs = {
        ref_stream(10, { change(1.10f, +1), change(2.10f, -1) }), // 100 ms
        ref_stream(20, { change(1.01f, +1), change(2.01f, -1) }), // 10 ms
    };

    auto links = link_commands_to_refs(cmds, refs);
    REQUIRE(links.size() == 2);
    REQUIRE(links[0].logical_ref_idx == 20); // tightest first
    REQUIRE(links[1].logical_ref_idx == 10);
}

TEST_CASE("link_commands_to_refs: commands that never fired produce no links", "[correlator][link]")
{
    std::vector<CommandEventStream> cmds = { stream(0, {}) };
    std::vector<EventStream>        refs = { ref_stream(42, { change(1.0f, +1) }) };

    auto links = link_commands_to_refs(cmds, refs);
    REQUIRE(links.empty());
}

// ── Separating the state ref from its cascade ───────────────────────────────

TEST_CASE("path_affinity: related electrical names score, unrelated ones do not", "[correlator][link]")
{
    // Singular/plural drift must still match: "batteries" vs "battery".
    REQUIRE(path_affinity("sim/electrical/batteries_toggle", "sim/cockpit/electrical/battery_on") >= 2);
    // A third-party warning lamp shares nothing meaningful with the command.
    REQUIRE(path_affinity("sim/electrical/batteries_toggle", "xrotors/aw139/warn_light_7") == 0);
    // Short tokens like "sim" and "on" must not manufacture a match.
    REQUIRE(path_affinity("sim/foo/on", "sim/bar/on") == 0);
}

TEST_CASE("classify_links: state ref wins over the cascade it triggers", "[correlator][link]")
{
    // The AW139 case. Firing the battery master flips its own state ref AND
    // lights a pile of unrelated warning lamps in the very same frame. Timing
    // cannot separate them, and the lamps can even outscore the state ref — so
    // name affinity has to carry the decision.
    std::vector<CommandRefLink> links;
    auto add = [&](uint32_t ref, float delay)
    {
        CommandRefLink l;
        l.command_idx     = 0;
        l.logical_ref_idx = ref;
        l.fires_matched   = 3;
        l.fires_total     = 3;
        l.median_delay_ms = delay;
        links.push_back(l);
    };
    add(1, 16.f); // battery_on
    add(2, 16.f); // warning lamp, identical timing
    add(3, 16.f); // another warning lamp

    auto facts_of = [](uint32_t idx) -> LinkRefFacts
    {
        LinkRefFacts f;
        // Deliberately give a lamp the HIGHEST score, so the test fails if the
        // implementation quietly falls back to ranking by score alone.
        f.score = (idx == 2) ? 150.f : (idx == 1 ? 120.f : 110.f);
        // All three are discrete lamps/states — value shape cannot separate
        // them either, which forces the decision onto name affinity.
        f.discrete      = true;
        f.bidirectional = true;
        if (idx == 1)
            f.path = "sim/cockpit/electrical/battery_on";
        else if (idx == 2)
            f.path = "xrotors/aw139/warn_light_7";
        else
            f.path = "xrotors/aw139/caution_panel_3";
        return f;
    };
    auto cmd_of = [](uint32_t) -> std::string { return "sim/electrical/batteries_toggle"; };

    classify_links(links, facts_of, cmd_of);

    int primaries = 0;
    for (const auto &l : links)
        if (l.primary)
            ++primaries;
    REQUIRE(primaries == 1);              // exactly one answer per command
    REQUIRE(links.front().primary);       // primaries sort first
    REQUIRE(links.front().logical_ref_idx == 1); // ...and it is the battery ref
}

TEST_CASE("classify_links: one primary per command", "[correlator][link]")
{
    // Two independent commands must each keep their own answer.
    std::vector<CommandRefLink> links;
    auto add = [&](uint32_t cmd, uint32_t ref)
    {
        CommandRefLink l;
        l.command_idx     = cmd;
        l.logical_ref_idx = ref;
        l.fires_matched   = 2;
        l.fires_total     = 2;
        l.median_delay_ms = 16.f;
        links.push_back(l);
    };
    add(0, 1);
    add(0, 2);
    add(1, 3);

    auto facts_of = [](uint32_t idx) -> LinkRefFacts
    {
        LinkRefFacts f;
        f.score         = (idx == 1 || idx == 3) ? 100.f : 50.f;
        f.discrete      = true;
        f.bidirectional = true;
        if (idx == 1)
            f.path = "sim/cockpit/electrical/battery_on";
        else if (idx == 2)
            f.path = "xrotors/aw139/lamp";
        else
            f.path = "sim/cockpit/electrical/generator_on";
        return f;
    };
    auto cmd_of = [](uint32_t cmd) -> std::string
    {
        return cmd == 0 ? "sim/electrical/batteries_toggle" : "sim/electrical/generator_toggle";
    };

    classify_links(links, facts_of, cmd_of);

    int p0 = 0, p1 = 0;
    for (const auto &l : links)
    {
        if (!l.primary)
            continue;
        (l.command_idx == 0 ? p0 : p1)++;
    }
    REQUIRE(p0 == 1);
    REQUIRE(p1 == 1);
}

TEST_CASE("classify_links: falls back to score when no name overlap exists", "[correlator][link]")
{
    // Fully custom naming (nothing shared with the command). Affinity is 0 all
    // round, so the ranking score must decide rather than the list order.
    std::vector<CommandRefLink> links;
    for (uint32_t ref = 1; ref <= 3; ++ref)
    {
        CommandRefLink l;
        l.command_idx     = 0;
        l.logical_ref_idx = ref;
        l.fires_matched   = 2;
        l.fires_total     = 2;
        l.median_delay_ms = 16.f;
        links.push_back(l);
    }
    auto facts_of = [](uint32_t idx) -> LinkRefFacts
    {
        LinkRefFacts f;
        f.score         = (idx == 3) ? 200.f : 50.f;
        f.discrete      = true;
        f.bidirectional = true;
        f.path          = "xrotors/aw139/thing_" + std::to_string(idx);
        return f;
    };
    auto cmd_of = [](uint32_t) -> std::string { return "sim/electrical/batteries_toggle"; };

    classify_links(links, facts_of, cmd_of);
    REQUIRE(links.front().primary);
    REQUIRE(links.front().logical_ref_idx == 3);
}

TEST_CASE("classify_links: causal order decides when names share nothing", "[correlator][link]")
{
    // The generic case: an aircraft that names everything in its own namespace,
    // so affinity is zero for every candidate and cannot decide anything. What
    // remains is physics — the switch's own state ref moves first, the lamps it
    // drives follow. This is the guard against the selection being only as good
    // as the naming convention.
    std::vector<CommandRefLink> links;
    auto add = [&](uint32_t ref, float delay)
    {
        CommandRefLink l;
        l.command_idx     = 0;
        l.logical_ref_idx = ref;
        l.fires_matched   = 4;
        l.fires_total     = 4;
        l.median_delay_ms = delay;
        links.push_back(l);
    };
    add(1, 90.f); // downstream lamp
    add(2, 8.f);  // the state ref — reacted first
    add(3, 75.f); // another downstream effect

    auto facts_of = [](uint32_t idx) -> LinkRefFacts
    {
        LinkRefFacts f;
        // The laggards even score HIGHER, so the test fails if promptness is
        // ignored in favour of the ranking score.
        f.score         = (idx == 2) ? 100.f : 140.f;
        f.discrete      = true;
        f.bidirectional = true;
        f.path          = "acme/heavyjet/sys/node_" + std::to_string(idx);
        return f;
    };
    auto cmd_of = [](uint32_t) -> std::string { return "acme/heavyjet/cmd/master_switch"; };

    classify_links(links, facts_of, cmd_of);
    REQUIRE(links.front().primary);
    REQUIRE(links.front().logical_ref_idx == 2);
}

TEST_CASE("classify_links: a sensor curve loses to a switch-shaped ref", "[correlator][link]")
{
    // Same delay, same names, same score — but one holds integer switch states
    // and the other is a continuous value that merely moved. The discrete one is
    // the state ref; the analogue one is a consequence.
    std::vector<CommandRefLink> links;
    for (uint32_t ref = 1; ref <= 2; ++ref)
    {
        CommandRefLink l;
        l.command_idx     = 0;
        l.logical_ref_idx = ref;
        l.fires_matched   = 3;
        l.fires_total     = 3;
        l.median_delay_ms = 16.f;
        links.push_back(l);
    }
    auto facts_of = [](uint32_t idx) -> LinkRefFacts
    {
        LinkRefFacts f;
        f.score         = 100.f;
        f.discrete      = (idx == 2);
        f.bidirectional = true;
        f.path          = "acme/heavyjet/thing_" + std::to_string(idx);
        return f;
    };
    auto cmd_of = [](uint32_t) -> std::string { return "acme/heavyjet/toggle"; };

    classify_links(links, facts_of, cmd_of);
    REQUIRE(links.front().primary);
    REQUIRE(links.front().logical_ref_idx == 2);
}

TEST_CASE("classify_links: a weak match loses to a consistent one", "[correlator][link]")
{
    // Everything else equal, the ref that answered every actuation beats the one
    // that only managed half.
    std::vector<CommandRefLink> links;
    auto add = [&](uint32_t ref, int matched)
    {
        CommandRefLink l;
        l.command_idx     = 0;
        l.logical_ref_idx = ref;
        l.fires_matched   = matched;
        l.fires_total     = 6;
        l.median_delay_ms = 16.f;
        links.push_back(l);
    };
    add(1, 3);
    add(2, 6);

    auto facts_of = [](uint32_t) -> LinkRefFacts
    {
        LinkRefFacts f;
        f.score         = 100.f;
        f.discrete      = true;
        f.bidirectional = true;
        f.path          = "acme/heavyjet/node";
        return f;
    };
    auto cmd_of = [](uint32_t) -> std::string { return "acme/heavyjet/toggle"; };

    classify_links(links, facts_of, cmd_of);
    REQUIRE(links.front().primary);
    REQUIRE(links.front().logical_ref_idx == 2);
}

// ── Causal ordering among DataRefs (no command involved) ────────────────────

TEST_CASE("median_onset_lag_ms: measures the first reaction per anchor", "[correlator][causal]")
{
    AnchorList anchors = { 0.0f, 1.0f, 2.0f };
    // Reacts ~20 ms after each click. Extra later movements within the same
    // window must not shift the measurement — only the FIRST reaction ranks
    // causality; a second bounce is the ref settling, not the cause.
    auto lag = median_onset_lag_ms({ 0.02f, 0.30f, 1.02f, 2.02f }, anchors);
    REQUIRE(lag == Catch::Approx(20.f).margin(1.0));
}

TEST_CASE("median_onset_lag_ms: no anchors means no measurement", "[correlator][causal]")
{
    AnchorList none;
    REQUIRE(median_onset_lag_ms({ 0.1f, 0.2f }, none) < 0.f);
}

TEST_CASE("median_onset_lag_ms: reactions outside the window do not count", "[correlator][causal]")
{
    // 4 s after a lone anchor is past MAX_RESPONSE_S — that is not a response.
    AnchorList anchors = { 0.0f };
    REQUIRE(median_onset_lag_ms({ 4.0f }, anchors) < 0.f);
}

TEST_CASE("rank: the first mover outranks the cascade it drives", "[correlator][causal]")
{
    // The pure-DataRef case: a switch with no command behind it. All three refs
    // track the clicks perfectly — same coverage, same bidirectional pattern —
    // so the usual signals tie. Only arrival order separates cause from effect.
    AnchorList anchors = { 0.0f, 1.0f, 2.0f, 3.0f };

    // onset offsets after each click: state ref first, then what it drives.
    auto build = [&](uint32_t idx, float onset_offset)
    {
        EventStream s;
        s.logical_ref_idx = idx;
        s.type            = RefType::Int;
        for (std::size_t k = 0; k < anchors.size(); ++k)
        {
            ChangeEvent e{};
            e.onset_t_sec = anchors[k] + onset_offset;
            e.t_sec       = e.onset_t_sec + 0.067f; // hold-frame confirmation delay
            e.direction   = (k % 2 == 0) ? +1 : -1; // alternating, like a toggle
            e.from.i      = (k % 2 == 0) ? 0 : 1;
            e.to.i        = (k % 2 == 0) ? 1 : 0;
            s.events.push_back(e);
        }
        return s;
    };

    std::vector<EventStream> streams = {
        build(10, 0.070f), // lamp — reacts 70 ms later
        build(11, 0.012f), // the switch's own state ref — first
        build(12, 0.055f), // bus voltage
    };
    std::vector<RefMeta> metas(streams.size());
    for (auto &m : metas)
    {
        m.type        = RefType::Int;
        m.path_length = 30;
    }

    Hints hints;
    hints.user_click_window_ms = 100.f;
    hints.expect_bidirectional = true;

    auto out = rank(streams, metas, anchors, hints);
    REQUIRE(out.size() == 3);
    REQUIRE(out[0].logical_ref_idx == 11); // the first mover wins
    REQUIRE(out[0].is_first_mover);
    REQUIRE(out[0].has_onset_lag);
    REQUIRE(out[0].onset_lag_ms == Catch::Approx(12.f).margin(1.0));
    // The followers are still listed — nothing is hidden, they just rank below.
    REQUIRE_FALSE(out[1].is_first_mover);
}

TEST_CASE("rank: causal order does not overrule bidirectional tracking", "[correlator][causal]")
{
    // Guard against the new signal becoming a dictator. A ref that twitched
    // early but only ever moved one way must NOT beat one that tracked the
    // switch both ways across every actuation. Being first is evidence, not
    // proof — the weighting has to keep that ordering.
    AnchorList anchors = { 0.0f, 1.0f, 2.0f, 3.0f };

    EventStream early_oneway;
    early_oneway.logical_ref_idx = 20;
    early_oneway.type            = RefType::Int;
    for (std::size_t k = 0; k < anchors.size(); ++k)
    {
        ChangeEvent e{};
        e.onset_t_sec = anchors[k] + 0.005f; // reacts first
        e.t_sec       = e.onset_t_sec + 0.067f;
        e.direction   = +1; // ...but only ever climbs
        e.from.i      = static_cast<int32_t>(k);
        e.to.i        = static_cast<int32_t>(k) + 1;
        early_oneway.events.push_back(e);
    }

    EventStream later_toggle;
    later_toggle.logical_ref_idx = 21;
    later_toggle.type            = RefType::Int;
    for (std::size_t k = 0; k < anchors.size(); ++k)
    {
        ChangeEvent e{};
        e.onset_t_sec = anchors[k] + 0.060f; // reacts later
        e.t_sec       = e.onset_t_sec + 0.067f;
        e.direction   = (k % 2 == 0) ? +1 : -1; // proper toggle
        e.from.i      = (k % 2 == 0) ? 0 : 1;
        e.to.i        = (k % 2 == 0) ? 1 : 0;
        later_toggle.events.push_back(e);
    }

    std::vector<EventStream> streams = { early_oneway, later_toggle };
    std::vector<RefMeta>     metas(2);
    for (auto &m : metas)
    {
        m.type        = RefType::Int;
        m.path_length = 30;
    }

    Hints hints;
    hints.user_click_window_ms = 100.f;
    hints.expect_bidirectional = true;

    auto out = rank(streams, metas, anchors, hints);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].logical_ref_idx == 21); // bidirectional still wins
    REQUIRE(out[1].is_first_mover);        // even though the other moved first
}

TEST_CASE("rank: without anchors no causal ordering is invented", "[correlator][causal]")
{
    // No anchors means no reference point. Making up an ordering from raw
    // timestamps would be noise dressed up as evidence.
    // Bidirectional so the candidate scores positively and actually reaches the
    // output — otherwise the test would pass for the wrong reason.
    EventStream s;
    s.logical_ref_idx = 30;
    s.type            = RefType::Int;
    for (int k = 0; k < 3; ++k)
    {
        ChangeEvent e{};
        e.onset_t_sec = 0.5f + 0.5f * static_cast<float>(k);
        e.t_sec       = e.onset_t_sec + 0.067f;
        e.direction   = (k % 2 == 0) ? +1 : -1;
        e.from.i      = (k % 2 == 0) ? 0 : 1;
        e.to.i        = (k % 2 == 0) ? 1 : 0;
        s.events.push_back(e);
    }

    std::vector<EventStream> streams = { s };
    std::vector<RefMeta>     metas(1);
    metas[0].type        = RefType::Int;
    metas[0].path_length = 30;

    AnchorList none;
    Hints      hints;

    auto out = rank(streams, metas, none, hints);
    REQUIRE(out.size() == 1);
    REQUIRE_FALSE(out[0].has_onset_lag);
    REQUIRE_FALSE(out[0].is_first_mover);
}
