/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

// Fixture header for reflectgen's golden tests. Deliberately exercises every
// supported field type, a transient field, an empty component, namespaced
// components, comment stripping, nested-brace initializers, and EntityRef
// include emission. If you change this header, regenerate the golden output:
//   REFLECTGEN_UPDATE_GOLDEN=1 python tools/reflectgen/tests/test_reflectgen.py

#include <cstdint>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/ECS/Entity.hpp>

// A commented-out annotation the parser MUST ignore. If comment stripping
// regresses, the generated output sprouts a phantom GhostComponent and the
// golden comparison fails loudly.
// ACOMP()
// struct GhostComponent { AFIELD() int ghost = 0; };

namespace Assisi::Runtime
{

ACOMP()
struct SampleAllTypes
{
    AFIELD() float f = 1.0f;
    AFIELD() double d = 2.0;
    AFIELD() int i = 3;
    AFIELD() int32_t i32 = 4;
    AFIELD() uint32_t u32 = 5;
    AFIELD() bool flag = true;
    AFIELD() glm::vec2 v2 = {1.0f, 2.0f};
    AFIELD() glm::vec3 v3 = {1.0f, 2.0f, 3.0f};
    AFIELD() glm::vec4 v4 = {1.0f, 2.0f, 3.0f, 4.0f};
    AFIELD() glm::quat q = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    AFIELD() glm::mat4 m = glm::mat4{1.0f};
    AFIELD() Assisi::Core::AssetPath assetPath; // fixed-capacity path, serialized as a string
    AFIELD(transient) float runtimeCache = 0.0f; // must not appear in (de)serialize
};

// Distinct component to prove the SceneSerializer include is emitted once an
// EntityRef field exists, and that multiple components in one header register.
ACOMP()
struct SampleRef
{
    AFIELD() ECS::Entity target = ECS::NullEntity;
};

// Empty component: exercises the no-fields serialize/deserialize branch.
ACOMP()
struct SampleEmpty
{
};

} // namespace Assisi::Runtime
