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

#include "about_window.hpp"
#include <wellys_about.hpp>

namespace xp_sherlock
{
namespace about_window
{

namespace
{

#ifdef XP_SHERLOCK_VERSION
constexpr const char *kPluginVersion = XP_SHERLOCK_VERSION;
#else
constexpr const char *kPluginVersion = "SNAPSHOT";
#endif

// Only what ships inside the .xpl — build- and test-only deps (Catch2,
// nlohmann/json, which no plugin source includes) are left out.
const WellysAbout::PluginInfo &plugin_info()
{
    static const WellysAbout::PluginInfo info{
        "xp_sherlock_dataref",
        kPluginVersion,
        "Copyright (C) 2026 thWelly",
        "Free software under the GNU General Public License v3 or later, provided without any warranty.",
        {
            {"Dear ImGui", "MIT"},
            {"X-Plane SDK", "Laminar Research SDK License"},
        },
    };
    return info;
}

} // namespace

void draw(bool &open) { WellysAbout::draw_window(open, plugin_info(), WellysAbout::photo_texture()); }

void release() { WellysAbout::release_photo(); }

} // namespace about_window
} // namespace xp_sherlock
