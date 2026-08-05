/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/Blueprint.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Scene.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <stdexcept>
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

// ---------------------------------------------------------------------------
// The instance table
// ---------------------------------------------------------------------------

uint32_t InstanceTable::Add(BlueprintInstance instance)
{
    const uint32_t id = _nextId++;
    _rows.emplace(id, std::move(instance));
    return id;
}

const BlueprintInstance *InstanceTable::Find(uint32_t id) const
{
    const auto it = _rows.find(id);
    return it != _rows.end() ? &it->second : nullptr;
}

void InstanceTable::Remove(uint32_t id)
{
    _rows.erase(id);
}

std::vector<std::pair<uint32_t, const BlueprintInstance *>> InstanceTable::All() const
{
    // Sorted, because a save writes one entry per row and the file's byte content
    // must be a function of the world rather than of hash iteration order.
    std::vector<std::pair<uint32_t, const BlueprintInstance *>> out;
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

std::vector<ECS::Entity> MembersOf(ECS::Scene &scene, uint32_t instanceId)
{
    std::vector<ECS::Entity> members;
    if (instanceId == 0)
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

ECS::Entity FindMember(ECS::Scene &scene, const InstanceTable &table, uint32_t instanceId,
                       std::string_view name)
{
    const BlueprintInstance *row = FindInstance(table, instanceId);
    if (row == nullptr)
        return ECS::NullEntity;

    const BlueprintDefinition *definition = GetBlueprintDefinition(row->source);
    if (definition == nullptr)
        return ECS::NullEntity;

    // Once, here — after which the scan compares integers rather than strings per
    // entity, which is the whole reason the tag carries an index.
    const std::optional<uint32_t> index = definition->IndexOf(name);
    if (!index.has_value())
        return ECS::NullEntity;

    for (auto [entity, tag] : scene.Query<ECS::BlueprintMember>())
    {
        if (tag.instanceId == instanceId && tag.memberIndex == *index)
            return entity;
    }
    return ECS::NullEntity;
}

const BlueprintInstance *FindInstance(const InstanceTable &table, uint32_t instanceId,
                                      std::string_view expectedSource)
{
    const BlueprintInstance *row = table.Find(instanceId);
    if (row == nullptr)
        return nullptr;
    if (!expectedSource.empty() && row->source != expectedSource)
        return nullptr;
    return row;
}

std::vector<LevelInstance> InstancesForSave(const InstanceTable &table)
{
    std::vector<LevelInstance> out;
    for (const auto &[id, row] : table.All())
    {
        // A runtime spawn is not level content: it exists because something in the
        // game asked for it, and writing it into the file would make it authored
        // the next time the level loads.
        if (row->levelInstanceIndex < 0)
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
std::map<std::string, BlueprintDefinition, std::less<>> &Cache()
{
    static std::map<std::string, BlueprintDefinition, std::less<>> cache;
    return cache;
}

/// `prefix + name`, with a leading '/' on @p name stripped first.
///
/// A leading slash means "the file that wrote this", which for a reference
/// authored inside a file *is* that file — so it prefixes exactly like a plain
/// name here. The two only diverge for an override, where the writing file and the
/// file being addressed are different (§6).
std::string QualifyName(std::string_view prefix, std::string_view name)
{
    if (!name.empty() && name.front() == '/')
        name.remove_prefix(1);
    return std::string{prefix} + std::string{name};
}

} // namespace

void QualifyReferences(nlohmann::json &components, std::string_view prefix)
{
    if (prefix.empty() || !components.is_object())
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

            *it = QualifyName(prefix, it->get<std::string>());
        }
    }
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

/// Reads @p source through the asset system and parses it. Throws with a message
/// naming the file on any failure; the caller turns that into a log line.
nlohmann::json ReadFile(std::string_view source)
{
    const auto text = Core::AssetSystem::ReadText(source);
    if (!text)
        throw std::runtime_error(std::format("cannot read '{}'", source));

    nlohmann::json doc = nlohmann::json::parse(*text);
    if (doc.value("version", 0) != 2)
    {
        throw std::runtime_error(
            std::format("'{}' is version {} (this build reads version 2)", source, doc.value("version", 0)));
    }
    return doc;
}

struct FlattenState
{
    BlueprintDefinition                    *out;
    std::vector<std::string>                stack;   ///< Sources currently being flattened, for cycle detection.
    std::unordered_set<std::string>         declared; ///< Member names claimed so far.
};

void FlattenInto(FlattenState &state, const nlohmann::json &doc, std::string_view source,
                 const std::string &prefix, const ECS::Transform &placement);

/// One nested instance entry: resolve the source, compose the placement, recurse.
void FlattenInstance(FlattenState &state, const nlohmann::json &entry, std::string_view source,
                     const std::string &prefix, const ECS::Transform &placement)
{
    if (!entry.contains("name") || !entry.at("name").is_string())
        throw std::runtime_error(std::format("'{}' has an instance with no name", source));
    if (!entry.contains("source") || !entry.at("source").is_string())
        throw std::runtime_error(std::format("'{}' instance '{}' has no source", source,
                                             entry.at("name").get<std::string>()));

    const std::string name        = entry.at("name").get<std::string>();
    const std::string childSource = entry.at("source").get<std::string>();

    // Cycles hard-fail rather than being detected later: `a` containing `b`
    // containing `a` expands forever, and it is unrecoverable if missed (§4).
    if (std::find(state.stack.begin(), state.stack.end(), childSource) != state.stack.end())
    {
        std::string chain;
        for (const std::string &link : state.stack)
            chain += link + " -> ";
        throw std::runtime_error(std::format("instance cycle: {}{}", chain, childSource));
    }

    const ECS::Transform local = TransformFromJson(entry.value("transform", nlohmann::json::object()));

    // At every level, not just the outermost (§4). Clamping to an axis was
    // rejected: it lets the file say one thing while the game does another.
    if (!HasUniformScale(local))
    {
        throw std::runtime_error(std::format("'{}' instance '{}' has a non-uniform scale ({}, {}, {}); an "
                                             "instance may only translate, rotate, or scale uniformly",
                                             source, name, local.scale.x, local.scale.y, local.scale.z));
    }

    const std::string childPrefix = prefix + name + "/";
    const std::size_t first       = state.out->members.size();

    state.stack.push_back(childSource);
    FlattenInto(state, ReadFile(childSource), childSource, childPrefix, ComposeTransform(placement, local));
    state.stack.pop_back();

    // The entry's claims apply to what the recursion just produced, and to nothing
    // else — an override is written where the edit was made and addresses downward
    // (§5). They land *after* the child flattened, so an override of a member the
    // child itself instances (`car_3/wheel_fl`) is reached by the same rule as a
    // direct one.
    LevelInstance claims;
    ReadInstanceClaims(entry, claims);
    if (claims.overrides.empty() && claims.removed.empty())
        return;

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

        ApplyMemberOverride(*member, componentOverrides, full);
    }
}

void FlattenInto(FlattenState &state, const nlohmann::json &doc, std::string_view source,
                 const std::string &prefix, const ECS::Transform &placement)
{
    if (std::find(state.out->closure.begin(), state.out->closure.end(), source) == state.out->closure.end())
        state.out->closure.emplace_back(source);

    if (const auto entities = doc.find("entities"); entities != doc.end() && entities->is_array())
    {
        for (const auto &entity : *entities)
        {
            if (!entity.contains("name") || !entity.at("name").is_string() ||
                entity.at("name").get<std::string>().empty())
            {
                throw std::runtime_error(std::format("'{}' has an entity with no name", source));
            }

            BlueprintMemberDesc member;
            member.name = prefix + entity.at("name").get<std::string>();
            if (!state.declared.insert(member.name).second)
                throw std::runtime_error(std::format("'{}' declares two members named '{}'", source, member.name));

            member.components = entity.value("components", nlohmann::json::object());
            QualifyReferences(member.components, prefix);
            member.parented = DeclaresParent(member.components);

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
            FlattenInstance(state, entry, source, prefix, placement);
    }
}

} // namespace

const BlueprintDefinition *GetBlueprintDefinition(std::string_view source)
{
    auto &cache = Cache();
    if (const auto it = cache.find(source); it != cache.end())
        return &it->second;

    BlueprintDefinition definition;
    definition.source = std::string{source};

    try
    {
        FlattenState state{.out = &definition, .stack = {std::string{source}}, .declared = {}};
        FlattenInto(state, ReadFile(source), source, /*prefix=*/"", ECS::Transform{});
    }
    catch (const std::exception &ex)
    {
        // Nothing is cached on failure. A blueprint that failed once because its
        // nested file was missing must be readable the moment somebody adds it,
        // rather than staying broken until the level reloads.
        Core::Log::Error("Blueprint: cannot use '{}': {}", source, ex.what());
        return nullptr;
    }

    std::sort(definition.closure.begin(), definition.closure.end());
    definition.closure.erase(std::unique(definition.closure.begin(), definition.closure.end()),
                             definition.closure.end());

    return &cache.emplace(std::string{source}, std::move(definition)).first->second;
}

void ClearBlueprintCache()
{
    Cache().clear();
}

void InvalidateBlueprint(std::string_view source)
{
    // Everything whose closure names it, not just the file itself: a parking lot's
    // flattened member list *contains* the car's members, so editing the car
    // changes the lot's definition too.
    std::erase_if(Cache(),
                  [source](const auto &entry)
                  {
                      const std::vector<std::string> &closure = entry.second.closure;
                      return std::find(closure.begin(), closure.end(), source) != closure.end();
                  });
}

} // namespace Assisi::Runtime
