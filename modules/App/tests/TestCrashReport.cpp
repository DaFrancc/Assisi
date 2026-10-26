/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCrashReport.cpp
/// @brief A real crash under the engine's crash handlers: the crash log, the
///        minidump, and the reporter they start, which names the frames.
///
/// Driven through Assisi-CrashHelper, which crashes on purpose, because a crash
/// is the one thing a test cannot do in its own process.

#include <doctest/doctest.h>

#ifndef _WIN32

#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spawn.h>
#include <sys/wait.h>

#include <Assisi/App/CrashReport.hpp>

extern char **environ;

namespace
{

/// The longest the reporter is waited for. It names frames with addr2line over
/// a debug binary of the whole engine, which takes a few seconds.
constexpr std::chrono::seconds kReporterTimeout{90};

/// How often a test looks for what the reporter wrote.
constexpr std::chrono::milliseconds kPollInterval{100};

struct CrashRun
{
    std::filesystem::path root;
    int status = 0;
};

/// Runs @p helper, asked to crash @p how, with its user root a fresh folder,
/// and waits for it to die.
CrashRun Crash(const std::filesystem::path &helper, std::string_view how, std::string_view label)
{
    CrashRun run;
    run.root = std::filesystem::temp_directory_path() / std::string("assisi_crash_").append(label);
    std::filesystem::remove_all(run.root);
    std::filesystem::create_directories(run.root);

    std::vector<std::string> environment;
    for (char **entry = environ; *entry != nullptr; ++entry)
    {
        if (!std::string_view(*entry).starts_with("ASSISI_USER_ROOT="))
        {
            environment.emplace_back(*entry);
        }
    }
    environment.push_back("ASSISI_USER_ROOT=" + run.root.string());
    std::vector<char *> environmentArray;
    for (std::string &entry : environment)
    {
        environmentArray.push_back(entry.data());
    }
    environmentArray.push_back(nullptr);

    std::string program = helper.string();
    std::string asked(how);
    std::vector<char *> arguments{program.data(), asked.data(), nullptr};
    pid_t child = 0;
    REQUIRE(posix_spawn(&child, program.c_str(), nullptr, nullptr, arguments.data(), environmentArray.data()) == 0);
    REQUIRE(waitpid(child, &run.status, 0) == child);
    return run;
}

std::string ReadAll(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/// The files in @p folder with @p extension.
std::vector<std::filesystem::path> FilesIn(const std::filesystem::path &folder, std::string_view extension)
{
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    for (const std::filesystem::directory_entry &entry : std::filesystem::recursive_directory_iterator(folder, ec))
    {
        if (entry.is_regular_file(ec) && entry.path().extension() == extension)
        {
            found.push_back(entry.path());
        }
    }
    return found;
}

/// The run's crash log once the reporter has written the named stack into it,
/// which is the last thing it does to the files; empty if it never does.
std::string ReportedLog(const CrashRun &run)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kReporterTimeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (const std::filesystem::path &candidate : FilesIn(run.root, Assisi::App::kCrashLogExtension))
        {
            if (candidate.parent_path() == run.root && candidate.filename().string().starts_with("crash-"))
            {
                const std::string text = ReadAll(candidate);
                if (text.find("Stack:\n") != std::string::npos)
                {
                    return text;
                }
            }
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    return {};
}

/// The named stack the reporter wrote, up to the raw frames below it.
std::string_view StackOf(std::string_view log)
{
    const std::size_t start = log.find("Stack:\n");
    const std::size_t end = log.find("Frames:\n");
    if (start == std::string_view::npos || end == std::string_view::npos || end < start)
    {
        return {};
    }
    return log.substr(start, end - start);
}

} // namespace

TEST_CASE("A crash leaves a crash log with its frames named by the reporter it starts, and a minidump beside it")
{
    const CrashRun run = Crash(ASSISI_CRASH_HELPER, "fault", "fault");
    REQUIRE(WIFSIGNALED(run.status));
    CHECK(WTERMSIG(run.status) == SIGSEGV);

    const std::string log = ReportedLog(run);
    REQUIRE_MESSAGE(!log.empty(), "the reporter never wrote the named stack into a crash log");
    CHECK(log.find("Signal: SIGSEGV") != std::string::npos);
    CHECK(log.find("Modules:\n") != std::string::npos);
    CHECK(log.find("Assisi-CrashHelper") != std::string::npos);

    const std::string_view stack = StackOf(log);
#ifdef ASSISI_RELEASE_BUILD
    // A shipped build carries no symbols, so the frame is given by where it is
    // and by no name, rather than by whichever exported function lies nearest.
    CHECK_MESSAGE(stack.find("#0  ??  (Assisi-CrashHelper + 0x") != std::string_view::npos, stack);
#else
    CHECK_MESSAGE(stack.find("CrashOnPurpose") != std::string_view::npos, stack);
    CHECK(stack.find("CrashHelper.cpp:") != std::string_view::npos);
#endif

#ifdef ASSISI_CRASHPAD
    // Moved out of crashpad_handler's folder and named for the run, beside the log.
    const std::vector<std::filesystem::path> dumps = FilesIn(run.root, Assisi::App::kCrashDumpExtension);
    REQUIRE(dumps.size() == 1);
    CHECK(dumps.front().parent_path() == run.root);
    CHECK(dumps.front().filename().string().starts_with("crash-"));
    CHECK(ReadAll(dumps.front()).starts_with("MDMP"));
#endif
}

TEST_CASE("Core::Abort's reason is the reason the crash log gives")
{
    const CrashRun run = Crash(ASSISI_CRASH_HELPER, "abort", "abort");
    REQUIRE(WIFSIGNALED(run.status));
    CHECK(WTERMSIG(run.status) == SIGABRT);

    const std::string log = ReportedLog(run);
    REQUIRE(!log.empty());
    CHECK(log.find("Reason: the crash helper was asked to abort") != std::string::npos);
    CHECK(log.find("Called from: ") != std::string::npos);
    CHECK(log.find("CrashHelper.cpp:") != std::string::npos);
}

TEST_CASE("Without crashpad_handler beside it, a crash still leaves its crash log and starts the reporter")
{
    // A copy of the helper alone in a folder, so there is no handler beside it.
    const std::filesystem::path alone = std::filesystem::temp_directory_path() / "assisi_crash_helper_alone";
    std::filesystem::remove_all(alone);
    std::filesystem::create_directories(alone);
    const std::filesystem::path helper = alone / std::filesystem::path(ASSISI_CRASH_HELPER).filename();
    std::filesystem::copy_file(ASSISI_CRASH_HELPER, helper);

    const CrashRun run = Crash(helper, "fault", "alone");
    REQUIRE(WIFSIGNALED(run.status));
    CHECK(WTERMSIG(run.status) == SIGSEGV);

    const std::string log = ReportedLog(run);
    REQUIRE(!log.empty());
    CHECK(log.find("Signal: SIGSEGV") != std::string::npos);
    CHECK(FilesIn(run.root, Assisi::App::kCrashDumpExtension).empty());
}

#endif
