/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TestAssetKinds.hpp
/// @brief Kinds registered only in test executables, the way a module registers
///        its own: from a translation unit of their own (TestAssetKinds.cpp),
///        with nothing in the engine naming them.
///
/// A test that links Assisi::TestAssetKinds gets both registered before main().

#include <Assisi/Core/CookedBlob.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Assisi::Testing
{

/// @brief What both test kinds load into: their payload, as it arrived.
struct TestBytes
{
    std::vector<std::byte> bytes;
    bool finished = false;
};

/// @brief `.tbytes`: cooked by reversing its bytes, and finished on the main
///        thread. A source that starts with 'X' fails the cook, and a payload
///        that starts with 'F' fails the finish.
inline constexpr Core::AssetKindId kReversedKind{"test reversed bytes"};

/// @brief `.traw`, and `.png` when a sidecar chooses it: no cook step and no
///        finish, so its payload is its source. An empty payload fails to load.
inline constexpr Core::AssetKindId kRawKind{"test raw bytes"};

/// @brief The detail each test kind's refusal carries, so a test can tell which
///        refusal it got.
inline constexpr std::string_view kEmptyPayloadDetail = "the payload is empty";
inline constexpr std::string_view kCookRefusedDetail = "the source asks the cook to fail";
inline constexpr std::string_view kFinishRefusedDetail = "the payload asks the finish to fail";

/// @brief How many times either kind's load has run, for tests that check a
///        load did or did not happen.
std::atomic<std::uint32_t> &TestKindLoads();

} // namespace Assisi::Testing
