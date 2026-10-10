/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Testing/TestAssetKinds.hpp>

#include <Assisi/Core/AssetKind.hpp>

#include <algorithm>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Testing
{

namespace
{

constexpr std::byte kFailCook{'X'};
constexpr std::byte kFailFinish{'F'};

/// The reversing step has never changed what it cooks.
constexpr std::uint32_t kReversedCookVersion = 1;
/// Nor has the linking one.
constexpr std::uint32_t kLinkedCookVersion = 1;

std::expected<TestBytes, Core::AssetError> Load(std::span<const std::byte> payload)
{
    TestKindLoads().fetch_add(1);
    if (payload.empty())
    {
        return std::unexpected(Core::AssetError{Core::AssetErrorCode::CorruptAsset, kEmptyPayloadDetail});
    }
    return TestBytes{.bytes = {payload.begin(), payload.end()}};
}

std::expected<TestCount, Core::AssetError> LoadCount(std::span<const std::byte> payload)
{
    TestKindLoads().fetch_add(1);
    return TestCount{.bytes = payload.size()};
}

std::expected<void, Core::AssetError> Finish(TestBytes &loaded)
{
    if (!loaded.bytes.empty() && loaded.bytes.front() == kFailFinish)
    {
        return std::unexpected(Core::AssetError{Core::AssetErrorCode::CorruptAsset, kFinishRefusedDetail});
    }
    loaded.finished = true;
    return {};
}

std::expected<std::vector<std::byte>, Core::AssetError> CookReversed(std::span<const std::byte> source)
{
    if (!source.empty() && source.front() == kFailCook)
    {
        return std::unexpected(Core::AssetError{Core::AssetErrorCode::CorruptAsset, kCookRefusedDetail});
    }
    std::vector<std::byte> reversed{source.begin(), source.end()};
    std::ranges::reverse(reversed);
    return reversed;
}

bool RegisterReversed()
{
    Core::AssetKind kind = Core::MakeAssetKind<TestBytes>(
        "test reversed bytes", {Core::AssetFormat{.extension = ".tbytes", .preferred = true}}, Load);
    kind.finish = Core::MakeAssetFinish<TestBytes>(Finish);
    const bool registered = Core::AssetKindRegistry::Instance().Register(std::move(kind));
    return registered && Core::AssetKindRegistry::Instance().RegisterCookStep(
                             Core::MakeAssetCookStep(kReversedKind, kReversedCookVersion, CookReversed));
}

std::string LinkedPath(std::span<const std::byte> source)
{
    return std::string{reinterpret_cast<const char *>(source.data()), source.size()};
}

std::expected<std::vector<std::byte>, Core::AssetError> CookLinked(std::span<const std::byte> source,
                                                                   const Core::AssetCookContext &context)
{
    std::expected<std::vector<std::byte>, std::string> linked = context.Read(LinkedPath(source));
    if (!linked)
    {
        context.Report(linked.error());
        return std::unexpected(Core::AssetError{Core::AssetErrorCode::CorruptAsset, kLinkUnreadableDetail});
    }
    return std::move(*linked);
}

std::vector<std::string> LinkedDependencies(std::span<const std::byte> source, const Core::AssetCookContext &)
{
    return {LinkedPath(source)};
}

bool RegisterLinked()
{
    const bool registered = Core::AssetKindRegistry::Instance().Register(Core::MakeAssetKind<TestBytes>(
        "test linked bytes", {Core::AssetFormat{.extension = ".tlink", .preferred = true}}, Load));
    return registered && Core::AssetKindRegistry::Instance().RegisterCookStep(Core::MakeContextCookStep(
                             kLinkedKind, kLinkedCookVersion, CookLinked, LinkedDependencies));
}

bool RegisterRaw()
{
    // Also reads `.png` without preferring it: a PNG can be chosen as this kind,
    // and a new one is still a texture.
    return Core::AssetKindRegistry::Instance().Register(Core::MakeAssetKind<TestBytes>(
        "test raw bytes",
        {Core::AssetFormat{.extension = ".traw", .preferred = true}, Core::AssetFormat{.extension = ".png"}}, Load));
}

bool RegisterCount()
{
    return Core::AssetKindRegistry::Instance().Register(Core::MakeAssetKind<TestCount>(
        "test byte count", {Core::AssetFormat{.extension = ".tcount", .preferred = true}}, LoadCount));
}

[[maybe_unused]] const bool kReversedRegistered = RegisterReversed();
[[maybe_unused]] const bool kRawRegistered = RegisterRaw();
[[maybe_unused]] const bool kCountRegistered = RegisterCount();
[[maybe_unused]] const bool kLinkedRegistered = RegisterLinked();

} // namespace

std::atomic<std::uint32_t> &TestKindLoads()
{
    static std::atomic<std::uint32_t> loads = 0;
    return loads;
}

} // namespace Assisi::Testing
