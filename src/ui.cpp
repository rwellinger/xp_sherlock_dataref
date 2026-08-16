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

#include "ui.hpp"
#include "clipboard.hpp"
#include "command_index.hpp"
#include "command_recorder.hpp"
#include "dataref_index.hpp"
#include "recorder.hpp"
#include <XPLM/XPLMDataAccess.h>
#include <XPLM/XPLMDisplay.h>
#include <XPLM/XPLMGraphics.h>
#include <XPLM/XPLMUtilities.h>
#include <backends/imgui_impl_opengl2.h>
#include <imgui.h>
#if defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace xp_sherlock
{
namespace ui
{

namespace
{

// ── XPLM window pattern (mirrors xp_pilot logbook_ui.cpp) ────────────────────
XPLMWindowID  s_wnd       = nullptr;
ImGuiContext *s_imgui_ctx = nullptr;
bool          s_open      = false;

double s_last_frame_time = 0.0;

// Scroll-anchor coalescing. One flick of the wheel arrives as a burst of
// notches; treating each as a separate actuation would fabricate anchors the
// user never placed. 0.25 s is comfortably longer than a burst and far shorter
// than the gap between two deliberate knob turns.
constexpr float SCROLL_ANCHOR_COALESCE_S = 0.25f;
float           s_last_scroll_anchor_t   = 0.f;

double get_xp_time()
{
    static XPLMDataRef dr = nullptr;
    if (!dr)
        dr = XPLMFindDataRef("sim/time/total_running_time_sec");
    return dr ? static_cast<double>(XPLMGetDataf(dr)) : 0.0;
}

// ── UI state ─────────────────────────────────────────────────────────────────
// Switch kind. Bool alternates (ON-OFF-ON), rotary walks one way. This is the
// part anchors cannot tell us — the NUMBER of actuations is derived from the
// anchor count instead of being typed in.
bool s_expect_bidirectional = true;
int  s_selected_candidate   = -1;
// Baseline window length. Raised from the old fixed 2.5 s and made adjustable:
// aircraft that chatter on a slow cycle need a longer look to be profiled.
float s_baseline_seconds = 5.0f;

// Test panel state — separate value buffer per scalar/array kind
int    s_write_int    = 1;
float  s_write_float  = 1.0f;
double s_write_double = 1.0;

bool        s_last_write_done     = false;
bool        s_last_write_writable = false;
SampleValue s_last_readback       = {};

// ── Snapshot exclusion filters (checkboxes) ──────────────────────────────────
//
// Each category maps to one or two namespace prefixes that are stripped at
// enumeration time. Defaults skip the noisiest namespaces that almost never
// hold cockpit switches (flight model state, weather, joystick raw input,
// etc.) so a fresh user can take a useful snapshot without tuning anything.
// Cockpit-relevant namespaces (sim/cockpit*, laminar/*, aircraft-specific)
// are deliberately never on this list.
struct ExclusionCategory
{
    const char *label;
    const char *prefix_a; // primary prefix
    const char *prefix_b; // optional second prefix, "" if unused
    bool        default_on;
};

constexpr ExclusionCategory s_exclusion_categories[] = {
    {"Flight model (positions, velocities, forces)", "sim/flightmodel/", "sim/flightmodel2/", true},
    {"Aircraft static config", "sim/aircraft/", "", true},
    {"Airfoil tables (aero coefficients)", "sim/airfoils/", "", true},
    {"Weather & atmosphere", "sim/weather/", "sim/atmosphere/", true},
    {"Joystick raw input", "sim/joystick/", "", true},
    {"Network / multiplayer", "sim/network/", "sim/multiplayer/", true},
    {"World / terrain", "sim/world/", "", true},
    {"Version info", "sim/version/", "", true},
    {"Test scaffolding", "sim/test/", "", true},
    {"Time", "sim/time/", "", false},
    {"Graphics / rendering", "sim/graphics/", "", false},
    {"Operation", "sim/operation/", "", false},
};
constexpr std::size_t s_exclusion_category_count = sizeof(s_exclusion_categories) / sizeof(s_exclusion_categories[0]);

bool s_exclude_flags[s_exclusion_category_count] = {};   // initialised in init()
bool s_filters_dirty                             = true; // forces rebuild() before first snapshot, then per-toggle

// ── Result filter (live substring filter on candidate table) ─────────────────
char s_result_filter[128] = "";
// Separate query for the "why is my ref missing" lookup — kept apart from the
// result filter so searching for an absent ref does not disturb the table view.
char s_excluded_filter[128] = "";
bool s_show_datarefs        = true;
bool s_show_commands        = true;
bool s_writable_only        = false;
// Float rows belonging to a command→DataRef pair to the top. On by default:
// when a pair exists it is the answer, and hunting for it down at rank 17
// defeats the point of detecting it.
bool s_linked_first = true;

// ── Capture-window callbacks ─────────────────────────────────────────────────
//
// Critical: the invisible XPLM window is full-screen, so without filtering it
// would steal every click that should go to the cockpit. We forward events to
// ImGui but only CONSUME them (return 1) when ImGui actually wants them
// (i.e. the click is over an ImGui window/widget). Otherwise we return 0 so
// X-Plane keeps propagating the event to the 3D cockpit layer underneath —
// without this, you cannot operate a switch while the Detective window is
// open, which would defeat the whole tool.
void DrawCallback(XPLMWindowID, void *) {}

// Decide whether THIS click belongs to ImGui. The decision is based on the
// state from the PREVIOUS frame:
//   - WantCaptureMouse = the cursor was hovering over an ImGui window.
//   - MouseDown[btn]   = ImGui already saw a MouseDown for this button and
//                        is mid-interaction (drag, slider, etc.). We MUST
//                        deliver the matching MouseUp or the button stays
//                        stuck "pressed" forever — that's the cause of the
//                        "after clicking a cockpit switch the window won't
//                        respond" bug.
bool imgui_owns_button(int btn)
{
    ImGuiIO &io = ImGui::GetIO();
    return io.WantCaptureMouse || (btn >= 0 && btn < IM_ARRAYSIZE(io.MouseDown) && io.MouseDown[btn]);
}

int MouseCallback(XPLMWindowID wnd, int x, int y, XPLMMouseStatus status, void *)
{
    int left, top, right, bottom;
    XPLMGetWindowGeometry(wnd, &left, &top, &right, &bottom);
    ImGui::SetCurrentContext(s_imgui_ctx);
    ImGuiIO &io = ImGui::GetIO();
    float    mx = static_cast<float>(x - left);
    float    my = static_cast<float>(top - y);
    io.AddMousePosEvent(mx, my); // position is harmless to update even outside our window

    if (!imgui_owns_button(0))
    {
        // The click is going through to the cockpit. During Record that IS the
        // user's actuation, so it anchors itself — no trip back to the window,
        // which is what made manual anchors unusable in practice. Down only:
        // anchoring the matching Up would double-count every press.
        //
        // No guessing involved: ImGui already told us it does not want this
        // click, so it cannot be a click on our own UI.
        if (status == xplm_MouseDown && recorder::phase() == Phase::Record)
            recorder::mark_user_action();
        return 0; // cockpit gets it — and we do NOT poison ImGui with a half-pair Down/Up
    }

    if (status == xplm_MouseDown)
        io.AddMouseButtonEvent(0, true);
    if (status == xplm_MouseUp)
        io.AddMouseButtonEvent(0, false);
    return 1;
}

int ScrollCallback(XPLMWindowID wnd, int x, int y, int, int clicks, void *)
{
    int left, top, right, bottom;
    XPLMGetWindowGeometry(wnd, &left, &top, &right, &bottom);
    ImGui::SetCurrentContext(s_imgui_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(static_cast<float>(x - left), static_cast<float>(top - y));
    if (!io.WantCaptureMouse)
    {
        // Rotary knobs are worked with the wheel, so a cockpit-bound scroll is
        // an actuation too. Coalesced: one flick of the wheel emits a burst of
        // notches, and anchoring each would invent a dozen actuations the user
        // never made — which would wreck the anchor-coverage metric.
        if (recorder::phase() == Phase::Record)
        {
            const float now = static_cast<float>(get_xp_time());
            if (now - s_last_scroll_anchor_t >= SCROLL_ANCHOR_COALESCE_S)
            {
                s_last_scroll_anchor_t = now;
                recorder::mark_user_action();
            }
        }
        return 0; // forward scroll wheel to cockpit (knobs, throttle wheels, etc.)
    }
    io.AddMouseWheelEvent(0.f, static_cast<float>(clicks));
    return 1;
}

int RightClickCallback(XPLMWindowID wnd, int x, int y, XPLMMouseStatus status, void *)
{
    int left, top, right, bottom;
    XPLMGetWindowGeometry(wnd, &left, &top, &right, &bottom);
    ImGui::SetCurrentContext(s_imgui_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(static_cast<float>(x - left), static_cast<float>(top - y));

    if (!imgui_owns_button(1))
        return 0;

    if (status == xplm_MouseDown)
        io.AddMouseButtonEvent(1, true);
    if (status == xplm_MouseUp)
        io.AddMouseButtonEvent(1, false);
    return 1;
}

XPLMCursorStatus CursorCallback(XPLMWindowID wnd, int x, int y, void *)
{
    int left, top, right, bottom;
    XPLMGetWindowGeometry(wnd, &left, &top, &right, &bottom);
    ImGui::SetCurrentContext(s_imgui_ctx);
    ImGui::GetIO().AddMousePosEvent(static_cast<float>(x - left), static_cast<float>(top - y));
    // Returning xplm_CursorDefault means "I don't claim this cursor position"
    // — X-Plane keeps asking the layers underneath. That lets cockpit
    // hover-hotspots still light up while the Detective window is open.
    return xplm_CursorDefault;
}

void KeyCallback(XPLMWindowID, char key, XPLMKeyFlags flags, char vkey, void *, int losing_focus)
{
    if (losing_focus)
        return;
    ImGui::SetCurrentContext(s_imgui_ctx);
    ImGuiIO &io = ImGui::GetIO();

    bool down = (flags & xplm_DownFlag) != 0;
    bool up   = (flags & xplm_UpFlag) != 0;

    // Escape always closes our window regardless of focus — convenience.
    if (down && vkey == XPLM_VK_ESCAPE)
    {
        s_open = false;
        if (s_wnd)
        {
            XPLMSetWindowIsVisible(s_wnd, 0);
            XPLMTakeKeyboardFocus(nullptr); // hand keys back to X-Plane
        }
        return;
    }

    // If ImGui isn't expecting keyboard input (no active text field), don't
    // swallow keystrokes — let X-Plane's normal key bindings work.
    if (!io.WantCaptureKeyboard)
        return;

    // Printable characters: only emit on key-down. Releases don't generate
    // text, so we must not call AddInputCharacter for them or every key would
    // be typed twice.
    if (down && key >= 32 && key < 127)
        io.AddInputCharacter(static_cast<unsigned>(key));

    // For non-character keys ImGui needs both edges (down AND up). Without the
    // matching key-up event ImGui's internal repeat state can latch and the
    // key appears to be held down forever — symptom: backspace deletes the
    // whole field instead of one char.
    auto edge = [&](ImGuiKey k)
    {
        if (down)
            io.AddKeyEvent(k, true);
        if (up)
            io.AddKeyEvent(k, false);
    };
    if (vkey == XPLM_VK_BACK)
        edge(ImGuiKey_Backspace);
    if (vkey == XPLM_VK_DELETE)
        edge(ImGuiKey_Delete);
    if (vkey == XPLM_VK_RETURN)
        edge(ImGuiKey_Enter);
    if (vkey == XPLM_VK_LEFT)
        edge(ImGuiKey_LeftArrow);
    if (vkey == XPLM_VK_RIGHT)
        edge(ImGuiKey_RightArrow);
    if (vkey == XPLM_VK_HOME)
        edge(ImGuiKey_Home);
    if (vkey == XPLM_VK_END)
        edge(ImGuiKey_End);
    if (vkey == XPLM_VK_TAB)
        edge(ImGuiKey_Tab);
}

// Synchronise XPLM keyboard focus with ImGui's active text-input state.
//
// XPLM only routes keystrokes to handleKeyFunc when our window holds the
// keyboard focus. Without this, clicking into an ImGui InputText looks
// active visually (cursor blinks) but no characters arrive. We grab focus
// the frame ImGui starts wanting text input, and release it the frame the
// user clicks away — so X-Plane's own key bindings (e.g. 'b' for brakes)
// keep working whenever no Detective text field is active.
void sync_keyboard_focus(const ImGuiIO &io)
{
    if (!s_wnd)
        return;
    bool wants = io.WantTextInput;
    bool has   = XPLMHasKeyboardFocus(s_wnd) != 0;
    if (wants && !has)
        XPLMTakeKeyboardFocus(s_wnd);
    else if (!wants && has)
        XPLMTakeKeyboardFocus(nullptr);
}

void create_capture_window_if_needed()
{
    if (s_wnd)
        return;
    int gl, gt, gr, gb;
    XPLMGetScreenBoundsGlobal(&gl, &gt, &gr, &gb);
    XPLMCreateWindow_t p       = {};
    p.structSize               = sizeof(p);
    p.left                     = gl;
    p.bottom                   = gb;
    p.right                    = gr;
    p.top                      = gt;
    p.visible                  = 1;
    p.drawWindowFunc           = DrawCallback;
    p.handleMouseClickFunc     = MouseCallback;
    p.handleKeyFunc            = KeyCallback;
    p.handleCursorFunc         = CursorCallback;
    p.handleMouseWheelFunc     = ScrollCallback;
    p.refcon                   = nullptr;
    p.decorateAsFloatingWindow = xplm_WindowDecorationNone;
    p.layer                    = xplm_WindowLayerFloatingWindows;
    p.handleRightClickFunc     = RightClickCallback;
    s_wnd                      = XPLMCreateWindowEx(&p);
}

// ── Phase-banner helpers ─────────────────────────────────────────────────────
const char *phase_label(Phase p)
{
    switch (p)
    {
    case Phase::Idle:
        return "Idle";
    case Phase::Baseline:
        return "Learning";
    case Phase::Record:
        return "Record";
    case Phase::Inspect:
        return "Inspect";
    case Phase::NoiseCapture:
        return "Learning Ambient";
    case Phase::Probe:
        return "Probing";
    }
    return "?";
}

std::string hint_text(Phase p)
{
    switch (p)
    {
    case Phase::Idle:
        return "Sit still, then Learn Baseline to model the environment.";
    case Phase::Baseline:
        return "Hold still - learning baseline. On a chatty aircraft, raise the baseline length under Advanced.";
    case Phase::Record:
        if (recorder::auto_stop_enabled())
            return "Click 'I Acted Now', then work the switch. Repeat several times - more cycles means a "
                   "sharper result. Stops on its own a few seconds after your last action.";
        else
            return "Click 'I Acted Now', then work the switch. Repeat several times - more cycles means a "
                   "sharper result. Click 'Stop' when done (auto-stop is disabled).";
    case Phase::Inspect:
        return "Top-ranked DataRef is most likely the cause. Lower ranks may be downstream effects.";
    case Phase::NoiseCapture:
        return "Learning the ambient profile - drive everything you want filtered out (e.g. power the bus). "
               "Every ref that moves is added to the ignore-set. Click 'Stop' when done, then Record your target.";
    case Phase::Probe:
        return "Measuring the cascade - hands off the cockpit until this finishes.";
    }
    return "";
}

std::string value_label(RefType t, SampleValue v)
{
    char buf[64];
    switch (t)
    {
    case RefType::Int:
    case RefType::IntArrayElem:
        snprintf(buf, sizeof(buf), "%d", v.i);
        break;
    case RefType::Float:
    case RefType::FloatArrayElem:
        snprintf(buf, sizeof(buf), "%.4f", static_cast<double>(v.f));
        break;
    case RefType::Double:
        snprintf(buf, sizeof(buf), "%.6f", v.d);
        break;
    }
    return buf;
}

const char *direction_icon(const Candidate &c)
{
    if (c.kind == Kind::Command)
    {
        // XPLM phase: Begin=0, Continue=1, End=2.
        switch (c.last_phase)
        {
        case 0:
            return ">";
        case 1:
            return "||";
        case 2:
            return "<";
        default:
            return "--";
        }
    }
    if (c.bidirectional)
        return "<->";
    if (c.monotonic_staircase)
        return "|||";
    if (c.pos_count > 0 && c.neg_count == 0)
        return "->";
    if (c.neg_count > 0 && c.pos_count == 0)
        return c.asymmetric_decay ? "v" : "<-";
    return "?";
}

// Resolve a candidate to its display path. Returns the DataRef display_path
// for DataRef rows and the command name for Command rows. Returns empty
// string when the underlying index entry is missing (defensive — should not
// happen in practice).
std::string candidate_path(const Candidate &c)
{
    if (c.kind == Kind::Command)
    {
        const auto &cmds = command_index::all();
        if (c.command_idx < cmds.size())
            return cmds[c.command_idx].name;
        return {};
    }
    const LogicalRef *lr = recorder::logical_ref_at(c.logical_ref_idx);
    return lr ? lr->display_path : std::string{};
}

// ── Filter helpers ───────────────────────────────────────────────────────────
std::vector<std::string> collect_active_prefixes()
{
    std::vector<std::string> out;
    out.reserve(s_exclusion_category_count * 2);
    for (std::size_t i = 0; i < s_exclusion_category_count; ++i)
    {
        if (!s_exclude_flags[i])
            continue;
        const auto &cat = s_exclusion_categories[i];
        if (cat.prefix_a && cat.prefix_a[0])
            out.emplace_back(cat.prefix_a);
        if (cat.prefix_b && cat.prefix_b[0])
            out.emplace_back(cat.prefix_b);
    }
    return out;
}

// Rebuild both indexes from the active snapshot-filter set and refresh the
// command hooks. Shared by the "Take Snapshot" / "Re-enumerate" buttons and by
// the automatic re-enumeration on aircraft load (ui::reenumerate). Must run on
// the main thread — it issues XPLM enumeration + command-handler calls.
void do_reenumerate()
{
    auto pfx = collect_active_prefixes();
    dataref_index::set_user_exclusions(pfx);
    dataref_index::rebuild();
    command_index::set_user_exclusions(std::move(pfx));
    // Disable BEFORE rebuild — handler refcons encode old indices.
    command_recorder::disable();
    command_index::rebuild();
    command_recorder::enable();
}

// Case-insensitive substring search. Used for the live result-filter — keeps
// the hot path cheap (no regex, no allocations per row).
bool path_matches(const std::string &path, const char *needle)
{
    if (!needle || needle[0] == '\0')
        return true;
    // Fast bail-out: needle longer than path can never match.
    std::size_t nlen = std::strlen(needle);
    if (nlen > path.size())
        return false;
    for (std::size_t i = 0; i + nlen <= path.size(); ++i)
    {
        std::size_t k = 0;
        for (; k < nlen; ++k)
        {
            char a = path[i + k];
            char b = needle[k];
            if (a >= 'A' && a <= 'Z')
                a = static_cast<char>(a + 32);
            if (b >= 'A' && b <= 'Z')
                b = static_cast<char>(b + 32);
            if (a != b)
                break;
        }
        if (k == nlen)
            return true;
    }
    return false;
}

// True if this row is currently hidden as baseline command noise. Queried live
// from command_recorder rather than read off Candidate::is_muted, so that an
// Unmute click takes effect on the very next frame without re-ranking.
bool row_is_muted(const Candidate &c) { return c.kind == Kind::Command && command_recorder::is_muted(c.command_idx); }

// Is this candidate one half of a detected command→DataRef pair? Those rows are
// the actionable conclusion, so they are worth floating to the top of the table
// regardless of where their raw score happened to land.
// Only PRIMARY links count here. Every ref in the cascade behind a command is
// technically "linked" — firing the battery master lights the whole warning
// panel in the same frame — but floating a dozen unrelated lamps to the top of
// the table is worse than not sorting at all.
bool row_is_linked(const Candidate &c)
{
    for (const auto &l : recorder::command_ref_links())
    {
        if (!l.primary)
            continue;
        if (c.kind == Kind::Command && c.command_idx == l.command_idx)
            return true;
        if (c.kind == Kind::DataRef && c.logical_ref_idx == l.logical_ref_idx)
            return true;
    }
    return false;
}

// ── UI body ──────────────────────────────────────────────────────────────────
void draw_excluded_lookup(); // defined below, rendered inside Advanced

void draw_snapshot_filters()
{
    if (!ImGui::CollapsingHeader("Snapshot filters (exclude noisy namespaces)"))
        return;

    ImGui::TextDisabled("Skipped at enumeration - applied on the next 'Take Snapshot'.");
    ImGui::Spacing();

    // Two-column layout keeps the block compact even with ~11 categories.
    const float col_width = 360.f;
    if (ImGui::BeginTable("excl", 2, ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthFixed, col_width);
        ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthFixed, col_width);
        for (std::size_t i = 0; i < s_exclusion_category_count; ++i)
        {
            if ((i % 2) == 0)
                ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char id[64];
            snprintf(id, sizeof(id), "%s##ex%zu", s_exclusion_categories[i].label, i);
            bool before = s_exclude_flags[i];
            if (ImGui::Checkbox(id, &s_exclude_flags[i]) && s_exclude_flags[i] != before)
                s_filters_dirty = true;
        }
        ImGui::EndTable();
    }

    if (s_filters_dirty)
    {
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "Snapshot will re-enumerate to apply filter changes.");
    }
}

void draw_status_bar()
{
    auto st = recorder::status();
    ImGui::Text("Phase: %s", phase_label(st.phase));
    ImGui::SameLine();
    if (st.phase == Phase::Baseline)
    {
        ImGui::Text("   %.1f / %.1f s", static_cast<double>(st.baseline_elapsed_s),
                    static_cast<double>(st.baseline_total_s));
    }
    else if (st.phase == Phase::Record)
    {
        // Show the live max-events counter so the user knows whether their
        // switch flips are being detected. Auto-stop needs >= 3 events on
        // some ref (for bool mode) or expected_clicks events (rotary).
        ImGui::Text("   %.1f s   |   best ref: %d events   |   anchors: %d", static_cast<double>(st.record_elapsed_s),
                    st.best_events_so_far, st.anchors_set);
        // Countdown, so a long mouse trip to the switch never ends in a
        // surprise stop — if it is running low, one more anchor resets it.
        if (st.auto_stop_in_s >= 0.f)
        {
            ImGui::SameLine();
            const bool soon = st.auto_stop_in_s < 3.f;
            ImGui::TextColored(soon ? ImVec4(1.0f, 0.55f, 0.35f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                               "   |   auto-stop in %.0f s", static_cast<double>(st.auto_stop_in_s));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Resets every time you click \"I Acted Now\".\nKeep going for as many cycles as "
                                  "you like - the window adapts to your pace.");
        }
    }
    else if (st.phase == Phase::Inspect)
    {
        ImGui::Text("   %d candidates", st.candidate_count);
        // Without anchors the strongest filter never ran, and the ranking is
        // weaker than it looks. Say so rather than presenting the list as if it
        // had the full evidence behind it.
        if (st.anchors_set == 0)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f),
                               "   No actions were registered - anchor coverage could not be scored.");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("During Record, clicking the cockpit switch registers the action automatically.\n"
                                  "If nothing was registered, the switch may have been operated by other means,\n"
                                  "or the recording ended before you reached it.");
        }
    }
    else if (st.phase == Phase::NoiseCapture)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "   learning ambient... %d refs profiled", st.ignored_count);
    }

    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Hint: %s", hint_text(st.phase).c_str());
    if (st.total_logical > 0)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(%d logical refs, %d ignored, %d watched)", st.total_logical, st.ignored_count,
                            st.watched_count);
    }
    if (st.muted_command_count > 0)
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%d commands muted as baseline noise.",
                           st.muted_command_count);
}

void draw_button_row()
{
    auto st = recorder::status();

    bool is_noise      = (st.phase == Phase::NoiseCapture);
    bool can_baseline  = (st.phase == Phase::Idle || st.phase == Phase::Inspect);
    bool baseline_done = !st.baseline_in_progress && st.ignored_count > 0;
    bool can_record    = baseline_done && st.phase != Phase::Record && st.phase != Phase::Baseline && !is_noise;
    bool can_act       = (st.phase == Phase::Record);
    bool can_stop      = (st.phase == Phase::Record);
    bool can_reset     = (st.phase != Phase::Baseline && !is_noise);
    // "Mark Noise" can start from Idle or Inspect (a baseline is recommended but
    // not required — start_noise_capture seeds its own reference if needed).
    bool can_noise = can_baseline;

    // Left-aligned group labels so the two action rows read as a clear sequence.
    constexpr float kLabelCol = 90.f;

    // ── Workflow: the primary three-step sequence ──
    ImGui::TextDisabled("Workflow");
    ImGui::SameLine(kLabelCol);

    ImGui::BeginDisabled(!can_baseline);
    if (ImGui::Button("Learn Baseline"))
    {
        // Re-enumerate first if exclusion filters have changed since the last
        // index build. Doing it here (rather than inside start_baseline) keeps
        // the filter logic owned by the UI layer and the recorder ignorant of
        // it. start_baseline() will pick up the freshly-built index.
        if (s_filters_dirty)
        {
            do_reenumerate();
            s_filters_dirty = false;
        }
        s_selected_candidate = -1;
        recorder::start_baseline(s_baseline_seconds);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();

    ImGui::BeginDisabled(!can_record);
    if (ImGui::Button("Record"))
    {
        s_selected_candidate = -1;
        recorder::start_record(s_expect_bidirectional);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();

    ImGui::BeginDisabled(!can_stop);
    if (ImGui::Button("Stop"))
        recorder::stop_record();
    ImGui::EndDisabled();

    // ── Refine: optional helpers used mid-flow ──
    ImGui::TextDisabled("Refine");
    ImGui::SameLine(kLabelCol);

    // Mark Noise (subtract): grow the ignore-set with whatever you drive while
    // it's active. While capturing, the button flips to "Stop Noise" and stays
    // enabled so the user can always end the capture.
    if (is_noise)
    {
        // Unique ImGui ID: the Record "Stop" button above is still rendered
        // (disabled) in this phase, so a bare "Stop" label would collide with
        // its ID. The "##noise" suffix is hidden from the visible label.
        if (ImGui::Button("Stop##noise"))
            recorder::stop_noise_capture();
    }
    else
    {
        ImGui::BeginDisabled(!can_noise);
        if (ImGui::Button("Learn Ambient"))
        {
            s_selected_candidate = -1;
            recorder::start_noise_capture();
        }
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Learn an ambient profile: start, drive everything you want filtered out (e.g. power the bus),\n"
            "then Stop. Those refs are excluded from the next Record so only your target stands out.");
    ImGui::SameLine();

    ImGui::BeginDisabled(!can_act);
    if (ImGui::Button("I Acted Now"))
        recorder::mark_user_action();
    ImGui::EndDisabled();

    // ── Advanced: mode, auto-stop, reset, re-enumerate, snapshot filters ──
    // Collapsed by default so first-time users only see the Workflow/Refine
    // rows. Everything power-users need stays one click away.
    if (ImGui::CollapsingHeader("Advanced"))
    {
        ImGui::Indent();

        // Baseline length. The single most effective knob on a noisy aircraft:
        // a command that only re-fires every few seconds can sit out a short
        // window entirely and escape the noise filter.
        ImGui::SetNextItemWidth(220.f);
        ImGui::SliderFloat("Baseline length", &s_baseline_seconds, 2.f, 20.f, "%.1f s");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How long Learn Baseline watches while you hold still.\n"
                              "Longer catches more ambient noise, including commands that only\n"
                              "fire every few seconds. On a chatty aircraft try 10-15 s.");

        // Switch kind. Only the KIND is asked for — how many times you actuate
        // is read from the anchors you place, so there is no count to get wrong.
        ImGui::TextDisabled("Switch kind");
        ImGui::SameLine();
        if (ImGui::RadioButton("Bool (on-off-on)", s_expect_bidirectional))
            s_expect_bidirectional = true;
        ImGui::SameLine();
        if (ImGui::RadioButton("Rotary (steps one way)", !s_expect_bidirectional))
            s_expect_bidirectional = false;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Bool alternates back and forth; rotary walks in one direction.\n"
                              "The number of actuations comes from your \"I Acted Now\" anchors -\n"
                              "just click one before each move and the count takes care of itself.");

        // Auto-stop toggle. With anchors set, Record now ends after a few
        // seconds of quiet following your last anchor, so it can no longer cut
        // a long sequence short. Turning it off makes Record run until Stop.
        bool as = recorder::auto_stop_enabled();
        if (ImGui::Checkbox("Auto-stop when you stop acting", &as))
            recorder::set_auto_stop_enabled(as);
        ImGui::SameLine();
        ImGui::TextDisabled("(off = always stop manually)");

        // Command noise muting. Some aircraft fire commands constantly without
        // any user input (the AW139 chatters on sim/autopilot/disconnect/...),
        // which buries the real candidate in the result table.
        bool mute = command_recorder::mute_enabled();
        if (ImGui::Checkbox("Mute commands that fire during baseline", &mute))
            command_recorder::set_mute_enabled(mute);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Some aircraft fire commands nonstop with no user input (e.g. the AW139 and\n"
                              "sim/autopilot/disconnect/...). Any command that fires while you hold still during\n"
                              "Learn Baseline or Learn Ambient is muted.\n\n"
                              "Muted does not mean discarded: those commands are still recorded in full and listed\n"
                              "under \"Muted as baseline noise\" below the results, where you can unmute any of them.\n"
                              "A muted command that fires close to your \"I Acted Now\" anchors is brought back\n"
                              "automatically and marked with an asterisk.");

        ImGui::Spacing();
        ImGui::BeginDisabled(!can_reset);
        if (ImGui::Button("Reset"))
        {
            s_selected_candidate = -1;
            s_last_write_done    = false;
            recorder::reset();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Re-enumerate"))
        {
            s_selected_candidate = -1;
            do_reenumerate();
            s_filters_dirty = false;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(rebuild ref/command index after an aircraft swap)");

        ImGui::Spacing();
        draw_snapshot_filters();

        ImGui::Spacing();
        draw_excluded_lookup();

        ImGui::Unindent();
    }
}

// "Where did my DataRef go?" — a ref that moved during the baseline is dropped
// from the entire recording, so it appears in no table and no ranking. Without
// this lookup its absence is unexplainable, and longer baselines (now the
// recommendation on noisy aircraft) make it more likely.
void draw_excluded_lookup()
{
    if (!ImGui::CollapsingHeader("Find a missing DataRef"))
        return;

    ImGui::TextWrapped("Can't find a ref you expected? It may have been excluded as baseline noise, or filtered out "
                       "before enumeration. Search for it here.");

    ImGui::SetNextItemWidth(320.f);
    ImGui::InputTextWithHint("##excl", "Part of the path, e.g. battery", s_excluded_filter, sizeof(s_excluded_filter));
    if (s_excluded_filter[0] == '\0')
        return;

    const auto &refs = dataref_index::all();
    int         hits = 0, shown = 0;
    // Cap the output: a two-character query can match thousands of refs, and
    // rendering all of them would stall the frame.
    constexpr int kMaxShown = 40;

    for (std::size_t i = 0; i < refs.size(); ++i)
    {
        if (!path_matches(refs[i].display_path, s_excluded_filter))
            continue;
        ++hits;
        if (shown >= kMaxShown)
            continue;
        ++shown;

        const bool ignored = recorder::is_ignored_as_noise(i);
        if (ignored)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.35f, 1.0f), "excluded as noise  %s", refs[i].display_path.c_str());
        else
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "being watched      %s", refs[i].display_path.c_str());
    }

    if (hits == 0)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f),
                           "Not in the index at all - excluded by a snapshot filter, or the aircraft registers it "
                           "late. Try Re-enumerate.");
    }
    else if (hits > shown)
    {
        ImGui::TextDisabled("...and %d more. Narrow the search.", hits - shown);
    }
}

void draw_muted_commands_section(); // defined below; rendered under the table

// Command→DataRef pairs. This answers the question the ranked table cannot:
// when both a command and a DataRef correlate with the same switch, which one
// do you bind and which one do you read? The command fires first and the ref
// follows — so the command is the actuator and the ref is the status output.
void draw_command_ref_links()
{
    const auto &links = recorder::command_ref_links();
    const auto &cmds  = command_index::all();

    // An empty section used to just vanish, which is indistinguishable from
    // "the feature did not run". Name the reason instead — the causes are
    // specific and actionable.
    if (links.empty())
    {
        if (recorder::status().phase != Phase::Inspect)
            return;

        int cmd_candidates = 0;
        for (const auto &c : recorder::candidates())
            if (c.kind == Kind::Command)
                ++cmd_candidates;

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "No command/DataRef pair found.");
        if (cmd_candidates == 0)
        {
            ImGui::TextWrapped("No command fired at all during the recording, so there was nothing to pair. Either "
                               "the switch is not bound to a command, or its command is not being watched.");
            ImGui::TextDisabled("Watching %zu commands. If this aircraft registers its own commands at runtime "
                                "(xlua/SASL/plugin), check the aircraft-asset scan line in Log.txt - X-Plane offers "
                                "no way to enumerate commands, so their names must be found in the aircraft files.",
                                cmds.size());
        }
        else
        {
            ImGui::TextWrapped("%d command(s) fired, but no DataRef followed them consistently enough to call it a "
                               "pair. The switch may drive its state entirely inside aircraft logic.",
                               cmd_candidates);
        }
        return;
    }

    int n_primary = 0;
    for (const auto &l : links)
        if (l.primary)
            ++n_primary;
    const int n_cascade = static_cast<int>(links.size()) - n_primary;

    // Renders one table over the subset selected by `want_primary`.
    auto link_table = [&](const char *table_id, bool want_primary)
    {
        ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable;
        if (!ImGui::BeginTable(table_id, 4, flags))
            return;

        ImGui::TableSetupColumn(want_primary ? "Command (send this)" : "Command", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(want_primary ? "DataRef (read this)" : "DataRef that also reacted",
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Fires", ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableSetupColumn("Delay", ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableHeadersRow();

        for (const auto &l : links)
        {
            if (l.primary != want_primary)
                continue;
            const LogicalRef *lr = recorder::logical_ref_at(l.logical_ref_idx);
            if (!lr || l.command_idx >= cmds.size())
                continue;

            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "%s", cmds[l.command_idx].name.c_str());

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(lr->display_path.c_str());
            if (want_primary && ImGui::IsItemHovered())
                ImGui::SetTooltip("Chosen over %d other ref(s) that reacted to the same command:\n"
                                  "%d shared name token(s) with the command, candidate score %.0f.",
                                  n_cascade, l.name_affinity, static_cast<double>(l.ref_score));

            ImGui::TableNextColumn();
            ImGui::Text("%d/%d", l.fires_matched, l.fires_total);

            ImGui::TableNextColumn();
            ImGui::Text("%.0f ms", static_cast<double>(l.median_delay_ms));
        }
        ImGui::EndTable();
    };

    ImGui::Spacing();
    char header[96];
    snprintf(header, sizeof(header), "Command drives DataRef (%d)###cmdlinks", n_primary);
    // Open by default: this is usually the answer the user came for.
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (!ImGui::CollapsingHeader(header))
        return;

    ImGui::TextWrapped("The command fired first and the DataRef followed within a few frames, on every single fire. "
                       "For a Streamdeck or hardware binding that means: send the COMMAND, read the DATAREF for "
                       "state. Writing the DataRef directly may only move the switch without driving the systems "
                       "behind it - use the Compare button below to check.");

    if (n_primary > 0)
        link_table("cmdlinks", true);

    // Everything else the command moved. Shown separately and collapsed: these
    // are consequences of the state ref (warning lamps, bus voltages), not
    // candidates for the binding, and listing them alongside the real pair is
    // what buried the answer in the first place.
    if (n_cascade > 0)
    {
        char casc[112];
        snprintf(casc, sizeof(casc), "Cascade: %d more ref(s) reacted to these commands###cmdcascade", n_cascade);
        if (ImGui::CollapsingHeader(casc))
        {
            ImGui::TextWrapped("Downstream effects - lamps, voltages, annunciators that the aircraft updates once "
                               "the state ref changes. Useful for confirming the cascade is real, not for binding.");
            link_table("cmdcascade_tbl", false);
        }
    }
}

void draw_candidates_table()
{
    const auto &cands = recorder::candidates();
    if (cands.empty())
    {
        auto st = recorder::status();
        if (st.phase == Phase::Inspect)
        {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "No correlating DataRef or Command found.");
            ImGui::TextWrapped("The switch may be handled entirely inside aircraft Lua/SASL with no observable "
                               "DataRef or sim/* Command. Try Re-enumerate after the aircraft has fully loaded - "
                               "plugin commands often register lazily.");
        }
        else
        {
            ImGui::Spacing();
            ImGui::TextDisabled("No candidates yet.");
        }
        return;
    }

    ImGui::Spacing();
    ImGui::Text("Candidates: %zu", cands.size());

    // Kind filter checkboxes — pure client-side row filter. Defaults to both
    // visible so existing DataRef-only workflows are unaffected.
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(16, 0));
    ImGui::SameLine();
    ImGui::Checkbox("DataRefs", &s_show_datarefs);
    ImGui::SameLine();
    ImGui::Checkbox("Commands", &s_show_commands);
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(16, 0));
    ImGui::SameLine();
    // Writable-only filter — hides read-only DataRefs so the list narrows to
    // refs the user can actually drive from outside. Commands are unaffected
    // (they have no writable concept; fires are always "doable").
    ImGui::Checkbox("Writable only", &s_writable_only);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hide read-only DataRefs. Commands are always shown.");

    if (!recorder::command_ref_links().empty())
    {
        ImGui::SameLine();
        ImGui::Checkbox("Pairs first", &s_linked_first);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Float rows belonging to a detected command/DataRef pair to the top of the list,\n"
                              "highlighted in green. The Rank column keeps showing each row's true position,\n"
                              "so nothing is renumbered. Turn off for pure score order.");
    }

    // Live substring filter on the path column. The displayed Rank stays the
    // true index in cands[], so hidden rows never push visible rows into a
    // different number — this is what keeps Copy path / Copy code snippet
    // unambiguous when something is filtered out.
    ImGui::SetNextItemWidth(300.f);
    ImGui::InputTextWithHint("##resultfilter", "Filter by keyword (e.g. cockpit)", s_result_filter,
                             sizeof(s_result_filter));
    ImGui::SameLine();
    ImGui::TextDisabled("(case-insensitive substring on path)");
    if (s_result_filter[0] != '\0')
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##rf"))
            s_result_filter[0] = '\0';
    }

    // The "now" value in the Delta column is read live every frame. After Record
    // stops you can re-flip the switch and watch which candidate still reacts
    // (its value moves and turns yellow) — a quick way to break ties.
    ImGui::TextDisabled("Tip: \"now\" values are live - re-flip the switch to see which candidate still reacts.");

    auto row_visible = [&](const Candidate &c, const std::string &path) -> bool
    {
        // Muted commands are not dropped — they move to the collapsible
        // "Muted (baseline noise)" section below, with their full fire history.
        if (row_is_muted(c))
            return false;
        if (c.kind == Kind::DataRef && !s_show_datarefs)
            return false;
        if (c.kind == Kind::Command && !s_show_commands)
            return false;
        if (s_writable_only && c.kind == Kind::DataRef && !c.is_writable)
            return false;
        if (s_result_filter[0] != '\0' && !path_matches(path, s_result_filter))
            return false;
        return true;
    };

    // Reads a DataRef candidate's current value straight from its cached handle.
    // The draw callback runs every frame regardless of recorder phase, so this
    // keeps the table live after Record stops (Inspect): re-flip the switch and
    // the reacting ref's value visibly moves while the others stay put — the
    // quickest way to confirm which candidate is really the one. Cheap: one SDK
    // read per visible DataRef row, handle already resolved at enumeration time.
    auto live_value = [](const Candidate &c, SampleValue &out) -> bool
    {
        if (c.kind != Kind::DataRef)
            return false;
        const LogicalRef *lr = recorder::logical_ref_at(c.logical_ref_idx);
        return lr && dataref_index::read(*lr, out);
    };

    // Display order. The Rank column keeps showing the TRUE index in cands[],
    // so reordering never renumbers anything — a row pulled to the top still
    // reads "17" and Copy path / the test panel stay unambiguous.
    const bool       group_linked = s_linked_first && !recorder::command_ref_links().empty();
    std::vector<int> order;
    order.reserve(cands.size());
    if (group_linked)
    {
        // Two passes rather than a sort: both groups keep their score ordering
        // and the pass is stable by construction.
        for (int i = 0; i < static_cast<int>(cands.size()); ++i)
            if (row_is_linked(cands[i]))
                order.push_back(i);
        for (int i = 0; i < static_cast<int>(cands.size()); ++i)
            if (!row_is_linked(cands[i]))
                order.push_back(i);
    }
    else
    {
        for (int i = 0; i < static_cast<int>(cands.size()); ++i)
            order.push_back(i);
    }

    ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    ImVec2 table_size(0.f, 280.f);
    if (ImGui::BeginTable("cands", 11, flags, table_size))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Rank", ImGuiTableColumnFlags_WidthFixed, 50.f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 56.f);
        ImGui::TableSetupColumn("R/W", ImGuiTableColumnFlags_WidthFixed, 36.f);
        ImGui::TableSetupColumn("Delta / Fires", ImGuiTableColumnFlags_WidthFixed, 180.f);
        ImGui::TableSetupColumn("Anchors", ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableSetupColumn("Reacted", ImGuiTableColumnFlags_WidthFixed, 90.f);
        ImGui::TableSetupColumn("Lat (ms)", ImGuiTableColumnFlags_WidthFixed, 80.f);
        ImGui::TableSetupColumn("Dir", ImGuiTableColumnFlags_WidthFixed, 50.f);
        ImGui::TableHeadersRow();

        for (int i : order)
        {
            const Candidate &c    = cands[i];
            std::string      path = candidate_path(c);
            if (path.empty())
                continue;

            if (!row_visible(c, path))
                continue;

            ImGui::TableNextRow();

            // Tint the paired rows so it stays obvious WHY they are on top —
            // otherwise a row at the head of the list looks like it simply
            // outscored everything.
            if (group_linked && row_is_linked(c))
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.16f, 0.30f, 0.22f, 1.f)));

            ImGui::TableNextColumn();
            char rank_buf[16];
            snprintf(rank_buf, sizeof(rank_buf), "%d##rk%d", i + 1, i);
            bool selected = (s_selected_candidate == i);
            if (ImGui::Selectable(rank_buf, selected, ImGuiSelectableFlags_SpanAllColumns))
            {
                s_selected_candidate = i;
                s_last_write_done    = false;
                // Pre-fill the test value with the current value so a write
                // is non-destructive unless the user changes it (DataRef rows).
                s_write_int    = c.current_value.i;
                s_write_float  = c.current_value.f;
                s_write_double = c.current_value.d;
            }

            ImGui::TableNextColumn();
            // Tint Command rows so the eye can sweep down the column quickly.
            if (c.kind == Kind::Command && c.auto_unmuted)
            {
                // This command fired during baseline (so it was muted as noise)
                // but behaved purposefully during Record. Flag it rather than
                // silently promoting it — the user should know it was suspect.
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "Command *");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Fired %d time(s) during baseline, so it was muted as noise.\n"
                                      "Brought back automatically: %d fire(s) during Record with a\n"
                                      "median latency of %.0f ms to your \"I Acted Now\" anchors.",
                                      c.baseline_fires, c.fire_count, static_cast<double>(c.median_latency_ms));
            }
            else if (c.kind == Kind::Command)
            {
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "Command");
            }
            else
            {
                ImGui::TextUnformatted("DataRef");
            }

            ImGui::TableNextColumn();
            ImGui::Text("%.0f", static_cast<double>(c.score));

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(path.c_str());

            ImGui::TableNextColumn();
            if (c.kind == Kind::Command)
                ImGui::TextDisabled("--");
            else
                ImGui::TextUnformatted(type_name(c.type));

            ImGui::TableNextColumn();
            if (c.kind == Kind::Command)
            {
                ImGui::TextDisabled("--");
            }
            else if (c.is_writable)
            {
                ImGui::TextUnformatted("RW");
            }
            else
            {
                // Dimmed read-only marker so the eye can spot the write-capable
                // refs without having to open the test panel for each one.
                ImGui::TextColored(ImVec4(0.75f, 0.55f, 0.55f, 1.0f), "RO");
            }

            ImGui::TableNextColumn();
            if (c.kind == Kind::Command)
            {
                ImGui::Text("Fires: %d", c.fire_count);
            }
            else
            {
                // Show observed range, not baseline→current. Switches that toggle
                // back to their starting state (e.g. ON-OFF-ON) end with baseline
                // == current — but the swing in between is the interesting part.
                // The "now" value is read live every frame (see live_value), so
                // re-toggling the switch in Inspect updates it in place.
                SampleValue       live{};
                const bool        have_live = live_value(c, live);
                const std::string nw        = value_label(c.type, have_live ? live : c.current_value);
                const bool        moved_now = have_live && nw != value_label(c.type, c.current_value);

                std::string mn = value_label(c.type, c.min_seen);
                std::string mx = value_label(c.type, c.max_seen);

                char cell[112];
                if (mn == mx)
                    snprintf(cell, sizeof(cell), "= %s", nw.c_str());
                else
                    snprintf(cell, sizeof(cell), "%s .. %s  (now %s)", mn.c_str(), mx.c_str(), nw.c_str());

                // Highlight when the live value currently differs from where
                // Record left it — i.e. the ref is reacting right now.
                if (moved_now)
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s", cell);
                else
                    ImGui::TextUnformatted(cell);
            }

            ImGui::TableNextColumn();
            if (c.anchors_total <= 0)
            {
                ImGui::TextDisabled("--");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("No \"I Acted Now\" anchors were placed, so coverage could not be measured.\n"
                                      "Anchors are the strongest filter this tool has - use them.");
            }
            else
            {
                // Full coverage with no strays is the signature of the real
                // target. Colour it so the eye finds those rows immediately.
                const bool perfect = (c.anchors_hit == c.anchors_total) && c.orphan_events == 0;
                if (perfect)
                    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%d/%d", c.anchors_hit, c.anchors_total);
                else
                    ImGui::Text("%d/%d", c.anchors_hit, c.anchors_total);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Answered %d of your %d actuations, with %d event(s) belonging to none of "
                                      "them.\nThe more times you work the switch, the harder this is to fake.",
                                      c.anchors_hit, c.anchors_total, c.orphan_events);
            }

            // Causal order: how long after your action this ref started moving.
            // The first mover is the cause; everything behind it is the cascade
            // that cause set off.
            ImGui::TableNextColumn();
            if (!c.has_onset_lag)
            {
                ImGui::TextDisabled("--");
            }
            else if (c.is_first_mover)
            {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "1st %.0fms", static_cast<double>(c.onset_lag_ms));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Reacted before every other candidate - the likely cause.\n"
                                      "Rows below it moved later and are probably effects of this one.");
            }
            else
            {
                ImGui::Text("+%.0f ms", static_cast<double>(c.onset_lag_ms));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Median delay from your action to this ref moving.\n"
                                      "Later than the first mover, so more likely a downstream effect.");
            }

            ImGui::TableNextColumn();
            if (c.has_latency)
                ImGui::Text("%.0f", static_cast<double>(c.min_latency_ms));
            else
                ImGui::TextDisabled("--");

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(direction_icon(c));
        }
        ImGui::EndTable();
    }

    // Per-row action buttons under the table (work on s_selected_candidate).
    // If the selected row is currently filtered out, hide the action block so
    // the user can't accidentally copy a path that isn't visible. Selection
    // state is preserved across filter changes — clearing the filter brings
    // the row (and its action buttons) back without losing context.
    if (s_selected_candidate >= 0 && s_selected_candidate < static_cast<int>(cands.size()))
    {
        const Candidate &c    = cands[s_selected_candidate];
        std::string      path = candidate_path(c);
        if (!path.empty())
        {
            bool selection_visible = row_visible(c, path);
            if (selection_visible)
            {
                ImGui::Spacing();
                ImGui::TextDisabled("Selected: %s", path.c_str());
                if (ImGui::Button("Copy path"))
                    clipboard::copy(path);
                ImGui::SameLine();
                if (ImGui::Button("Copy code snippet"))
                {
                    if (c.kind == Kind::Command)
                    {
                        const auto &cmds = command_index::all();
                        if (c.command_idx < cmds.size())
                            clipboard::copy(command_index::code_snippet(cmds[c.command_idx]));
                    }
                    else
                    {
                        const LogicalRef *lr = recorder::logical_ref_at(c.logical_ref_idx);
                        if (lr)
                            clipboard::copy(dataref_index::code_snippet(*lr));
                    }
                }
            }
            else
            {
                ImGui::Spacing();
                ImGui::TextDisabled("Selected entry is hidden by the filter - clear the filter to act on it.");
            }
        }
    }

    draw_muted_commands_section();
}

// Commands muted as baseline noise. Deliberately a full table rather than a
// plain name list: the whole point of muting-instead-of-dropping is that the
// evidence survives, so the user can judge for themselves whether something
// was wrongly filtered. Columns are reduced to what applies to commands.
void draw_muted_commands_section()
{
    const auto &cands = recorder::candidates();
    const auto &cmds  = command_index::all();

    int muted_rows = 0;
    for (const auto &c : cands)
        if (row_is_muted(c))
            ++muted_rows;
    if (muted_rows == 0)
        return;

    ImGui::Spacing();
    char header[96];
    snprintf(header, sizeof(header), "Muted as baseline noise (%d)###mutedcmds", muted_rows);
    if (!ImGui::CollapsingHeader(header))
        return;

    ImGui::TextWrapped("These commands already fired while the cockpit was idle, so they were muted to keep the "
                       "list readable. They were still recorded in full - Unmute to move one back up.");

    ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    ImVec2 table_size(0.f, 160.f);
    if (!ImGui::BeginTable("mutedcands", 6, flags, table_size))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Rank", ImGuiTableColumnFlags_WidthFixed, 50.f);
    ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, 60.f);
    ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Baseline", ImGuiTableColumnFlags_WidthFixed, 80.f);
    ImGui::TableSetupColumn("Record", ImGuiTableColumnFlags_WidthFixed, 70.f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80.f);
    ImGui::TableHeadersRow();

    for (int i = 0; i < static_cast<int>(cands.size()); ++i)
    {
        const Candidate &c = cands[i];
        if (!row_is_muted(c) || c.command_idx >= cmds.size())
            continue;
        const std::string &name = cmds[c.command_idx].name;
        if (s_result_filter[0] != '\0' && !path_matches(name, s_result_filter))
            continue;

        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::TextDisabled("%d", i + 1);

        ImGui::TableNextColumn();
        ImGui::TextDisabled("%.0f", static_cast<double>(c.score));

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(name.c_str());

        ImGui::TableNextColumn();
        ImGui::Text("%d fires", c.baseline_fires);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Fires observed while the cockpit was idle. This is why the row was muted.");

        ImGui::TableNextColumn();
        ImGui::Text("%d fires", c.fire_count);

        ImGui::TableNextColumn();
        char btn[32];
        snprintf(btn, sizeof(btn), "Unmute##um%d", i);
        if (ImGui::SmallButton(btn))
            command_recorder::set_muted(c.command_idx, false);
    }
    ImGui::EndTable();
}

void draw_test_panel_command(const Candidate &c)
{
    const auto &cmds = command_index::all();
    if (c.command_idx >= cmds.size())
        return;
    const CommandEntry &cmd = cmds[c.command_idx];

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Test panel - fire command");
    ImGui::TextDisabled("%s", cmd.name.c_str());
    if (!cmd.description.empty())
        ImGui::TextWrapped("%s", cmd.description.c_str());

    ImGui::Spacing();
    if (ImGui::Button("Once"))
        command_recorder::test_fire(c.command_idx, /*mode=*/0);
    ImGui::SameLine();
    if (ImGui::Button("Begin"))
        command_recorder::test_fire(c.command_idx, /*mode=*/1);
    ImGui::SameLine();
    if (ImGui::Button("End"))
        command_recorder::test_fire(c.command_idx, /*mode=*/2);

    ImGui::TextWrapped("Once is the safe default (Begin+End in one call). Begin/End are paired - use only when "
                       "the command is meant to be held (e.g. starter motor). Mismatched Begin/End can leave the "
                       "command stuck.");

    // Let the user push a row back into the muted set. Reachable for any
    // command row, but mainly for the auto-unmuted ones: if the heuristic
    // guessed wrong, one click gets the list clean again.
    if (c.baseline_fires > 0)
    {
        ImGui::Spacing();
        if (ImGui::SmallButton("Mute as noise"))
            command_recorder::set_muted(c.command_idx, true);
        ImGui::SameLine();
        ImGui::TextDisabled("(fired %d time(s) during baseline)", c.baseline_fires);
    }
}

// Cascade comparison. Firing the command runs the aircraft's own logic and the
// whole cascade follows; writing the status DataRef may only move the switch
// graphic. Measuring both settles the question instead of guessing at it.
void draw_cascade_compare(const Candidate &c)
{
    const auto &cmd_res = recorder::probe_command_result();
    const auto &ref_res = recorder::probe_dataref_result();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Compare: command vs. direct DataRef write");

    if (recorder::probe_in_progress())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "Measuring... hands off the cockpit.");
        return;
    }

    ImGui::TextWrapped("Run both probes, then compare. Each one performs the action and counts how many watched "
                       "refs move within %.1f s. If the command moves clearly more, the DataRef is only the tip of "
                       "the cascade and you must send the command.",
                       1.5);

    // Probe the command side. For a DataRef row we offer the command from its
    // detected link, so the user never has to hunt for the counterpart.
    int cmd_candidate = -1;
    if (c.kind == Kind::Command)
    {
        cmd_candidate = s_selected_candidate;
    }
    else
    {
        for (const auto &l : recorder::command_ref_links())
        {
            if (l.logical_ref_idx != c.logical_ref_idx)
                continue;
            const auto &cands = recorder::candidates();
            for (int i = 0; i < static_cast<int>(cands.size()); ++i)
            {
                if (cands[i].kind == Kind::Command && cands[i].command_idx == l.command_idx)
                {
                    cmd_candidate = i;
                    break;
                }
            }
            if (cmd_candidate >= 0)
                break;
        }
    }

    ImGui::BeginDisabled(cmd_candidate < 0);
    if (ImGui::Button("Probe: fire command"))
        recorder::probe_start(static_cast<std::size_t>(cmd_candidate), recorder::ProbeAction::FireCommand,
                              SampleValue{});
    ImGui::EndDisabled();
    if (cmd_candidate < 0 && ImGui::IsItemHovered())
        ImGui::SetTooltip("No linked command found for this DataRef - select a Command row instead.");

    // Probe the DataRef side, using whatever the test-panel value box holds.
    int ref_candidate = -1;
    if (c.kind == Kind::DataRef)
    {
        ref_candidate = s_selected_candidate;
    }
    else
    {
        for (const auto &l : recorder::command_ref_links())
        {
            if (l.command_idx != c.command_idx)
                continue;
            const auto &cands = recorder::candidates();
            for (int i = 0; i < static_cast<int>(cands.size()); ++i)
            {
                if (cands[i].kind == Kind::DataRef && cands[i].logical_ref_idx == l.logical_ref_idx)
                {
                    ref_candidate = i;
                    break;
                }
            }
            if (ref_candidate >= 0)
                break;
        }
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(ref_candidate < 0);
    if (ImGui::Button("Probe: write DataRef"))
    {
        const auto &cands = recorder::candidates();
        SampleValue v{};
        if (ref_candidate < static_cast<int>(cands.size()))
        {
            switch (cands[ref_candidate].type)
            {
            case RefType::Int:
            case RefType::IntArrayElem:
                v.i = s_write_int;
                break;
            case RefType::Float:
            case RefType::FloatArrayElem:
                v.f = s_write_float;
                break;
            case RefType::Double:
                v.d = s_write_double;
                break;
            }
        }
        recorder::probe_start(static_cast<std::size_t>(ref_candidate), recorder::ProbeAction::WriteDataRef, v);
    }
    ImGui::EndDisabled();
    if (ref_candidate < 0 && ImGui::IsItemHovered())
        ImGui::SetTooltip("No linked DataRef found for this command - select a DataRef row instead.");

    ImGui::SameLine();
    if (ImGui::Button("Clear##probes"))
        recorder::probe_clear();

    if (!cmd_res.valid && !ref_res.valid)
        return;

    ImGui::Spacing();
    if (ImGui::BeginTable("probes", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 120.f);
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Refs moved", ImGuiTableColumnFlags_WidthFixed, 110.f);
        ImGui::TableHeadersRow();

        auto row = [](const char *label, const recorder::ProbeResult &r)
        {
            if (!r.valid)
                return;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.action_label.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d of %d", r.refs_moved, r.refs_sampled);
        };
        row("Fire command", cmd_res);
        row("Write DataRef", ref_res);
        ImGui::EndTable();
    }

    // Only draw a conclusion once both sides have actually been measured.
    // Half a comparison is not evidence.
    if (!cmd_res.valid || !ref_res.valid)
    {
        ImGui::TextDisabled("Run both probes to get a verdict.");
        return;
    }

    if (cmd_res.refs_moved > ref_res.refs_moved)
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                           "The command moved %d more ref(s). Send the COMMAND and read the DataRef for state.",
                           cmd_res.refs_moved - ref_res.refs_moved);
    else if (ref_res.refs_moved > cmd_res.refs_moved)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f),
                           "The DataRef write moved MORE refs than the command (%d vs %d). Unexpected - the two "
                           "probes may have started from different switch states. Reset both and retry from the "
                           "same starting position.",
                           ref_res.refs_moved, cmd_res.refs_moved);
    else
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                           "Both moved %d ref(s) - no measurable difference. Writing the DataRef appears equivalent "
                           "here, but this only covers the refs watched in the last Record.",
                           cmd_res.refs_moved);
}

// Slim bar shown instead of the main window while recording, so the cockpit
// stays reachable. It has to earn its screen space three ways: prove the clicks
// are being counted, say how long is left, and offer Stop without a hunt.
void draw_recording_overlay(float screen_w)
{
    const auto st = recorder::status();

    constexpr float kBarW   = 460.f;
    constexpr float kMargin = 16.f;
    ImGui::SetNextWindowPos(ImVec2(screen_w - kBarW - kMargin, kMargin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kBarW, 0.f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_AlwaysAutoResize;

    if (!ImGui::Begin("##recbar", nullptr, flags))
    {
        ImGui::End();
        return;
    }

    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "REC");
    ImGui::SameLine();
    ImGui::Text("%.0f s", static_cast<double>(st.record_elapsed_s));
    ImGui::SameLine();

    // The anchor count is the whole point of the overlay: it is live proof that
    // clicking the cockpit switch is registering, with no trip back here.
    if (st.anchors_set > 0)
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "| %d actions", st.anchors_set);
    else
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "| click the switch...");

    ImGui::SameLine();
    if (st.auto_stop_in_s >= 0.f)
    {
        const bool soon = st.auto_stop_in_s < 3.f;
        ImGui::TextColored(soon ? ImVec4(1.0f, 0.55f, 0.35f, 1.0f) : ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "| ends %.0fs",
                           static_cast<double>(st.auto_stop_in_s));
    }
    else
    {
        ImGui::TextDisabled("| manual stop");
    }

    ImGui::SameLine();
    // Clicks on this button are consumed by ImGui, so they can never be
    // mistaken for a cockpit actuation — the anchor hook only fires on clicks
    // ImGui declined.
    if (ImGui::Button("Stop"))
        recorder::stop_record();

    ImGui::TextDisabled("Work the switch several times - every cockpit click counts as one action.");
    ImGui::End();
}

void draw_test_panel()
{
    const auto &cands = recorder::candidates();
    if (s_selected_candidate < 0 || s_selected_candidate >= static_cast<int>(cands.size()))
        return;

    const Candidate &c = cands[s_selected_candidate];

    if (c.kind == Kind::Command)
    {
        // Skip rendering when the row is currently filtered out (same guard
        // logic as the DataRef branch below).
        const auto &cmds = command_index::all();
        if (c.command_idx >= cmds.size())
            return;
        if (!s_show_commands)
            return;
        if (s_result_filter[0] != '\0' && !path_matches(cmds[c.command_idx].name, s_result_filter))
            return;
        draw_test_panel_command(c);
        draw_cascade_compare(c);
        return;
    }

    const LogicalRef *lr = recorder::logical_ref_at(c.logical_ref_idx);
    if (!lr)
        return;

    // Don't render the test panel while the selected ref is hidden by the
    // active result filter — same rationale as the Copy-button guard above.
    if (!s_show_datarefs)
        return;
    if (s_result_filter[0] != '\0' && !path_matches(lr->display_path, s_result_filter))
        return;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Test panel - write value to candidate");

    ImGui::Text("Writable (XPLMCanWriteDataRef): %s", lr->is_writable ? "YES" : "NO");
    if (!lr->is_writable)
        ImGui::TextDisabled("The SDK reports this ref as read-only. Write attempts will be refused.");

    ImGui::Spacing();
    bool can_write = lr->is_writable;
    switch (c.type)
    {
    case RefType::Int:
    case RefType::IntArrayElem:
        ImGui::SetNextItemWidth(160.f);
        ImGui::InputInt("Value (int)", &s_write_int);
        break;
    case RefType::Float:
    case RefType::FloatArrayElem:
        ImGui::SetNextItemWidth(160.f);
        ImGui::InputFloat("Value (float)", &s_write_float, 0.f, 0.f, "%.6f");
        break;
    case RefType::Double:
        ImGui::SetNextItemWidth(160.f);
        ImGui::InputDouble("Value (double)", &s_write_double, 0.0, 0.0, "%.8f");
        break;
    }

    ImGui::BeginDisabled(!can_write);
    if (ImGui::Button("Write"))
    {
        SampleValue v{};
        switch (c.type)
        {
        case RefType::Int:
        case RefType::IntArrayElem:
            v.i = s_write_int;
            break;
        case RefType::Float:
        case RefType::FloatArrayElem:
            v.f = s_write_float;
            break;
        case RefType::Double:
            v.d = s_write_double;
            break;
        }
        s_last_write_done = recorder::test_write(static_cast<std::size_t>(s_selected_candidate), v,
                                                 s_last_write_writable, s_last_readback);
    }
    ImGui::EndDisabled();

    if (s_last_write_done)
    {
        ImGui::SameLine();
        ImGui::Text("Readback: %s", value_label(c.type, s_last_readback).c_str());
    }

    ImGui::TextWrapped("Write executed (or refused). If the cockpit shows no reaction, the DataRef may still be the "
                       "correct one but is read-only, or the aircraft's own logic overwrites it every frame - in that "
                       "case it cannot be driven from outside; look for an associated Command instead.");

    draw_cascade_compare(c);
}

} // namespace

// Public entry point for an out-of-band re-enumeration (e.g. the plugin's
// aircraft-load handler). Same effect as the "Re-enumerate" button, honouring
// the currently active snapshot filters. Main thread only.
void reenumerate() { do_reenumerate(); }

// ── Lifecycle ────────────────────────────────────────────────────────────────
void init()
{
    IMGUI_CHECKVERSION();
    s_imgui_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(s_imgui_ctx);

    ImGuiIO &io    = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui::StyleColorsDark();
    auto &style          = ImGui::GetStyle();
    style.WindowRounding = 6.f;
    style.FrameRounding  = 3.f;
    style.WindowPadding  = ImVec2(8, 6);

    ImGui_ImplOpenGL2_Init();
    s_last_frame_time = get_xp_time();

    // Seed exclusion checkboxes from the per-category defaults. s_filters_dirty
    // stays true so the first Take Snapshot rebuilds the index with these
    // filters applied (otherwise rebuild() never runs and defaults are no-op).
    for (std::size_t i = 0; i < s_exclusion_category_count; ++i)
        s_exclude_flags[i] = s_exclusion_categories[i].default_on;
}

void stop()
{
    if (s_wnd)
    {
        XPLMDestroyWindow(s_wnd);
        s_wnd = nullptr;
    }
    if (s_imgui_ctx)
    {
        ImGui::SetCurrentContext(s_imgui_ctx);
        ImGui_ImplOpenGL2_Shutdown();
        ImGui::DestroyContext(s_imgui_ctx);
        s_imgui_ctx = nullptr;
    }
}

void draw()
{
    if (!s_open)
        return;

    ImGui::SetCurrentContext(s_imgui_ctx);

    int gl, gt, gr, gb;
    XPLMGetScreenBoundsGlobal(&gl, &gt, &gr, &gb);
    int sw = gr - gl;
    int sh = gt - gb;
    if (sw <= 0 || sh <= 0)
        return;

    if (s_wnd)
    {
        int wl, wt, wr, wb;
        XPLMGetWindowGeometry(s_wnd, &wl, &wt, &wr, &wb);
        if (wl != gl || wb != gb || wr != gr || wt != gt)
            XPLMSetWindowGeometry(s_wnd, gl, gt, gr, gb);
    }

    GLint prev_viewport[4];
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glPushAttrib(GL_TRANSFORM_BIT | GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_SCISSOR_BIT |
                 GL_TEXTURE_BIT);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int fb_w = prev_viewport[2];
    int fb_h = prev_viewport[3];

    glViewport(0, 0, fb_w, fb_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, sw, sh, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    ImGuiIO &io       = ImGui::GetIO();
    double   now      = get_xp_time();
    io.DeltaTime      = static_cast<float>(std::max(now - s_last_frame_time, 0.001));
    s_last_frame_time = now;
    io.DisplaySize    = ImVec2(static_cast<float>(sw), static_cast<float>(sh));
    io.DisplayFramebufferScale =
        ImVec2(static_cast<float>(fb_w) / static_cast<float>(sw), static_cast<float>(fb_h) / static_cast<float>(sh));

    ImGui_ImplOpenGL2_NewFrame();
    ImGui::NewFrame();

    // During Record the main window steps aside entirely: it would sit between
    // the user and the cockpit switch they need to reach. Only a slim bar stays
    // up. The XPLM capture window MUST remain visible throughout — hiding it
    // would cut off the very clicks we now use as anchors.
    if (recorder::phase() == Phase::Record)
    {
        draw_recording_overlay(static_cast<float>(sw));
    }
    else
    {
        float win_w = 980.f, win_h = 720.f;
        ImGui::SetNextWindowPos(
            ImVec2((static_cast<float>(sw) - win_w) * 0.5f, (static_cast<float>(sh) - win_h) * 0.5f),
            ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(win_w, win_h), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(640, 360), ImVec2(3840, 2160));

        bool open = true;
#ifdef XP_SHERLOCK_VERSION
        static const std::string title = std::string("DataRef Detective v") + XP_SHERLOCK_VERSION + "##xp_sherlock";
#else
        static const std::string title = "DataRef Detective##xp_sherlock";
#endif
        if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse))
        {
            draw_status_bar();
            ImGui::Separator();
            draw_button_row();
            ImGui::Separator();
            // Pairs come before the ranked list: when a link exists it is the
            // conclusion, and the table below is the supporting evidence.
            draw_command_ref_links();
            draw_candidates_table();
            draw_test_panel();
        }
        ImGui::End();
        s_open = open;
        if (!s_open && s_wnd)
        {
            XPLMSetWindowIsVisible(s_wnd, 0);
            XPLMTakeKeyboardFocus(nullptr);
        }
    }

    sync_keyboard_focus(io);

    ImGui::Render();
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glPopAttrib();
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
}

void toggle()
{
    if (s_open)
    {
        s_open = false;
        if (s_wnd)
        {
            XPLMSetWindowIsVisible(s_wnd, 0);
            XPLMTakeKeyboardFocus(nullptr);
        }
        return;
    }
    create_capture_window_if_needed();
    if (s_wnd)
    {
        XPLMSetWindowIsVisible(s_wnd, 1);
        XPLMBringWindowToFront(s_wnd);
    }
    s_open = true;
}

bool is_open() { return s_open; }

} // namespace ui
} // namespace xp_sherlock
