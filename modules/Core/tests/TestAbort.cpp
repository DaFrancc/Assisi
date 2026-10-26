/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestAbort.cpp
/// @brief Core::Abort ends the process in every build, with its reason logged
///        and kept where a crash handler reads it.
///
/// Each case runs the abort in a forked child, since the point is that the
/// process does not survive it.

#include <doctest/doctest.h>

#ifndef _WIN32

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <Assisi/Core/Abort.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Sinks.hpp>

using namespace Assisi::Core;

namespace
{

/// Where the child's SIGABRT handler writes what Abort kept, as a crash handler
/// would read it.
std::int32_t gReasonFile = -1;

void WriteReason(std::int32_t)
{
    const char *reason = AbortReason();
    (void)write(gReasonFile, reason, std::strlen(reason));
    (void)write(gReasonFile, "\n", 1);
    const char *location = AbortLocation();
    (void)write(gReasonFile, location, std::strlen(location));
    close(gReasonFile);
    std::signal(SIGABRT, SIG_DFL);
    std::raise(SIGABRT);
}

std::string ReadAll(const std::filesystem::path &path)
{
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

struct Fixture
{
    std::filesystem::path folder = std::filesystem::temp_directory_path() / "assisi_abort_test";
    std::filesystem::path log = folder / "abort.log";
    std::filesystem::path reason = folder / "reason.txt";

    Fixture()
    {
        std::filesystem::remove_all(folder);
        std::filesystem::create_directories(folder);
    }
    ~Fixture() { std::filesystem::remove_all(folder); }
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;
};

} // namespace

TEST_CASE("Abort ends the process with SIGABRT, its reason in the log and kept for the crash handler")
{
    Fixture fixture;
    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        GetLogger().AddSink(std::make_shared<FileSink>(fixture.log));
        gReasonFile = open(fixture.reason.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        std::signal(SIGABRT, WriteReason);
        Abort("the package {} is corrupt", "assets.pak");
    }

    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGABRT);

    const std::string log = ReadAll(fixture.log);
    CHECK(log.find("[FATAL]") != std::string::npos);
    CHECK(log.find("the package assets.pak is corrupt") != std::string::npos);
    CHECK(log.find("TestAbort.cpp") != std::string::npos);

    const std::string kept = ReadAll(fixture.reason);
    CHECK(kept.starts_with("the package assets.pak is corrupt\n"));
    CHECK(kept.find("TestAbort.cpp:") != std::string::npos);
}

#endif
