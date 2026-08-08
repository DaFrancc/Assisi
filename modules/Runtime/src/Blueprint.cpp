/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/Blueprint.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/Naming.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <algorithm>
#include <cmath>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <utility>

namespace Assisi::Runtime
{

// ---------------------------------------------------------------------------
// Transform composition
// ---------------------------------------------------------------------------

bool HasUniformScale(const ECS::Transform &transform)
{
    // Relative, so a metre-scale and a kilometre-scale instance are held to the
    // same standard. The tolerance exists for a hand-typed 1.0000001, not to let a
    // genuinely non-uniform scale through: the smallest visible non-uniformity is
    // orders of magnitude above this.
    constexpr float kTolerance = 1e-5f;

    const glm::vec3 &scale = transform.scale;
    const float      mean  = (std::abs(scale.x) + std::abs(scale.y) + std::abs(scale.z)) / 3.f;
    if (mean <= 0.f)
        return scale.x == scale.y && scale.y == scale.z;

    return std::abs(scale.x - scale.y) / mean < kTolerance && std::abs(scale.y - scale.z) / mean < kTolerance;
}

ECS::Transform TransformFromJson(const nlohmann::json &value)
{
    ECS::Transform out;
    if (!value.is_object())
        return out;

    if (const auto it = value.find("position"); it != value.end() && it->is_array() && it->size() == 3)
        out.position = {(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>()};
    if (const auto it = value.find("rotation"); it != value.end() && it->is_array() && it->size() == 4)
        out.rotation = glm::quat{(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>(),
                                 (*it)[3].get<float>()};
    if (const auto it = value.find("scale"); it != value.end() && it->is_array() && it->size() == 3)
        out.scale = {(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>()};

    return out;
}

nlohmann::json TransformToJson(const ECS::Transform &transform)
{
    return {{"position", {transform.position.x, transform.position.y, transform.position.z}},
            {"rotation",
             {transform.rotation.w, transform.rotation.x, transform.rotation.y, transform.rotation.z}},
            {"scale", {transform.scale.x, transform.scale.y, transform.scale.z}}};
}

ECS::Transform ComposeTransform(const ECS::Transform &placement, const ECS::Transform &local)
{
    ECS::Transform out;
    out.position = placement.position + (placement.rotation * (placement.scale * local.position));
    out.rotation = glm::normalize(placement.rotation * local.rotation);
    out.scale    = placement.scale * local.scale;
    return out;
}

ECS::Transform AuthoringOrigin(const ECS::Transform &root)
{
    ECS::Transform out;
    out.position = root.position;
    out.rotation = root.rotation;
    // …and scale stays at 1. See the header for why this one field is dropped.
    return out;
}

ECS::Transform InverseComposeTransform(const ECS::Transform &placement, const ECS::Transform &world)
{
    // Exact only under the uniform-scale rule, same as the forward form: with one
    // scale factor the division below is a scalar and the rotation is unaffected
    // by it.
    const float scale = placement.scale.x != 0.f ? placement.scale.x : 1.f;

    ECS::Transform out;
    out.rotation = glm::normalize(glm::inverse(placement.rotation) * world.rotation);
    out.position = (glm::inverse(placement.rotation) * (world.position - placement.position)) / scale;
    out.scale    = world.scale / scale;
    return out;
}

// ---------------------------------------------------------------------------
// The instance table
// ---------------------------------------------------------------------------

ECS::InstanceId InstanceTable::Add(BlueprintInstance instance)
{
    // The one place that turns a raw counter into an id. Everywhere else the type
    // is opaque, which is the point — see ECS::InstanceId.
    const ECS::InstanceId id{_nextId++};
    _rows.emplace(id, std::move(instance));
    return id;
}

const BlueprintInstance *InstanceTable::Find(ECS::InstanceId id) const
{
    const auto it = _rows.find(id);
    return it != _rows.end() ? &it->second : nullptr;
}

void InstanceTable::Remove(ECS::InstanceId id)
{
    _rows.erase(id);
}

void InstanceTable::RestoreAt(ECS::InstanceId id, BlueprintInstance instance)
{
    if (!id.IsValid())
        return; // 0 is never a live instance

    _rows[id] = std::move(instance);
    _nextId   = std::max(_nextId, id.value + 1);
}

std::vector<std::pair<ECS::InstanceId, const BlueprintInstance *>> InstanceTable::All() const
{
    // Sorted, because a save writes one entry per row and the file's byte content
    // must be a function of the world rather than of hash iteration order.
    std::vector<std::pair<ECS::InstanceId, const BlueprintInstance *>> out;
    out.reserve(_rows.size());
    for (const auto &[id, row] : _rows)
        out.emplace_back(id, &row);
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    return out;
}

void InstanceTable::Clear()
{
    _rows.clear();
    _nextId = 1;
}

std::vector<ECS::Entity> MembersOf(ECS::Scene &scene, ECS::InstanceId instanceId)
{
    std::vector<ECS::Entity> members;
    if (!instanceId.IsValid())
        return members;

    for (auto [entity, tag] : scene.Query<ECS::BlueprintMember>())
    {
        if (tag.instanceId == instanceId)
            members.push_back(entity);
    }
    return members;
}

bool PruneFromInstance(ECS::Scene &scene, ECS::Entity entity)
{
    if (!scene.Has<ECS::BlueprintMember>(entity))
        return false;

    scene.Remove<ECS::BlueprintMember>(entity);
    return true;
}

ECS::Entity FindMember(ECS::Scene &scene, const InstanceTable &table, ECS::InstanceId instanceId,
                       std::string_view name)
{
    const BlueprintInstance *row = FindInstance(table, instanceId);
    if (row == nullptr)
        return ECS::NullEntity;

    const BlueprintResult definition = GetBlueprintDefinition(row->source);
    if (!definition)
        return ECS::NullEntity;

    // Once, here — after which the scan compares integers rather than strings per
    // entity, which is the whole reason the tag carries an index.
    const std::optional<uint32_t> index = (*definition)->IndexOf(name);
    if (!index.has_value())
        return ECS::NullEntity;

    for (auto [entity, tag] : scene.Query<ECS::BlueprintMember>())
    {
        if (tag.instanceId == instanceId && tag.memberIndex == *index)
            return entity;
    }
    return ECS::NullEntity;
}

const BlueprintInstance *FindInstance(const InstanceTable &table, ECS::InstanceId instanceId,
                                      std::string_view expectedSource)
{
    const BlueprintInstance *row = table.Find(instanceId);
    if (row == nullptr)
        return nullptr;
    if (!expectedSource.empty() && row->source != expectedSource)
        return nullptr;
    return row;
}

std::string UniqueInstanceName(const InstanceTable &table, std::string_view stem)
{
    return UniqueName(stem,
                      [&table](std::string_view candidate)
                      {
                          for (const auto &[id, row] : table.All())
                          {
                              if (row->name == candidate)
                                  return true;
                          }
                          return false;
                      });
}

std::vector<LevelInstance> InstancesForSave(const InstanceTable &table)
{
    std::vector<LevelInstance> out;
    for (const auto &[id, row] : table.All())
    {
        // A runtime spawn is not level content: it exists because something in the
        // game asked for it, and writing it into the file would make it authored
        // the next time the level loads.
        if (!row->authored)
            continue;

        out.push_back(LevelInstance{.name      = row->name,
                                    .source    = row->source,
                                    .transform = row->transform,
                                    .overrides = row->overrides,
                                    .removed   = row->removed});
    }
    return out;
}

// ---------------------------------------------------------------------------
// Definition parsing and flattening
// ---------------------------------------------------------------------------

std::optional<uint32_t> BlueprintDefinition::IndexOf(std::string_view name) const
{
    for (uint32_t i = 0; i < members.size(); ++i)
    {
        if (members[i].name == name)
            return i;
    }
    return std::nullopt;
}

namespace
{

/// Every cached definition, keyed by virtual path. Deliberately not evicted during
/// a level: the tag's memberIndex is only meaningful while its file's member list
/// is cached, and both are discarded together at unload (§2).
///
/// Shared ownership rather than storage, because the cache is not the only owner.
/// A worker thread staging instances during async travel is walking a definition
/// it asked for a moment ago, and a level unload or a blueprint save on the main
/// thread must not pull it out from under that walk. Eviction drops the cache's
/// claim; the definition goes when the last holder does.
std::map<std::string, std::shared_ptr<const BlueprintDefinition>, std::less<>> &Cache()
{
    static std::map<std::string, std::shared_ptr<const BlueprintDefinition>, std::less<>> cache;
    return cache;
}

/// Guards every access to Cache(). Held across the *build* too, not just the map
/// operations: the build reads a file and flattens it, and releasing the lock in
/// the middle would let a level unload land between "not cached" and "insert",
/// leaving the dead level's definition in the new level's cache. Building the same
/// file twice is the only thing the wider hold costs, and it costs it once per
/// file per level.
///
/// No re-entrancy to worry about: nesting recurses through FlattenInto/ReadFile,
/// never back through GetBlueprintDefinition.
std::mutex &CacheMutex()
{
    static std::mutex mutex;
    return mutex;
}

/// `prefix + name`, with a leading '/' on @p name stripped first.
///
/// A leading slash means "the file that wrote this", which for a reference
/// authored inside a file *is* that file — so it prefixes exactly like a plain
/// name here. The two only diverge for an override, where the writing file and the
/// file being addressed are different (§6); QualifyOverrideReferences is that case.
std::string QualifyName(std::string_view prefix, std::string_view name)
{
    if (!name.empty() && name.front() == '/')
        name.remove_prefix(1);
    return std::string{prefix} + std::string{name};
}

/// Calls @p rewrite on every EntityRef field of every component in @p components.
///
/// Reflection is what makes this possible at all: a reference is a *string* in the
/// file, indistinguishable from any other string without asking the component what
/// its fields mean.
/// True if @p components declares a Parent whose target is not null. Such a member
/// is positioned relative to that parent, so the instance's placement must not be
/// composed onto it a second time.
bool DeclaresParent(const nlohmann::json &components)
{
    if (!components.is_object())
        return false;
    const auto it = components.find("Parent");
    if (it == components.end() || !it->is_object())
        return false;
    const auto parent = it->find("parent");
    return parent != it->end() && !parent->is_null();
}

template <typename Fn> void ForEachEntityRef(nlohmann::json &components, Fn &&rewrite)
{
    if (!components.is_object())
        return;

    const auto &registry = Core::Reflect::ComponentRegistry::Instance();
    for (auto &[componentName, componentData] : components.items())
    {
        const Core::Reflect::ComponentMeta *meta = registry.Find(componentName);
        if (meta == nullptr || !componentData.is_object())
            continue;

        for (const Core::Reflect::FieldMeta &field : meta->fields)
        {
            if (field.type != Core::Reflect::FieldType::EntityRef)
                continue;

            const auto it = componentData.find(field.name);
            if (it == componentData.end() || !it->is_string())
                continue;

            *it = rewrite(it->get<std::string>());
        }
    }
}

} // namespace

void QualifyReferences(nlohmann::json &components, std::string_view prefix)
{
    // Runs even at an empty prefix, where it only strips the slash. That strip is
    // the whole point: it is what makes a surviving '/' downstream mean exactly one
    // thing — level scope — instead of two. See the invariant on the declaration.
    ForEachEntityRef(components, [prefix](const std::string &name) { return QualifyName(prefix, name); });
}

void QualifyOverrideReferences(nlohmann::json &componentOverrides, std::string_view writerPrefix,
                               std::string_view targetPrefix)
{
    if (!componentOverrides.is_object())
        return;

    for (auto &[componentName, claim] : componentOverrides.items())
    {
        // A null claim is a removal note, not an edit — it has no fields to resolve.
        if (!claim.is_object())
            continue;

        // The one place the two namespaces are both live. A plain name addresses
        // downward into the instance being overridden; a leading slash addresses the
        // file that wrote the claim. Both leave here fully qualified, so nothing
        // downstream has to know which form the author used.
        nlohmann::json wrapper{{componentName, claim}};
        ForEachEntityRef(wrapper,
                         [writerPrefix, targetPrefix](std::string_view name)
                         {
                             if (!name.empty() && name.front() == '/')
                             {
                                 name.remove_prefix(1);
                                 return std::string{writerPrefix} + std::string{name};
                             }
                             return std::string{targetPrefix} + std::string{name};
                         });
        claim = wrapper.at(componentName);
    }
}

void QualifyInstanceReferences(nlohmann::json &components, std::string_view prefix)
{
    ForEachEntityRef(components,
                     [prefix](std::string_view name)
                     {
                         // By now every reference a blueprint could resolve already is
                         // (the invariant on QualifyReferences), so a surviving slash
                         // can only be level-scoped: it names an entity of the file that
                         // placed this instance, and must not take the instance prefix.
                         if (!name.empty() && name.front() == '/')
                             return std::string{name.substr(1)};
                         return std::string{prefix} + std::string{name};
                     });
}

void ApplyMemberOverride(BlueprintMemberDesc &member, const nlohmann::json &componentOverrides,
                         std::string_view context)
{
    if (!componentOverrides.is_object())
        return;

    for (const auto &[componentName, claim] : componentOverrides.items())
    {
        const bool alreadyRemoved =
            std::find(member.removedComponents.begin(), member.removedComponents.end(), componentName) !=
            member.removedComponents.end();

        if (claim.is_null())
        {
            // A note saying "this instance does not have it", not an edit — so if
            // the blueprint later drops the component itself, the note becomes a
            // harmless no-op rather than an error.
            member.components.erase(componentName);
            if (!alreadyRemoved)
                member.removedComponents.emplace_back(componentName);
            continue;
        }
        if (!claim.is_object())
            continue;

        if (alreadyRemoved)
        {
            Core::Log::Warn("Blueprint: '{}' overrides fields of '{}', which an inner file removed. The "
                            "removal wins and the override is dropped — decide which one should go.",
                            context, componentName);
            continue;
        }

        // Per field, outermost winning. An add lands here too, against an absent
        // component: the object becomes the whole claim and the deserialize fills
        // the rest from C++ defaults.
        nlohmann::json &target = member.components[componentName];
        if (!target.is_object())
            target = nlohmann::json::object();
        for (const auto &[fieldName, value] : claim.items())
            target[fieldName] = value;
    }

    // Here rather than at the call sites, because `parented` is a fact *about*
    // `components` and this is the only thing that changes them after flatten
    // computed it. An override may add a Parent (the member stops being in world
    // space) or null one out (it starts being), and that answer decides whether the
    // instance's placement composes onto its Transform — so a stale value puts the
    // member the whole placement away from where the author put it.
    const bool wasParented = member.parented;
    member.parented        = DeclaresParent(member.components);
    if (wasParented == member.parented)
        return;

    // The flip also invalidates the composition flatten already did. A member that
    // has just become parented is holding a Transform with the chain's placement
    // baked in and must give it back; one that has just been cut loose never got it
    // and now needs it. Doing only the first half is what put a member the whole
    // nesting placement away from its instance.
    ECS::Transform current;
    if (const auto it = member.components.find("Transform"); it != member.components.end())
        current = TransformFromJson(*it);

    member.components["Transform"] = TransformToJson(
        member.parented ? InverseComposeTransform(member.placement, current)
                        : ComposeTransform(member.placement, current));
}

bool IsMemberRemoved(std::string_view memberName, const std::vector<std::string> &removed)
{
    for (const std::string &path : removed)
    {
        if (memberName == path)
            return true;
        if (memberName.size() > path.size() && memberName.starts_with(path) && memberName[path.size()] == '/')
            return true;
    }
    return false;
}

namespace
{

/// Reads an instance entry's `overrides` and `removed` into @p out.
void ReadInstanceClaims(const nlohmann::json &entry, LevelInstance &out)
{
    if (const auto it = entry.find("overrides"); it != entry.end() && it->is_object())
        out.overrides = *it;
    if (const auto it = entry.find("removed"); it != entry.end() && it->is_array())
    {
        for (const auto &path : *it)
        {
            if (path.is_string())
                out.removed.push_back(path.get<std::string>());
        }
    }
}

/// Reads @p source through the asset system and parses it.
///
/// Logs which file is wrong before returning the kind, here and at every failure
/// site below: by the time this reaches the caller, `source` may be a file three
/// levels of nesting under the one it asked for, and nothing above can name it.
///
/// Parsed with `allow_exceptions=false` so a malformed file is a value here rather
/// than a throw the boundary has to catch — the same shape AssetSidecar and
/// MaterialFile use.
std::expected<nlohmann::json, BlueprintError> ReadFile(std::string_view source)
{
    const auto text = Core::AssetSystem::ReadText(source);
    if (!text)
    {
        Core::Log::Error("Blueprint: cannot read '{}'.", source);
        return std::unexpected(BlueprintError::FileUnreadable);
    }

    nlohmann::json doc = nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded())
    {
        Core::Log::Error("Blueprint: '{}' is not readable JSON.", source);
        return std::unexpected(BlueprintError::MalformedJson);
    }

    if (doc.value("version", 0) != 2)
    {
        Core::Log::Error("Blueprint: '{}' is version {} (this build reads version 2).", source,
                         doc.value("version", 0));
        return std::unexpected(BlueprintError::UnsupportedVersion);
    }
    return doc;
}

struct FlattenState
{
    BlueprintDefinition                    *out;
    std::vector<std::string>                stack;   ///< Sources currently being flattened, for cycle detection.
    std::unordered_set<std::string>         declared; ///< Member names claimed so far.
};

std::expected<void, BlueprintError> FlattenInto(FlattenState &state, const nlohmann::json &doc,
                                                std::string_view source, const std::string &prefix,
                                                const ECS::Transform &placement);

/// One nested instance entry: resolve the source, compose the placement, recurse.
std::expected<void, BlueprintError> FlattenInstance(FlattenState &state, const nlohmann::json &entry,
                                                    std::string_view source, const std::string &prefix,
                                                    const ECS::Transform &placement)
{
    if (!entry.contains("name") || !entry.at("name").is_string())
    {
        Core::Log::Error("Blueprint: '{}' has an instance with no name.", source);
        return std::unexpected(BlueprintError::MissingName);
    }
    if (!entry.contains("source") || !entry.at("source").is_string())
    {
        Core::Log::Error("Blueprint: '{}' instance '{}' has no source.", source,
                         entry.at("name").get<std::string>());
        return std::unexpected(BlueprintError::MissingSource);
    }

    const std::string name        = entry.at("name").get<std::string>();
    const std::string childSource = entry.at("source").get<std::string>();

    // Cycles hard-fail rather than being detected later: `a` containing `b`
    // containing `a` expands forever, and it is unrecoverable if missed (§4).
    if (std::find(state.stack.begin(), state.stack.end(), childSource) != state.stack.end())
    {
        std::string chain;
        for (const std::string &link : state.stack)
            chain += link + " -> ";
        Core::Log::Error("Blueprint: instance cycle: {}{}.", chain, childSource);
        return std::unexpected(BlueprintError::InstanceCycle);
    }

    const ECS::Transform local = TransformFromJson(entry.value("transform", nlohmann::json::object()));

    // At every level, not just the outermost (§4). Clamping to an axis was
    // rejected: it lets the file say one thing while the game does another.
    if (!HasUniformScale(local))
    {
        Core::Log::Error("Blueprint: '{}' instance '{}' has a non-uniform scale ({}, {}, {}); an instance may "
                         "only translate, rotate, or scale uniformly.",
                         source, name, local.scale.x, local.scale.y, local.scale.z);
        return std::unexpected(BlueprintError::NonUniformScale);
    }

    const std::string childPrefix = prefix + name + "/";
    const std::size_t first       = state.out->members.size();

    // Hoisted out of the recursive call: the read has to be checked before the
    // recursion it feeds, which is the one shape the old throw let us skip.
    const std::expected<nlohmann::json, BlueprintError> childDoc = ReadFile(childSource);
    if (!childDoc)
        return std::unexpected(childDoc.error());

    state.stack.push_back(childSource);
    const std::expected<void, BlueprintError> flattened =
        FlattenInto(state, *childDoc, childSource, childPrefix, ComposeTransform(placement, local));
    state.stack.pop_back();
    if (!flattened)
        return std::unexpected(flattened.error());

    // The entry's claims apply to what the recursion just produced, and to nothing
    // else — an override is written where the edit was made and addresses downward
    // (§5). They land *after* the child flattened, so an override of a member the
    // child itself instances (`car_3/wheel_fl`) is reached by the same rule as a
    // direct one.
    LevelInstance claims;
    ReadInstanceClaims(entry, claims);
    if (claims.overrides.empty() && claims.removed.empty())
        return {};

    // Removed here, not held as a hole: these removals are authored in the file, so
    // every instance of it has the same member list, and the list *is* the index
    // NetIds are assigned from. A per-instance removal (from a level placing this
    // one) is different and leaves a hole — see StageInstance.
    for (std::size_t i = state.out->members.size(); i-- > first;)
    {
        const std::string_view path = state.out->members[i].name;
        if (!path.starts_with(childPrefix))
            continue;
        if (IsMemberRemoved(path.substr(childPrefix.size()), claims.removed))
        {
            state.declared.erase(state.out->members[i].name);
            state.out->members.erase(state.out->members.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    for (const auto &[memberPath, componentOverrides] : claims.overrides.items())
    {
        const std::string full = childPrefix + memberPath;

        BlueprintMemberDesc *member = nullptr;
        for (std::size_t i = first; i < state.out->members.size(); ++i)
        {
            if (state.out->members[i].name == full)
            {
                member = &state.out->members[i];
                break;
            }
        }

        if (member == nullptr)
        {
            // Dropped rather than refused, and banning renames is what makes that
            // clean: a missing member can only mean deliberate deletion, so there
            // is no second reading in which this discards a real edit (§6).
            Core::Log::Warn("Blueprint: '{}' overrides '{}', which '{}' no longer declares — dropped.", source,
                            full, childSource);
            continue;
        }

        // Resolved here, where both scopes are still known: `prefix` is where the
        // writing file's own entities live, `childPrefix` is the instance being
        // addressed. One step later there is no way to tell them apart.
        nlohmann::json qualified = componentOverrides;
        QualifyOverrideReferences(qualified, prefix, childPrefix);
        ApplyMemberOverride(*member, qualified, full);
    }

    return {};
}

std::expected<void, BlueprintError> FlattenInto(FlattenState &state, const nlohmann::json &doc,
                                                std::string_view source, const std::string &prefix,
                                                const ECS::Transform &placement)
{
    if (std::find(state.out->closure.begin(), state.out->closure.end(), source) == state.out->closure.end())
        state.out->closure.emplace_back(source);

    // The closure's systems, unioned. A nested blueprint's needs are this file's
    // needs too — spawning a lot spawns the cars, and a car's systems have to be
    // there for them.
    if (const auto systems = doc.find("systems"); systems != doc.end() && systems->is_array())
    {
        for (const auto &name : *systems)
        {
            if (!name.is_string())
                continue;
            std::string value = name.get<std::string>();
            if (std::find(state.out->systems.begin(), state.out->systems.end(), value) ==
                state.out->systems.end())
            {
                state.out->systems.push_back(std::move(value));
            }
        }
    }

    if (const auto entities = doc.find("entities"); entities != doc.end() && entities->is_array())
    {
        for (const auto &entity : *entities)
        {
            if (!entity.contains("name") || !entity.at("name").is_string() ||
                entity.at("name").get<std::string>().empty())
            {
                Core::Log::Error("Blueprint: '{}' has an entity with no name.", source);
                return std::unexpected(BlueprintError::MissingName);
            }

            BlueprintMemberDesc member;
            member.name = prefix + entity.at("name").get<std::string>();
            if (!state.declared.insert(member.name).second)
            {
                Core::Log::Error("Blueprint: '{}' declares two members named '{}'.", source, member.name);
                return std::unexpected(BlueprintError::DuplicateMember);
            }

            member.components = entity.value("components", nlohmann::json::object());
            QualifyReferences(member.components, prefix);
            member.parented = DeclaresParent(member.components);

            // Kept whether or not it is used below: an override applied after this
            // can flip `parented`, and reversing the decision needs the exact
            // transform the decision was made with.
            member.placement = placement;

            // The nested placement composes onto a member that is in its file's own
            // space. A parented one is already relative to a member that got the
            // composition, and applying it again would apply it twice.
            if (!member.parented)
            {
                ECS::Transform local;
                if (const auto it = member.components.find("Transform"); it != member.components.end())
                    local = TransformFromJson(*it);

                member.components["Transform"] = TransformToJson(ComposeTransform(placement, local));
            }

            state.out->members.push_back(std::move(member));
        }
    }

    if (const auto instances = doc.find("instances"); instances != doc.end() && instances->is_array())
    {
        for (const auto &entry : *instances)
        {
            const std::expected<void, BlueprintError> flattened =
                FlattenInstance(state, entry, source, prefix, placement);
            if (!flattened)
                return std::unexpected(flattened.error());
        }
    }

    return {};
}

} // namespace

std::string_view Describe(BlueprintError error)
{
    switch (error)
    {
    case BlueprintError::FileUnreadable:
        return "the file, or one it instances, could not be read";
    case BlueprintError::MalformedJson:
        return "the file is not readable JSON";
    case BlueprintError::UnsupportedVersion:
        return "the file is a version this build does not read";
    case BlueprintError::MissingName:
        return "an entity or instance in the file has no name";
    case BlueprintError::MissingSource:
        return "an instance in the file names no source";
    case BlueprintError::InstanceCycle:
        return "the file is reachable from itself by instancing";
    case BlueprintError::DuplicateMember:
        return "two members flatten to the same name";
    case BlueprintError::NonUniformScale:
        return "an instance has a non-uniform scale";
    case BlueprintError::ComponentRejected:
        return "a member holds component values the reflection layer refuses";
    }
    return "the file cannot be used";
}

BlueprintResult GetBlueprintDefinition(std::string_view source)
{
    const std::lock_guard lock(CacheMutex());

    auto &cache = Cache();
    if (const auto it = cache.find(source); it != cache.end())
        return it->second;

    auto definition    = std::make_shared<BlueprintDefinition>();
    definition->source = std::string{source};

    // Reading and flattening report failure by value; the try is here for the
    // *prepare* step and for nlohmann. Preparing deserializes each member, and a
    // generated deserializer reads a float as `j.at("fov").get<float>()` — so a
    // file saying "wide" where a number goes throws from the last line of the
    // build. That throw is a dependency's, caught here and turned into an error
    // the caller can read, because callers as ordinary as the editor's Save reach
    // this while walking a scene.
    //
    // Nothing is cached on failure, on either path: a blueprint that failed once
    // because its nested file was missing must be readable the moment somebody
    // adds it, rather than staying broken until the level reloads.
    try
    {
        const std::expected<nlohmann::json, BlueprintError> doc = ReadFile(source);
        if (!doc)
            return std::unexpected(doc.error());

        FlattenState state{.out = definition.get(), .stack = {std::string{source}}, .declared = {}};
        if (const std::expected<void, BlueprintError> flattened =
                FlattenInto(state, *doc, source, /*prefix=*/"", ECS::Transform{});
            !flattened)
        {
            return std::unexpected(flattened.error());
        }

        std::sort(definition->closure.begin(), definition->closure.end());
        definition->closure.erase(std::unique(definition->closure.begin(), definition->closure.end()),
                                  definition->closure.end());

        // Once, here — a blueprint is parsed and encoded one time, because spawning
        // a hundred bullets must not re-read and re-parse bullet.abp a hundred
        // times.
        if (!SceneSerializer::PrepareBlueprint(*definition))
            return std::unexpected(BlueprintError::ComponentRejected);
    }
    catch (const std::exception &ex)
    {
        Core::Log::Error("Blueprint: cannot use '{}': {}", source, ex.what());
        return std::unexpected(BlueprintError::ComponentRejected);
    }

    return cache.emplace(std::string{source}, std::move(definition)).first->second;
}

void ClearBlueprintCache()
{
    const std::lock_guard lock(CacheMutex());
    Cache().clear();
}

void InvalidateBlueprint(std::string_view source)
{
    const std::lock_guard lock(CacheMutex());

    // Everything whose closure names it, not just the file itself: a parking lot's
    // flattened member list *contains* the car's members, so editing the car
    // changes the lot's definition too.
    std::erase_if(Cache(),
                  [source](const auto &entry)
                  {
                      const std::vector<std::string> &closure = entry.second->closure;
                      return std::find(closure.begin(), closure.end(), source) != closure.end();
                  });
}

} // namespace Assisi::Runtime
