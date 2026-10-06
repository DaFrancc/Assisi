/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Transform.hpp
/// @brief Local-space TRS component, and the render-blend lanes its pool
///        keeps beside it.
///
/// Transform is the engine's most foundational component: rendering, physics,
/// and the scene-graph hierarchy all read and write it. It lives here, in the
/// ECS layer, precisely so it stays free of any renderer dependency — lower
/// modules (e.g. Physics) can use it without pulling in nvrhi/GPU headers.
/// Runtime re-exports it from Components.hpp, so `Runtime::Transform` names this
/// exact type.

#include <Assisi/Prelude.hpp>
#include <Assisi/ECS/SparseSet.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Math/GLM.hpp>

#include <cstdint>
#include <vector>

namespace Assisi::ECS
{

/// @brief Local-space TRS: the simulation pose.
///
/// Write to position/rotation/scale to move an entity — through Scene::GetMut (or
/// Scene::MarkChanged for a by-offset writer), since Transform is ACOMP(tracked):
/// PropagateTransforms uses that change signal to skip entities whose local TRS
/// and ancestors are unchanged.
///
/// What is drawn is not always this. A Transform written during a fixed step
/// (inside an ECS::FixedStepScope) is drawn blended between its pose before
/// the step and this one, by the frame's blend alpha, into its WorldMatrix.
///
/// `replicable` as well: pose is the one thing every mirrored entity needs.
/// `tracked` is spelled out beside it rather than left to `replicable`'s
/// implication, so that dropping `replicable` would not silently take
/// PropagateTransforms's change signal with it.
///
/// Requires WorldMatrix, which PropagateTransforms writes and renderers read.
ACOMP(replicable, tracked, requires = {WorldMatrix})
struct Transform
{
    AFIELD() glm::vec3 position{0.f, 0.f, 0.f};
    AFIELD() glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    AFIELD() glm::vec3 scale{1.f, 1.f, 1.f};
};

struct Scene;
class FixedStepScope;
struct Propagation;

/// @brief The Transform pool's lanes: what the render blend needs to draw an
/// entity between two fixed steps.
///
/// The first write to a Transform in a fixed step records the pose before it
/// (`prev`); the first write after the step ends records the pose the step left
/// (`end`). The drawn pose is the step's motion, `blend(prev, end, alpha)`, with
/// whatever was written since, `current - end`, added on top: a per-frame write
/// such as mouse look shows at once while motion from fixed steps is smoothed.
///
/// Fed from the pool's stamp, which runs before the caller writes for GetMut and
/// a Mut query element, so it sees the old value. Add and MarkChanged stamp after the value
/// is in place, so a spawn or a by-offset write records its new pose and is
/// drawn there with no slide.
template <> struct SparseSetLanes<Transform>
{
public:
    // Pool hooks; see SparseSetLanes.
    void Push() { _marks.push_back(BlendMark{}); }
    void Move(uint32_t to, uint32_t from) { _marks[to] = _marks[from]; }
    void Pop() { _marks.pop_back(); }
    void Clear();

    /// Records @p current, the Transform at dense slot @p slot owned by
    /// @p entity, if this write is its first in the current fixed step or the
    /// first since that step ended.
    void OnStamp(uint32_t slot, Entity entity, const Transform &current)
    {
        BlendMark &mark = _marks[slot];
        if (_inFixedStep)
        {
            if (mark.step != _step)
            {
                RecordPrevious(mark, entity, current);
            }
            return;
        }
        if (mark.step == _step && !_current[mark.entry].hasEnd)
        {
            RecordEnd(_current[mark.entry], current);
        }
    }

private:
    friend class FixedStepScope;
    friend struct Propagation;
    friend void SetBlendAlpha(Scene &scene, float alpha);
    friend void SettleTransforms(Scene &scene);
    friend void SnapTransform(Scene &scene, Entity entity);
    friend uint32_t LastPropagationResolved(const Scene &scene);
    friend uint64_t PropagateTransforms(Scene &scene, uint64_t lastTick);

    /// Where a dense slot's blend entry is, if it has one this step.
    struct BlendMark
    {
        uint32_t step = 0;  ///< The fixed step the entry was made in; 0 for none.
        uint32_t entry = 0; ///< Index into _current.
    };

    /// One entity's poses for the current step.
    struct BlendEntry
    {
        glm::quat prevRotation{1.f, 0.f, 0.f, 0.f};
        glm::quat endRotation{1.f, 0.f, 0.f, 0.f};
        glm::vec3 prevPosition{0.f};
        glm::vec3 prevScale{1.f};
        glm::vec3 endPosition{0.f};
        glm::vec3 endScale{1.f};
        Entity entity{NullEntity};
        bool hasEnd = false;
    };

    void RecordPrevious(BlendMark &mark, Entity entity, const Transform &current);
    static void RecordEnd(BlendEntry &entry, const Transform &current);

    /// Moves this step's movers to the list the next propagation lands
    /// exactly, and starts step @p next.
    void Advance();

    void BeginFixedStep();
    void EndFixedStep();

    std::vector<BlendMark> _marks; ///< Per dense slot.

    /// Entries made in the current step, in the order they were made.
    std::vector<BlendEntry> _current;

    /// Entities whose blend ended since the last propagation, which must be
    /// drawn once more to land exactly on their pose.
    std::vector<Entity> _ended;

    float _alpha = 1.f;

    /// The alpha and step the last propagation drew the movers at. A pass that
    /// would draw them the same again skips them.
    float _drawnAlpha = -1.f;
    uint32_t _drawnStep = 0;

    /// Counts fixed steps and settles. Never 0, so a mark of 0 means no entry.
    uint32_t _step = 1;

    uint32_t _lastResolved = 0;

    /// Set when _ended outgrew the pool, so the next propagation redraws every
    /// entity instead. A world that steps but is never drawn would otherwise
    /// grow the list forever.
    bool _redrawAll = false;

    bool _inFixedStep = false;
};

} // namespace Assisi::ECS
