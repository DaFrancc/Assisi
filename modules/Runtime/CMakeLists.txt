add_library(Assisi-Runtime)
Assisi_apply_defaults(Assisi-Runtime)

target_sources(Assisi-Runtime
  PUBLIC
    "include/Assisi/Runtime/AnimatorGraph.hpp"
    "include/Assisi/Runtime/AssetResolve.hpp"
    "include/Assisi/Runtime/AudioEmitter.hpp"
    "include/Assisi/Runtime/Blueprint.hpp"
    "include/Assisi/Runtime/CookedScene.hpp"
    "include/Assisi/Runtime/Camera.hpp"
    "include/Assisi/Runtime/CameraPath.hpp"
    "include/Assisi/Runtime/Components.hpp"
    "include/Assisi/Runtime/IndirectResolve.hpp"
    "include/Assisi/Runtime/LightComponents.hpp"
    "include/Assisi/Runtime/LightingSystem.hpp"
    "include/Assisi/Runtime/Lifecycle.hpp"
    "include/Assisi/Runtime/LodSelection.hpp"
    "include/Assisi/Runtime/Naming.hpp"
    "include/Assisi/Runtime/Renderer.hpp"
    "include/Assisi/Runtime/SceneRenderer.hpp"
    "include/Assisi/Runtime/ShadowCasterGather.hpp"
    "include/Assisi/Runtime/SceneSerializer.hpp"
    "include/Assisi/Runtime/SkyComponents.hpp"
    "include/Assisi/Runtime/SkyResolve.hpp"
    "include/Assisi/Runtime/TimeOfDay.hpp"
  PRIVATE
    "src/AssetResolve.cpp"
    "src/AnimationPlayback.cpp"
    "src/AnimatorGraph.cpp"
    "src/SkinnedMeshPose.cpp"
    "src/Blueprint.cpp"
    "src/Camera.cpp"
    "src/CameraPath.cpp"
    "src/LightingSystem.cpp"
    "src/LodSelection.cpp"
    "src/Naming.cpp"
    "src/Renderer.cpp"
    "src/SceneRenderer.cpp"
    "src/SceneRendererShadows.cpp"
    "src/ShadowCasterGather.cpp"
    "src/SceneSerializerBlueprint.cpp"
    "src/SceneSerializerCooked.cpp"
    "src/SceneSerializerContext.hpp"
    "src/SceneSerializerDocument.cpp"
    "src/SceneSerializerFiles.cpp"
    "src/SceneSerializerText.cpp"
    "src/SceneSerializerInstances.cpp"
    "src/SceneSerializerInstances.hpp"
    "src/SceneSerializerLevel.cpp"
    "src/SceneSerializerRefs.cpp"
    "src/SkyResolve.cpp"
    "src/TimeOfDay.cpp"
)

target_include_directories(Assisi-Runtime
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Runtime
  PUBLIC
    Assisi::Core
    Assisi::ECS
    Assisi::Math
    Assisi::Render
    # Audio: the SoundHandle an AudioEmitter keeps for each sound it plays.
    Assisi::Audio
    # Sigil: the graphs and code an Animator runs.
    Assisi::Sigil
)

assisi_reflect(
  TARGET Assisi-Runtime
  HEADERS
    include/Assisi/Runtime/AudioEmitter.hpp
    include/Assisi/Runtime/CameraPath.hpp
    include/Assisi/Runtime/Components.hpp
    include/Assisi/Runtime/EditorOnly.hpp
    include/Assisi/Runtime/Lifecycle.hpp
    include/Assisi/Runtime/LightComponents.hpp
    include/Assisi/Runtime/NameComponent.hpp
    include/Assisi/Runtime/SkyComponents.hpp
    include/Assisi/Runtime/TimeOfDay.hpp
)

add_library(Assisi::Runtime ALIAS Assisi-Runtime)

# Compiling `.sgl` files into animator graphs: what the editor and the cook do,
# and a shipped game never does. A library of its own so the game's link can't
# reach the Sigil compiler or the glTF reader the skeleton check uses.
add_library(Assisi-RuntimeImport)
Assisi_apply_defaults(Assisi-RuntimeImport)

target_sources(Assisi-RuntimeImport
  PUBLIC
    "import/include/Assisi/Runtime/Import/AnimatorCompiler.hpp"
  PRIVATE
    "import/src/AnimatorCompiler.cpp"
)

target_include_directories(Assisi-RuntimeImport
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/import/include"
)

target_link_libraries(Assisi-RuntimeImport
  PUBLIC
    Assisi::Runtime
    Assisi::SigilCompile
  PRIVATE
    Assisi::GeometryImport
)

add_library(Assisi::RuntimeImport ALIAS Assisi-RuntimeImport)

# The animator kind loads in every build; cooking one compiles Sigil, so the
# cook step links the import library a shipped game never does.
assisi_asset_kind(
  TARGET      Assisi-Runtime
  LOAD        src/AnimatorGraphKind.cpp
  COOK        import/src/AnimatorGraphCook.cpp
  COOK_TARGET Assisi-RuntimeImport
)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
