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
