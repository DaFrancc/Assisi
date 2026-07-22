#!/usr/bin/env python3
"""reflect_types — the C++ type → codegen mapping for reflectgen.

Holds the TYPES table (each entry a TypeCodegen with serialize/deserialize
templates) plus the type-name sets and the UNSUPPORTED_TYPES diagnostic map that
the code generator consults. No parsing or emission logic lives here — just the
knowledge of which C++ types reflectgen can (de)serialize and how.
"""

from dataclasses import dataclass


@dataclass
class TypeCodegen:
    enum_value:  str  # Core::Reflect::FieldType::Xxx
    serialize:   str  # expression; {a} = member accessor e.g. "c.foo"
    deserialize: str  # statement;  {f} = field name, {a} = accessor


# Shared codegen for std::vector<Core::AssetPath> — an IIFE builds the JSON
# array on serialize; deserialize clears then repopulates. Defined once because
# several element-name spellings (unqualified/qualified) map to it.
_ASSET_PATH_VECTOR = TypeCodegen(
    'AssetPathVector',
    '[&]{{ nlohmann::json _arr = nlohmann::json::array(); '
    'for (const auto& _p : {a}) _arr.push_back(std::string(_p.View())); return _arr; }}()',
    '{{ if (j.contains("{f}")) {{ {a}.clear(); '
    'for (const auto& _e : j.at("{f}")) {{ Assisi::Core::AssetPath _p; '
    '_p.Assign(_e.get<std::string>()); {a}.push_back(_p); }} }} }}')


# Shared codegen for std::vector<Core::AssetId> — the material-override list.
# Each element is a { guid, path-hint } object built by SerializeAssetId; the
# hint is discarded on load. Mirrors _ASSET_PATH_VECTOR but routes through the
# AssetId JSON helpers instead of raw strings.
_ASSET_ID_VECTOR = TypeCodegen(
    'AssetIdVector',
    '[&]{{ nlohmann::json _arr = nlohmann::json::array(); '
    'for (const auto& _e : {a}) _arr.push_back(Assisi::Core::SerializeAssetId(_e)); return _arr; }}()',
    '{{ if (j.contains("{f}")) {{ {a}.clear(); '
    'for (const auto& _e : j.at("{f}")) {a}.push_back(Assisi::Core::DeserializeAssetId(_e)); }} }}')


# Serialize expressions produce values for json initializer lists.
# Deserialize statements read from j.at("{f}") and assign to comp.{f}.
#
# GLM quat: memory layout {x,y,z,w}, constructor glm::quat(w,x,y,z).
# We serialize as [w,x,y,z] to match math convention.
# The deserialize template's {a} is the write target (accessor). It is
# "comp.<field>" for a component and "a.<field>" for an AASSET instance, so
# every template must route writes through {a} — never a hardcoded "comp.".
TYPES: dict[str, TypeCodegen] = {
    'float':     TypeCodegen(
        'Float',
        '{a}',
        'if (j.contains("{f}")) {a} = j.at("{f}").get<float>();'),
    'double':    TypeCodegen(
        'Double',
        '{a}',
        'if (j.contains("{f}")) {a} = j.at("{f}").get<double>();'),
    'int32_t':   TypeCodegen(
        'Int32',
        '{a}',
        'if (j.contains("{f}")) {a} = j.at("{f}").get<int32_t>();'),
    'uint32_t':  TypeCodegen(
        'UInt32',
        '{a}',
        'if (j.contains("{f}")) {a} = j.at("{f}").get<uint32_t>();'),
    'int64_t':   TypeCodegen(
        'Int64',
        '{a}',
        'if (j.contains("{f}")) {a} = j.at("{f}").get<int64_t>();'),
    'uint64_t':  TypeCodegen(
        'UInt64',
        '{a}',
        'if (j.contains("{f}")) {a} = j.at("{f}").get<uint64_t>();'),
    'bool':      TypeCodegen(
        'Bool',
        '{a}',
        'if (j.contains("{f}")) {a} = j.at("{f}").get<bool>();'),
    'glm::vec2': TypeCodegen(
        'Vec2',
        '{{ {a}.x, {a}.y }}',
        '{{ if (j.contains("{f}")) {{ const auto& _v = j.at("{f}"); {a} = {{ _v[0].get<float>(), _v[1].get<float>() }}; }} }}'),
    'glm::vec3': TypeCodegen(
        'Vec3',
        '{{ {a}.x, {a}.y, {a}.z }}',
        '{{ if (j.contains("{f}")) {{ const auto& _v = j.at("{f}"); {a} = {{ _v[0].get<float>(), _v[1].get<float>(), _v[2].get<float>() }}; }} }}'),
    'glm::vec4': TypeCodegen(
        'Vec4',
        '{{ {a}.x, {a}.y, {a}.z, {a}.w }}',
        '{{ if (j.contains("{f}")) {{ const auto& _v = j.at("{f}"); {a} = {{ _v[0].get<float>(), _v[1].get<float>(), _v[2].get<float>(), _v[3].get<float>() }}; }} }}'),
    'glm::quat': TypeCodegen(
        'Quat',
        '{{ {a}.w, {a}.x, {a}.y, {a}.z }}',
        '{{ if (j.contains("{f}")) {{ const auto& _v = j.at("{f}"); {a} = glm::quat{{ _v[0].get<float>(), _v[1].get<float>(), _v[2].get<float>(), _v[3].get<float>() }}; }} }}'),
    # glm::mat4 — 16 floats in column-major order (m[col][row]). Serialized as a
    # flat JSON array of 16; glm::mat4's 16-scalar constructor consumes the same
    # column-major order, so the round-trip is exact.
    'glm::mat4': TypeCodegen(
        'Mat4',
        '{{ {a}[0][0], {a}[0][1], {a}[0][2], {a}[0][3], {a}[1][0], {a}[1][1], {a}[1][2], {a}[1][3], {a}[2][0], {a}[2][1], {a}[2][2], {a}[2][3], {a}[3][0], {a}[3][1], {a}[3][2], {a}[3][3] }}',
        '{{ if (j.contains("{f}")) {{ const auto& _v = j.at("{f}"); {a} = glm::mat4{{ _v[0].get<float>(), _v[1].get<float>(), _v[2].get<float>(), _v[3].get<float>(), _v[4].get<float>(), _v[5].get<float>(), _v[6].get<float>(), _v[7].get<float>(), _v[8].get<float>(), _v[9].get<float>(), _v[10].get<float>(), _v[11].get<float>(), _v[12].get<float>(), _v[13].get<float>(), _v[14].get<float>(), _v[15].get<float>() }}; }} }}'),
    # ECS::Entity — serialized as a stable serial index via SceneSerializer.
    # Accepts both qualified and unqualified names.
    'ECS::Entity': TypeCodegen(
        'EntityRef',
        '({a} != Assisi::ECS::NullEntity ? nlohmann::json(Assisi::Runtime::SceneSerializer::EntityToIndex({a}).value_or(~0ull)) : nlohmann::json(nullptr))',
        '{{ if (j.contains("{f}") && !j.at("{f}").is_null()) {{ {a} = Assisi::Runtime::SceneSerializer::IndexToEntity(j.at("{f}").get<uint64_t>()); }} else {{ {a} = Assisi::ECS::NullEntity; }} }}'),
    'Assisi::ECS::Entity': TypeCodegen(
        'EntityRef',
        '({a} != Assisi::ECS::NullEntity ? nlohmann::json(Assisi::Runtime::SceneSerializer::EntityToIndex({a}).value_or(~0ull)) : nlohmann::json(nullptr))',
        '{{ if (j.contains("{f}") && !j.at("{f}").is_null()) {{ {a} = Assisi::Runtime::SceneSerializer::IndexToEntity(j.at("{f}").get<uint64_t>()); }} else {{ {a} = Assisi::ECS::NullEntity; }} }}'),
    # Core::ShortString — a small fixed-capacity inline string (e.g. an entity
    # Name). Serialized as a JSON string of its view; Assign() re-imposes the
    # capacity on load. Same codegen as AssetPath but a distinct FieldType so the
    # editor renders a plain text box (no asset-browse button). Accepts every
    # spelling.
    'ShortString': TypeCodegen(
        'String',
        'std::string({a}.View())',
        '{{ if (j.contains("{f}")) {a}.Assign(j.at("{f}").get<std::string>()); }}'),
    'Core::ShortString': TypeCodegen(
        'String',
        'std::string({a}.View())',
        '{{ if (j.contains("{f}")) {a}.Assign(j.at("{f}").get<std::string>()); }}'),
    'Assisi::Core::ShortString': TypeCodegen(
        'String',
        'std::string({a}.View())',
        '{{ if (j.contains("{f}")) {a}.Assign(j.at("{f}").get<std::string>()); }}'),
    # Core::AssetPath — a fixed-capacity virtual asset path. Serialized as a JSON
    # string of its view; Assign() re-imposes the length limit on load. Accepts
    # both qualified and unqualified names.
    'AssetPath': TypeCodegen(
        'AssetPath',
        'std::string({a}.View())',
        '{{ if (j.contains("{f}")) {a}.Assign(j.at("{f}").get<std::string>()); }}'),
    'Assisi::Core::AssetPath': TypeCodegen(
        'AssetPath',
        'std::string({a}.View())',
        '{{ if (j.contains("{f}")) {a}.Assign(j.at("{f}").get<std::string>()); }}'),
    # std::vector<Core::AssetPath> — a variable-length list of virtual paths
    # (e.g. MeshRenderer's per-slot material overrides). Serialized as a JSON
    # array of strings; deserialize clears then rebuilds, so a shorter saved
    # array shrinks the vector rather than leaving stale tail entries. Accepts
    # unqualified and qualified element spellings (no internal-whitespace forms;
    # keep the declaration spelled like the keys below).
    'std::vector<AssetPath>':               _ASSET_PATH_VECTOR,
    'std::vector<Core::AssetPath>':         _ASSET_PATH_VECTOR,
    'std::vector<Assisi::Core::AssetPath>': _ASSET_PATH_VECTOR,
    # Core::AssetId — a stable GUID reference. Serialized as { guid, path-hint }
    # via the Core AssetId JSON helpers; deserialize reads the guid and discards
    # the hint (see AssetIdJson.hpp / decision D2). Accepts every spelling.
    'AssetId': TypeCodegen(
        'AssetId',
        'Assisi::Core::SerializeAssetId({a})',
        '{{ if (j.contains("{f}")) {a} = Assisi::Core::DeserializeAssetId(j.at("{f}")); }}'),
    'Core::AssetId': TypeCodegen(
        'AssetId',
        'Assisi::Core::SerializeAssetId({a})',
        '{{ if (j.contains("{f}")) {a} = Assisi::Core::DeserializeAssetId(j.at("{f}")); }}'),
    'Assisi::Core::AssetId': TypeCodegen(
        'AssetId',
        'Assisi::Core::SerializeAssetId({a})',
        '{{ if (j.contains("{f}")) {a} = Assisi::Core::DeserializeAssetId(j.at("{f}")); }}'),
    # std::vector<Core::AssetId> — MeshRenderer's per-slot material overrides.
    'std::vector<AssetId>':               _ASSET_ID_VECTOR,
    'std::vector<Core::AssetId>':         _ASSET_ID_VECTOR,
    'std::vector<Assisi::Core::AssetId>': _ASSET_ID_VECTOR,
}

# Field types whose codegen calls the Core AssetId JSON helpers; a generated file
# with any such field must include the helper header.
_ASSET_ID_TYPES = {
    'AssetId', 'Core::AssetId', 'Assisi::Core::AssetId',
    'std::vector<AssetId>', 'std::vector<Core::AssetId>', 'std::vector<Assisi::Core::AssetId>',
}

# EntityRef is meaningless in a standalone asset (there is no scene to resolve
# a serial index against), and its codegen references Runtime::SceneSerializer,
# which an asset's home module (e.g. Geometry) does not link. Forbid it.
_ENTITY_REF_TYPES = {'ECS::Entity', 'Assisi::ECS::Entity'}

# reflectgen is default-deny: any non-transient AFIELD whose type is not in
# TYPES is a hard generation error (see _check_unsupported), because emitting a
# stub that silently drops that field on every save is worse than failing the
# build. This dict is optional colour on that failure: a known-unsupported type
# mapped to a human reason ("no string codegen yet") produces a better message
# than the generic default. Fields listed here fail exactly like any other
# unknown type — the entry only improves the diagnostic.
UNSUPPORTED_TYPES: dict[str, str] = {
    # The engine requires explicit-width integer types everywhere, so the
    # implementation-defined spellings are rejected by name rather than silently
    # reflected. Previously `int` was a supported type while int64_t/uint64_t were
    # hard build errors — exactly backwards from the project's own rule.
    'int':                'use int32_t (or int64_t) — bare int has an implementation-defined width',
    'unsigned':           'use uint32_t (or uint64_t) — bare unsigned has an implementation-defined width',
    'unsigned int':       'use uint32_t (or uint64_t) — bare unsigned int has an implementation-defined width',
    'long':               'use int64_t — long is 32-bit on Windows and 64-bit elsewhere',
    'unsigned long':      'use uint64_t — unsigned long is 32-bit on Windows and 64-bit elsewhere',
    'long long':          'use int64_t',
    'unsigned long long': 'use uint64_t',
    'short':              'use int16_t',
    'unsigned short':     'use uint16_t',
    'char':               'use int8_t/uint8_t for a number, or Core::ShortString for text',
}
