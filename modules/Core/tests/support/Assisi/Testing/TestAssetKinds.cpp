/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Testing/TestAssetKinds.hpp>

#include <Assisi/Core/AssetKind.hpp>

#include <algorithm>
#include <expected>
#include <functional>
#include <span>
#include <string>

namespace Assisi::Testing
{

namespace
{

constexpr std::byte kFailCook{'X'};
constexpr std::byte kFailFinish{'F'};

std::expected<TestBytes, std::string> Load(std::span<const std::byte> payload)
{
    TestKindLoads().fetch_add(1);
    if (payload.empty())
    {
        return std::unexpected(std::string{"the payload is empty"});
    }
    return TestBytes{.bytes = {payload.begin(), payload.end()}};
}

bool RegisterReversed()
{
    Core::AssetKind kind = Core::MakeAssetKind<TestBytes>(
        "test reversed bytes", {Core::AssetFormat{.extension = ".tbytes", .preferred = true}}, Load);
    kind.finish = [](void *value) -> std::expected<void, std::string>
    {
        TestBytes &loaded = *static_cast<TestBytes *>(value);
        if (!loaded.bytes.empty() && loaded.bytes.front() == kFailFinish)
        {
            return std::unexpected(std::string{"the payload asks the finish to fail"});
        }
        loaded.finished = true;
        return {};
    };
    const bool registered = Core::AssetKindRegistry::Instance().Register(std::move(kind));

    Core::AssetCookStep step;
    step.kind = kReversedKind;
    step.version = 1;
    step.cook = [](std::span<const std::byte> source) -> std::expected<std::vector<std::byte>, std::string>
    {
        if (!source.empty() && source.front() == kFailCook)
        {
            return std::unexpected(std::string{"the source asks the cook to fail"});
        }
        std::vector<std::byte> reversed{source.begin(), source.end()};
        std::ranges::reverse(reversed);
        return reversed;
    };
    return registered && Core::AssetKindRegistry::Instance().RegisterCookStep(std::move(step));
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
