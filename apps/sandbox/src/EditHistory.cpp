/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "EditHistory.hpp"

#include <utility>

#include <Assisi/Core/Reflect/ComponentMeta.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

namespace Sandbox
{

namespace Reflect = Assisi::Core::Reflect;
namespace Rt      = Assisi::Runtime;
using Assisi::ECS::Entity;

namespace
{
// Visit each command exactly once in apply order — reverse for undo, forward for
// redo. The two-phase entity handling walks the transaction three times, so the
// direction logic is centralised here instead of duplicated at each pass.
template <typename Fn> void ForEachCommand(const Transaction &txn, bool reverse, Fn &&fn)
{
    if (reverse)
    {
        for (auto it = txn.cmds.rbegin(); it != txn.cmds.rend(); ++it)
            fn(*it);
    }
    else
    {
        for (const auto &cmd : txn.cmds)
            fn(cmd);
    }
}

const std::string kEmptyLabel;

// RAII set/reset for the re-entrancy flag, so a throw mid-apply (e.g. a
// malformed payload reaching addToScene) still clears it and the history isn't
// wedged into a permanent "applying" state.
struct ApplyingGuard
{
    bool &flag;
    explicit ApplyingGuard(bool &f) : flag(f) { flag = true; }
    ~ApplyingGuard() { flag = false; }
    ApplyingGuard(const ApplyingGuard &)            = delete;
    ApplyingGuard &operator=(const ApplyingGuard &) = delete;
};
} // namespace

// ---------------------------------------------------------------------------
// Construction / stack management
// ---------------------------------------------------------------------------

EditHistory::EditHistory(Assisi::ECS::Scene &scene, RebindHook rebind)
    : _scene(scene), _rebind(std::move(rebind))
{
    // A missing hook becomes a no-op so the engine never has to null-check it;
    // tests and the pre-wiring stages pass none.
    if (!_rebind)
        _rebind = [](Entity, Reflect::ComponentId, bool) {};
}

void EditHistory::Push(Transaction txn)
{
    if (txn.cmds.empty())
        return; // nothing reversible — a coalesced no-op gesture

    // A fresh edit invalidates the redo future: this is the linear-history
    // invariant that ReviveAt's exact-identity safety rests on (design doc §7.1).
    _redo.clear();
    _undo.push_back(std::move(txn));

    if (_undo.size() > kMaxDepth)
        _undo.erase(_undo.begin()); // drop the oldest; JSON payloads are heavy
}

std::optional<Entity> EditHistory::Undo()
{
    if (_undo.empty())
        return std::nullopt;

    Transaction txn = std::move(_undo.back());
    _undo.pop_back();

    ApplyTransaction(txn, Direction::Undo);

    const Entity selection = txn.selectionBefore;
    _redo.push_back(std::move(txn));
    return selection;
}

std::optional<Entity> EditHistory::Redo()
{
    if (_redo.empty())
        return std::nullopt;

    Transaction txn = std::move(_redo.back());
    _redo.pop_back();

    ApplyTransaction(txn, Direction::Redo);

    const Entity selection = txn.selectionAfter;
    _undo.push_back(std::move(txn));
    return selection;
}

void EditHistory::Clear()
{
    _undo.clear();
    _redo.clear();
}

const std::string &EditHistory::NextUndoLabel() const
{
    return _undo.empty() ? kEmptyLabel : _undo.back().label;
}

const std::string &EditHistory::NextRedoLabel() const
{
    return _redo.empty() ? kEmptyLabel : _redo.back().label;
}

// ---------------------------------------------------------------------------
// Apply engine
// ---------------------------------------------------------------------------

void EditHistory::RestoreComponent(Entity entity, Reflect::ComponentId id,
                                   const std::optional<nlohmann::json> &target)
{
    const Reflect::ComponentMeta *meta = Reflect::ComponentRegistry::Instance().ById(id);

    if (!target.has_value())
    {
        // The component should be absent on this side. Only act (and rebind) if
        // it is actually present, so a no-op restore doesn't tear down state that
        // was never built.
        if (meta && meta->getByEntity && meta->getByEntity(&_scene, entity.index, entity.generation))
        {
            _scene.RemoveById(entity, id);
            _rebind(entity, id, /*present=*/false);
        }
        return;
    }

    // The component should be present with `target`. A transient (non-serializable)
    // component has no addToScene hook and was never in the payload — skip it; its
    // state is rebuilt by the rebind hook off a sibling durable component instead.
    if (!meta || !meta->serializable || !meta->addToScene)
        return;

    // Remove-first-then-add: Scene::Add (which addToScene bottoms out in) silently
    // rejects an already-present component, so a value edit must clear the old one
    // first (design doc §6/§8.8).
    _scene.RemoveById(entity, id);
    meta->addToScene(&_scene, entity.index, entity.generation, *target);
    _rebind(entity, id, /*present=*/true);
}

void EditHistory::ApplyTransaction(const Transaction &txn, Direction dir)
{
    const bool undo = (dir == Direction::Undo);

    // Guard capture against the edits this apply itself makes (re-entrancy).
    const ApplyingGuard applyingGuard(_applying);

    // Restore EntityRef fields against raw handles, not save/load serial indices;
    // exact because entities come back at their original slot via ReviveAt.
    {
        Rt::SceneSerializer::ScopedRawEntityContext rawContext(_scene);

        // Phase 1 — entity existence: revive every entity that must exist on this
        // side, before any component is added. A component may reference another
        // subtree member (Parent), which must already be alive for the raw context
        // to resolve it — so all revives precede all component work.
        ForEachCommand(txn, undo, [&](const EditCommand &cmd) {
            if (const auto *ed = std::get_if<EntityDelta>(&cmd))
            {
                const auto &state = undo ? ed->before : ed->after;
                if (state.has_value() && !_scene.IsAlive(ed->handle))
                    _scene.ReviveAt(ed->handle);
            }
        });

        // Phase 2 — components: restore/remove standalone component deltas, and add
        // the full component set of every just-revived entity.
        ForEachCommand(txn, undo, [&](const EditCommand &cmd) {
            if (const auto *cd = std::get_if<ComponentDelta>(&cmd))
            {
                RestoreComponent(cd->entity, cd->id, undo ? cd->before : cd->after);
                return;
            }
            const auto &ed    = std::get<EntityDelta>(cmd);
            const auto &state  = undo ? ed.before : ed.after;
            if (state.has_value())
                for (const ComponentSnapshot &snap : *state)
                    RestoreComponent(ed.handle, snap.id, snap.data);
        });

        // Phase 3 — destroy entities that must NOT exist on this side. Tear down
        // each one's transient state first (the populated, non-target side lists
        // the components it currently has), then queue the destroy.
        bool anyDestroyed = false;
        ForEachCommand(txn, undo, [&](const EditCommand &cmd) {
            const auto *ed = std::get_if<EntityDelta>(&cmd);
            if (ed == nullptr)
                return;
            const auto &state = undo ? ed->before : ed->after;
            if (state.has_value() || !_scene.IsAlive(ed->handle))
                return; // should exist, or already gone

            const auto &current = undo ? ed->after : ed->before; // the live side
            if (current.has_value())
                for (const ComponentSnapshot &snap : *current)
                    _rebind(ed->handle, snap.id, /*present=*/false);

            _scene.Destroy(ed->handle);
            anyDestroyed = true;
        });

        // Apply the destroys now so a freed slot is available for a later exact
        // ReviveAt, and so a re-killed entity can't linger in the queue.
        if (anyDestroyed)
            _scene.FlushDestroyed();
    }
}

} // namespace Sandbox
