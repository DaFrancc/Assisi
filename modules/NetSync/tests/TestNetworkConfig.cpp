/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Core/Reflect/AssetDocument.hpp>
#include <Assisi/NetSync/BodyState.hpp>
#include <Assisi/NetSync/NetProtocol.hpp>
#include <Assisi/NetSync/NetworkConfig.hpp>

#include <string_view>

using namespace Assisi::NetSync;
using Assisi::Core::ShortString;

namespace
{
/// @brief Apply @p text as a network config, as LoadNetworkConfig would once it
///        has the file's bytes.
bool ApplyText(std::string_view text)
{
    NetworkConfig config;
    if (!Assisi::Core::Reflect::ApplyAssetDocument(text, config))
    {
        return false;
    }
    ApplyNetworkConfig(config);
    return true;
}

/// @brief Puts the process globals back, so a case cannot leak into the next.
struct RestoreGlobals
{
    BodyQuantization quantization = Quantization();
    ViewSmoothing smoothing       = Smoothing();

    ~RestoreGlobals()
    {
        SetQuantization(quantization);
        SetSmoothing(smoothing);
    }
};
} // namespace

TEST_CASE("a config that quantizes differently changes the hash two peers compare")
{
    // The end of the chain the handshake protects: a file on disk reaches the
    // process globals, and the protocol hash moves with it. Without this the
    // hash test proves only that SetQuantization works, never that a config ever
    // reaches it.
    const RestoreGlobals restore;
    const std::uint64_t baseline = NetProtocolHash();

    REQUIRE(ApplyText(R"({ "version": 1, "type": "NetworkConfig", "positionExtent": 512.0 })"));

    CHECK(Quantization().positionExtent == doctest::Approx(512.f));
    CHECK(NetProtocolHash() != baseline);
    CHECK(NetProtocolSummary().find("positionExtent=512") != std::string::npos);
}

TEST_CASE("a bad quantization block costs only itself")
{
    // The two blocks are validated apart because they fail differently: a broken
    // quantization refuses to pair with every other build, while broken
    // smoothing only looks wrong. Rejecting both together would mean one typo
    // silently discarding the other's valid settings.
    const RestoreGlobals restore;
    const BodyQuantization quantizationBefore = Quantization();

    REQUIRE(ApplyText(R"({ "version": 1, "type": "NetworkConfig",
                           "positionBits": 0, "hardSnapDistance": 9.0 })"));

    CHECK(Quantization() == quantizationBefore);
    CHECK(Smoothing().hardSnapDistance == doctest::Approx(9.f));
}

TEST_CASE("a bad smoothing block costs only itself")
{
    const RestoreGlobals restore;
    const float snapBefore = Smoothing().hardSnapDistance;

    // largeErrorDistance under smallErrorDistance inverts the blend.
    REQUIRE(ApplyText(R"({ "version": 1, "type": "NetworkConfig",
                           "smallErrorDistance": 5.0, "largeErrorDistance": 1.0,
                           "positionExtent": 300.0 })"));

    CHECK(Smoothing().hardSnapDistance == doctest::Approx(snapBefore));
    CHECK(Quantization().positionExtent == doctest::Approx(300.f));
}

TEST_CASE("the never-replicate list and the relevancy settings reach the accessors")
{
    const RestoreGlobals restore;

    REQUIRE(ApplyText(R"({ "version": 1, "type": "NetworkConfig",
                           "neverReplicate": ["Health", "Inventory"],
                           "relevancyProvider": "distance", "relevancyRadius": 40.0 })"));

    REQUIRE(NeverReplicate().size() == 2);
    CHECK(NeverReplicate()[0] == "Health");
    CHECK(Relevancy().provider == RelevancyConfig::Provider::Distance);
    CHECK(Relevancy().radius == doctest::Approx(40.f));
}

TEST_CASE("a provider this build does not know leaves everything relevant")
{
    // Failing wide rather than narrow, and loudly: quietly telling everyone
    // about everything would leave an author believing a radius is in force.
    const RestoreGlobals restore;

    REQUIRE(ApplyText(R"({ "version": 1, "type": "NetworkConfig", "relevancyProvider": "nearby" })"));

    CHECK(Relevancy().provider == RelevancyConfig::Provider::All);
}
