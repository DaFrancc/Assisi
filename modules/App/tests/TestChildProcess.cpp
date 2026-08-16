/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestChildProcess.cpp
/// @brief The spawn/watch/kill contract, exercised against real processes.
///
/// Worth testing against the operating system rather than a mock, because every
/// interesting failure here *is* the operating system: a child that outlives its
/// parent, a zombie nobody waited on, a SIGTERM a hung process ignores. The
/// editor's play-in-editor clients are the caller, and a leaked one holds a
/// window, a socket, and a GPU context for the rest of the session.
///
/// POSIX-only, like the implementation.

#include <doctest/doctest.h>

#include <Assisi/App/ChildProcess.hpp>

#include <chrono>
#include <thread>

#if !defined(_WIN32)
#include <csignal>
#include <sys/types.h>

using Assisi::App::ChildProcess;

namespace
{

/// `/bin/sleep`, the least interesting long-running program on the system.
constexpr const char *kSleep = "/bin/sleep";

/// Give the OS a moment to actually start (or finish with) a process. Polling
/// rather than sleeping a fixed span keeps the test quick when the machine is
/// idle and reliable when it is not.
bool WaitUntil(ChildProcess &child, bool wantRunning, double seconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (child.IsRunning() == wantRunning)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return child.IsRunning() == wantRunning;
}

} // namespace

TEST_CASE("ChildProcess: a spawned process runs and can be terminated")
{
    ChildProcess child;
    REQUIRE(child.Spawn(kSleep, {"30"}));
    CHECK(child.Valid());
    CHECK(child.Pid() > 0);
    CHECK(child.IsRunning());

    child.Terminate();

    // Terminated *and* reaped: the handle clears, so a later Terminate cannot
    // signal a pid the system has since handed to someone else.
    CHECK_FALSE(child.IsRunning());
    CHECK_FALSE(child.Valid());
}

TEST_CASE("ChildProcess: a child that exits on its own is noticed and reaped")
{
    ChildProcess child;
    REQUIRE(child.Spawn(kSleep, {"0"}));

    // The case a play-in-editor client hits every time someone closes its
    // window by hand. Without the reap it would linger as a zombie for the rest
    // of the editor's life.
    CHECK(WaitUntil(child, /*wantRunning=*/ false, 5.0));
    CHECK_FALSE(child.Valid());
}

TEST_CASE("ChildProcess: terminating twice, or an empty handle, is harmless")
{
    ChildProcess empty;
    CHECK_FALSE(empty.Valid());
    CHECK_FALSE(empty.IsRunning());
    empty.Terminate(); // must not signal pid 0, which on POSIX is the whole group

    ChildProcess child;
    REQUIRE(child.Spawn(kSleep, {"30"}));
    child.Terminate();
    child.Terminate();
    CHECK_FALSE(child.Valid());
}

TEST_CASE("ChildProcess: a missing executable fails loudly rather than silently")
{
    // The pre-fork check earns its keep here. Without it fork() succeeds, the
    // child fails its exec and exits 127, and Spawn has already returned true —
    // so an editor with a mistyped path would report three clients launched and
    // watch none appear.
    ChildProcess child;
    CHECK_FALSE(child.Spawn("/definitely/not/a/program", {}));
    CHECK_FALSE(child.Valid());
}

TEST_CASE("ChildProcess: moving transfers ownership without killing the child")
{
    ChildProcess original;
    REQUIRE(original.Spawn(kSleep, {"30"}));
    const std::int64_t pid = original.Pid();

    ChildProcess moved = std::move(original);
    CHECK(moved.Pid() == pid);
    CHECK(moved.IsRunning());
    CHECK_FALSE(original.Valid()); // NOLINT(bugprone-use-after-move) — checking the moved-from state is the point

    moved.Terminate();
    CHECK_FALSE(moved.IsRunning());
}

TEST_CASE("ChildProcess: the destructor terminates")
{
    std::int64_t pid = 0;
    {
        ChildProcess child;
        REQUIRE(child.Spawn(kSleep, {"30"}));
        pid = child.Pid();
    }
    REQUIRE(pid > 0);

    // Dropping the handle meant killing the child, not losing track of it. The
    // process is gone, so signalling it fails — and because the destructor also
    // reaped it, the pid is not a zombie still answering to signal 0.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    CHECK(::kill(static_cast<pid_t>(pid), 0) != 0);
}

#endif // !_WIN32
