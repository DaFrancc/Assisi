/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

// Fixture header for reflectgen's golden tests. Exercises the scalar, glm,
// enum, AssetPath/ShortString and path-vector field types, a transient field, a
// tracked (change-detection) component, a replicable component with a norep
// field, an empty component, namespaced components, comment stripping,
// nested-brace initializers, and EntityRef include emission. AssetId,
// EntityName and InstanceId have their own cases in test_reflectgen.py rather
// than living here; ComponentMask has none. If you change this header,
// regenerate the golden output:
//   REFLECTGEN_UPDATE_GOLDEN=1 python tools/reflectgen/tests/test_reflectgen.py

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Core/ShortString.hpp>
#include <Assisi/ECS/Entity.hpp>

// A commented-out annotation the parser MUST ignore. If comment stripping
// regresses, the generated output sprouts a phantom GhostComponent and the
// golden comparison fails loudly.
// ACOMP()
// struct GhostComponent { AFIELD() int32_t ghost = 0; };

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
// tracksChanges = true. Doubles as the component carrying the field types above.
ACOMP(tracked)
struct SampleAllTypes
{
    AFIELD() float f = 1.0f;
    AFIELD() double d = 2.0;
    AFIELD() int32_t i32 = 4;
    AFIELD() uint32_t u32 = 5;
    AFIELD() int64_t i64 = 6;
    AFIELD() uint64_t u64 = 7;
    AFIELD() bool flag = true;
    AFIELD() SampleShape shape = SampleShape::Sphere;
    AFIELD() glm::vec2 v2 = {1.0f, 2.0f};
    AFIELD() glm::vec3 v3 = {1.0f, 2.0f, 3.0f};
    AFIELD() glm::vec4 v4 = {1.0f, 2.0f, 3.0f, 4.0f};
    AFIELD() glm::quat q = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    AFIELD() glm::mat4 m = glm::mat4{1.0f};
    AFIELD() Assisi::Core::AssetPath assetPath; // fixed-capacity path, serialized as a string
    AFIELD() Assisi::Core::ShortString label; // fixed-capacity name, serialized as a string
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
    AFIELD() int32_t ignored = 0;
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

// ACOMP(replicable): grants the capability to cross the wire, which implies
// tracked — so its registration carries tracksChanges *and* replicable.
// `serverOnly` is AFIELD(norep): saved to disk like any other field, excluded
// from the binary codec, which is the one place the two serializers
// deliberately disagree.
ACOMP(replicable)
struct SampleReplicated
{
    AFIELD() float shared = 0.0f;
    AFIELD(norep) int32_t serverOnly = 0;
};

// Reflected containers: a flat vector, a vector of an AENUM enum (whose
// enumerators become the field's metadata), both map flavours, and the one
// permitted nesting level. The map fields are also what makes this struct
// non-standard-layout, so the generated file carries the offsetof pragma.
ACOMP()
struct SampleContainers
{
    AFIELD() std::vector<int32_t> numbers;
    AFIELD() std::vector<SampleMode> modes;
    AFIELD() std::vector<Assisi::Core::ShortString> labels;
    AFIELD() std::map<int32_t, float> weights;
    AFIELD() std::unordered_map<Assisi::Core::ShortString, int32_t> counts;
    AFIELD() std::unordered_map<Assisi::Core::ShortString, std::vector<SampleMode>> bindings;
};

} // namespace Assisi::Runtime
