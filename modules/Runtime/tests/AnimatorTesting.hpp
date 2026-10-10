/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AnimatorTesting.hpp
/// @brief The asset tree a `.sgl` file compiles against in a test: the real
///        UAL1 model, blend space and clips by their real kinds, and libraries
///        written in memory. And the character the book teaches from.

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Runtime/AnimatorGraph.hpp>
#include <Assisi/Runtime/Import/AnimatorCompiler.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Assisi::Runtime::Testing
{

inline constexpr std::string_view kModel = "quaternius/UAL1/UAL1.glb";
inline constexpr std::string_view kSpace = "quaternius/UAL1/Locomotion.ablnd";
inline constexpr std::string_view kMaterial = "quaternius/UAL1/UAL1_M_Joints.amat";
inline constexpr std::string_view kClips = "quaternius/UAL1/UAL1_animations/";

/// The asset tree as a cook sees it: the real UAL1 files by their real kinds,
/// and any `.sgl` libraries a test writes in memory.
class TestTree final : public Core::AssetCookContext
{
  public:
    TestTree()
    {
        REQUIRE(Core::AssetSystem::SetRoot(std::filesystem::path{ASSISI_SOURCE_ASSET_ROOT}).has_value());
        Name(std::string{kModel}, "mesh");
        Name(std::string{kSpace}, "blend space");
        Name(std::string{kMaterial}, "reflected");
        for (const std::string_view clip : {"Idle_Loop", "Walk_Loop", "Jump_Start", "Jump_Loop", "Jump_Land",
                                            "Pistol_Aim_Neutral", "Hit_Chest"})
        {
            Name(std::string{kClips} + std::string{clip} + ".glb", "animation");
        }
    }

    void Library(const std::string &vpath, std::string text)
    {
        Name(vpath, "animator");
        _libraries[vpath] = std::move(text);
    }

    [[nodiscard]] std::string_view Path() const override { return "characters/hero.sgl"; }

    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> Read(std::string_view vpath) const override
    {
        const std::unordered_map<std::string, std::string>::const_iterator library =
            _libraries.find(std::string{vpath});
        if (library != _libraries.end())
        {
            const std::byte *first = reinterpret_cast<const std::byte *>(library->second.data());
            return std::vector<std::byte>{first, first + library->second.size()};
        }
        std::expected<std::vector<std::byte>, Core::AssetError> bytes = Core::AssetSystem::ReadBinary(vpath);
        if (!bytes)
        {
            return std::unexpected("not in the tree");
        }
        return std::move(*bytes);
    }

    [[nodiscard]] std::optional<Core::AssetId> IdFor(std::string_view vpath) const override
    {
        const Core::AssetId id = Core::DerivedAssetId(vpath);
        return _kinds.contains(id) ? std::optional<Core::AssetId>{id} : std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> KindNameOf(Core::AssetId id) const override
    {
        const std::unordered_map<Core::AssetId, std::string>::const_iterator found = _kinds.find(id);
        return found != _kinds.end() ? std::optional<std::string>{found->second} : std::nullopt;
    }

    void Report(std::string message) const override { _reported += message; }

  private:
    void Name(const std::string &vpath, std::string kind) { _kinds[Core::DerivedAssetId(vpath)] = std::move(kind); }

    std::unordered_map<Core::AssetId, std::string> _kinds;
    std::unordered_map<std::string, std::string> _libraries;
    mutable std::string _reported;
};

/// The character the book teaches from: locomotion on a blend space, a jump
/// that falls until it lands, and an aim held over the upper body.
inline constexpr std::string_view kCharacter = R"(use animation;
skeleton "quaternius/UAL1/UAL1.glb";

param speed: float;
param grounded: bool;
param jump: trigger;
param aiming: float;

const locomotion_space = "quaternius/UAL1/Locomotion.ablnd";
const jump_start_clip = "quaternius/UAL1/UAL1_animations/Jump_Start.glb";
const fall_clip = "quaternius/UAL1/UAL1_animations/Jump_Loop.glb";
const land_clip = "quaternius/UAL1/UAL1_animations/Jump_Land.glb";
const aim_clip = "quaternius/UAL1/UAL1_animations/Pistol_Aim_Neutral.glb";

layer base {
    state locomotion { play locomotion_space; x speed; }
    state airborne {
        state jump_start { play jump_start_clip; then fall; }
        state fall { play fall_clip; }
    }
    state land { play land_clip; then locomotion; }

    locomotion -> airborne when jump && grounded { fade 0.05; };
    airborne -> land when grounded { fade 0.1; };
    land -> airborne when !grounded { fade 0.15; interrupt; };
}

layer upper {
    mask "spine_01";
    weight aiming;
    state aim { pose aim_clip; }
}
)";

inline std::string Clip(std::string_view name)
{
    return std::string{kClips} + std::string{name} + ".glb";
}

} // namespace Assisi::Runtime::Testing
