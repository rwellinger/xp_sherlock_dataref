# CLAUDE.md

Project context and coding guidelines for Claude Code. Read this before writing or modifying any code.

## Project

X-Plane 12 plugin: **DataRef Detective** — a behavioral correlation tool that identifies which DataRef a cockpit switch drives, for aircraft that repurpose unbranded `sim/...` DataRefs (where name-based search fails). The full build brief is in the separate spec document; this file governs *how* code is written, not *what* is built.

- Reference/foundation project: `../xp_pilot` — read its conventions first and follow them.
- Target: macOS, Apple Silicon (ARM64). Match whatever the reference Makefile already targets (keep universal builds if present).
- Language: C++ against the XPLM (C) SDK.

## Non-negotiable working principles

1. **Ask, don't assume.** If the SDK version, an API signature, the build setup, or the reference project's conventions are unclear, stop and ask. Do not guess.
2. **Never fabricate XPLM API.** Verify every XPLM call against the SDK headers in the project. If you cannot confirm a signature, say so — do not invent it.
3. **Honest uncertainty over confident wrong answers.** This is a diagnostic tool. A tool that confidently reports a wrong DataRef is worse than no tool. When a result is inconclusive (e.g. a write-back that does nothing), report it as inconclusive, not as success or failure.
4. **Match the existing project.** Reuse the reference project's structure, build system, logging, naming, and file layout. Do not introduce a new architecture, a new build tool, or a new dependency without asking.

## Code quality (adapted Clean Code for C++/real-time/C-API)

These are the principles that apply here. Where classic Clean Code advice conflicts with C++ or real-time constraints, the constraint wins.

### Naming & structure
- Intention-revealing names. No abbreviations except established domain terms (`dref`, `cmd` are fine; invented short forms are not).
- Small, single-purpose functions — but **readability over a hard line limit.** Do not split a coherent XPLM setup sequence just to hit a line count.
- No magic numbers. Epsilon values, buffer sizes, and timing windows are named constants with a comment explaining the chosen value.
- One level of abstraction per function. Keep the flight-loop callback thin: it should delegate, not contain the whole sampling logic inline.

### Comments — required here, contrary to "code should be self-documenting"
- Comment the **why**, never the what. Specifically required:
  - Why a DataRef handle is cached (never `XPLMFindDataRef` in the loop).
  - Why float comparison uses an epsilon and how the threshold was chosen.
  - Any XPLM-specific lifecycle ordering or callback contract that isn't obvious from the code.
- Do not comment trivial lines. Do not leave commented-out code — delete it; git is the history.

### C++ specifics
- RAII for anything with a lifetime (windows, registered callbacks/handlers, buffers). Register in enable, unregister in disable, symmetrically. No leaked callback registrations.
- Prefer `std::vector` / `std::array` and standard containers over raw arrays; reserve capacity for per-frame buffers so the hot path does not allocate.
- `const`-correctness throughout. Pass by reference/const-reference, not by value, for non-trivial types.
- No raw owning pointers. `std::unique_ptr` where ownership exists; non-owning observation via reference or raw non-owning pointer clearly marked.
- Use `enum class` for states (phase: Idle/Baseline/Recording/etc.), not bare ints or `#define`.
- Initialize everything; no uninitialized members. Prefer brace-init.
- Keep C-API boundary code (XPLM calls, `void*` refcons, C-style callbacks) thin and isolated; wrap it so the rest of the code is idiomatic C++.

### Real-time / flight-loop discipline
- The per-frame callback must not allocate, must not log per frame, and must not call `XPLMFindDataRef`. Resolve handles once at enumeration.
- Sampling all DataRefs every frame is the heavy path — keep it tight, but correctness first. Do not micro-optimize at the cost of clarity for a dev tool.

### Error handling
- Check `XPLMCanWriteDataRef` before writing. Surface results; do not swallow.
- Handle array DataRefs per index; handle float-as-discrete with epsilon (see spec).
- Log skipped/unsupported types (e.g. `xplmType_Data` in v1) explicitly — never silently ignore.

## Formatting
- Follow the reference project's existing style (indentation, brace placement, header guards vs `#pragma once`). If a `.clang-format` exists in `../xp_pilot`, use it. If not, ask whether to add one before reformatting anything.
- Do not reformat existing reference-project files.

## What to deliver
- A summary of the reference architecture and your integration plan first (per the spec's Step 0), and wait for confirmation if anything is ambiguous.
- Source following all of the above, plus build instructions consistent with the existing Makefile/CMake.
