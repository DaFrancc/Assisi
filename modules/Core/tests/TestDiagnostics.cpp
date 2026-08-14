/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestDiagnostics.cpp
/// @brief Launch stamps and artifact retention.
///
/// The property under test throughout is that a run cannot lose its own log.
/// Every case here corresponds to a way that has actually failed: a filename
/// that sorts wrong because local time moved backwards, and two processes that
/// computed the same name because the stamp had one-second resolution. Both
/// were silent — the log went missing with nothing to say it had.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
    // The case that made this function wrong. LaunchStamp() is local time, so a
    // DST fall-back, a westward flight or an NTP step backwards gives this run a
    // name that sorts OLDEST of the set. Sorting alone would delete it first —
    // on POSIX while its descriptor is still open, so the run would go on
    // writing into an unlinked inode and lose everything at exit.
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

    // 0 means "keep no history", not "delete the log being written" — which is
    // what game.json documents and what the removed clamp used to contradict.
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
