/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/Naming.hpp>

#include <Assisi/Core/ShortString.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/NameComponent.hpp>

namespace Assisi::Runtime
{

std::string_view Describe(NameError error)
{
    switch (error)
    {
    case NameError::Empty:
        return "a name cannot be empty";
    case NameError::TooLong:
        return "a name is at most 32 characters";
    case NameError::ContainsSeparator:
        return "a name cannot contain '/' — that separates an instance from its member";
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
    if (name.size() > Core::kShortStringMax)
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

std::string UniqueEntityName(ECS::Scene &scene, std::string_view stem)
{
    return UniqueName(stem, [&scene](std::string_view candidate) { return EntityNameTaken(scene, candidate); });
}

} // namespace Assisi::Runtime
