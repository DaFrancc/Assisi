/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AnimatorStep.hpp
/// @brief One frame of an Animator: each layer moves to the state its
///        transitions choose, and the player is told what that state plays.
///        The system that runs it each Update only loads the file.
///
/// Game code sets the file's params through SetAnimator* and FireAnimatorTrigger,
/// in any stage before the Animator's Update.

#include <Assisi/Runtime/AnimatorGraph.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Runtime
{

/// @brief Points @p animator at @p graph, a new file or a new version of the
///        one it runs, and puts @p player on what each layer plays.
///
/// Each layer stays in the states of the same names, as far down as they are
/// still there, and otherwise starts at the first; a param keeps its value
/// when the new file has one of the same name and type. Layers are matched by
/// name too.
void BindAnimator(Animator &animator, AnimationPlayer &player, std::shared_ptr<const AnimatorGraph> graph);

/// @brief Runs one frame of @p animator's file and writes the result into
///        @p player. Does nothing until it is bound.
///
/// For each layer in order: writes `progress()`, works out the lets, and fires
/// the first transition that may, outer states first; one that may not
/// interrupt waits while the layer is fading. With none, a clip played once
/// that has finished moves on to its `then`. Then the state's rate, blend
/// position and the layer's weight are written. Triggers are cleared last.
void StepAnimator(Animator &animator, AnimationPlayer &player);

/// @brief The states @p layer of @p animator is in, outermost first; empty
///        while it is unbound or has no such layer.
[[nodiscard]] std::vector<std::string> AnimatorStateNames(const Animator &animator, uint32_t layer);

/// @brief Sets a param of @p animator's file. False, changing nothing, when it
///        is unbound, has no param @p name, or the param is of another type;
///        an int param takes SetAnimatorInt, a trigger FireAnimatorTrigger.
bool SetAnimatorFloat(Animator &animator, std::string_view name, float value);
bool SetAnimatorInt(Animator &animator, std::string_view name, int32_t value);
bool SetAnimatorBool(Animator &animator, std::string_view name, bool value);

/// @brief Sets trigger @p name for this frame. False when there is none.
bool FireAnimatorTrigger(Animator &animator, std::string_view name);

/// @brief The value of float param @p name, for code that eases it towards a
///        target; nothing when it is unbound or has no such float param.
[[nodiscard]] std::optional<float> AnimatorFloat(const Animator &animator, std::string_view name);

} // namespace Assisi::Runtime
