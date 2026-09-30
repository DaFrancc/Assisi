/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Cooker.hpp
/// @brief One asset in, one cooked blob out, and the interface every kind's
///        cooker answers.
///
/// Which cooker takes a file is not the cooker's question: the file's sidecar
/// names its kind, and the cooker is the one whose Kind() that is. A cooker is
/// asked what else its output depends on and what the bytes are; splitting the
/// first out of the second is what makes the cook incremental.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Geometry/MaterialChannels.hpp>
#include <Assisi/Image/Compress.hpp>

namespace Assisi::Core
{
class AssetDatabase;
}

namespace Assisi::Cook
{

class TextureRoles;

/// @brief A cook that did not happen, and which file it was about.
///
/// The path travels with the reason because a build log is the only thing anyone
/// sees: "a mesh failed to import" names nothing a person can open.
struct CookError
{
    std::string vpath;
    std::string reason;
};

/// @brief Everything a cook of one asset needs that is not the asset.
///
/// A struct rather than parameters because every cooker takes all of it and each
/// member is used by only one or two cookers — passing them positionally through
/// the ones that ignore them is how a later argument ends up in the wrong slot.
struct CookContext
{
    /// Resolves ids to paths and paths to ids, and answers what a mesh's sidecar
    /// says about its material slots.
    const Core::AssetDatabase *database = nullptr;

    /// Which material channel each texture id is bound to, gathered from every
    /// `.amat` before any texture is cooked. A texture's format follows from its
    /// role, and a file alone does not say what its role is.
    const TextureRoles *roles = nullptr;

    /// How hard texture compression searches. Best for anything that ships; Fast
    /// for a cook somebody is waiting on, whose output must never be packaged.
    Image::CompressQuality textureQuality = Image::CompressQuality::Best;
};

/// @brief One asset type's cooker.
class Cooker
{
public:
    virtual ~Cooker() = default;

    /// @brief A short name, for the line a failure prints.
    [[nodiscard]] virtual std::string_view Name() const = 0;

    /// @brief The kind this cooker cooks, which every blob it writes carries in
    ///        its header. A file whose sidecar names this kind comes here.
    [[nodiscard]] virtual Core::AssetKindId Kind() const = 0;

    /// @brief What besides the source changes this cooker's output, folded into
    ///        the cache key.
    ///
    /// Zero for a cooker whose bytes depend on the source alone. Without it, a
    /// setting that changes the output but touches no file would leave a blob
    /// cooked under the old setting marked current.
    [[nodiscard]] virtual std::uint64_t KeyVariant(const CookContext & /*context*/) const { return 0; }

    /// @brief Other files the cooked bytes depend on, as virtual paths.
    ///
    /// Folded into the cache key, so editing a `.gltf`'s external `.bin` re-cooks
    /// the mesh. A dependency that cannot be resolved is left out rather than
    /// reported: the cook itself will fail on it with a better message than a
    /// hash mismatch could give.
    [[nodiscard]] virtual std::vector<std::string> Dependencies(std::string_view /*vpath*/) const { return {}; }

    /// @brief Whether a file this kind consumes as part of another asset is
    ///        accounted for elsewhere in the tree.
    ///
    /// A consumed file's bytes reach the cooked tree through something else, so
    /// the cook has to check that the something else is there. A shader source
    /// whose `.spv` is missing is the case: the build compiles one per source,
    /// and an absent one means that compile did not happen. Without this the
    /// cook would skip the source, skip the output that is not there, and
    /// produce a tree quietly missing a stage.
    [[nodiscard]] virtual std::expected<void, CookError> CheckSource(std::string_view /*vpath*/) const
    {
        return {};
    }

    /// @brief The id a cooked blob is named by, when the tree has none to give.
    ///
    /// Almost every asset's id is the GUID in its `.aast`, committed and so the
    /// same on every machine. Shaders are the exception in both halves: a `.spv`
    /// is a build output, and `.gitignore` excludes it *and* its sidecar — so on
    /// a fresh clone neither exists, and once built, each machine mints a
    /// different GUID for the same shader.
    ///
    /// A cooker that returns a real id here is saying its assets are named by
    /// something derivable instead. The default is nil, which means "this one
    /// needs its sidecar".
    [[nodiscard]] virtual Core::AssetId DerivedId(std::string_view /*vpath*/) const { return {}; }

    /// @brief The cooked bytes for @p vpath.
    ///
    /// Called only for a file of this cooker's kind. @p id is the
    /// asset's GUID, which a cooker needs when the blob has to name itself or
    /// its siblings.
    [[nodiscard]] virtual std::expected<std::vector<std::byte>, CookError>
    Cook(std::string_view vpath, Core::AssetId id, const CookContext &context) const = 0;
};

/// @brief Which material channel a texture is bound to, and which material said
///        so.
///
/// Gathered from every `.amat` in the tree before a single texture is cooked,
/// because a texture's compressed format follows from the channel that binds it
/// and the image file alone does not say.
class TextureRoles
{
public:
    /// @brief Record that @p material binds @p texture to @p channel.
    ///
    /// @return false if @p texture is already bound to a different channel — one
    ///         GUID names one blob, so a texture wanted as two formats has to be
    ///         two files. The names of both materials are kept for the message.
    bool Bind(Core::AssetId texture, Geometry::MaterialChannel channel, std::string_view material);

    /// @brief The channel @p texture is bound to, or nullopt when no material
    ///        references it.
    [[nodiscard]] std::optional<Geometry::MaterialChannel> ChannelOf(Core::AssetId texture) const;

    /// @brief The two materials that disagreed about @p texture, for the failure
    ///        message. Empty when they did not.
    [[nodiscard]] std::pair<std::string, std::string> Conflict(Core::AssetId texture) const;

    /// @brief A material that binds @p texture, or empty when none does.
    [[nodiscard]] std::string BoundBy(Core::AssetId texture) const;

  private:
    struct Binding
    {
        std::string material;
        std::string conflictingMaterial;
        Geometry::MaterialChannel channel = Geometry::MaterialChannel::BaseColor;
    };

    std::unordered_map<Core::AssetId, Binding> _bindings;
};

/// @brief Every cooker: one per kind, the engine's own and every one a module
///        registered. No two share a kind.
[[nodiscard]] std::vector<std::unique_ptr<Cooker>> MakeCookers();

} // namespace Assisi::Cook
