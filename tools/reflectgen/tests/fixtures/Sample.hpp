/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

// Fixture header for reflectgen's golden tests. Deliberately exercises every
// supported field type, a transient field, a tracked (change-detection)
// component, an empty component, namespaced components, comment stripping,
// nested-brace initializers, and EntityRef include emission. If you change this
// header, regenerate the golden output:
//   REFLECTGEN_UPDATE_GOLDEN=1 python tools/reflectgen/tests/test_reflectgen.py

#include <cstdint>
#include <vector>

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

// AENUM: an enum class usable as an AFIELD type. reflectgen records its
// enumerators so the field (de)serializes by value and the inspector shows a
// dropdown. Exercises implicit auto-increment and an explicit value.
AENUM()
enum class SampleShape : uint32_t
{
    Box,
    Sphere,
    Capsule = 5,
    Cylinder,
};

// ACOMP(tracked): opts into change detection, so its registration carries
// tracksChanges = true. Doubles as the "all supported field types" component.
ACOMP(tracked)
struct SampleAllTypes
{
    AFIELD() float f = 1.0f;
    AFIELD() double d = 2.0;
    AFIELD() int i = 3;
    AFIELD() int32_t i32 = 4;
    AFIELD() uint32_t u32 = 5;
    AFIELD() bool flag = true;
    AFIELD() SampleShape shape = SampleShape::Sphere;
    AFIELD() glm::vec2 v2 = {1.0f, 2.0f};
    AFIELD() glm::vec3 v3 = {1.0f, 2.0f, 3.0f};
    AFIELD() glm::vec4 v4 = {1.0f, 2.0f, 3.0f, 4.0f};
    AFIELD() glm::quat q = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    AFIELD() glm::mat4 m = glm::mat4{1.0f};
    AFIELD() Assisi::Core::AssetPath assetPath; // fixed-capacity path, serialized as a string
    AFIELD() std::vector<Assisi::Core::AssetPath> paths; // list of paths -> JSON string array
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

// id-only component: ACOMP(transient) registers for a ComponentId but emits no
// serialization hooks (serializable = false). Any AFIELD here is deliberately
// ignored — it proves the transient branch skips field codegen entirely.
ACOMP(transient)
struct SampleTransient
{
    AFIELD() int ignored = 0;
};

// Radio: declarative editor visibility. `mode` is a broadcaster
// (AFIELD(radioBroadcast) on an AENUM enum); `sub` is BOTH a listener (of mode)
// and a broadcaster (of level) — a chain. The listeners exercise both behaviors,
// single-value and set-value forms, and a bound coexisting with a radio object
// on the same field.
// 1-byte underlying: exercises reflectgen recording a non-default enum width so
// the inspector reads/writes it at 1 byte instead of assuming 4.
AENUM()
enum class SampleMode : std::uint8_t
{
    Off,
    Low,
    High,
};

AENUM()
enum class SampleSub
{
    A,
    B,
};

ACOMP()
struct SampleRadio
{
    AFIELD(radioBroadcast) SampleMode mode = SampleMode::Off;
    AFIELD(radioListen = {source = mode, value = High, behavior = vanish}) float intensity = 1.0f;
    AFIELD(radioBroadcast, radioListen = {source = mode, value = {Low, High}, behavior = vanish})
    SampleSub sub = SampleSub::A;
    AFIELD(min = 0, radioListen = {source = sub, value = B, behavior = grey}) int32_t level = 0;
};

} // namespace Assisi::Runtime
