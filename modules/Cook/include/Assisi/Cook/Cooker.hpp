/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Cooker.hpp
/// @brief One asset in, one cooked blob out, and the interface every type's
///        cooker answers.
///
/// A cooker is asked three things about a virtual path: whether it claims it,
/// what else the answer depends on, and what the bytes are. Splitting the first
/// two out of the third is what makes the cook incremental and what makes an
/// unclaimed file a build failure rather than a file quietly left behind.
///
/// **Claiming is not the same as producing.** A `.vert` is claimed — it is
/// content, and a cook that did not recognise it would fail — but it produces
/// nothing, because the renderer loads the `.spv` the build compiled from it and
/// GLSL never leaves the source tree. That is a third answer, not a missing one.

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

namespace Assisi::Core
{
class AssetDatabase;
}

namespace Assisi::Cook
{

class TextureRoles;

/// @brief What a cooker does with a path.
enum class Claim : std::uint8_t
{
    /// Not this cooker's. The tree walk tries the next one, and a path no cooker
    /// claims fails the cook.
    None,

    /// Claimed, and produces a cooked blob.
    Output,

    /// Claimed, and deliberately produces nothing — an import source or a shader
    /// source, which is content in the tree and absent from a shipped build.
    SourceOnly,
};

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
/// A struct rather than four parameters because every cooker takes all of it and
/// two of the four are only used by two cookers — passing them positionally
/// through the ones that ignore them is how a later argument ends up in the
/// wrong slot.
struct CookContext
{
    /// Resolves ids to paths and paths to ids, and answers what a mesh's sidecar
    /// says about its material slots.
    const Core::AssetDatabase *database = nullptr;

    /// Which material channel each texture id is bound to, gathered from every
    /// `.amat` before any texture is cooked. A texture's format follows from its
    /// role, and a file alone does not say what its role is.
    const TextureRoles *roles = nullptr;
};

/// @brief One asset type's cooker.
class Cooker
{
public:
    virtual ~Cooker() = default;

    /// @brief A short name, for the line a failure prints.
    [[nodiscard]] virtual std::string_view Name() const = 0;

    /// @brief Whether @p vpath is this cooker's, and whether it produces bytes.
    [[nodiscard]] virtual Claim Claims(std::string_view vpath) const = 0;

    /// @brief Other files the cooked bytes depend on, as virtual paths.
    ///
    /// Folded into the cache key, so editing a `.gltf`'s external `.bin` re-cooks
    /// the mesh. A dependency that cannot be resolved is left out rather than
    /// reported: the cook itself will fail on it with a better message than a
    /// hash mismatch could give.
    [[nodiscard]] virtual std::vector<std::string> Dependencies(std::string_view /*vpath*/) const { return {}; }

    /// @brief Whether a file this cooker answered SourceOnly for is accounted
    ///        for elsewhere in the tree.
    ///
    /// SourceOnly means "content, and its bytes reach the cooked tree through
    /// something else" — so the cook has to check that the something else is
    /// there. A shader source whose `.spv` is missing is the case: the build
    /// compiles one per source, and an absent one means that compile did not
    /// happen. Without this the cook would skip the source, skip the output that
    /// is not there, and produce a tree quietly missing a stage.
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
    /// Called only for a path this cooker answered Output for. @p id is the
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

private:
    struct Binding
    {
        std::string material;
        std::string conflictingMaterial;
        Geometry::MaterialChannel channel = Geometry::MaterialChannel::BaseColor;
    };

    std::unordered_map<Core::AssetId, Binding> _bindings;
};

/// @brief An id derived from @p vpath alone, for an asset whose sidecar does not
///        ship.
///
/// Two hashes of the path under different salts fill the sixteen bytes. The
/// version and variant nibbles are then set to values RFC 4122 does not use, so
/// one of these can never equal a minted v4 — and the leading bytes are never
/// all zero, so it cannot land in the reserved built-in range either. Both are
/// structural rather than improbable, which is the same guarantee MintAssetId
/// gives from the other direction.
[[nodiscard]] Core::AssetId DerivedAssetId(std::string_view vpath);

/// @brief Every cooker, in the order the tree walk tries them.
///
/// Order matters only where two cookers could claim one path, which is why the
/// verbatim catch-all is last: it claims what nothing else did, and putting it
/// first would swallow every asset in the tree.
[[nodiscard]] std::vector<std::unique_ptr<Cooker>> MakeCookers();

} // namespace Assisi::Cook
