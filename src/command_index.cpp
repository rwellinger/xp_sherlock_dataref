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

#include "command_index.hpp"
#include <XPLM/XPLMUtilities.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace xp_sherlock
{

namespace
{

std::vector<CommandEntry> s_index;
bool                      s_built = false;
std::vector<std::string>  s_user_exclusions;

// Engine-internal commands that never correspond to cockpit controls. Same
// rationale as `sim/private/` for DataRefs: filtering at enum time keeps the
// candidate noise floor manageable.
constexpr const char *PRIVATE_PREFIX     = "sim/private/";
constexpr std::size_t PRIVATE_PREFIX_LEN = 12;

bool starts_with(const std::string &s, const char *prefix, std::size_t prefix_len)
{
    return s.size() >= prefix_len && std::strncmp(s.c_str(), prefix, prefix_len) == 0;
}

// Strip leading + trailing ASCII whitespace in-place.
void trim(std::string &s)
{
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    if (b > 0 || e < s.size())
        s = s.substr(b, e - b);
}

// Locate <X-Plane root>/Resources/plugins/Commands.txt via the XPLM. We rely on
// XPLM_USE_NATIVE_PATHS being enabled in XPluginStart so the path is a real
// POSIX/Windows path, not the legacy HFS-style one.
std::string find_commands_txt_path()
{
    char sys[1024] = {0};
    XPLMGetSystemPath(sys);
    std::string base = sys;
    if (!base.empty() && base.back() != '/' && base.back() != '\\')
        base += '/';
    return base + "Resources/plugins/Commands.txt";
}

// Parse one Commands.txt line into (name, description). The X-Plane file is
// whitespace-separated: first token is the command name, the rest of the line
// is the description (may contain spaces). Empty lines and lines starting with
// '#' are comments. Returns false if the line yields no name.
bool parse_line(const std::string &line, std::string &name, std::string &desc)
{
    std::string s = line;
    trim(s);
    if (s.empty() || s[0] == '#')
        return false;
    std::size_t i = 0;
    while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
    name = s.substr(0, i);
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
    desc = (i < s.size()) ? s.substr(i) : std::string{};
    return !name.empty();
}

// Embedded fallback list — covers a handful of stock cockpit commands so the
// feature still produces something usable if Commands.txt cannot be found.
// Intentionally small; the real list comes from the installed X-Plane.
const char *const FALLBACK_COMMANDS[] = {
    "sim/electrical/battery_1_on",
    "sim/electrical/battery_1_off",
    "sim/electrical/battery_1_toggle",
    "sim/electrical/generator_1_on",
    "sim/electrical/generator_1_off",
    "sim/electrical/generator_1_toggle",
    "sim/electrical/avionics_on",
    "sim/electrical/avionics_off",
    "sim/electrical/avionics_toggle",
    "sim/lights/landing_lights_on",
    "sim/lights/landing_lights_off",
    "sim/lights/landing_lights_toggle",
    "sim/lights/beacon_lights_toggle",
    "sim/lights/nav_lights_toggle",
    "sim/lights/strobe_lights_toggle",
    "sim/lights/taxi_lights_toggle",
    "sim/engines/engage_starters",
    "sim/engines/magnetos_off",
    "sim/engines/magnetos_left",
    "sim/engines/magnetos_right",
    "sim/engines/magnetos_both",
    "sim/engines/magnetos_start",
    "sim/flight_controls/landing_gear_toggle",
    "sim/flight_controls/landing_gear_up",
    "sim/flight_controls/landing_gear_down",
    "sim/flight_controls/flaps_up",
    "sim/flight_controls/flaps_down",
    "sim/autopilot/heading",
    "sim/autopilot/altitude_hold",
    "sim/autopilot/autothrottle_toggle",
};
constexpr std::size_t FALLBACK_COMMAND_COUNT =
    sizeof(FALLBACK_COMMANDS) / sizeof(FALLBACK_COMMANDS[0]);

// Try `name`; if found, push CommandEntry to s_index and return true.
bool try_register(const std::string &name, const std::string &desc)
{
    if (name.empty())
        return false;

    // Hardcoded private-namespace filter.
    if (starts_with(name, PRIVATE_PREFIX, PRIVATE_PREFIX_LEN))
        return false;

    // User exclusion prefixes (snapshot filter checkboxes).
    for (const auto &px : s_user_exclusions)
    {
        if (!px.empty() && starts_with(name, px.c_str(), px.size()))
            return false;
    }

    XPLMCommandRef h = XPLMFindCommand(name.c_str());
    if (!h)
        return false;

    CommandEntry e;
    e.handle      = h;
    e.name        = name;
    e.description = desc;
    s_index.push_back(std::move(e));
    return true;
}

} // namespace

namespace command_index
{

void rebuild()
{
    s_index.clear();
    s_built = false;

    const std::string path = find_commands_txt_path();
    std::ifstream     in(path);

    int  total_lines    = 0;
    int  parsed         = 0;
    int  resolved       = 0;
    int  unresolved     = 0;
    int  skipped_filter = 0;
    bool used_fallback  = false;

    if (in.is_open())
    {
        std::string line;
        while (std::getline(in, line))
        {
            ++total_lines;
            std::string name, desc;
            if (!parse_line(line, name, desc))
                continue;
            ++parsed;

            // Pre-filter check so we don't burn an XPLMFindCommand on
            // explicitly excluded names — keeps the rebuild fast on payware
            // installs with very large Commands.txt files.
            if (starts_with(name, PRIVATE_PREFIX, PRIVATE_PREFIX_LEN))
            {
                ++skipped_filter;
                continue;
            }
            bool excluded = false;
            for (const auto &px : s_user_exclusions)
            {
                if (!px.empty() && starts_with(name, px.c_str(), px.size()))
                {
                    excluded = true;
                    break;
                }
            }
            if (excluded)
            {
                ++skipped_filter;
                continue;
            }

            if (try_register(name, desc))
                ++resolved;
            else
                ++unresolved;
        }
    }
    else
    {
        used_fallback = true;
        XPLMDebugString(
            "[xp_sherlock] Commands.txt not found - falling back to embedded mini-list. "
            "Command detection coverage will be limited.\n");
        for (std::size_t i = 0; i < FALLBACK_COMMAND_COUNT; ++i)
        {
            ++parsed;
            std::string name = FALLBACK_COMMANDS[i];
            std::string desc;
            if (try_register(name, desc))
                ++resolved;
            else
                ++unresolved;
        }
    }

    char banner[320];
    if (used_fallback)
    {
        snprintf(banner, sizeof(banner),
                 "[xp_sherlock] Command index built from FALLBACK list: %d resolved, %d unresolved.\n",
                 resolved, unresolved);
    }
    else
    {
        snprintf(banner, sizeof(banner),
                 "[xp_sherlock] Command index built from %s: %d lines, %d parsed, "
                 "%d resolved, %d unresolved, %d filtered.\n",
                 path.c_str(), total_lines, parsed, resolved, unresolved, skipped_filter);
    }
    XPLMDebugString(banner);
    XPLMDebugString("[xp_sherlock] Note: late-binding aircraft/plugin commands may not be present yet. "
                    "Use Re-enumerate after aircraft load.\n");

    s_built = true;
}

bool                              is_built() { return s_built; }
const std::vector<CommandEntry>  &all()      { return s_index; }
std::size_t                       size()     { return s_index.size(); }

void set_user_exclusions(std::vector<std::string> prefixes)
{
    s_user_exclusions = std::move(prefixes);
}

std::string code_snippet(const CommandEntry &cmd)
{
    char buf[1400];
    snprintf(buf, sizeof(buf),
             "XPLMCommandRef cmd = XPLMFindCommand(\"%s\");\nXPLMCommandOnce(cmd);",
             cmd.name.c_str());
    return buf;
}

} // namespace command_index
} // namespace xp_sherlock
