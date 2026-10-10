/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AnimatorCompiler.hpp
/// @brief The animation vocabulary, and a `.sgl` file compiled into the
///        AnimatorGraph the game runs.
///
/// Cook and editor only, so a shipped game never links the Sigil compiler or
/// the glTF reader the skeleton check uses.

#include <Assisi/Core/AssetKind.hpp>
#include <Assisi/Runtime/AnimatorGraph.hpp>
#include <Assisi/Sigil/Compile/Vocabulary.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace Assisi::Runtime::Import
{

/// @brief The animation vocabulary's clauses, in the order it lists them, so a
///        checked clause's spec index is one of these.
enum class AnimationClause : uint8_t
{
    Skeleton,  ///< `skeleton "model.glb";` at the top of the file.
    Mask,      ///< `mask "joint";` in a layer.
    Exclude,   ///< `exclude "joint";` in a layer, any number of times.
    Additive,  ///< `additive;` in a layer.
    Weight,    ///< `weight <float>;` in a layer.
    Fade,      ///< `fade <float>;` in a layer, for its transitions, or on one transition.
    Play,      ///< `play <clip>;` in a state.
    Pose,      ///< `pose <clip>;` in a state.
    X,         ///< `x <float>;` in a state: where in a blend space it plays.
    Y,         ///< `y <float>;` in a state.
    Rate,      ///< `rate <float>;` in a state: how fast it plays. Not `speed`, which a file wants for a param.
    Then,      ///< `then <state>;` in a state: play the clip once, then go there.
    Interrupt, ///< `interrupt;` on a transition: it may fire during a fade.
    Count_,
};

/// @brief The animation vocabulary's types, in the order it lists them.
enum class AnimationType : uint8_t
{
    Clip,  ///< A clip or blend space file.
    Joint, ///< A joint of the file's skeleton.
    Model, ///< A model with a skeleton.
    Count_,
};

/// @brief Checks the names a file writes against the asset tree: that a clip is
///        a clip or blend space, a model has a skeleton, and a joint is in it.
///        Each gives the reason a name is refused, which the compile reports
///        where the name is written.
class AnimationAssets
{
  public:
    virtual ~AnimationAssets() = default;

    [[nodiscard]] virtual std::expected<void, std::string> CheckClip(std::string_view vpath) const = 0;
    [[nodiscard]] virtual std::expected<void, std::string> CheckModel(std::string_view vpath) const = 0;
    [[nodiscard]] virtual std::expected<void, std::string> CheckJoint(std::string_view name) const = 0;
};

/// @brief The animation vocabulary, checking names against @p assets, or only
///        that they aren't empty when it is null, as sglc does without a tree.
///        @p assets must outlive every compile against it.
[[nodiscard]] Sigil::Compile::Vocabulary AnimationVocabulary(const AnimationAssets *assets);

/// @brief @p source, the `.sgl` file @p context is cooking, as the graph the
///        game runs; a library compiles to one with no layers. On failure,
///        every diagnostic, formatted for a person to read.
[[nodiscard]] std::expected<AnimatorGraph, std::string> CompileAnimator(std::string_view source,
                                                                       const Core::AssetCookContext &context);

} // namespace Assisi::Runtime::Import
