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
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

// posix_spawn needs the caller's environment; macOS exposes it via environ.
extern char **environ;
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace xp_sherlock
{
namespace clipboard
{

#if defined(__APPLE__)
namespace
{

// Owns one file descriptor. The write end has to be closed *before* waitpid()
// so pbcopy sees EOF on stdin and exits — hence the explicit close().
class FdGuard
{
  public:
    explicit FdGuard(int fd = -1) : fd_{fd} {}
    ~FdGuard() { close(); }

    FdGuard(const FdGuard &)            = delete;
    FdGuard &operator=(const FdGuard &) = delete;

    int get() const { return fd_; }

    void close()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
    }

  private:
    int fd_{-1};
};

// posix_spawn_file_actions_t must be destroyed even on early error returns.
class SpawnFileActions
{
  public:
    SpawnFileActions() : valid_{posix_spawn_file_actions_init(&actions_) == 0} {}
    ~SpawnFileActions()
    {
        if (valid_)
            posix_spawn_file_actions_destroy(&actions_);
    }

    SpawnFileActions(const SpawnFileActions &)            = delete;
    SpawnFileActions &operator=(const SpawnFileActions &) = delete;

    bool                        valid() const { return valid_; }
    posix_spawn_file_actions_t *get() { return &actions_; }

  private:
    posix_spawn_file_actions_t actions_{};
    bool                       valid_{false};
};

// If pbcopy dies early, writing to the pipe raises SIGPIPE, whose default
// action would take down the whole X-Plane process. Ignore it for the duration
// of the write so the failure surfaces as EPIPE instead, then restore the
// host's handler — the plugin must not permanently alter X-Plane's signal
// disposition.
class SigPipeGuard
{
  public:
    SigPipeGuard()
    {
        struct sigaction ignore_action{};
        ignore_action.sa_handler = SIG_IGN;
        sigemptyset(&ignore_action.sa_mask);
        saved_valid_ = sigaction(SIGPIPE, &ignore_action, &saved_) == 0;
    }
    ~SigPipeGuard()
    {
        if (saved_valid_)
            sigaction(SIGPIPE, &saved_, nullptr);
    }

    SigPipeGuard(const SigPipeGuard &)            = delete;
    SigPipeGuard &operator=(const SigPipeGuard &) = delete;

  private:
    struct sigaction saved_{};
    bool             saved_valid_{false};
};

// write(2) may return short counts and may be interrupted; loop until the
// whole payload is out or a hard error occurs.
bool write_all(int fd, const std::string &text)
{
    std::size_t written = 0;
    while (written < text.size())
    {
        const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace
#endif

bool copy(const std::string &text)
{
#if defined(__APPLE__)
    // Spawn pbcopy directly instead of going through popen(): popen always
    // runs /bin/sh -c, which adds a shell interpreter to a call that needs
    // none. Absolute path, fixed argv — nothing here is user controlled.
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0)
    {
        XPLMDebugString("[xp_sherlock] clipboard: pipe() failed\n");
        return false;
    }
    FdGuard read_end{fds[0]};
    FdGuard write_end{fds[1]};

    SpawnFileActions actions;
    if (!actions.valid())
    {
        XPLMDebugString("[xp_sherlock] clipboard: posix_spawn_file_actions_init failed\n");
        return false;
    }
    // The child reads the payload from stdin; both inherited pipe fds are
    // closed afterwards so only the dup'ed descriptor remains.
    if (posix_spawn_file_actions_adddup2(actions.get(), read_end.get(), STDIN_FILENO) != 0 ||
        posix_spawn_file_actions_addclose(actions.get(), read_end.get()) != 0 ||
        posix_spawn_file_actions_addclose(actions.get(), write_end.get()) != 0)
    {
        XPLMDebugString("[xp_sherlock] clipboard: posix_spawn_file_actions setup failed\n");
        return false;
    }

    char        arg0[] = "pbcopy";
    char *const argv[] = {arg0, nullptr};
    pid_t       pid    = -1;
    if (posix_spawn(&pid, "/usr/bin/pbcopy", actions.get(), nullptr, argv, environ) != 0)
    {
        XPLMDebugString("[xp_sherlock] clipboard: spawning pbcopy failed\n");
        return false;
    }

    read_end.close();

    bool write_ok = false;
    {
        SigPipeGuard sigpipe_guard;
        write_ok = write_all(write_end.get(), text);
    }
    // Must close before waiting, otherwise pbcopy never sees EOF and both
    // processes deadlock.
    write_end.close();

    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
    {
        if (errno != EINTR)
        {
            XPLMDebugString("[xp_sherlock] clipboard: waitpid on pbcopy failed\n");
            return false;
        }
    }

    if (!write_ok)
    {
        XPLMDebugString("[xp_sherlock] clipboard: writing to pbcopy failed\n");
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
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
