# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.2.0] - 2026-08-16

This release is about telling **cause from effect**. Earlier versions could tell
you that a dozen things reacted when you flipped a switch; they could not tell
you which one to bind. It also removes two failure modes that made the tool
appear to work while quietly measuring nothing.

### Added

#### Command noise muting

Some aircraft fire commands continuously with no user input at all (the AW139
re-triggers `sim/autopilot/disconnect/...` while the cockpit sits idle), which
flooded every result table.

- Any command that fires during **Learn Baseline** or **Learn Ambient** is muted
  as ambient noise. Baseline resets the mute set; Learn Ambient grows it, mirroring
  how the DataRef ignore set already behaved.
- **Learn Ambient now collects commands at all** — previously it ignored them
  entirely, which was an asymmetry with its DataRef handling.
- Muted does **not** mean discarded. Muted commands keep recording in full and
  move to a collapsible *Muted as baseline noise* section showing baseline and
  record fire counts side by side. **Unmute** returns any row with its complete
  history intact, so muting can never lose the answer.
- **Auto-unmute**: a muted command is promoted back automatically when it stops
  behaving like noise — median (not minimum) latency to your actions within
  250 ms *and* a fire count inside the expected budget. Both are required; a
  constant chatterer eventually lands near an action by chance, but its median
  stays high and it overshoots the budget.
- Master switch under **Advanced**, on by default.

#### Command → DataRef pairing

Answers the question the ranked list could not: when both a command and a DataRef
correlate with the same switch, which do you bind and which do you read?

- A new **Command drives DataRef** panel above the results, open by default.
  A pair is reported when the command fires first and the DataRef follows within
  a few frames across a majority of fires. The ordering is the evidence: the
  command is the actuator, the DataRef the aircraft logic's status output.
- **Cascade separation.** Firing a battery master lights every warning lamp on
  the panel in the same frame, so all of them technically "follow" the command.
  Each command now yields exactly one primary pair; the rest move to a collapsed
  *Cascade* section. Selection combines match ratio, causal ordering, value shape
  (switch state vs. sensor curve), ranking score and path-name affinity — no
  single signal dictates, so it works for aircraft that use their own naming
  throughout.
- **Pairs first** (on by default) floats paired rows to the top of the candidate
  table, tinted green. The Rank column keeps each row's true position, so nothing
  is renumbered.
- `ChangeEvent` now carries `onset_t_sec` — when a value *starts* moving, not
  when the change is confirmed. The confirmation delay is identical for every ref
  and would have erased the few-frame gaps this depends on.

#### Anchor coverage

- Candidates are scored on how cleanly their activity maps 1:1 onto the actions
  you performed, shown in a new **Anchors** column (`5/5` in green for a perfect
  match). Events belonging to no action count as strays and cost score.
- This is the strongest available filter and it strengthens with repetition: one
  actuation cannot separate a target from noise that moved at the same moment,
  five can.

#### Causal ordering (the Reacted column)

For switches with **no** command behind them, every ref in the cascade correlates
with the click equally well, which is why the right answer landed somewhere in
the top ten rather than first.

- The new **Reacted** column shows the median delay from your action to that ref
  starting to move, with the earliest marked `1st`. Ranking rewards earlier refs
  on a curve that decays over ~100 ms.
- Deliberately weighted below bidirectional tracking and anchor coverage: being
  first is evidence, not proof. Nothing is hidden — later refs simply rank lower.
- Inactive without recorded actions; inventing an ordering from raw timestamps
  would be noise presented as evidence.

#### Cascade probe (Compare)

Settles "send the command or write the DataRef?" by measurement instead of
heuristics. Each probe performs one action and counts how many watched refs move
within 1.5 s. If the command moves clearly more, the DataRef is only the tip of
the cascade. A verdict appears only once **both** probes have run.

#### Diagnostics

- When no pair is found the section no longer vanishes silently — it names the
  reason (no command fired at all vs. commands fired but nothing followed
  consistently) and reports how many commands are being watched.
- A warning in Inspect when no actions were registered, since the strongest
  filter then never ran.
- **Advanced → Find a missing DataRef**: search whether a ref you expected was
  excluded as baseline noise. Such refs are dropped from the entire recording and
  were previously untraceable.
- The log now reports command provenance per source, including a dedicated
  `Aircraft assets scanned:` line.

### Changed

#### Recording no longer requires reaching the window

Previously each action had to be stamped with an **I Acted Now** button, which
meant travelling between the cockpit switch and the dialog for every cycle. In
practice this made actions impossible to record, so anchor-based scoring never
ran at all.

- Clicking **Record** now steps the main window aside, leaving a slim bar with a
  live action counter and **Stop**.
- **Every cockpit click registers as one action automatically.** Mouse-wheel
  actuations (rotary knobs) count too, coalesced so one flick of the wheel is one
  action rather than a dozen. Clicks on the plugin's own UI are excluded by
  construction.
- Side benefit: the timestamp is now the exact moment of actuation instead of a
  button pressed some unknown time beforehand, which is what makes the causal
  ordering above measurable.

#### Command discovery is no longer limited to text listings

X-Plane offers no command-enumeration API, and most aircraft ship no command list
— they register everything at runtime. Any switch bound to such a command was
invisible, so no pair could ever be found for it. Two vendor-neutral sources were
added on top of the existing two:

- `ATTR_manip_command*` in the aircraft's `.obj` files — the X-Plane standard for
  clickable cockpit surfaces, i.e. exactly where switch commands live.
- Registration calls in `.lua` / `.slua` scripts (`create_command`,
  `find_command`, `sasl.createCommand`, …), covering xlua, SASL and FlyWithLua.

Every harvested name is verified with `XPLMFindCommand`, so a false positive
costs one lookup and disappears. The scan is capped at 4000 files and says so in
the log when the cap is reached.

#### Auto-stop

- With actions recorded, Record ends after a quiet period following your last
  action, on a threshold that **adapts to your own pace** (at least 8 s, and at
  least 1.8× your widest observed gap). The previous fixed rule ended the run
  after ~5 s once any ref showed a pattern, cutting long sequences short — which
  directly undermined anchor coverage.
- A countdown appears in the status bar and recording overlay; every action
  resets it.
- Without actions there is now a 25 s grace period before the old pattern-based
  fallback may fire, so travelling to the switch cannot end the recording.

#### Baseline

- Default window raised from 2.5 s to **5 s**, now adjustable 2–20 s under
  **Advanced**. A command that only re-fires every few seconds can sit out a
  short window entirely and escape the noise filter; 10–15 s is recommended on
  chatty aircraft.

#### Switch configuration simplified

- The rotary **click count** input is gone. The number of actuations is derived
  from the actions you actually performed, so it can no longer be set wrong.
- **Advanced** now only asks for the switch *kind* — **Bool** (alternates) or
  **Rotary** (steps one way) — the one thing that cannot be inferred.

#### Link detection relaxed

- A link previously required **every** command fire to be answered. That scaled
  badly once users were encouraged to actuate five or more times: a single brief
  press or one stray `Begin` destroyed the whole finding. A **majority** (≥ 60 %,
  minimum 2) now suffices, and `fires_matched`/`fires_total` report the real
  ratio instead of always being equal.
- Correlation window widened from 150 ms to 300 ms, with a 20 ms backward
  tolerance: command timestamps come from the command callback and ref onsets
  from the flight loop, so a genuinely caused onset can land marginally before
  the fire.

#### Documentation

- README rewritten for the new workflow, with sections on command noise muting,
  command/DataRef pairing, the cascade probe, anchor coverage and causal ordering
   — including the honest limits of each.
- Corrected the stale "command sniffing is NOT implemented" caveat.
- New screenshots; the previous one was removed.

### Fixed

- **Learn Ambient ignored commands entirely.** It grew the DataRef ignore set but
  left commands untouched, so a noisy command could not be filtered out by
  driving it.
- Recording could end before the user reached the switch, producing results with
  no recorded actions and therefore no coverage scoring — while presenting the
  ranking as if it had full evidence behind it.
- A rotary switch never reverses direction, so with the click count removed it
  would have fallen through to the bidirectional auto-stop bar and never stopped.
  An explicit floor was added.

### Notes for existing users

- **Scores are not comparable with 1.1.x.** Anchor coverage and causal ordering
  add new terms, so absolute values have shifted. Relative ranking is what
  matters.
- Command mutes are keyed by index and are therefore lost on re-enumeration; take
  a fresh baseline after an aircraft swap.
- A cascade probe only covers the refs watched in the last Record. "Both moved
  the same number" means no measurable difference *among those refs*, not proof
  of equivalence.
- Command→DataRef pairs are correlational. Requiring a majority of fires to be
  followed within a few frames is strong evidence of causation, but a third party
  driving both would look identical.
