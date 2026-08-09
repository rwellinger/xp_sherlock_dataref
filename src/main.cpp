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

// Plugin lifecycle, menu integration, draw-callback dispatch, custom command.
// Mirrors the xp_pilot main.cpp pattern verbatim — only the module wiring is
// different.

#include "command_recorder.hpp"
#include "dataref_index.hpp"
#include "recorder.hpp"
#include "ui.hpp"
#include <XPLM/XPLMDisplay.h>
#include <XPLM/XPLMMenus.h>
#include <XPLM/XPLMPlanes.h>
#include <XPLM/XPLMPlugin.h>
#include <XPLM/XPLMProcessing.h>
#include <XPLM/XPLMUtilities.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using namespace xp_sherlock;

// ── Draw callback (xplm_Phase_Window) ────────────────────────────────────────
static int DrawCallback(XPLMDrawingPhase, int, void *)
{
    ui::draw();
    return 1;
}

// ── Menu + Command ───────────────────────────────────────────────────────────
static XPLMCommandRef s_cmd_toggle  = nullptr;
static XPLMMenuID     s_plugin_menu = nullptr;

static void PluginMenuHandler(void *, void *item_ref)
{
    if (reinterpret_cast<intptr_t>(item_ref) == 1)
        ui::toggle();
}

static int CmdToggle(XPLMCommandRef, XPLMCommandPhase phase, void *)
{
    if (phase == xplm_CommandBegin)
        ui::toggle();
    return 1;
}

// ── Deferred re-enumeration on aircraft load ─────────────────────────────────
// When the user swaps aircraft, the new plane registers its (often thousands of)
// custom datarefs and creates its commands lazily — and its *_Commands.txt is
// only meaningful once that plane is the active one. A snapshot taken against a
// stale index silently misses all of them. We therefore re-enumerate on
// XPLM_MSG_PLANE_LOADED, but deferred by a short delay via a one-shot flight
// loop: the message can arrive slightly before registration is complete, and a
// flight loop also guarantees we run on the main thread (XPLM enumeration is not
// thread-safe to call from the message handler context otherwise).
static bool s_reenum_scheduled = false;

// Chosen empirically: 1 s is comfortably past the point where stock + payware
// aircraft finish registering their refs after PLANE_LOADED, while still feeling
// instant to the user.
static constexpr float kReenumDelaySeconds = 1.0f;

static float ReenumFlightLoop(float, float, int, void *)
{
    ui::reenumerate();
    // Returning 0 only deactivates the callback (it stays registered, per the
    // XPLM contract); unregister explicitly so nothing lingers.
    XPLMUnregisterFlightLoopCallback(ReenumFlightLoop, nullptr);
    s_reenum_scheduled = false;
    return 0.f;
}

static void schedule_reenumerate()
{
    if (s_reenum_scheduled)
    {
        // Plane reloaded again before the pending tick fired — push the delay
        // back instead of registering a second callback.
        XPLMSetFlightLoopCallbackInterval(ReenumFlightLoop, kReenumDelaySeconds, 1, nullptr);
        return;
    }
    XPLMRegisterFlightLoopCallback(ReenumFlightLoop, kReenumDelaySeconds, nullptr);
    s_reenum_scheduled = true;
}

// ════════════════════════════════════════════════════════════════
// X-Plane Plugin entry points
// ════════════════════════════════════════════════════════════════

PLUGIN_API int XPluginStart(char *outName, char *outSig, char *outDesc)
{
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);

    snprintf(outName, 255, "DataRef Detective v%s", XP_SHERLOCK_VERSION);
    strncpy(outSig, "thWelly.xp_sherlock_dataref", 255);
    snprintf(outDesc, 255, "Behavioural-correlation DataRef finder v%s", XP_SHERLOCK_VERSION);

    try
    {
        XPLMDebugString("[xp_sherlock] XPluginStart: entry\n");

        recorder::init();
        ui::init();

        XPLMRegisterDrawCallback(DrawCallback, xplm_Phase_Window, 1, nullptr);

        s_cmd_toggle = XPLMCreateCommand("xp_sherlock_dataref/window/toggle", "Toggle DataRef Detective window");
        XPLMRegisterCommandHandler(s_cmd_toggle, CmdToggle, 1, nullptr);

        XPLMMenuID plugins_menu = XPLMFindPluginsMenu();
        int        sub          = XPLMAppendMenuItem(plugins_menu, "DataRef Detective", nullptr, 0);
        s_plugin_menu           = XPLMCreateMenu("DataRef Detective", plugins_menu, sub, PluginMenuHandler, nullptr);
        XPLMAppendMenuItem(s_plugin_menu, "Open / Close Window", reinterpret_cast<void *>(1), 0);

        char banner[128];
        snprintf(banner, sizeof(banner), "[xp_sherlock] *** DataRef Detective v%s by thWelly ***\n",
                 XP_SHERLOCK_VERSION);
        XPLMDebugString(banner);
        return 1;
    }
    catch (const std::exception &e)
    {
        XPLMDebugString(("[xp_sherlock] FATAL: XPluginStart threw: " + std::string(e.what()) + "\n").c_str());
        return 0;
    }
    catch (...)
    {
        XPLMDebugString("[xp_sherlock] FATAL: XPluginStart threw unknown exception\n");
        return 0;
    }
}

PLUGIN_API void XPluginStop()
{
    ui::stop();
    recorder::stop();
    // Cancel a pending deferred re-enumeration so we don't leave a registered
    // flight-loop callback behind on unload.
    if (s_reenum_scheduled)
    {
        XPLMUnregisterFlightLoopCallback(ReenumFlightLoop, nullptr);
        s_reenum_scheduled = false;
    }
    // command_recorder::disable() is symmetric to enable() in XPluginEnable;
    // call it here too in case Enable was never reached (unusual but defensive).
    command_recorder::disable();
    if (s_cmd_toggle)
        XPLMUnregisterCommandHandler(s_cmd_toggle, CmdToggle, 1, nullptr);
    XPLMUnregisterDrawCallback(DrawCallback, xplm_Phase_Window, 1, nullptr);
    XPLMDebugString("[xp_sherlock] Plugin unloaded.\n");
}

PLUGIN_API int XPluginEnable()
{
    // command_recorder hooks every command in command_index. The index is
    // built lazily on the first "Take Snapshot" (same pattern as dataref_index),
    // so at XPluginEnable time it may still be empty — enable() handles that
    // case by registering zero handlers; the UI will (re)build and
    // re-enable() on the first snapshot.
    command_recorder::enable();
    return 1;
}
PLUGIN_API void XPluginDisable() { command_recorder::disable(); }

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int msg, void *param)
{
    // Re-enumerate datarefs + commands when the USER aircraft changes, so a
    // freshly loaded plane's custom refs/commands become visible automatically.
    if (msg == XPLM_MSG_PLANE_LOADED && reinterpret_cast<intptr_t>(param) == XPLM_USER_AIRCRAFT)
    {
        schedule_reenumerate();
    }
}
