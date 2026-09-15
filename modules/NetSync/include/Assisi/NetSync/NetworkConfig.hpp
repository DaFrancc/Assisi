/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file NetworkConfig.hpp
/// @brief Everything a game tunes about the network, as one file.
///
/// The three kinds of value here are deliberately together, because they share
/// a lifetime: all of them ship fixed with the build and none of them is the
/// player's to change. They do *not* share a consequence, and the field
/// comments say which is which — quantization is inside the protocol hash, so
/// two builds that disagree refuse to pair; smoothing and the policy fields are
/// not, so two builds that disagree pair fine.
///
/// Flat rather than grouped into objects. Grouping by topic is what previously
/// hid the distinction that matters: `neverReplicate` sat inside the same
/// `networking` object as the quantization while behaving nothing like it.
///
/// Read once at startup by LoadNetworkConfig and applied to the process
/// globals; nothing holds an instance afterwards.

#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Core/Reflect/AssetDocument.hpp>
#include <Assisi/Core/ShortString.hpp>
#include <Assisi/NetSync/BodyState.hpp>
#include <Assisi/NetSync/ReplicationConfig.hpp>

#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

namespace Assisi::NetSync
{

/// @brief The network config document, as it sits on disk.
///
/// Every default here is read from the struct that owns the value rather than
/// restated, so this file cannot drift from the defaults the engine uses when
/// no config is present.
AASSET()
struct NetworkConfig
{
    /// Component type names this game never sends. Not hashed: it changes which
    /// blocks are sent, never how bytes decode, so two builds that differ still
    /// pair and the server's list governs.
    AFIELD() std::vector<Assisi::Core::ShortString> neverReplicate;

    // --- Quantization. Inside the protocol hash: a mismatched pair refuses to
    // connect rather than decoding each other's bytes into the wrong numbers.
    AFIELD() float positionExtent = BodyQuantization{}.positionExtent;
    AFIELD() uint32_t positionBits = BodyQuantization{}.positionBits;
    AFIELD() float linearVelocityMax = BodyQuantization{}.linearVelocityMax;
    AFIELD() uint32_t linearVelocityBits = BodyQuantization{}.linearVelocityBits;
    AFIELD() float angularVelocityMax = BodyQuantization{}.angularVelocityMax;
    AFIELD() uint32_t angularVelocityBits = BodyQuantization{}.angularVelocityBits;

    // --- Smoothing. Purely view-side and not hashed: two machines that smooth
    // differently still agree about every byte.
    AFIELD() float positionCorrectionTime = ViewSmoothing{}.positionCorrectionTime;
    AFIELD() float positionCorrectionTimeFast = ViewSmoothing{}.positionCorrectionTimeFast;
    AFIELD() float smallErrorDistance = ViewSmoothing{}.smallErrorDistance;
    AFIELD() float largeErrorDistance = ViewSmoothing{}.largeErrorDistance;
    AFIELD() float rotationCorrectionTime = ViewSmoothing{}.rotationCorrectionTime;
    AFIELD() float snapBelowDistance = ViewSmoothing{}.snapBelowDistance;
    AFIELD() float hardSnapDistance = ViewSmoothing{}.hardSnapDistance;

    // --- Relevancy: who is told about what. Not hashed, same reason as the
    // never-replicate list.
    AFIELD() float relevancyRadius = RelevancyConfig{}.radius;
    AFIELD() float relevancyExitRadius = RelevancyConfig{}.exitRadius;
    AFIELD() uint32_t relevancyDwellTicks = RelevancyConfig{}.dwellTicks;

    /// Which provider decides relevancy: `all` or `distance`. A name rather than
    /// a number so the file says what it means; a name this build does not know
    /// warns and leaves everything relevant.
    ///
    /// Last among the fields because it is the only one that is not four bytes
    /// wide: after the scalars it starts on an offset its own two-byte alignment
    /// already satisfies, so the struct carries no interior padding.
    AFIELD() Assisi::Core::ShortString relevancyProvider{"all"};
};

/// @brief Apply @p config to this process: quantization, smoothing, the
///        never-replicate list, and the relevancy settings.
///
/// Each block is validated and refused independently — a typo in the
/// quantization must not cost the smoothing, and vice versa. A refused block
/// warns and keeps the defaults, because the alternative is a build that
/// encodes garbage or refuses to pair with every other one without saying why.
void ApplyNetworkConfig(const NetworkConfig &config);

/// @brief Read the network config and apply it. Call once at startup, before
///        any session exists: quantization is inside the handshake hash, so it
///        has to be settled before the first hello is written.
///
/// Nothing to report, so nothing is returned: a missing file is not a failure
/// (the defaults are a complete answer), and an unreadable one warns and leaves
/// the same defaults in place. There is no outcome a caller could act on that
/// this has not already acted on.
void LoadNetworkConfig(std::string_view assetPath = "config/network.json");

/// @brief The component type names this game never sends, as applied.
[[nodiscard]] const std::vector<std::string> &NeverReplicate();

/// @brief The relevancy settings this game uses, as applied.
[[nodiscard]] const RelevancyConfig &Relevancy();

} // namespace Assisi::NetSync
