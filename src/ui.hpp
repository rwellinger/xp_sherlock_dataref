// ImGui-based UI for DataRef Detective. Mirrors the xp_pilot LogbookUI pattern
// verbatim: own ImGuiContext, invisible XPLMCreateWindowEx capture window,
// xplm_Phase_Window draw callback, full GL state save/restore.

#pragma once

namespace xp_sherlock
{
namespace ui
{

void init();
void stop();

// Called from the main draw callback every frame. Cheap no-op when the
// window is closed.
void draw();

void toggle();
bool is_open();

} // namespace ui
} // namespace xp_sherlock
