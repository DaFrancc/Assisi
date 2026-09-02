add_library(Assisi-Runtime)
Assisi_apply_defaults(Assisi-Runtime)

target_sources(Assisi-Runtime
  PUBLIC
    "include/Assisi/Runtime/AssetResolve.hpp"
    "include/Assisi/Runtime/Blueprint.hpp"
    "include/Assisi/Runtime/Camera.hpp"
    "include/Assisi/Runtime/Components.hpp"
    "include/Assisi/Runtime/Hierarchy.hpp"
    "include/Assisi/Runtime/IndirectResolve.hpp"
    "include/Assisi/Runtime/LightComponents.hpp"
    "include/Assisi/Runtime/LightingSystem.hpp"
    "include/Assisi/Runtime/Lifecycle.hpp"
    "include/Assisi/Runtime/Naming.hpp"
    "include/Assisi/Runtime/Renderer.hpp"
    "include/Assisi/Runtime/SceneRenderer.hpp"
    "include/Assisi/Runtime/SceneSerializer.hpp"
    "include/Assisi/Runtime/SkyComponents.hpp"
    "include/Assisi/Runtime/SkyResolve.hpp"
  PRIVATE
    "src/AssetResolve.cpp"
    "src/Blueprint.cpp"
    "src/Camera.cpp"
    "src/Hierarchy.cpp"
    "src/LightingSystem.cpp"
    "src/Naming.cpp"
    "src/Renderer.cpp"
    "src/SceneRenderer.cpp"
    "src/SceneSerializerBlueprint.cpp"
    "src/SceneSerializerContext.hpp"
    "src/SceneSerializerFiles.cpp"
    "src/SceneSerializerInstances.cpp"
    "src/SceneSerializerInstances.hpp"
    "src/SceneSerializerLevel.cpp"
    "src/SceneSerializerRefs.cpp"
    "src/SkyResolve.cpp"
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
)

assisi_reflect(
  TARGET Assisi-Runtime
  HEADERS
    include/Assisi/Runtime/Components.hpp
    include/Assisi/Runtime/EditorOnly.hpp
    include/Assisi/Runtime/Hierarchy.hpp
    include/Assisi/Runtime/Lifecycle.hpp
    include/Assisi/Runtime/LightComponents.hpp
    include/Assisi/Runtime/NameComponent.hpp
    include/Assisi/Runtime/SkyComponents.hpp
)

add_library(Assisi::Runtime ALIAS Assisi-Runtime)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
