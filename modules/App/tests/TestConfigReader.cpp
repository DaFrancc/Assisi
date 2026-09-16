/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestConfigReader.cpp
/// @brief A config reads the same through the text reader and the cooked one, and
/// each tells a missing config from a broken one.

#include <doctest/doctest.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include <Assisi/App/AppConfig.hpp>
#include <Assisi/Core/AssetProvider.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/ConfigReader.hpp>
#include <Assisi/Core/CookedPayload.hpp>
#include <Assisi/Core/Reflect/AssetTypeRegistry.hpp>
#include <Assisi/Window/InputBindings.hpp>

using namespace Assisi;

namespace
{

constexpr double kConfiguredHz = 30.0;

/// Cooked blobs by path, held in memory: a pak without the file.
class MemoryProvider final : public Core::AssetProvider
{
public:
    void Add(std::string_view vpath, std::vector<std::byte> bytes)
    {
        _blobs[Core::DerivedAssetId(vpath)] = std::move(bytes);
    }

    [[nodiscard]] std::expected<std::vector<std::byte>, Core::AssetError> Open(Core::AssetId id) const override
    {
        const auto found = _blobs.find(id);
        if (found == _blobs.end())
        {
            return std::unexpected(Core::AssetError::UnknownAssetId);
        }
        return found->second;
    }

    [[nodiscard]] std::expected<Core::AssetId, Core::AssetError> Resolve(std::string_view vpath) const override
    {
        const Core::AssetId id = Core::DerivedAssetId(vpath);
        if (!_blobs.contains(id))
        {
            return std::unexpected(Core::AssetError::UnknownAssetId);
        }
        return id;
    }

private:
    std::unordered_map<Core::AssetId, std::vector<std::byte>> _blobs;
};

std::vector<std::byte> Cooked(std::type_index type, const void *instance)
{
    const Core::Reflect::AssetTypeMeta *meta = Core::Reflect::AssetTypeRegistry::Instance().Find(type);
    REQUIRE(meta != nullptr);
    Core::BitWriter writer;
    REQUIRE(Core::WriteReflectedBlob(writer, *meta, instance));
    const std::span<const std::byte> bytes = writer.Data();
    return {bytes.begin(), bytes.end()};
}

std::filesystem::path MountTestRoot()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "assisi-config-reader";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "config");
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());
    return root;
}

void WriteText(const std::filesystem::path &file, std::string_view text)
{
    std::ofstream out(file);
    REQUIRE(out.good());
    out << text;
}

} // namespace

TEST_CASE("The text config reader fills a config, and names a missing or broken one")
{
    const std::filesystem::path root = MountTestRoot();
    WriteText(root / "config" / "game.json", R"({ "version": 1, "type": "AppConfig", "physicsHz": 30.0 })");
    WriteText(root / "config" / "broken.json", R"({ "version": 1, "type": "InputBindings" })");

    App::AppConfig config;
    REQUIRE(Core::ReadTextConfig("config/game.json", std::type_index(typeid(App::AppConfig)), &config));
    CHECK(config.physicsHz == doctest::Approx(kConfiguredHz));

    const auto missing = Core::ReadTextConfig("config/absent.json", std::type_index(typeid(App::AppConfig)), &config);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == Core::ConfigError::Missing);

    const auto wrongType =
        Core::ReadTextConfig("config/broken.json", std::type_index(typeid(App::AppConfig)), &config);
    REQUIRE_FALSE(wrongType.has_value());
    CHECK(wrongType.error() == Core::ConfigError::Malformed);
}

TEST_CASE("The cooked config reader fills a config, and names a missing or broken one")
{
    MemoryProvider provider;
    App::AppConfig shipped;
    shipped.physicsHz = kConfiguredHz;
    provider.Add("config/game.json", Cooked(std::type_index(typeid(App::AppConfig)), &shipped));
    const Window::InputBindings bindings;
    provider.Add("config/input.json", Cooked(std::type_index(typeid(Window::InputBindings)), &bindings));

    App::AppConfig config;
    REQUIRE(Core::ReadCookedConfig(provider, "config/game.json", std::type_index(typeid(App::AppConfig)), &config));
    CHECK(config.physicsHz == doctest::Approx(kConfiguredHz));

    const auto missing =
        Core::ReadCookedConfig(provider, "config/absent.json", std::type_index(typeid(App::AppConfig)), &config);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == Core::ConfigError::Missing);

    // A blob of another reflected type is refused before any field reaches the instance.
    const auto wrongType =
        Core::ReadCookedConfig(provider, "config/input.json", std::type_index(typeid(App::AppConfig)), &config);
    REQUIRE_FALSE(wrongType.has_value());
    CHECK(wrongType.error() == Core::ConfigError::Malformed);
}

TEST_CASE("A config loads through whichever reader is installed")
{
    MemoryProvider provider;
    App::AppConfig shipped;
    shipped.physicsHz = kConfiguredHz;
    provider.Add("config/game.json", Cooked(std::type_index(typeid(App::AppConfig)), &shipped));

    const Core::ConfigReader previous = Core::SetConfigReader(
        [&provider](std::string_view vpath, std::type_index type, void *instance)
        { return Core::ReadCookedConfig(provider, vpath, type, instance); });
    const App::AppConfig loaded = App::AppConfig::Load();

    // And with none installed, nothing is read, not even a loose file.
    (void)Core::SetConfigReader({});
    App::AppConfig unread;
    const auto refused = Core::ReadConfig("config/game.json", unread);
    (void)Core::SetConfigReader(previous);

    CHECK(loaded.physicsHz == doctest::Approx(kConfiguredHz));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == Core::ConfigError::Unreadable);
}
