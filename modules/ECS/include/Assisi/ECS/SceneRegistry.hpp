#pragma once

/// @file SceneRegistry.hpp
/// @brief Manages a named collection of Scenes with active-scene tracking.

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include <Assisi/ECS/Scene.hpp>

namespace Assisi::ECS
{

enum class SceneError
{
    NameAlreadyTaken, ///< Returned by Create() if the name is already in use.
    NotFound,         ///< Returned by SetActive() if no scene with that name exists.
};

struct SceneRegistry
{
    /// @brief Creates a new scene with the given name.
    ///
    /// @return Pointer to the new scene on success, or SceneError::NameAlreadyTaken
    ///         if a scene with that name already exists.
    [[nodiscard]] std::expected<Scene*, SceneError> Create(std::string_view name);

    /// @brief Destroys the named scene. Clears the active pointer if it was active.
    void Destroy(std::string_view name);

    Scene*       Get(std::string_view name);
    const Scene* Get(std::string_view name) const;
    bool         Has(std::string_view name) const;

    /// @brief Sets the active scene.
    ///
    /// @return SceneError::NotFound if no scene with that name exists.
    [[nodiscard]] std::expected<void, SceneError> SetActive(std::string_view name);

    Scene*       Active();
    const Scene* Active() const;

  private:
    std::unordered_map<std::string, std::unique_ptr<Scene>> _scenes;
    Scene* _active = nullptr;
};

} // namespace Assisi::ECS
