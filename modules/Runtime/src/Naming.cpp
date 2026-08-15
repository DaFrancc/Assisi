/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/Naming.hpp>

#include <Assisi/Core/TrivialString.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/NameComponent.hpp>

#include <algorithm>

namespace Assisi::Runtime
{

std::string_view Describe(NameError error)
{
    switch (error)
    {
    case NameError::Empty:
        return "a name cannot be empty";
    case NameError::TooLong:
        // Spelled out rather than formatted, because this returns a view of a
        // literal. The assert is what keeps the number honest if the capacity moves.
        static_assert(Core::kEntityNameMax == 64, "the refusal below spells the limit");
        return "a name is at most 64 characters";
    case NameError::ContainsSeparator:
        return "a name cannot contain '/' — that separates an instance from its member";
    case NameError::Taken:
        return "another entity already has this name";
    }
    return "invalid name";
}

std::expected<void, NameError> ValidateName(std::string_view name)
{
    if (name.empty())
        return std::unexpected(NameError::Empty);

    // Refused rather than truncated: truncation is how two names become one, and
    // a reference that then picks the wrong entity is exactly what named
    // entities exist to prevent.
    if (name.size() > Core::kEntityNameMax)
        return std::unexpected(NameError::TooLong);

    if (name.find(kNameSeparator) != std::string_view::npos)
        return std::unexpected(NameError::ContainsSeparator);

    return {};
}

bool EntityNameTaken(ECS::Scene &scene, std::string_view name, ECS::Entity except)
{
    if (name.empty())
        return false; // "no name" is not a name, and any number of entities may have none

    for (auto [entity, entityName] : scene.Query<Name>())
    {
        if (entity != except && entityName.value.View() == name)
            return true;
    }
    return false;
}

namespace
{

/// Writes @p name onto @p entity, adding the component if it has none.
///
/// The only place in the engine that constructs a Name. Both doors end here, so
/// the name that was checked is the name that gets stored.
void StoreName(ECS::Scene &scene, ECS::Entity entity, std::string_view name)
{
    if (Name *existing = scene.GetMut<Name>(entity))
    {
        (void)existing->value.Assign(name);
        return;
    }
    (void)scene.Add(entity, Name{Core::EntityName{name}});
}

} // namespace

NameBatch::NameBatch(ECS::Scene &scene, std::span<const ECS::Entity> rebuilding) : _scene(scene)
{
    for (auto [entity, name] : scene.Query<Name>())
    {
        if (name.value.Empty())
            continue;
        if (std::find(rebuilding.begin(), rebuilding.end(), entity) != rebuilding.end())
            continue;
        _taken.emplace(name.value.View());
    }
}

std::string NameBatch::Give(ECS::Entity entity, std::string_view stem)
{
    std::string chosen = UniqueName(
        stem, [this](std::string_view candidate) { return _taken.contains(std::string{candidate}); });

    _taken.insert(chosen);
    StoreName(_scene, entity, chosen);
    return chosen;
}

std::string GiveEntityName(ECS::Scene &scene, ECS::Entity entity, std::string_view stem)
{
    return NameBatch{scene}.Give(entity, stem);
}

std::expected<void, NameError> CheckEntityName(ECS::Scene &scene, ECS::Entity entity, std::string_view name)
{
    if (name.empty())
        return {}; // clearing a name, not setting a bad one

    if (const std::expected<void, NameError> valid = ValidateName(name); !valid.has_value())
        return valid;

    if (EntityNameTaken(scene, name, entity))
        return std::unexpected(NameError::Taken);

    return {};
}

std::expected<void, NameError> RenameEntity(ECS::Scene &scene, ECS::Entity entity, std::string_view name)
{
    if (const std::expected<void, NameError> allowed = CheckEntityName(scene, entity, name);
        !allowed.has_value())
    {
        return allowed;
    }

    StoreName(scene, entity, name);
    return {};
}

} // namespace Assisi::Runtime
