/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file Abort.cpp

#include <Assisi/Core/Abort.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <span>

namespace Assisi::Core
{
namespace
{

/// Room for the reason a crash report quotes. A longer one is cut there only;
/// the log line keeps all of it.
constexpr std::size_t kReasonCapacity = 1024;

/// Room for "file:line", the file being a full path.
constexpr std::size_t kLocationCapacity = 512;

// Fixed storage, so a signal handler can read it without allocating.
std::array<char, kReasonCapacity> gReason{};
std::array<char, kLocationCapacity> gLocation{};

// The first Abort takes the storage. One racing it on another thread leaves it
// alone, so the reason a report quotes is never two reasons spliced together.
std::atomic_flag gClaimed = ATOMIC_FLAG_INIT;

// Set once the storage holds a whole reason, so a reader never sees half of one.
std::atomic<bool> gPublished{false};

void CopyTerminated(std::span<char> to, std::string_view from) noexcept
{
    const std::size_t length = std::min(from.size(), to.size() - 1);
    std::memcpy(to.data(), from.data(), length);
    to[length] = '\0';
}

} // namespace

void AbortAt(std::string_view reason, std::string_view file, std::uint32_t line) noexcept
{
    // Kept before it is logged: logging formats and allocates, and if that fails
    // the abort still has its reason.
    if (!gClaimed.test_and_set())
    {
        CopyTerminated(gReason, reason);
        const std::format_to_n_result<char *> written =
            std::format_to_n(gLocation.data(), gLocation.size() - 1, "{}:{}", file, line);
        *written.out = '\0';
        gPublished.store(true, std::memory_order_release);
    }

    GetLogger().Log(LogLevel::Fatal, std::format("Abort: {}  [{}:{}]", reason, file, line));
    std::abort();
}

const char *AbortReason() noexcept
{
    return gPublished.load(std::memory_order_acquire) ? gReason.data() : "";
}

const char *AbortLocation() noexcept
{
    return gPublished.load(std::memory_order_acquire) ? gLocation.data() : "";
}

} // namespace Assisi::Core
