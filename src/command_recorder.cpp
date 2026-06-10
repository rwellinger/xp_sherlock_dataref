#include "command_recorder.hpp"
#include "command_index.hpp"
#include <XPLM/XPLMDataAccess.h>
#include <XPLM/XPLMUtilities.h>
#include <cstdint>
#include <cstdio>

namespace xp_sherlock
{

namespace
{

// All state lives on the sim main thread — XPLM dispatches command callbacks,
// flight loops and draw callbacks on the same thread, so no mutex is needed.
bool                                    s_enabled = false;
bool                                    s_recording = false;
bool                                    s_baseline_phase = false;
float                                   s_record_start_t = 0.f;
uint32_t                                s_frame_at_start = 0;
int                                     s_baseline_fires = 0;

std::vector<CommandEventStream>         s_streams;
// Track registered handles in lock-step with what we asked for, so disable()
// can unregister exactly what it registered even if command_index has been
// rebuilt in between (which would be a contract violation, but we want to
// fail safe).
struct Registration
{
    XPLMCommandRef handle = nullptr;
    uintptr_t      refcon = 0;
};
std::vector<Registration>               s_registrations;

XPLMDataRef                             s_dr_time = nullptr;

float now_sec()
{
    if (!s_dr_time)
        s_dr_time = XPLMFindDataRef("sim/time/total_running_time_sec");
    return s_dr_time ? XPLMGetDataf(s_dr_time) : 0.f;
}

// One shared callback. The refcon encodes the index into command_index::all()
// (cast through uintptr_t — standard XPLM pattern, no global hash needed).
//
// We register inBefore=0 (after sim+other plugins) and always return 1, so we
// observe passively without suppressing anyone.
int command_cb(XPLMCommandRef /*cmd*/, XPLMCommandPhase phase, void *refcon)
{
    if (s_baseline_phase && phase == xplm_CommandBegin)
        ++s_baseline_fires;

    if (!s_recording)
        return 1;

    std::uintptr_t idx_raw = reinterpret_cast<std::uintptr_t>(refcon);
    if (idx_raw >= s_streams.size())
        return 1;

    auto idx = static_cast<std::size_t>(idx_raw);
    CommandFireEvent ev{};
    ev.frame  = s_frame_at_start; // best-effort; recorder owns the live frame counter
    ev.t_sec  = now_sec() - s_record_start_t;
    ev.phase  = static_cast<uint8_t>(phase);
    s_streams[idx].events.push_back(ev);
    s_streams[idx].last_phase = ev.phase;
    return 1;
}

} // namespace

namespace command_recorder
{

void enable()
{
    if (s_enabled)
        return;

    const auto &cmds = command_index::all();
    s_registrations.clear();
    s_registrations.reserve(cmds.size());
    s_streams.assign(cmds.size(), CommandEventStream{});
    for (std::size_t i = 0; i < cmds.size(); ++i)
    {
        s_streams[i].command_idx = static_cast<uint32_t>(i);
        s_streams[i].events.reserve(2); // most commands fire 0-2 times in a record window
        if (!cmds[i].handle)
            continue;
        // NOLINTNEXTLINE(performance-no-int-to-ptr) — canonical XPLM refcon pattern.
        auto refcon = reinterpret_cast<void *>(static_cast<std::uintptr_t>(i));
        XPLMRegisterCommandHandler(cmds[i].handle, command_cb, /*inBefore=*/0, refcon);
        Registration r;
        r.handle = cmds[i].handle;
        r.refcon = static_cast<std::uintptr_t>(i);
        s_registrations.push_back(r);
    }

    char banner[160];
    snprintf(banner, sizeof(banner),
             "[xp_sherlock] command_recorder enabled: %zu handlers registered.\n",
             s_registrations.size());
    XPLMDebugString(banner);
    s_enabled = true;
}

void disable()
{
    if (!s_enabled)
        return;
    for (const auto &r : s_registrations)
    {
        if (!r.handle)
            continue;
        // NOLINTNEXTLINE(performance-no-int-to-ptr) — canonical XPLM refcon pattern.
        auto refcon = reinterpret_cast<void *>(r.refcon);
        XPLMUnregisterCommandHandler(r.handle, command_cb, /*inBefore=*/0, refcon);
    }
    s_registrations.clear();
    s_streams.clear();
    s_enabled   = false;
    s_recording = false;
    XPLMDebugString("[xp_sherlock] command_recorder disabled.\n");
}

void begin_record(float record_start_t_sec, uint32_t frame_counter_at_start)
{
    s_record_start_t = record_start_t_sec;
    s_frame_at_start = frame_counter_at_start;
    // Clear any previous events but keep the stream count == command_index size.
    for (auto &s : s_streams)
    {
        s.events.clear();
        s.last_phase = 0xFF;
    }
    s_recording = true;
}

void end_record()
{
    s_recording = false;
}

void reset()
{
    for (auto &s : s_streams)
    {
        s.events.clear();
        s.last_phase = 0xFF;
    }
    s_recording      = false;
    s_baseline_fires = 0;
}

bool is_enabled() { return s_enabled; }

int  baseline_fires_observed()          { return s_baseline_fires; }
void clear_baseline_diagnostics()       { s_baseline_fires = 0; }
void set_baseline_phase(bool on)        { s_baseline_phase = on; }

const std::vector<CommandEventStream> &streams() { return s_streams; }

bool test_fire(std::size_t command_idx, int mode)
{
    const auto &cmds = command_index::all();
    if (command_idx >= cmds.size())
        return false;
    XPLMCommandRef h = cmds[command_idx].handle;
    if (!h)
        return false;
    switch (mode)
    {
    case 0: XPLMCommandOnce(h); break;
    case 1: XPLMCommandBegin(h); break;
    case 2: XPLMCommandEnd(h); break;
    default: return false;
    }
    return true;
}

} // namespace command_recorder
} // namespace xp_sherlock
