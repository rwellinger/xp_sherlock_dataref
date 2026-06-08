#include "ui.hpp"
#include "clipboard.hpp"
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
    if (!(flags & xplm_DownFlag))
        return;
    ImGui::SetCurrentContext(s_imgui_ctx);
    ImGuiIO &io = ImGui::GetIO();

    // Escape always closes our window regardless of focus — convenience.
    if (vkey == XPLM_VK_ESCAPE)
    {
        s_open = false;
        if (s_wnd)
            XPLMSetWindowIsVisible(s_wnd, 0);
        return;
    }

    // If ImGui isn't expecting keyboard input (no active text field), don't
    // swallow keystrokes — let X-Plane's normal key bindings work.
    if (!io.WantCaptureKeyboard)
        return;

    if (key >= 32 && key < 127)
        io.AddInputCharacter(static_cast<unsigned>(key));
    if (vkey == XPLM_VK_BACK)
        io.AddKeyEvent(ImGuiKey_Backspace, true);
    if (vkey == XPLM_VK_DELETE)
        io.AddKeyEvent(ImGuiKey_Delete, true);
    if (vkey == XPLM_VK_RETURN)
        io.AddKeyEvent(ImGuiKey_Enter, true);
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
    case Phase::Idle:     return "Idle";
    case Phase::Baseline: return "Baseline";
    case Phase::Record:   return "Record";
    case Phase::Inspect:  return "Inspect";
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
        return "Hold still — building noise ignore-set...";
    case Phase::Record:
        if (recorder::auto_stop_enabled())
            return "Flip the switch THREE times (e.g. ON-OFF-ON). Click 'I Acted Now' before each flip. "
                   "Auto-stops after >=5 s once a ref hits the pattern.";
        else
            return "Flip the switch several times. Click 'I Acted Now' before each flip. "
                   "Click 'Stop' when done (auto-stop is disabled).";
    case Phase::Inspect:
        return "Top-ranked DataRef is most likely the cause. Lower ranks may be downstream effects.";
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

// ── UI body ──────────────────────────────────────────────────────────────────
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

    bool can_baseline = (st.phase == Phase::Idle || st.phase == Phase::Inspect);
    bool baseline_done = !st.baseline_in_progress && st.ignored_count > 0;
    bool can_record   = baseline_done && st.phase != Phase::Record && st.phase != Phase::Baseline;
    bool can_act      = (st.phase == Phase::Record);
    bool can_stop     = (st.phase == Phase::Record);
    bool can_reset    = (st.phase != Phase::Baseline);

    ImGui::BeginDisabled(!can_baseline);
    if (ImGui::Button("Take Snapshot"))
    {
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

    ImGui::BeginDisabled(!can_act);
    if (ImGui::Button("I Acted Now"))
        recorder::mark_user_action();
    ImGui::EndDisabled();
    ImGui::SameLine();

    ImGui::BeginDisabled(!can_stop);
    if (ImGui::Button("Stop"))
        recorder::stop_record();
    ImGui::EndDisabled();
    ImGui::SameLine();

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
        dataref_index::rebuild();
    }

    // Mode row
    ImGui::Spacing();
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
                               "No correlating DataRef found — likely Command-based or internal aircraft logic.");
            ImGui::TextWrapped("The switch may fire an X-Plane Command without writing a DataRef, or the aircraft's "
                               "SASL/Lua logic may handle it entirely in-process. Command sniffing is not implemented "
                               "in v1.");
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

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_Resizable;
    ImVec2 table_size(0.f, 280.f);
    if (ImGui::BeginTable("cands", 7, flags, table_size))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Rank", ImGuiTableColumnFlags_WidthFixed, 50.f);
        ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 56.f);
        ImGui::TableSetupColumn("Delta", ImGuiTableColumnFlags_WidthFixed, 180.f);
        ImGui::TableSetupColumn("Lat (ms)", ImGuiTableColumnFlags_WidthFixed, 80.f);
        ImGui::TableSetupColumn("Dir", ImGuiTableColumnFlags_WidthFixed, 50.f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(cands.size()); ++i)
        {
            const Candidate &c   = cands[i];
            const LogicalRef *lr = recorder::logical_ref_at(c.logical_ref_idx);
            if (!lr)
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
                // is non-destructive unless the user changes it.
                s_write_int    = c.current_value.i;
                s_write_float  = c.current_value.f;
                s_write_double = c.current_value.d;
            }

            ImGui::TableNextColumn();
            ImGui::Text("%.0f", static_cast<double>(c.score));

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(lr->display_path.c_str());

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(type_name(c.type));

            ImGui::TableNextColumn();
            // Show observed range, not baseline→current. Switches that toggle
            // back to their starting state (e.g. ON-OFF-ON) end with baseline
            // == current — but the swing in between is the interesting part.
            std::string mn = value_label(c.type, c.min_seen);
            std::string mx = value_label(c.type, c.max_seen);
            std::string nw = value_label(c.type, c.current_value);
            if (mn == mx)
                ImGui::Text("= %s", nw.c_str());
            else
                ImGui::Text("%s .. %s  (now %s)", mn.c_str(), mx.c_str(), nw.c_str());

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

    // Per-row action buttons under the table (work on s_selected_candidate)
    if (s_selected_candidate >= 0 && s_selected_candidate < static_cast<int>(cands.size()))
    {
        const Candidate &c   = cands[s_selected_candidate];
        const LogicalRef *lr = recorder::logical_ref_at(c.logical_ref_idx);
        if (lr)
        {
            ImGui::Spacing();
            ImGui::TextDisabled("Selected: %s", lr->display_path.c_str());
            if (ImGui::Button("Copy path"))
                clipboard::copy(lr->display_path);
            ImGui::SameLine();
            if (ImGui::Button("Copy code snippet"))
                clipboard::copy(dataref_index::code_snippet(*lr));
        }
    }
}

void draw_test_panel()
{
    const auto &cands = recorder::candidates();
    if (s_selected_candidate < 0 || s_selected_candidate >= static_cast<int>(cands.size()))
        return;

    const Candidate &c   = cands[s_selected_candidate];
    const LogicalRef *lr = recorder::logical_ref_at(c.logical_ref_idx);
    if (!lr)
        return;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Test panel — write value to candidate");

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
                       "correct one but is read-only, or the aircraft's own logic overwrites it every frame — in that "
                       "case it cannot be driven from outside; look for an associated Command instead.");
}

} // namespace

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
            XPLMSetWindowIsVisible(s_wnd, 0);
    }

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
            XPLMSetWindowIsVisible(s_wnd, 0);
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
