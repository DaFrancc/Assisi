/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ChildProcess.hpp
/// @brief Spawn, watch, and reliably kill a child process.
///
/// One job, and it is the awkward half of play-in-editor: the editor launches
/// sibling processes as clients of its own listen server, and every one of them
/// must be gone when the play session ends — including when the editor crashes,
/// when the user closes a client window by hand, and when a client hangs before
/// it has drawn a frame. A leaked viewer is not a cosmetic problem: it holds a
/// socket, a GPU context, and a lock on whatever it was reading.
///
/// The discipline is therefore: ask, wait, insist, collect. SIGTERM first so a
/// child gets to shut its Vulkan device down; a short grace; SIGKILL if it is
/// still there; and a `waitpid` in every path, because an unreaped child is a
/// zombie the editor keeps for the rest of its life. On Linux the child also
/// sets `PR_SET_PDEATHSIG`, which closes the one case the parent cannot handle
/// from its own side — its own death.
///
/// POSIX first, deliberately: the GameNetworkingSockets chain has never built on
/// Windows (docs/replication-plan-v4.md §5), so a Windows implementation would
/// be dead code today. The Windows path fails to spawn and says so, rather than
/// silently pretending.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Assisi::App
{

/// @brief A spawned process, owned by whoever spawned it.
///
/// Move-only, and the destructor terminates: an owner that drops the handle
/// meant to kill the child, not to lose track of it.
class ChildProcess
{
  public:
    ChildProcess() = default;
    ~ChildProcess();

    ChildProcess(const ChildProcess &)            = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;
    ChildProcess(ChildProcess &&other) noexcept;
    ChildProcess &operator=(ChildProcess &&other) noexcept;

    /// @brief Launch @p executable with @p args (argv[0] is supplied for you).
    ///
    /// @p environment is a list of `KEY=VALUE` entries added to (or replacing
    /// in) the child's environment — how a play-in-editor client is pointed at
    /// its own user root so it cannot write over its parent's settings.
    ///
    /// @return true on success. On failure the object stays empty and the
    /// reason is logged; there is no half-spawned state to clean up.
    bool Spawn(const std::filesystem::path &executable, const std::vector<std::string> &args,
               const std::vector<std::string> &environment = {},
               const std::filesystem::path   &workingDirectory = {});

    /// @brief Whether the child is still alive.
    ///
    /// Reaps it if it has exited, so a child that closed its own window stops
    /// being a zombie the first time anyone asks. Not const for that reason.
    [[nodiscard]] bool IsRunning();

    /// @brief SIGTERM, wait up to @p graceSeconds, SIGKILL, reap.
    ///
    /// Idempotent and safe on an empty handle. Blocks for at most the grace
    /// period — called from a Stop, where a few hundred milliseconds of "the
    /// editor thought about it" beats a leaked window.
    void Terminate(double graceSeconds = 2.0);

    [[nodiscard]] bool  Valid() const { return _pid > 0; }
    [[nodiscard]] std::int64_t Pid() const { return _pid; }

  private:
    /// The platform process id; 0 means "no child". Signed because POSIX pids
    /// are, and because a negative value is how the failure is spelled there.
    std::int64_t _pid = 0;
};

} // namespace Assisi::App
