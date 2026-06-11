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

#include "clipboard.hpp"
#include <XPLM/XPLMUtilities.h>
#include <cstdio>

#if defined(__APPLE__)
#include <cstdlib>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace xp_sherlock
{
namespace clipboard
{

bool copy(const std::string &text)
{
#if defined(__APPLE__)
    // popen pbcopy and write the string to its stdin. Reasonably fast for
    // single click-driven copies; not suitable for high-frequency use.
    FILE *pipe = popen("/usr/bin/pbcopy", "w");
    if (!pipe)
    {
        XPLMDebugString("[xp_sherlock] clipboard: popen(pbcopy) failed\n");
        return false;
    }
    fwrite(text.data(), 1, text.size(), pipe);
    int rc = pclose(pipe);
    if (rc != 0)
    {
        XPLMDebugString("[xp_sherlock] clipboard: pbcopy returned non-zero\n");
        return false;
    }
    return true;
#elif defined(_WIN32)
    if (!OpenClipboard(nullptr))
        return false;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (!h)
    {
        CloseClipboard();
        return false;
    }
    auto *p = static_cast<char *>(GlobalLock(h));
    if (p)
    {
        memcpy(p, text.data(), text.size());
        p[text.size()] = '\0';
        GlobalUnlock(h);
        SetClipboardData(CF_TEXT, h);
    }
    CloseClipboard();
    return true;
#else
    // Linux: xclip / wl-copy. Stubbed in v1 — log and bail.
    (void)text;
    XPLMDebugString("[xp_sherlock] clipboard: not implemented on this platform (v1)\n");
    return false;
#endif
}

} // namespace clipboard
} // namespace xp_sherlock
