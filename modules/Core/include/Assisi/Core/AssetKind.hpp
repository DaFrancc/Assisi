/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AssetKind.hpp
/// @brief Kinds of asset a module adds without editing the engine: what a kind
///        loads into, which files are its, and how it is cooked.
///
/// A kind is what the data represents — a sound — and each of its file
/// extensions is one format of it: `.wav`, `.flac`, `.ogg`. A module registers
/// its kind once, and from then on the cook claims its files, a package carries
/// them, and AssetStore loads them by id, with nothing in Core, the cook or an
/// app naming the kind.
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

namespace Assisi::Core
{

/// @brief Bytes to a loaded value, run on a worker. The error is the reason, for
///        the warning a failed load logs once.
using AssetLoadFunction =
    std::function<std::expected<std::shared_ptr<void>, std::string>(std::span<const std::byte> payload)>;

/// @brief Main-thread work a loaded value needs before anything may use it, such
///        as creating GPU resources. Runs once, after the load and before the
///        value is shared.
using AssetFinishFunction = std::function<std::expected<void, std::string>(void *value)>;

/// @brief Source file bytes to the payload a package carries, run by the cook
///        and, in the editor, on every load.
using AssetCookFunction =
    std::function<std::expected<std::vector<std::byte>, std::string>(std::span<const std::byte> source)>;

/// @brief One registered kind.
struct AssetKind
{
    std::string name;

    AssetLoadFunction load;

    /// Empty when the loaded value is ready as it is.
    AssetFinishFunction finish;

    /// Source file extensions, dot included, matched case-sensitively as the
    /// engine's own cookers match theirs.
    std::vector<std::string> extensions;

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

/// @brief Every registered kind and cook step.
class AssetKindRegistry
{
  public:
    static AssetKindRegistry &Instance();

    /// @brief Add @p kind. Refused, with an error logged, when its name or one of
    ///        its extensions is already registered, or it names a built-in kind.
    bool Register(AssetKind kind);

    /// @brief Add @p step. Refused, with an error logged, when its kind already
    ///        has one.
    bool RegisterCookStep(AssetCookStep step);

    [[nodiscard]] const AssetKind *Find(AssetKindId id) const;

    /// @brief The kind whose extensions @p vpath ends with, or null.
    [[nodiscard]] const AssetKind *ForPath(std::string_view vpath) const;

    [[nodiscard]] const AssetCookStep *CookStepFor(AssetKindId id) const;

    /// @brief Every kind, in registration order. A deque, so a pointer to one
    ///        stays valid as later ones register.
    [[nodiscard]] const std::deque<AssetKind> &All() const { return _kinds; }

  private:
    AssetKindRegistry() = default;

    std::deque<AssetKind> _kinds;
    std::deque<AssetCookStep> _cookSteps;
};

/// @brief A kind whose load returns a @p T, ready to register.
///
/// Takes the load typed and stores it erased, so the value type recorded for the
/// kind is always the one its load produces.
template <typename T>
[[nodiscard]] AssetKind MakeAssetKind(std::string name, std::vector<std::string> extensions,
                                      std::function<std::expected<T, std::string>(std::span<const std::byte>)> load)
{
    AssetKind kind;
    kind.id = AssetKindId{name};
    kind.name = std::move(name);
    kind.extensions = std::move(extensions);
    kind.valueType = typeid(T);
    kind.load = [typed = std::move(load)](
                    std::span<const std::byte> payload) -> std::expected<std::shared_ptr<void>, std::string>
    {
        std::expected<T, std::string> value = typed(payload);
        if (!value)
        {
            return std::unexpected(std::move(value.error()));
        }
        return std::make_shared<T>(std::move(*value));
    };
    return kind;
}

/// @brief The cooked blob for a file of @p kind: the envelope, then the kind's
///        cook step applied to @p source, or @p source unchanged when the kind
///        has no step.
[[nodiscard]] std::expected<std::vector<std::byte>, std::string> CookAssetBytes(const AssetKind &kind,
                                                                                std::span<const std::byte> source);

} // namespace Assisi::Core
