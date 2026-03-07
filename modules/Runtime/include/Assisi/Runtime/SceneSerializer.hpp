#pragma once

/// @file SceneSerializer.hpp
/// @brief JSON-based level file save/load for an ECS Scene.
///
/// All reflected (non-transient) component fields are persisted automatically
/// via ComponentRegistry.  Unrecognised component names in a file are skipped
/// with a warning so old levels remain loadable after component renames.
///
/// ## Format (version 1)
/// @code{.json}
/// {
///   "version": 1,
///   "entities": [
///     {
///       "components": {
///         "TransformComponent": { "position": [0,0,0], "rotation": [1,0,0,0], "scale": [1,1,1] },
///         "PointLightComponent": { "color": [1,1,1], "intensity": 100.0, "radius": 20.0 }
///       }
///     }
///   ]
/// }
/// @endcode
///
/// Entity IDs are not persisted; loading always clears the scene first and
/// allocates fresh sequential entities so generation numbers stay at zero.
///
/// ## Transient fields
/// Fields marked AFIELD(transient) (e.g. GPU handles, raw pointers) are
/// excluded from serialization.  Components where every field is transient
/// (e.g. MeshRendererComponent) are saved as an empty object `{}` — their
/// presence on the entity is preserved but no data is restored.

#include <filesystem>
#include <string_view>

#include <nlohmann/json.hpp>

#include <Assisi/ECS/Scene.hpp>

namespace Assisi::Runtime
{

class SceneSerializer
{
  public:
    /// @brief Serialize the entire scene to a JSON value.
    static nlohmann::json Save(ECS::Scene &scene);

    /// @brief Deserialize entities and components from a JSON value into the scene.
    ///
    /// Clears the scene before loading.  Only components registered in
    /// ComponentRegistry are restored; unrecognised names are skipped with a warning.
    static void Load(ECS::Scene &scene, const nlohmann::json &j);

    /// @brief Write the scene to a JSON file at the given filesystem path.
    ///
    /// @return true on success, false if the file could not be opened.
    static bool SaveToFile(ECS::Scene &scene, const std::filesystem::path &path);

    /// @brief Load the scene from an asset-relative path via AssetSystem.
    ///
    /// @param assetPath  Virtual path relative to the asset root (e.g. "levels/main.json").
    /// @return true on success, false on any IO or parse error.
    static bool LoadFromFile(ECS::Scene &scene, std::string_view assetPath);
};

} // namespace Assisi::Runtime