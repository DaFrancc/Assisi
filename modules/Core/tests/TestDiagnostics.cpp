/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestDiagnostics.cpp
/// @brief Launch stamps, artifact retention, and the per-thread signal stack.
///
/// The property under test through the first two is that a run cannot lose its
/// own log, and each case covers a way it can: a filename that sorts wrong
/// because local time moved backwards, and two processes computing the same name
/// at one-second resolution. Both fail silently — the log goes missing with
/// nothing to say so.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#    include <array>
#    include <csignal>
#endif

#include <Assisi/Core/Diagnostics.hpp>

using namespace Assisi::Core;

namespace fs = std::filesystem;

namespace
{

/// A scratch directory that cleans up after itself, so a failing CHECK cannot
/// leave state behind for the next test.
struct TempDir
{
    fs::path path;

    explicit TempDir(std::string_view name) : path(fs::temp_directory_path() / ("assisi-diag-" + std::string(name)))
    {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir &)            = delete;
    TempDir &operator=(const TempDir &) = delete;

    void Touch(std::string_view name) const
    {
        std::ofstream file(path / name);
        file << "x\n";
    }

    [[nodiscard]] bool Has(std::string_view name) const
    {
        std::error_code ec;
        return fs::exists(path / name, ec);
    }

    [[nodiscard]] std::vector<std::string> Names() const
    {
        std::vector<std::string> names;
        std::error_code ec;
        for (const fs::directory_entry &entry : fs::directory_iterator(path, ec))
        {
            names.push_back(entry.path().filename().string());
        }
        std::sort(names.begin(), names.end());
        return names;
    }
};

} // namespace

TEST_CASE("LaunchStamp is stable and process-unique")
{
    // Constant for the life of the process: the log and the crash report are
    // named from separate calls and must pair up.
    CHECK(LaunchStamp() == LaunchStamp());

    // date-time-pid. The pid is what stops two launches in the same second
    // computing one filename and truncating each other's log.
    const std::string stamp = LaunchStamp();
    CHECK(stamp.size() > 16);
    CHECK(stamp.find('-') != std::string::npos);
    CHECK(std::count(stamp.begin(), stamp.end(), '-') == 2);

    // Colons would be illegal in a Windows filename.
    CHECK(stamp.find(':') == std::string::npos);
}

TEST_CASE("PruneOldFiles keeps the newest and deletes the rest")
{
    const TempDir dir("keep-newest");
    for (const char *name : {"assisi-20260101-000000-1.log", "assisi-20260102-000000-1.log",
                             "assisi-20260103-000000-1.log", "assisi-20260104-000000-1.log"})
    {
        dir.Touch(name);
    }

    PruneOldFiles(dir.path, "assisi-", ".log", 2);

    CHECK(dir.Names().size() == 2);
    CHECK(dir.Has("assisi-20260104-000000-1.log"));
    CHECK(dir.Has("assisi-20260103-000000-1.log"));
    CHECK_FALSE(dir.Has("assisi-20260101-000000-1.log"));
}

TEST_CASE("PruneOldFiles never deletes the protected file")
{
    // LaunchStamp() is local time, so a DST fall-back or an NTP step backwards
    // gives this run a name that sorts OLDEST of the set. Sorting alone would
    // delete it first — on POSIX while its descriptor is still open, so the run
    // would go on writing into an unlinked inode and lose everything at exit.
    const std::string current = "assisi-20260809-140000-1.log"; // launched in New York
    const auto populate = [&current](const TempDir &target)
                          {
                              for (const char *name : {"assisi-20260809-200000-1.log", "assisi-20260809-201000-1.log",
                                                       "assisi-20260809-202000-1.log", "assisi-20260809-203000-1.log",
                                                       "assisi-20260809-204000-1.log"})
                              {
                                  target.Touch(name); // earlier session, later local wall-clock
                              }
                              target.Touch(current);
                          };

    {
        const TempDir dir("protect");
        populate(dir);
        PruneOldFiles(dir.path, "assisi-", ".log", 5, current);

        REQUIRE(dir.Has(current));
        // Protected file counts toward the budget, so five in total remain.
        CHECK(dir.Names().size() == 5);
    }

    {
        // Control: the same six files and the same budget, without the guard,
        // must still lose the current run. This is what makes the case above
        // meaningful rather than vacuous.
        const TempDir dir("protect-control");
        populate(dir);
        PruneOldFiles(dir.path, "assisi-", ".log", 5);

        CHECK_FALSE(dir.Has(current));
    }
}

TEST_CASE("PruneOldFiles with keep = 0 still keeps the current run")
{
    const TempDir dir("keep-zero");
    const std::string current = "assisi-20260809-120000-1.log";
    dir.Touch("assisi-20260809-100000-1.log");
    dir.Touch("assisi-20260809-110000-1.log");
    dir.Touch(current);

    // 0 means "keep no history", not "delete the log being written", matching
    // what game.json documents.
    PruneOldFiles(dir.path, "assisi-", ".log", 0, current);

    CHECK(dir.Names() == std::vector<std::string>{current});
}

TEST_CASE("PruneOldFiles only touches files it recognises")
{
    const TempDir dir("scoped");
    dir.Touch("assisi-20260101-000000-1.log");
    dir.Touch("assisi-20260102-000000-1.log");
    dir.Touch("crash-20260101-000000-1.txt"); // different prefix
    dir.Touch("assisi-20260101-000000-1.txt"); // different extension
    dir.Touch("savegame.dat");                // nothing to do with us

    PruneOldFiles(dir.path, "assisi-", ".log", 1);

    CHECK(dir.Has("crash-20260101-000000-1.txt"));
    CHECK(dir.Has("assisi-20260101-000000-1.txt"));
    CHECK(dir.Has("savegame.dat"));
    CHECK(dir.Has("assisi-20260102-000000-1.log"));
    CHECK_FALSE(dir.Has("assisi-20260101-000000-1.log"));
}

TEST_CASE("PruneOldFiles deletes everything past the budget, not one file")
{
    // Lowering keepLogs from 20 to 5 has to converge on the next launch, not
    // fifteen launches later.
    const TempDir dir("bulk");
    for (int i = 10; i < 30; ++i)
    {
        dir.Touch("assisi-202601" + std::to_string(i) + "-000000-1.log");
    }

    PruneOldFiles(dir.path, "assisi-", ".log", 5);

    CHECK(dir.Names().size() == 5);
}

TEST_CASE("PruneOldFiles survives a directory that is not there")
{
    // The ordinary first-launch case, and the reason this is best-effort rather
    // than an error: failing to tidy up must never fail a launch.
    const TempDir dir("missing");
    PruneOldFiles(dir.path / "nope", "assisi-", ".log", 5);
    CHECK(true); // reaching here without a throw or a crash is the assertion
}

#ifndef _WIN32

// ── The per-thread signal stack ──────────────────────────────────────────────
//
// Both cases run on a spawned thread, because sigaltstack is per-thread and the
// main thread's is whatever the runner left there.

TEST_CASE("InstallSignalStackForThisThread gives a bare thread an alt stack")
{
    // The thread is made bare explicitly rather than assumed to be: under a
    // sanitizer it arrives with one already installed, and the skip added for
    // that (see the case below) would leave this asserting on the sanitizer's
    // stack instead of ours — passing without exercising the install at all.
    bool queried = false;
    stack_t installed{};
    const auto body = [&queried, &installed]
                      {
                          stack_t none{};
                          none.ss_flags = SS_DISABLE;
                          stack_t saved{};
                          if (sigaltstack(&none, &saved) != 0)
                              return;

                          InstallSignalStackForThisThread();
                          queried = sigaltstack(nullptr, &installed) == 0;

                          (void)sigaltstack(&saved, nullptr); // put the sanitizer's back
                      };

    std::thread worker(body);
    worker.join();

    REQUIRE(queried);
    CHECK(installed.ss_sp != nullptr);
    CHECK(installed.ss_size > 0u);
    CHECK((installed.ss_flags & SS_DISABLE) == 0);
}

TEST_CASE("InstallSignalStackForThisThread leaves an alt stack somebody else installed alone")
{
    // The one that matters, and the one that was wrong. A sanitizer runtime
    // installs its own alt stack on every thread and then frees it at thread
    // exit by *querying* — ASan's UnsetAlternateSignalStack asks the kernel
    // which stack is current and munmaps that, assuming it is the one ASan
    // mmapped. Displacing it therefore hands ASan a pointer it never allocated;
    // ours was a thread_local, so the address is a TLS-block offset, not
    // page-aligned, munmap fails EINVAL, and ASan treats that as fatal.
    //
    // Every `gcc-asan` run of a binary that starts a JobSystem worker died at
    // thread teardown because of this — Core and App — so ASan never actually
    // covered the threaded half of the engine. The stand-in below is a plain
    // buffer, which is enough: what is asserted is that the call does not
    // replace what it finds.
    static std::array<char, 64 * 1024> foreignStorage;

    bool queried = false;
    stack_t afterInstall{};
    const auto body = [&queried, &afterInstall]
                      {
                          stack_t foreign{};
                          foreign.ss_sp    = foreignStorage.data();
                          foreign.ss_size  = foreignStorage.size();
                          foreign.ss_flags = 0;

                          // Saved and put back below, and this is not tidiness:
                          // leaving our buffer installed when the thread exits
                          // reproduces the very bug under test against the
                          // sanitizer running this suite. A test that displaces
                          // an alt stack has to restore it.
                          stack_t saved{};
                          if (sigaltstack(&foreign, &saved) != 0)
                              return; // the platform refused it; nothing to assert

                          InstallSignalStackForThisThread();
                          queried = sigaltstack(nullptr, &afterInstall) == 0;

                          (void)sigaltstack(&saved, nullptr);
                      };

    std::thread worker(body);
    worker.join();

    REQUIRE(queried);
    CHECK(afterInstall.ss_sp == foreignStorage.data());
    CHECK(afterInstall.ss_size == foreignStorage.size());
}

#endif // !_WIN32
