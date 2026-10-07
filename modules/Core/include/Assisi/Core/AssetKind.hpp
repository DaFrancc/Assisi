/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AssetKind.hpp
/// @brief Kinds of asset, and the ones a module adds without editing the engine:
///        what a kind loads into, which formats it reads, and how it is cooked.
///
/// A kind is what the data represents — a sound — and each of its file
/// extensions is one format of it: `.wav`, `.flac`, `.ogg`. One format can be
/// several kinds (a `.png` can be a texture or a heightmap); a file's sidecar
/// says which it is. A module registers its kind once, and from then on the
/// cook cooks its files, a package carries them, and AssetStore loads them by
/// id, with nothing in Core, the cook or an app naming the kind.
///
/// Registration has two halves, because a shipped game must not link cook code:
/// the kind itself, which every executable that loads it needs, and its cook
/// step, which only the cook tool and the editor link. Both register from static
/// initialisers in translation units built as OBJECT libraries (see
/// assisi_asset_kind in cmake/AssisiReflect.cmake), since a static library would
/// let the linker drop them as unreferenced.
///
/// The registry is filled before main() and only read afterwards, so workers
/// read it without a lock.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Core/Errors.hpp>

namespace Assisi::Core
{

/// @brief Bytes to a loaded value, run on a worker. Built by MakeAssetKind.
///
/// A kind reports what went wrong as an AssetError, whatever its own decoder
/// calls it: the file's contents are corrupt, or in a form this build cannot
/// read. The decoder's own description of the problem goes in the detail.
using AssetLoadFunction =
    std::function<std::expected<std::shared_ptr<void>, AssetError>(std::span<const std::byte> payload)>;

/// @brief Main-thread work a loaded value needs before anything may use it, such
///        as creating GPU resources. Runs once, after the load and before the
///        value is shared. Built by MakeAssetFinish.
using AssetFinishFunction = std::function<std::expected<void, AssetError>(void *value)>;

/// @brief Source file bytes to the payload a package carries, run by the cook
///        and, in the editor, on every load.
using AssetCookFunction =
    std::function<std::expected<std::vector<std::byte>, AssetError>(std::span<const std::byte> source)>;

/// @brief What a kind does with files of one format.
enum class FormatRole : std::uint8_t
{
    /// A file of this format can be an asset of the kind, when its sidecar says so.
    Reads,

    /// A file of this format is part of an asset of the kind and never an asset
    /// of its own: a glTF's `.bin`, a font's `.ttf`, a shader's GLSL source.
    Consumes,

    /// A build output with no sidecar of its own, named by its path instead of an
    /// id. Only the engine's compiled shaders; a module cannot declare one.
    Generated,

    Count_,
};

/// @brief One format a kind handles.
struct AssetFormat
{
    /// Dot included, matched case-sensitively: `.WAV` is not `.wav`.
    std::string extension;

    FormatRole role = FormatRole::Reads;

    /// Whether a new file of this format is given this kind when several kinds
    /// read the format. The choice is written into the file's sidecar, so
    /// changing it later changes only files imported afterwards.
    bool preferred = false;
};

/// @brief One kind: what its files are, and how they load.
struct AssetKind
{
    std::string name;

    /// Empty for the engine's own kinds, which have loaders of their own.
    AssetLoadFunction load;

    /// Empty when the loaded value is ready as it is.
    AssetFinishFunction finish;

    std::vector<AssetFormat> formats;

    std::type_index valueType = typeid(void);
    AssetKindId id;
};

/// @brief How a kind's source files are cooked.
struct AssetCookStep
{
    AssetCookFunction cook;
    AssetKindId kind;

    /// Folded into the cook's cache key: bump it when the step's output changes
    /// for the same source.
    std::uint32_t version = 0;
};

/// @brief Every kind, the engine's own included, and every cook step.
///
/// Which kind a file is comes from its sidecar, never from its extension; the
/// formats here say which kinds a file *can* be, which new files are given, and
/// which files are parts of other assets.
class AssetKindRegistry
{
  public:
    static AssetKindRegistry &Instance();

    /// @brief Add @p kind. Refused, with an error logged, when its name is taken,
    ///        it has no load function, or it declares a Generated format.
    bool Register(AssetKind kind);

    /// @brief Add @p step. Refused, with an error logged, when its kind already
    ///        has one.
    bool RegisterCookStep(AssetCookStep step);

    [[nodiscard]] const AssetKind *Find(AssetKindId id) const;

    /// @brief The kind called @p name, or null.
    [[nodiscard]] const AssetKind *FindByName(std::string_view name) const;

    /// @brief Whether @p kind can be an asset in the format of @p extension.
    [[nodiscard]] bool Reads(AssetKindId kind, std::string_view extension) const;

    /// @brief Every kind that reads @p extension, ordered by name.
    [[nodiscard]] std::vector<const AssetKind *> KindsReading(std::string_view extension) const;

    /// @brief The kind a new file of @p extension is given: the one that prefers
    ///        the format, or the first by name when none or several do. Null when
    ///        no kind reads it.
    [[nodiscard]] const AssetKind *KindForNewFile(std::string_view extension) const;

    /// @brief The kind that consumes @p extension as part of its assets, or null.
    [[nodiscard]] const AssetKind *ConsumerOf(std::string_view extension) const;

    /// @brief The kind whose build outputs have @p extension, or null.
    [[nodiscard]] const AssetKind *GeneratorOf(std::string_view extension) const;

    [[nodiscard]] const AssetCookStep *CookStepFor(AssetKindId id) const;

    /// @brief Every kind, in registration order, the engine's own first. A deque,
    ///        so a pointer to one stays valid as later ones register.
    [[nodiscard]] const std::deque<AssetKind> &All() const { return _kinds; }

  private:
    /// Declares the engine's own kinds, so they are there before any module's.
    AssetKindRegistry();

    [[nodiscard]] const AssetKind *WithRole(std::string_view extension, FormatRole role) const;

    std::deque<AssetKind> _kinds;
    std::deque<AssetCookStep> _cookSteps;
};

/// @brief The extension of the file at @p vpath, dot included, or empty. Only
///        the last one: `mesh.vert.spv` is a `.spv`.
[[nodiscard]] std::string_view ExtensionOf(std::string_view vpath);

/// @brief A kind whose load returns a @p T, ready to register.
///
/// Takes the load typed and stores it erased, so the value type recorded for the
/// kind is always the one its load produces.
template <typename T>
[[nodiscard]] AssetKind MakeAssetKind(std::string name, std::vector<AssetFormat> formats,
                                      std::function<std::expected<T, AssetError>(std::span<const std::byte>)> load)
{
    AssetKind kind;
    kind.id = AssetKindId{name};
    kind.name = std::move(name);
    kind.formats = std::move(formats);
    kind.valueType = typeid(T);
    kind.load = [typed = std::move(load)](
                    std::span<const std::byte> payload) -> std::expected<std::shared_ptr<void>, AssetError>
    {
        std::expected<T, AssetError> value = typed(payload);
        if (!value)
        {
            return std::unexpected(value.error());
        }
        return std::make_shared<T>(std::move(*value));
    };
    return kind;
}

/// @brief A kind's finish, for a kind whose load returns a @p T.
template <typename T>
[[nodiscard]] AssetFinishFunction MakeAssetFinish(std::function<std::expected<void, AssetError>(T &)> finish)
{
    return [typed = std::move(finish)](void *value) { return typed(*static_cast<T *>(value)); };
}

/// @brief The cook step for @p kind, ready to register. Raise @p version whenever
///        the step would cook the same source differently.
[[nodiscard]] inline AssetCookStep MakeAssetCookStep(AssetKindId kind, std::uint32_t version, AssetCookFunction cook)
{
    AssetCookStep step;
    step.cook = std::move(cook);
    step.kind = kind;
    step.version = version;
    return step;
}

/// @brief The cooked blob for a file of @p kind: the envelope, then the kind's
///        cook step applied to @p source, or @p source unchanged when the kind
///        has no step.
[[nodiscard]] std::expected<std::vector<std::byte>, AssetError> CookAssetBytes(const AssetKind &kind,
                                                                               std::span<const std::byte> source);

} // namespace Assisi::Core
