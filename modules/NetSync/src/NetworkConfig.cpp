/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/NetworkConfig.hpp>

#include <Assisi/Core/ConfigReader.hpp>
#include <Assisi/Core/Logger.hpp>

#include <expected>
#include <string>

namespace Assisi::NetSync
{
namespace
{
/// Narrowest and widest bit count a quantized component may use. One bit is the
/// least that encodes anything; a component is read back into a 32-bit lane, so
/// beyond 32 there is nowhere for the bits to go.
constexpr std::uint32_t kMinQuantizationBits = 1;
constexpr std::uint32_t kMaxQuantizationBits = 32;

/// The provider names a config file may use, matching RelevancyConfig::Provider.
constexpr const char *kProviderAll      = "all";
constexpr const char *kProviderDistance = "distance";

std::vector<std::string> gNeverReplicate;
RelevancyConfig gRelevancy;

[[nodiscard]] bool BitsInRange(std::uint32_t bits)
{
    return bits >= kMinQuantizationBits && bits <= kMaxQuantizationBits;
}

/// @brief Whether @p config's quantization can be encoded and decoded at all.
///
/// A typo that produced a zero bit count or an inverted range would encode
/// garbage on one machine and refuse to pair with every other build, without
/// saying which key did it. Refusing the config says it.
[[nodiscard]] bool QuantizationInRange(const NetworkConfig &config)
{
    return config.positionExtent > 0.f && config.linearVelocityMax > 0.f && config.angularVelocityMax > 0.f &&
           BitsInRange(config.positionBits) && BitsInRange(config.linearVelocityBits) &&
           BitsInRange(config.angularVelocityBits);
}

/// @brief Whether @p config's smoothing describes a convergence that terminates.
///
/// A zero or negative convergence time divides by zero in the decay; a
/// large-error distance under the small one inverts the blend.
[[nodiscard]] bool SmoothingInRange(const NetworkConfig &config)
{
    return config.positionCorrectionTime > 0.f && config.positionCorrectionTimeFast > 0.f &&
           config.rotationCorrectionTime > 0.f && config.largeErrorDistance > config.smallErrorDistance;
}

void ApplyQuantization(const NetworkConfig &config)
{
    if (!QuantizationInRange(config))
    {
        Core::Log::Warn("NetSync: the network config's quantization is out of range (extents must be positive, "
                        "bit counts {}..{}) - keeping the defaults.",
                        kMinQuantizationBits, kMaxQuantizationBits);
        return;
    }

    const BodyQuantization loaded{.positionExtent      = config.positionExtent,
                                  .positionBits        = config.positionBits,
                                  .linearVelocityMax   = config.linearVelocityMax,
                                  .linearVelocityBits  = config.linearVelocityBits,
                                  .angularVelocityMax  = config.angularVelocityMax,
                                  .angularVelocityBits = config.angularVelocityBits};
    SetQuantization(loaded);
    Core::Log::Info("NetSync: body quantization - position +/-{:g} m at {} bits, linear +/-{:g} m/s at {} bits, "
                    "angular +/-{:g} rad/s at {} bits.",
                    static_cast<double>(loaded.positionExtent), loaded.positionBits,
                    static_cast<double>(loaded.linearVelocityMax), loaded.linearVelocityBits,
                    static_cast<double>(loaded.angularVelocityMax), loaded.angularVelocityBits);
}

void ApplySmoothing(const NetworkConfig &config)
{
    if (!SmoothingInRange(config))
    {
        Core::Log::Warn("NetSync: the network config's smoothing is out of range (times must be positive, "
                        "largeErrorDistance must exceed smallErrorDistance) - keeping the defaults.");
        return;
    }

    const ViewSmoothing loaded{.positionCorrectionTime     = config.positionCorrectionTime,
                               .positionCorrectionTimeFast = config.positionCorrectionTimeFast,
                               .smallErrorDistance         = config.smallErrorDistance,
                               .largeErrorDistance         = config.largeErrorDistance,
                               .rotationCorrectionTime     = config.rotationCorrectionTime,
                               .snapBelowDistance          = config.snapBelowDistance,
                               .hardSnapDistance           = config.hardSnapDistance};
    SetSmoothing(loaded);
    Core::Log::Info("NetSync: correction smoothing - converge over {:g}s (fast {:g}s), rotation {:g}s, snap "
                    "below {:g} m and beyond {:g} m.",
                    static_cast<double>(loaded.positionCorrectionTime),
                    static_cast<double>(loaded.positionCorrectionTimeFast),
                    static_cast<double>(loaded.rotationCorrectionTime),
                    static_cast<double>(loaded.snapBelowDistance),
                    static_cast<double>(loaded.hardSnapDistance));
}

void ApplyPolicy(const NetworkConfig &config)
{
    gNeverReplicate.clear();
    gNeverReplicate.reserve(config.neverReplicate.size());
    for (const Assisi::Core::ShortString &name : config.neverReplicate)
    {
        gNeverReplicate.emplace_back(name.View());
    }

    RelevancyConfig relevancy;
    const std::string_view provider = config.relevancyProvider.View();
    if (provider == kProviderDistance)
    {
        relevancy.provider = RelevancyConfig::Provider::Distance;
    }
    else if (provider != kProviderAll)
    {
        // Loudly: falling back to "everything" quietly would leave the author
        // believing a radius is in force when it is not.
        Core::Log::Warn("NetSync: relevancyProvider is '{}', which is not a provider this build knows ('{}' or "
                        "'{}') - telling every connection about everything.",
                        provider, kProviderAll, kProviderDistance);
    }
    relevancy.radius     = config.relevancyRadius;
    relevancy.exitRadius = config.relevancyExitRadius;
    relevancy.dwellTicks = config.relevancyDwellTicks;
    gRelevancy           = relevancy;
}
} // namespace

void ApplyNetworkConfig(const NetworkConfig &config)
{
    // Independently, so one bad block costs only itself.
    ApplyQuantization(config);
    ApplySmoothing(config);
    ApplyPolicy(config);
}

void LoadNetworkConfig(std::string_view assetPath)
{
    NetworkConfig config;
    const std::expected<void, Core::ConfigError> read = Core::ReadConfig(assetPath, config);
    if (!read)
    {
        // No config is not a problem; the defaults are a complete answer.
        if (read.error() != Core::ConfigError::Missing)
        {
            Core::Log::Warn("NetSync: cannot read '{}' ({}) - keeping the defaults.", assetPath,
                            Core::ToString(read.error()));
        }
        return;
    }

    ApplyNetworkConfig(config);
}

const std::vector<std::string> &NeverReplicate() { return gNeverReplicate; }

const RelevancyConfig &Relevancy() { return gRelevancy; }

} // namespace Assisi::NetSync
