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
#include <XPLM/XPLMPlanes.h>
#include <XPLM/XPLMUtilities.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace xp_sherlock
{

namespace
{

std::vector<CommandEntry> s_index;
bool                      s_built = false;
std::vector<std::string>  s_user_exclusions;

// Names already placed in s_index this rebuild. Used to dedup across the global
// Commands.txt and the aircraft-local *_Commands.txt (a command must be hooked
// exactly once — command_recorder registers one handler per index entry).
std::set<std::string> s_indexed_names;

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
    // Aircraft-local *_Commands.txt (e.g. the Zibo 737's B738_Commands.txt) use
    // '--' for comments and '----' rule lines between sections; the stock
    // Commands.txt never starts a command name with '--', so skipping these is
    // safe and keeps the unresolved counter honest.
    if (s.size() >= 2 && s[0] == '-' && s[1] == '-')
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

    // Dedup across command sources (global + aircraft-local files).
    if (s_indexed_names.count(name))
        return false;

    XPLMCommandRef h = XPLMFindCommand(name.c_str());
    if (!h)
        return false;

    CommandEntry e;
    e.handle      = h;
    e.name        = name;
    e.description = desc;
    s_index.push_back(std::move(e));
    s_indexed_names.insert(name);
    return true;
}

// Per-file parse/resolve tally, accumulated across all command sources.
struct FileStats
{
    int total_lines    = 0;
    int parsed         = 0;
    int resolved       = 0;
    int unresolved     = 0;
    int skipped_filter = 0;
};

// Read one Commands.txt-format file and register every resolvable command.
// Shared by the global Resources/plugins/Commands.txt and the aircraft-local
// *_Commands.txt files — both use the same whitespace-separated format. Returns
// false if the file could not be opened.
bool process_command_file(const std::string &path, FileStats &st)
{
    std::ifstream in(path);
    if (!in.is_open())
        return false;

    std::string line;
    while (std::getline(in, line))
    {
        ++st.total_lines;
        std::string name, desc;
        if (!parse_line(line, name, desc))
            continue;
        ++st.parsed;

        // Pre-filter so we don't burn an XPLMFindCommand on excluded names —
        // keeps the rebuild fast on payware installs with large command files.
        if (starts_with(name, PRIVATE_PREFIX, PRIVATE_PREFIX_LEN))
        {
            ++st.skipped_filter;
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
            ++st.skipped_filter;
            continue;
        }

        if (try_register(name, desc))
            ++st.resolved;
        else
            ++st.unresolved;
    }
    return true;
}

// Case-insensitive suffix match (filenames are case-insensitive on macOS/Win).
bool ends_with_ci(const std::string &s, const char *suffix)
{
    const std::size_t n = std::strlen(suffix);
    if (s.size() < n)
        return false;
    const std::size_t off = s.size() - n;
    for (std::size_t i = 0; i < n; ++i)
    {
        if (std::tolower(static_cast<unsigned char>(s[off + i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i])))
            return false;
    }
    return true;
}

// Directory containing the user aircraft's .acf (trailing separator included),
// or empty if it cannot be determined. Relies on XPLM_USE_NATIVE_PATHS (enabled
// in XPluginStart) so the path is a real POSIX/Windows path.
std::string user_aircraft_dir()
{
    char file_name[256]  = {0};
    char acf_path[1024]  = {0};
    XPLMGetNthAircraftModel(XPLM_USER_AIRCRAFT, file_name, acf_path);
    std::string p = acf_path;
    if (p.empty())
        return {};
    const std::size_t slash = p.find_last_of("/\\");
    return (slash == std::string::npos) ? std::string{} : p.substr(0, slash + 1);
}

} // namespace

namespace command_index
{

void rebuild()
{
    s_index.clear();
    s_indexed_names.clear();
    s_built = false;

    // ── Source 1: stock X-Plane commands (Resources/plugins/Commands.txt) ──
    const std::string path = find_commands_txt_path();
    FileStats         global{};
    bool              used_fallback = false;

    if (!process_command_file(path, global))
    {
        used_fallback = true;
        XPLMDebugString(
            "[xp_sherlock] Commands.txt not found - falling back to embedded mini-list. "
            "Command detection coverage will be limited.\n");
        for (std::size_t i = 0; i < FALLBACK_COMMAND_COUNT; ++i)
        {
            ++global.parsed;
            if (try_register(FALLBACK_COMMANDS[i], std::string{}))
                ++global.resolved;
            else
                ++global.unresolved;
        }
    }

    // ── Source 2: aircraft-local *_Commands.txt ──
    // The SDK cannot enumerate runtime-registered commands, so custom aircraft
    // (e.g. the Zibo 737) ship their command names in <aircraft>/*_Commands.txt.
    // This is the same source DataRefTool reads — no network/REST API involved.
    int               aircraft_files    = 0;
    int               aircraft_resolved = 0;
    const std::string ac_dir            = user_aircraft_dir();
    if (!ac_dir.empty())
    {
        std::error_code              ec;
        std::filesystem::directory_iterator it(ac_dir, ec);
        const std::filesystem::directory_iterator end;
        for (; !ec && it != end; it.increment(ec))
        {
            std::error_code fec;
            if (!it->is_regular_file(fec) || fec)
                continue;
            const std::string fname = it->path().filename().string();
            if (!ends_with_ci(fname, "Commands.txt"))
                continue;

            FileStats st{};
            if (process_command_file(it->path().string(), st))
            {
                ++aircraft_files;
                aircraft_resolved += st.resolved;
            }
        }
    }

    char banner[480];
    if (used_fallback)
    {
        snprintf(banner, sizeof(banner),
                 "[xp_sherlock] Command index built from FALLBACK list: %d resolved, %d unresolved; "
                 "aircraft files: %d (%d resolved). Total indexed: %zu.\n",
                 global.resolved, global.unresolved, aircraft_files, aircraft_resolved, s_index.size());
    }
    else
    {
        snprintf(banner, sizeof(banner),
                 "[xp_sherlock] Command index: global %s (%d parsed, %d resolved, %d filtered); "
                 "aircraft files: %d (%d resolved). Total indexed: %zu.\n",
                 path.c_str(), global.parsed, global.resolved, global.skipped_filter,
                 aircraft_files, aircraft_resolved, s_index.size());
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
