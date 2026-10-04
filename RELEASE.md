# Release 1.2.1 (unreleased)

A stability release. A user reported X-Plane crashing as soon as they clicked
**Learn Baseline**; the report came without a log or OS, and the crash does not
reproduce on macOS. This release fixes the one platform-specific crash path
found in that code, makes sure no exception of ours can ever take the sim down,
and adds log lines so the next crash report points at its cause.

## Fixed

### Windows: crash when the aircraft folder contains non-ASCII file names

Learn Baseline (and the automatic re-enumeration after an aircraft load) scans
the aircraft folder for command names. On Windows, file names were converted
through the system ANSI code page, and that conversion **throws** for any
character the code page cannot represent: a livery named in Greek, Polish,
Cyrillic or Chinese, or one containing an emoji, was enough. The exception was
not caught and terminated X-Plane.

- All paths received from X-Plane are now handled as UTF-8 end to end
  (`std::filesystem::u8path` / `u8string`), and files are opened through
  `std::filesystem::path`, which uses the wide-character API on Windows.
- macOS and Linux were not affected; their paths are UTF-8 natively.

### Windows: stock `Commands.txt` not found under non-ASCII install paths

Same root cause, no crash: if X-Plane was installed under a path containing,
for example, an umlaut (`C:\Users\Jürg\X-Plane 12`), the stock
`Resources/plugins/Commands.txt` could not be opened and the plugin silently
fell back to its small built-in command list, so most stock commands were
never watched. Fixed by the same UTF-8 path handling.

## Changed

### Exceptions can no longer crash X-Plane

Every callback X-Plane calls into the plugin now catches exceptions and turns
them into a log line instead of letting them unwind into the sim:

- the window draw callback, which covers every button, including Learn Baseline,
  Record and Re-enumerate;
- the deferred re-enumeration after an aircraft load;
- the recorder's flight loop, which on an exception falls back to Idle and stops
  itself rather than failing again every frame.

Look for `[xp_sherlock] ERROR: ... threw: ...` in `Log.txt`.

### Log lines that locate a crash

Learn Baseline is the first time the plugin reads **every** DataRef of **every**
installed plugin. A third-party plugin with a broken DataRef accessor crashes
inside its own code, where no plugin can catch it. New one-time log lines (none
per frame) show where in the sequence a crash happened:

| Last `[xp_sherlock]` line in `Log.txt` | Where it crashed |
| --- | --- |
| `Enumerating N datarefs ...` | listing DataRefs (array sizes from other plugins) |
| `Scanning aircraft assets in <path> ...` | scanning the aircraft folder for commands |
| `Baseline: reading N refs ...` without `... seeded.` | reading another plugin's DataRef values |

## Reporting a crash

Please attach `X-Plane 12/Log.txt` from the crashed session, and say which OS,
aircraft and other plugins you use. Without the log there is no way to tell what
went wrong.
