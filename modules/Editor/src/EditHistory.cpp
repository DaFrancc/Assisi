/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/EditHistory.hpp>

#include <algorithm>
#include <utility>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentMeta.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/NameComponent.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

namespace Assisi::Editor
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

EditHistory::EditHistory(Assisi::ECS::Scene &scene, RebindHook rebind,
                         Assisi::Runtime::InstanceTable *instances)
    : _scene(scene), _instances(instances), _rebind(std::move(rebind))
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
    txn.seq = _nextSeq++; // unique, monotonic — the dirty-tracking state token
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
    _open.clear(); // any in-flight gesture references entity handles about to dangle
}

namespace
{

/// Whether @p txn acts on any entity in @p destroyed.
///
/// An InstanceDelta names an instance rather than an entity, and an instance
/// survives its members being re-expanded — the row is exactly what a blueprint edit
/// does not touch. So only the two entity-shaped commands are asked.
bool NamesAny(const Transaction &txn, std::span<const Assisi::ECS::Entity> destroyed)
{
    const auto hit = [destroyed](Assisi::ECS::Entity entity)
    { return std::find(destroyed.begin(), destroyed.end(), entity) != destroyed.end(); };

    for (const EditCommand &cmd : txn.cmds)
    {
        if (const auto *component = std::get_if<ComponentDelta>(&cmd); component != nullptr && hit(component->entity))
            return true;
        if (const auto *entity = std::get_if<EntityDelta>(&cmd); entity != nullptr && hit(entity->handle))
            return true;
    }
    return false;
}

/// One past the newest transaction in @p undo that names a destroyed entity, or 0 if
/// none does — which is exactly how many steps have to go. See ForgetEntities for
/// why the answer is a suffix rather than a set.
std::size_t ForgettableCount(const std::vector<Transaction>       &undo,
                             std::span<const Assisi::ECS::Entity> destroyed)
{
    if (destroyed.empty())
        return 0;

    for (std::size_t i = undo.size(); i > 0; --i)
    {
        if (NamesAny(undo[i - 1], destroyed))
            return i;
    }
    return 0;
}

} // namespace

std::size_t EditHistory::CountForgettable(const Assisi::ECS::Scene             &scene,
                                          std::span<const Assisi::ECS::Entity> destroyed) const
{
    if (&scene != &_scene)
        return 0;
    return ForgettableCount(_undo, destroyed);
}

std::size_t EditHistory::ForgetEntities(const Assisi::ECS::Scene             &scene,
                                        std::span<const Assisi::ECS::Entity> destroyed)
{
    // Refused before anything is compared, not filtered afterwards: the handles
    // themselves carry nothing that distinguishes them from this scene's.
    if (&scene != &_scene)
        return 0;

    const std::size_t drop = ForgettableCount(_undo, destroyed);
    if (drop == 0)
        return 0;

    _undo.erase(_undo.begin(), _undo.begin() + static_cast<std::ptrdiff_t>(drop));
    // Whole, not filtered: a redo replays forward from where undo left off, so a
    // surviving redo below a dropped one has the same broken-chain problem.
    _redo.clear();
    // An open gesture on a destroyed entity would commit a transaction naming it the
    // moment the sweep runs — putting back exactly what was just removed.
    std::erase_if(_open, [destroyed](const OpenGesture &gesture) {
        return std::find(destroyed.begin(), destroyed.end(), gesture.entity) != destroyed.end();
    });
    return drop;
}

const std::string &EditHistory::NextUndoLabel() const
{
    return _undo.empty() ? kEmptyLabel : _undo.back().label;
}

const std::string &EditHistory::NextRedoLabel() const
{
    return _redo.empty() ? kEmptyLabel : _redo.back().label;
}

std::vector<std::string> EditHistory::UndoLabels() const
{
    std::vector<std::string> labels;
    labels.reserve(_undo.size());
    for (const Transaction &txn : _undo) // oldest → newest (back = next Undo target)
        labels.push_back(txn.label);
    return labels;
}

std::vector<std::string> EditHistory::RedoLabels() const
{
    std::vector<std::string> labels;
    labels.reserve(_redo.size());
    // _redo.back() is the next Redo target; expose next-to-redo first.
    for (auto it = _redo.rbegin(); it != _redo.rend(); ++it)
        labels.push_back(it->label);
    return labels;
}

std::vector<ComponentSnapshot> EditHistory::CaptureEntityComponents(Entity entity) const
{
    std::vector<ComponentSnapshot> components;
    if (!_scene.IsAlive(entity))
        return components;

    Rt::SceneSerializer::ScopedRawEntityContext rawContext(_scene);
    for (const Reflect::ComponentMeta *meta : Reflect::ComponentRegistry::Instance().SerializableComponents())
    {
        if (const void *comp = meta->getByEntity(&_scene, entity.index, entity.generation))
            components.push_back({meta->id, meta->serialize(comp)});
    }
    return components;
}

// ---------------------------------------------------------------------------
// Capture (record-before-write, design doc §5)
// ---------------------------------------------------------------------------

std::optional<nlohmann::json> EditHistory::SnapshotComponent(Entity entity, Reflect::ComponentId id) const
{
    const Reflect::ComponentMeta *meta = Reflect::ComponentRegistry::Instance().ById(id);
    if (!meta || !meta->serializable || !meta->getByEntity || !meta->serialize)
        return std::nullopt;
    const void *comp = meta->getByEntity(&_scene, entity.index, entity.generation);
    if (comp == nullptr)
        return std::nullopt; // component absent — a valid capture state (add/remove)

    // Raw handles for EntityRef fields, matching the apply-time restore context.
    Rt::SceneSerializer::ScopedRawEntityContext rawContext(_scene);
    return meta->serialize(comp);
}

EditHistory::OpenGesture *EditHistory::FindOpen(Entity entity, Reflect::ComponentId id)
{
    for (OpenGesture &gesture : _open)
        if (gesture.id == id && gesture.entity == entity)
            return &gesture;
    return nullptr;
}

void EditHistory::RecordBefore(Entity entity, Reflect::ComponentId id, std::string label, Entity selection)
{
    if (_applying)
        return; // an apply's writes are not themselves edits to capture

    if (OpenGesture *existing = FindOpen(entity, id))
    {
        existing->touchedThisFrame = true;    // keep the original `before`; just refresh liveness
        existing->label            = std::move(label); // last writer wins the label: a later
                                                       // "Remove X" overrides the inspector's
                                                       // per-frame "Edit X" on the same gesture
        return;
    }
    _open.push_back(OpenGesture{.entity           = entity,
                                .id               = id,
                                .label            = std::move(label),
                                .before           = SnapshotComponent(entity, id),
                                .selection        = selection,
                                .touchedThisFrame = true});
}

bool EditHistory::CommitOpenGesture(const OpenGesture &gesture)
{
    const std::optional<nlohmann::json> after = SnapshotComponent(gesture.entity, gesture.id);
    if (gesture.before == after)
        return false; // no net change — drops click-without-drag and Escape-revert

    Transaction txn;
    txn.label           = gesture.label;
    txn.selectionBefore = gesture.selection;
    txn.selectionAfter  = gesture.selection;
    txn.cmds.push_back(ComponentDelta{gesture.entity, gesture.id, gesture.before, after});

    // Transacted with the edit, not recorded separately: undo has to take the
    // override back with the value, or it leaves a note claiming this instance
    // changed a field it no longer does.
    if (std::optional<InstanceDelta> record = RecordOverride(gesture.entity, gesture.id, gesture.before, after))
        txn.cmds.push_back(std::move(*record));

    Push(std::move(txn));
    return true;
}

std::optional<std::string> EditHistory::NameForOverrideTarget(Entity target, ECS::InstanceId instanceId) const
{
    if (_instances == nullptr || target == Assisi::ECS::NullEntity || !_scene.IsAlive(target))
        return std::nullopt;

    if (const Assisi::ECS::BlueprintMember *tag = _scene.Get<Assisi::ECS::BlueprintMember>(target))
    {
        const Rt::BlueprintInstance *row = _instances->Find(tag->instanceId);
        if (row == nullptr)
            return std::nullopt;

        const std::shared_ptr<const Rt::BlueprintDefinition> definition = Rt::GetBlueprintDefinition(row->source);
        if (definition == nullptr || tag->memberIndex >= definition->members.size())
            return std::nullopt;

        const std::string &memberPath = definition->members[tag->memberIndex].name;

        // Same instance: relative, because expansion prefixes an override's
        // references with the instance's own name — so `wheel_fl` here becomes
        // `car_3/wheel_fl` when it is applied.
        if (tag->instanceId == instanceId)
            return memberPath;

        // Another instance: reach it through the writing file's scope, which is
        // where its full path is addressable.
        return row->name.empty() ? std::optional<std::string>{} : "/" + row->name + "/" + memberPath;
    }

    // An ordinary entity of the writing file. The leading slash is what keeps it
    // from being read as a member of this instance.
    if (const Rt::Name *name = _scene.Get<Rt::Name>(target); name != nullptr && !name->value.View().empty())
        return "/" + std::string{name->value.View()};

    return std::nullopt;
}

nlohmann::json EditHistory::ReferenceSafeOverride(const nlohmann::json &component,
                                                  const Reflect::ComponentMeta &meta,
                                                  ECS::InstanceId               instanceId) const
{
    nlohmann::json out = component;

    for (const Reflect::FieldMeta &field : meta.fields)
    {
        if (field.type != Reflect::FieldType::EntityRef)
            continue;

        const auto it = out.find(field.name);
        if (it == out.end() || it->is_null())
            continue; // no target is the same thing in both spellings
        if (!it->is_number_unsigned())
            continue; // not a raw-context capture; leave whatever it is alone

        const std::uint64_t packed = it->get<std::uint64_t>();
        const Entity        target{static_cast<std::uint32_t>(packed & 0xFFFFFFFFull),
                            static_cast<std::uint32_t>(packed >> 32)};

        if (const std::optional<std::string> name = NameForOverrideTarget(target, instanceId))
        {
            *it = *name;
            continue;
        }

        Core::Log::Warn("Editor: '{}::{}' points at an entity this level cannot name, so the override does "
                        "not record it. Give the target a name, or wire it inside the blueprint.",
                        meta.name, field.name);
        out.erase(field.name);
    }

    return out;
}

std::optional<InstanceDelta> EditHistory::RecordOverride(Entity entity, Reflect::ComponentId id,
                                                         const std::optional<nlohmann::json> &before,
                                                         const std::optional<nlohmann::json> &after)
{
    if (_instances == nullptr)
        return std::nullopt;

    const Assisi::ECS::BlueprintMember *tag = _scene.Get<Assisi::ECS::BlueprintMember>(entity);
    if (tag == nullptr)
        return std::nullopt;

    const Rt::BlueprintInstance *row = _instances->Find(tag->instanceId);
    if (row == nullptr)
        return std::nullopt;

    const std::shared_ptr<const Rt::BlueprintDefinition> definition = Rt::GetBlueprintDefinition(row->source);
    if (definition == nullptr || tag->memberIndex >= definition->members.size())
        return std::nullopt;

    const Reflect::ComponentMeta *meta = Reflect::ComponentRegistry::Instance().ById(id);
    if (meta == nullptr)
        return std::nullopt;

    const std::string &memberPath = definition->members[tag->memberIndex].name;

    Rt::BlueprintInstance updated = *row;
    if (!updated.overrides.is_object())
        updated.overrides = nlohmann::json::object();
    nlohmann::json &claim = updated.overrides[memberPath];
    if (!claim.is_object())
        claim = nlohmann::json::object();

    if (!after.has_value())
    {
        // The component was removed. `null` reads unambiguously as removal, since a
        // real component is always an object.
        claim[meta->name] = nullptr;
    }
    else if (!before.has_value())
    {
        // Added: the whole object is the claim, since there is no blueprint value
        // for any of its fields to fall back to.
        claim[meta->name] = ReferenceSafeOverride(*after, *meta, tag->instanceId);
    }
    else
    {
        // Only the fields this gesture actually moved. Recording the whole object
        // would pin every *other* field of the component at today's blueprint
        // value, which is how "fix it once, fixed everywhere" quietly stops being
        // true for that member.
        nlohmann::json &componentClaim = claim[meta->name];
        if (!componentClaim.is_object())
            componentClaim = nlohmann::json::object();

        const nlohmann::json safeAfter = ReferenceSafeOverride(*after, *meta, tag->instanceId);
        for (const auto &[fieldName, value] : safeAfter.items())
        {
            const auto previous = before->find(fieldName);
            if (previous == before->end() || *previous != value)
                componentClaim[fieldName] = value;
        }

        if (componentClaim.empty())
            return std::nullopt; // nothing the file can express changed
    }

    InstanceDelta delta{.instanceId = tag->instanceId, .before = *row, .after = updated};
    _instances->RestoreAt(tag->instanceId, std::move(updated));
    return delta;
}

void EditHistory::CommitGesture(Entity entity, Reflect::ComponentId id)
{
    if (_applying)
        return;
    OpenGesture *gesture = FindOpen(entity, id);
    if (gesture == nullptr)
        return;
    CommitOpenGesture(*gesture);
    // Erase by key (CommitOpenGesture may have pushed, but never mutates _open).
    std::erase_if(_open, [&](const OpenGesture &g) { return g.id == id && g.entity == entity; });
}

void EditHistory::EndFrameSweep(bool editingActive)
{
    if (_applying)
        return;

    for (auto it = _open.begin(); it != _open.end();)
    {
        OpenGesture &gesture = *it;

        // The entity died mid-gesture (a delete elsewhere): nothing coherent to
        // commit against — abandon.
        if (!_scene.IsAlive(gesture.entity))
        {
            it = _open.erase(it);
            continue;
        }

        // Still being manipulated this frame (a drag/type in progress, or the gizmo
        // held) and its block was drawn — leave it open to coalesce.
        if (editingActive && gesture.touchedThisFrame)
        {
            gesture.touchedThisFrame = false; // reset for next frame's RecordBefore
            ++it;
            continue;
        }

        // The gesture ended — either the widget released (!editingActive) or its
        // block is no longer drawn (!touched). Commit if it changed, else drop.
        CommitOpenGesture(gesture);
        it = _open.erase(it);
    }
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

    // The component should be present with `target`. Add it, then rebind (a single
    // component edit has no sibling ordering concern).
    if (AddComponentForRestore(entity, id, *target))
        _rebind(entity, id, /*present=*/true);
}

bool EditHistory::AddComponentForRestore(Entity entity, Reflect::ComponentId id, const nlohmann::json &data)
{
    const Reflect::ComponentMeta *meta = Reflect::ComponentRegistry::Instance().ById(id);

    // A transient (non-serializable) component has no addToScene hook and was never
    // in the payload — skip it; its state is rebuilt by the rebind hook off a
    // sibling durable component instead.
    if (!meta || !meta->serializable || !meta->addToScene)
        return false;

    // Remove-first-then-add: Scene::Add (which addToScene bottoms out in) silently
    // rejects an already-present component, so a value edit must clear the old one
    // first (design doc §6/§8.8).
    _scene.RemoveById(entity, id);
    meta->addToScene(&_scene, entity.index, entity.generation, data);
    return true;
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

        // Phase 1b — instance records. Before the components, because a member's
        // component restore may want the row it belongs to; and it is pure
        // bookkeeping either way, with no scene state to order against.
        ForEachCommand(txn, undo, [&](const EditCommand &cmd) {
            const auto *idl = std::get_if<InstanceDelta>(&cmd);
            if (idl == nullptr || _instances == nullptr)
                return;

            const auto &state = undo ? idl->before : idl->after;
            if (state.has_value())
                _instances->RestoreAt(idl->instanceId, *state);
            else
                _instances->Remove(idl->instanceId);
        });

        // Phase 2 — components: restore/remove standalone component deltas, and add
        // the full component set of every just-revived entity.
        ForEachCommand(txn, undo, [&](const EditCommand &cmd) {
            if (const auto *cd = std::get_if<ComponentDelta>(&cmd))
            {
                RestoreComponent(cd->entity, cd->id, undo ? cd->before : cd->after);
                return;
            }
            if (std::holds_alternative<InstanceDelta>(cmd))
                return; // handled above
            const auto &ed    = std::get<EntityDelta>(cmd);
            const auto &state  = undo ? ed.before : ed.after;
            if (state.has_value())
            {
                // Add the whole component set first, THEN rebind each — so a
                // component's rebind hook (e.g. the physics-body rebuild keyed off
                // RigidBodyDescriptor) sees every sibling already restored, not just
                // the ones that happen to sort before it. Firing the hook per
                // component mid-restore dropped the Jolt body on undo-of-delete.
                for (const ComponentSnapshot &snap : *state)
                    AddComponentForRestore(ed.handle, snap.id, snap.data);
                for (const ComponentSnapshot &snap : *state)
                {
                    // Rebind exactly the components that were added (serializable
                    // with an addToScene hook) — the same set AddComponentForRestore
                    // acted on — now that every sibling is present.
                    const auto *meta = Reflect::ComponentRegistry::Instance().ById(snap.id);
                    if (meta && meta->serializable && meta->addToScene)
                        _rebind(ed.handle, snap.id, /*present=*/true);
                }
            }
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

} // namespace Assisi::Editor
