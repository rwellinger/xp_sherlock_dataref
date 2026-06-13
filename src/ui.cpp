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
XPLMWindowID  s_wnd        = nullptr;
ImGuiContext *s_imgui_ctx  = nullptr;
bool          s_open       = false;

double s_last_frame_time = 0.0;

double get_xp_time()
{
    static XPLMDataRef dr = nullptr;
    if (!dr)
        dr = XPLMFindDataRef("sim/time/total_running_time_sec");
    return dr ? static_cast<double>(XPLMGetDataf(dr)) : 0.0;
}

// ── UI state ─────────────────────────────────────────────────────────────────
bool s_expect_bidirectional = true;
int  s_expected_clicks      = 3;
int  s_selected_candidate   = -1;

// Test panel state — separate value buffer per scalar/array kind
int    s_write_int    = 1;
float  s_write_float  = 1.0f;
double s_write_double = 1.0;

bool        s_last_write_done    = false;
bool        s_last_write_writable = false;
SampleValue s_last_readback      = {};

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
    {"Aircraft static config",                       "sim/aircraft/",    "",                  true},
    {"Airfoil tables (aero coefficients)",           "sim/airfoils/",    "",                  true},
    {"Weather & atmosphere",                         "sim/weather/",     "sim/atmosphere/",   true},
    {"Joystick raw input",                           "sim/joystick/",    "",                  true},
    {"Network / multiplayer",                        "sim/network/",     "sim/multiplayer/",  true},
    {"World / terrain",                              "sim/world/",       "",                  true},
    {"Version info",                                 "sim/version/",     "",                  true},
    {"Test scaffolding",                             "sim/test/",        "",                  true},
    {"Time",                                         "sim/time/",        "",                  false},
    {"Graphics / rendering",                         "sim/graphics/",    "",                  false},
    {"Operation",                                    "sim/operation/",   "",                  false},
};
constexpr std::size_t s_exclusion_category_count =
    sizeof(s_exclusion_categories) / sizeof(s_exclusion_categories[0]);

bool s_exclude_flags[s_exclusion_category_count] = {}; // initialised in init()
bool s_filters_dirty = true; // forces rebuild() before first snapshot, then per-toggle

// ── Result filter (live substring filter on candidate table) ─────────────────
char s_result_filter[128] = "";
bool s_show_datarefs   = true;
bool s_show_commands   = true;
bool s_writable_only   = false;

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
        return 0; // cockpit gets it — and we do NOT poison ImGui with a half-pair Down/Up

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
        return 0; // forward scroll wheel to cockpit (knobs, throttle wheels, etc.)
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
    auto edge = [&](ImGuiKey k) {
        if (down) io.AddKeyEvent(k, true);
        if (up)   io.AddKeyEvent(k, false);
    };
    if (vkey == XPLM_VK_BACK)   edge(ImGuiKey_Backspace);
    if (vkey == XPLM_VK_DELETE) edge(ImGuiKey_Delete);
    if (vkey == XPLM_VK_RETURN) edge(ImGuiKey_Enter);
    if (vkey == XPLM_VK_LEFT)   edge(ImGuiKey_LeftArrow);
    if (vkey == XPLM_VK_RIGHT)  edge(ImGuiKey_RightArrow);
    if (vkey == XPLM_VK_HOME)   edge(ImGuiKey_Home);
    if (vkey == XPLM_VK_END)    edge(ImGuiKey_End);
    if (vkey == XPLM_VK_TAB)    edge(ImGuiKey_Tab);
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
    case Phase::Idle:         return "Idle";
    case Phase::Baseline:     return "Baseline";
    case Phase::Record:       return "Record";
    case Phase::Inspect:      return "Inspect";
    case Phase::NoiseCapture: return "Mark Noise";
    }
    return "?";
}

std::string hint_text(Phase p)
{
    switch (p)
    {
    case Phase::Idle:
        return "Sit still. Take a snapshot to learn what's noisy.";
    case Phase::Baseline:
        return "Hold still - building noise ignore-set...";
    case Phase::Record:
        if (recorder::auto_stop_enabled())
            return "Flip the switch THREE times (e.g. ON-OFF-ON). Click 'I Acted Now' before each flip. "
                   "Auto-stops after >=5 s once a ref hits the pattern.";
        else
            return "Flip the switch several times. Click 'I Acted Now' before each flip. "
                   "Click 'Stop' when done (auto-stop is disabled).";
    case Phase::Inspect:
        return "Top-ranked DataRef is most likely the cause. Lower ranks may be downstream effects.";
    case Phase::NoiseCapture:
        return "Capturing noise: drive everything you want EXCLUDED (e.g. power the bus). "
               "Every ref that moves is added to the ignore-set. Click 'Stop Noise' when done, then Record your target.";
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
        case 0: return ">";
        case 1: return "||";
        case 2: return "<";
        default: return "--";
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
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
            if (a != b)
                break;
        }
        if (k == nlen)
            return true;
    }
    return false;
}

// ── UI body ──────────────────────────────────────────────────────────────────
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
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f),
                           "Snapshot will re-enumerate to apply filter changes.");
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
        ImGui::Text("   %.1f s   |   best ref: %d events   |   anchors: %d",
                    static_cast<double>(st.record_elapsed_s), st.best_events_so_far, st.anchors_set);
    }
    else if (st.phase == Phase::Inspect)
    {
        ImGui::Text("   %d candidates", st.candidate_count);
    }
    else if (st.phase == Phase::NoiseCapture)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "   capturing... %d refs excluded as noise",
                           st.ignored_count);
    }

    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Hint: %s", hint_text(st.phase).c_str());
    if (st.total_logical > 0)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(%d logical refs, %d ignored, %d watched)", st.total_logical, st.ignored_count,
                            st.watched_count);
    }
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
    bool can_noise     = can_baseline;

    // Left-aligned group labels so the two action rows read as a clear sequence.
    constexpr float kLabelCol = 90.f;

    // ── Workflow: the primary three-step sequence ──
    ImGui::TextDisabled("Workflow");
    ImGui::SameLine(kLabelCol);

    ImGui::BeginDisabled(!can_baseline);
    if (ImGui::Button("Take Snapshot"))
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
        recorder::start_baseline(2.5f);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();

    ImGui::BeginDisabled(!can_record);
    if (ImGui::Button("Record"))
    {
        s_selected_candidate = -1;
        recorder::start_record(s_expect_bidirectional, s_expected_clicks);
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
        if (ImGui::Button("Stop Noise"))
            recorder::stop_noise_capture();
    }
    else
    {
        ImGui::BeginDisabled(!can_noise);
        if (ImGui::Button("Mark Noise"))
        {
            s_selected_candidate = -1;
            recorder::start_noise_capture();
        }
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Subtract a cascade: start, drive everything you want IGNORED (e.g. power the bus),\n"
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

        // Record mode: bool toggle vs. rotary with an explicit click count.
        ImGui::RadioButton("Bool ON-OFF-ON", &s_expected_clicks, 3);
        ImGui::SameLine();
        int rotary_now = (s_expected_clicks == 3) ? 4 : s_expected_clicks;
        if (ImGui::RadioButton("Rotary clicks:", s_expected_clicks != 3))
            s_expected_clicks = rotary_now;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.f);
        int clicks_edit = (s_expected_clicks == 3) ? 4 : s_expected_clicks;
        if (ImGui::InputInt("##clicks", &clicks_edit, 1, 1))
        {
            if (clicks_edit < 2) clicks_edit = 2;
            if (clicks_edit > 16) clicks_edit = 16;
            if (s_expected_clicks != 3)
                s_expected_clicks = clicks_edit;
        }
        s_expect_bidirectional = (s_expected_clicks == 3);

        // Auto-stop toggle. On large displays the mouse travel between cockpit
        // switch and the "I Acted Now" button can take seconds — auto-stop
        // (default 5 s + 3 events) may fire before you've finished the sequence.
        // Turning it off makes Record run until you click Stop.
        bool as = recorder::auto_stop_enabled();
        if (ImGui::Checkbox("Auto-stop when pattern detected", &as))
            recorder::set_auto_stop_enabled(as);
        ImGui::SameLine();
        ImGui::TextDisabled("(off = always stop manually)");

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

        ImGui::Unindent();
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
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                               "No correlating DataRef or Command found.");
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

    // Live substring filter on the path column. The displayed Rank stays the
    // true index in cands[], so hidden rows never push visible rows into a
    // different number — this is what keeps Copy path / Copy code snippet
    // unambiguous when something is filtered out.
    ImGui::SetNextItemWidth(300.f);
    ImGui::InputTextWithHint("##resultfilter", "Filter by keyword (e.g. cockpit)",
                             s_result_filter, sizeof(s_result_filter));
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

    auto row_visible = [&](const Candidate &c, const std::string &path) -> bool {
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
    auto live_value = [](const Candidate &c, SampleValue &out) -> bool {
        if (c.kind != Kind::DataRef)
            return false;
        const LogicalRef *lr = recorder::logical_ref_at(c.logical_ref_idx);
        return lr && dataref_index::read(*lr, out);
    };

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_Resizable;
    ImVec2 table_size(0.f, 280.f);
    if (ImGui::BeginTable("cands", 9, flags, table_size))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Rank", ImGuiTableColumnFlags_WidthFixed, 50.f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 56.f);
        ImGui::TableSetupColumn("R/W", ImGuiTableColumnFlags_WidthFixed, 36.f);
        ImGui::TableSetupColumn("Delta / Fires", ImGuiTableColumnFlags_WidthFixed, 180.f);
        ImGui::TableSetupColumn("Lat (ms)", ImGuiTableColumnFlags_WidthFixed, 80.f);
        ImGui::TableSetupColumn("Dir", ImGuiTableColumnFlags_WidthFixed, 50.f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(cands.size()); ++i)
        {
            const Candidate &c    = cands[i];
            std::string      path = candidate_path(c);
            if (path.empty())
                continue;

            if (!row_visible(c, path))
                continue;

            ImGui::TableNextRow();

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
            if (c.kind == Kind::Command)
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "Command");
            else
                ImGui::TextUnformatted("DataRef");

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
        case RefType::IntArrayElem:    v.i = s_write_int;    break;
        case RefType::Float:
        case RefType::FloatArrayElem:  v.f = s_write_float;  break;
        case RefType::Double:          v.d = s_write_double; break;
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

    ImGuiIO &io  = ImGui::GetIO();
    double   now = get_xp_time();
    io.DeltaTime = static_cast<float>(std::max(now - s_last_frame_time, 0.001));
    s_last_frame_time          = now;
    io.DisplaySize             = ImVec2(static_cast<float>(sw), static_cast<float>(sh));
    io.DisplayFramebufferScale = ImVec2(static_cast<float>(fb_w) / static_cast<float>(sw),
                                        static_cast<float>(fb_h) / static_cast<float>(sh));

    ImGui_ImplOpenGL2_NewFrame();
    ImGui::NewFrame();

    {
        float win_w = 980.f, win_h = 720.f;
        ImGui::SetNextWindowPos(ImVec2((static_cast<float>(sw) - win_w) * 0.5f, (static_cast<float>(sh) - win_h) * 0.5f),
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
