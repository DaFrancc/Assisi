/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AnimatorGraph.hpp
/// @brief An animation state machine as the cook makes it from a `.sgl` file:
///        one Sigil graph per layer, and beside each state and transition what
///        it plays and how it fades.
///
/// Everything an Animator works out each frame is code in `code`, run against
/// a block laid out by `layout`; everything known when the file cooked is a
/// plain value. A cooked file is checked when it loads, so a damaged one is
/// refused there rather than read past its end while the game runs.

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Core/Errors.hpp>
#include <Assisi/Core/InternedString.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Sigil/Bytecode.hpp>
#include <Assisi/Sigil/Graph.hpp>
#include <Assisi/Sigil/Layout.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Runtime
{

/// @brief What a `.sgl` file's sidecar names as its kind.
inline constexpr std::string_view kAnimatorGraphKindName = "animator";

inline constexpr Core::AssetKindId kAnimatorGraphKind{kAnimatorGraphKindName};

/// @brief Marks a value the file didn't write: no code to run, no param, no state.
inline constexpr uint32_t kNotWritten = std::numeric_limits<uint32_t>::max();

/// @brief What a state plays: a clip or blend space named in the file, or the
///        one a `clip` param is bound to on the Animator.
struct AnimatorClip
{
    Core::AssetId asset;           ///< Nil when `param` names it instead.
    uint32_t param = kNotWritten;  ///< Into AnimatorGraph::clipParams.
};

/// @brief What a state plays and how. One per node of its layer's graph; the
///        root and states holding states play nothing, and leave theirs as is.
///
/// `rate`, `x` and `y` are where their code starts, or kNotWritten for the
/// player's defaults: rate 1, and the blend space's origin.
struct AnimatorState
{
    AnimatorClip clip;
    uint32_t rate = kNotWritten;
    uint32_t x = kNotWritten;
    uint32_t y = kNotWritten;
    /// The state entered when the clip has played once, as an index among
    /// this state's siblings; kNotWritten for a clip that loops.
    uint32_t then = kNotWritten;
    bool pose = false; ///< Holds the clip's first frame.
};

/// @brief How a transition fades. One per transition of its layer's graph.
struct AnimatorTransition
{
    uint32_t fade = kNotWritten; ///< Its code, or kNotWritten for the layer's fade.
    bool interrupt = false;      ///< Whether it may fire while a fade runs.
};

/// @brief One layer: the base when it is the first, otherwise one of the
///        player's layers over it.
struct AnimatorLayer
{
    Sigil::Graph graph;
    std::string name;
    std::vector<AnimatorState> states;           ///< Parallel to graph.nodes.
    std::vector<AnimatorTransition> transitions; ///< Parallel to graph.transitions.
    std::vector<Core::InternedString> exclusions;
    Core::InternedString maskRoot;
    uint32_t weight = kNotWritten; ///< Its code, or kNotWritten for 1.
    uint32_t fade = kNotWritten;   ///< Its code, or kNotWritten for kDefaultAnimatorFade.
    LayerMode mode = LayerMode::Override;
};

/// @brief Seconds a transition fades over when neither it nor its layer says.
///        The same as an AnimationPlayer's own default.
inline constexpr float kDefaultAnimatorFade = 0.15f;

struct AnimatorGraph
{
    Sigil::Layout layout;
    std::vector<Sigil::Word> code;
    std::vector<uint32_t> letEntries;   ///< Where each let's code starts, in order.
    std::vector<uint32_t> triggerSlots; ///< Cleared at the end of every frame.
    std::vector<std::string> clipParams; ///< The names of the `clip` params, in order.
    std::vector<AnimatorLayer> layers;   ///< Empty for a library, which plays nothing.
    Core::AssetId skeleton;              ///< The model the file's joints were checked against.
    uint32_t progressSlot = kNotWritten; ///< Where `progress()` is written before each layer steps.
};

/// @brief The payload a cooked `.sgl` carries.
[[nodiscard]] std::vector<std::byte> WriteAnimatorGraph(const AnimatorGraph &graph);

/// @brief The graph @p payload holds, if every part of it is safe to run: each
///        expression and graph passes its check, every index lands inside what
///        it indexes, and the first layer has no mask. A library's payload,
///        with no layers, is refused too, since it plays nothing.
[[nodiscard]] std::expected<AnimatorGraph, Core::AssetError> ReadAnimatorGraph(std::span<const std::byte> payload);

} // namespace Assisi::Runtime
