/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AnimatorParams.hpp
/// @brief Handles game code feeds an animator's params through: one per
///        param, naming it and its type, which finds where the param lives in
///        each file once.
///
/// A handle is usually a `static` in the system that sets the param, before
/// the Animators system runs. It looks its name up the first time it meets a
/// file, and again when the file changes while the game runs, so every other
/// call writes straight to the param. A name the file doesn't have, or a param
/// of another type, is reported once per file, naming the entity and the
/// param, and the call changes nothing.
///
/// A handle remembers what it found, so two systems that may run at the same
/// time must not share one.

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/Runtime/AnimatorGraph.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Sigil/Layout.hpp>

#include <concepts>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Assisi::Runtime
{

/// @brief What an AnimatorParam holds: a `float`, an `int` (an enum param by
///        its value's position) or a `bool`.
template <typename T>
concept AnimatorValue = std::same_as<T, float> || std::same_as<T, int32_t> || std::same_as<T, bool>;

/// @brief Where one param lives in the file a run is bound to, looked up by
///        name once per file. What AnimatorParam and AnimatorTrigger share.
class AnimatorParamSlot
{
public:
    /// @brief The param called @p name, of @p type. @p trigger: whether the
    ///        param is a trigger, which a `bool` param is not; both are bools
    ///        in the block.
    AnimatorParamSlot(std::string name, Sigil::SlotType type, bool trigger);

    /// @brief The param's slot in @p run's file; nothing when it has none, or
    ///        isn't loaded yet. A miss is reported, once per file, naming
    ///        @p entity.
    [[nodiscard]] std::optional<uint32_t> Find(ECS::Entity entity, const AnimatorRun &run);

    /// @brief Find, reporting nothing, for reading a value back.
    [[nodiscard]] std::optional<uint32_t> FindQuietly(const AnimatorRun &run);

    /// @brief How many times the name was looked up: once for each file met,
    ///        and again each time the file changes from the one before.
    [[nodiscard]] uint32_t Lookups() const { return _lookups; }

private:
    /// Looks the name up in @p graph, when it is not the file looked in last.
    void Resolve(const std::shared_ptr<const AnimatorGraph> &graph);

    /// Says once for @p graph why the param isn't there.
    void Report(ECS::Entity entity, const std::shared_ptr<const AnimatorGraph> &graph);

    std::string _name;
    /// The files a miss was reported for, so each is reported once.
    std::vector<std::weak_ptr<const AnimatorGraph>> _reported;
    /// The file looked in last. Weak, and compared by owner: a file freed and
    /// another made where it was is never taken for it.
    std::weak_ptr<const AnimatorGraph> _graph;
    std::optional<uint32_t> _slot;
    Sigil::ParamMiss _miss = Sigil::ParamMiss::Missing;
    uint32_t _lookups = 0;
    Sigil::SlotType _type;
    bool _trigger;
};

/// @brief A `float`, `int` or `bool` param of an animator's file.
///
/// ```cpp
/// static Runtime::AnimatorParam<float> speed{"speed"};
/// speed.SetDamped(entity, run, state.groundSpeed, 0.1f, ctx.dt);
/// ```
template <AnimatorValue T> class AnimatorParam
{
public:
    /// @brief A handle for the param called @p name, as the file declares it.
    ///        Looks nothing up until it is first used.
    explicit AnimatorParam(std::string name);

    /// @brief Sets the param in @p run, the AnimatorRun of @p entity. False,
    ///        changing nothing, when the file isn't loaded or has no such
    ///        param; the second is reported once.
    bool Set(ECS::Entity entity, AnimatorRun &run, T value);

    /// A value of another type doesn't convert: `true` given to a `float`
    /// param is a mistake the build reports.
    template <typename Other> bool Set(ECS::Entity entity, AnimatorRun &run, Other value) = delete;

    /// @brief Moves a float param towards @p target, closing about two thirds
    ///        of the way every @p seconds, and the same share of it in the
    ///        same time at any frame rate. @p dt is the frame's length. Zero
    ///        @p seconds goes straight there. False as Set.
    bool SetDamped(ECS::Entity entity, AnimatorRun &run, float target, float seconds, float dt)
    requires std::same_as<T, float>;

    /// @brief The param's value in @p run; nothing when the file isn't loaded
    ///        or has no such param.
    [[nodiscard]] std::optional<T> Get(const AnimatorRun &run);

    /// @brief See AnimatorParamSlot::Lookups.
    [[nodiscard]] uint32_t Lookups() const { return _slot.Lookups(); }

private:
    AnimatorParamSlot _slot;
};

/// @brief A `trigger` param of an animator's file: set for one frame, and used
///        up by the transition that reads it.
class AnimatorTrigger
{
public:
    /// @brief A handle for the trigger called @p name, as the file declares
    ///        it. Looks nothing up until it is first used.
    explicit AnimatorTrigger(std::string name);

    /// @brief Sets the trigger in @p run, the AnimatorRun of @p entity, for
    ///        this frame. False as AnimatorParam::Set.
    bool Fire(ECS::Entity entity, AnimatorRun &run);

    /// @brief Fires the trigger when @p count differs from the one @p run
    ///        last gave it, such as CharacterState::jumps: once for each
    ///        change, at any frame rate, wrapping included. The first count is
    ///        only remembered, so a character appearing fires nothing. A file
    ///        changed while the game runs keeps the count. True when it fired.
    bool FireOnChange(ECS::Entity entity, AnimatorRun &run, uint32_t count);

    /// @brief See AnimatorParamSlot::Lookups.
    [[nodiscard]] uint32_t Lookups() const { return _slot.Lookups(); }

private:
    AnimatorParamSlot _slot;
};

extern template class AnimatorParam<float>;
extern template class AnimatorParam<int32_t>;
extern template class AnimatorParam<bool>;

} // namespace Assisi::Runtime
