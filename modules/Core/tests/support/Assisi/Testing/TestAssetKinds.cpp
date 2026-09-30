/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Testing/TestAssetKinds.hpp>

#include <Assisi/Core/AssetKind.hpp>

#include <algorithm>
#include <expected>
#include <span>
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

std::expected<TestBytes, Core::AssetError> Load(std::span<const std::byte> payload)
{
    TestKindLoads().fetch_add(1);
    if (payload.empty())
    {
        return std::unexpected(Core::AssetError{Core::AssetErrorCode::CorruptAsset, kEmptyPayloadDetail});
    }
    return TestBytes{.bytes = {payload.begin(), payload.end()}};
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

bool RegisterRaw()
{
    // Also reads `.png` without preferring it: a PNG can be chosen as this kind,
    // and a new one is still a texture.
    return Core::AssetKindRegistry::Instance().Register(Core::MakeAssetKind<TestBytes>(
        "test raw bytes",
        {Core::AssetFormat{.extension = ".traw", .preferred = true}, Core::AssetFormat{.extension = ".png"}}, Load));
}

[[maybe_unused]] const bool kReversedRegistered = RegisterReversed();
[[maybe_unused]] const bool kRawRegistered = RegisterRaw();

} // namespace

std::atomic<std::uint32_t> &TestKindLoads()
{
    static std::atomic<std::uint32_t> loads = 0;
    return loads;
}

} // namespace Assisi::Testing
