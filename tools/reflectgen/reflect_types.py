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
    '{{ const nlohmann::json* _r = nullptr; '
    'if (Assisi::Core::Reflect::FindArray(j, _comp, "{f}", _r)) {{ {a}.clear(); '
    'for (const auto& _e : *_r) {{ if (!_e.is_string()) {{ '
    'Assisi::Core::Reflect::ReportBadField(_comp, "{f}", "an array of strings", *_r); return false; }} '
    'Assisi::Core::AssetPath _p; _p.Assign(_e.get<std::string>()); {a}.push_back(_p); }} }} '
    'else if (j.contains("{f}")) return false; }}')


# Shared codegen for std::vector<Core::AssetId> — the material-override list.
# Each element is a { guid, path-hint } object built by SerializeAssetId; the
# hint is discarded on load. Mirrors _ASSET_PATH_VECTOR but routes through the
# AssetId JSON helpers instead of raw strings.
_ASSET_ID_VECTOR = TypeCodegen(
    'AssetIdVector',
    '[&]{{ nlohmann::json _arr = nlohmann::json::array(); '
    'for (const auto& _e : {a}) _arr.push_back(Assisi::Core::SerializeAssetId(_e)); return _arr; }}()',
    '{{ const nlohmann::json* _r = nullptr; '
    'if (Assisi::Core::Reflect::FindArray(j, _comp, "{f}", _r)) {{ {a}.clear(); '
    'for (const auto& _e : *_r) {a}.push_back(Assisi::Core::DeserializeAssetId(_e)); }} '
    'else if (j.contains("{f}")) return false; }}')


# Shared codegen for Reflect::ComponentMask. Both directions route through the
# Core helpers, which own the bit<->name translation and the load-time warnings
# for names that no longer resolve (see ComponentMaskJson.hpp).
_COMPONENT_MASK = TypeCodegen(
    'ComponentMask',
    'Assisi::Core::Reflect::SerializeComponentMask({a})',
    '{{ const nlohmann::json* _r = nullptr; if (Assisi::Core::Reflect::FindField(j, "{f}", _r)) {a} = Assisi::Core::Reflect::DeserializeComponentMask(*_r); }}')


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
        'if (!Assisi::Core::Reflect::ReadFloat(j, _comp, "{f}", {a})) return false;'),
    'double':    TypeCodegen(
        'Double',
        '{a}',
        'if (!Assisi::Core::Reflect::ReadDouble(j, _comp, "{f}", {a})) return false;'),
    'int32_t':   TypeCodegen(
        'Int32',
        '{a}',
        'if (!Assisi::Core::Reflect::ReadInt32(j, _comp, "{f}", {a})) return false;'),
    'uint32_t':  TypeCodegen(
        'UInt32',
        '{a}',
        'if (!Assisi::Core::Reflect::ReadUInt32(j, _comp, "{f}", {a})) return false;'),
    'int64_t':   TypeCodegen(
        'Int64',
        '{a}',
        'if (!Assisi::Core::Reflect::ReadInt64(j, _comp, "{f}", {a})) return false;'),
    'uint64_t':  TypeCodegen(
        'UInt64',
        '{a}',
        'if (!Assisi::Core::Reflect::ReadUInt64(j, _comp, "{f}", {a})) return false;'),
    'bool':      TypeCodegen(
        'Bool',
        '{a}',
        'if (!Assisi::Core::Reflect::ReadBool(j, _comp, "{f}", {a})) return false;'),
    'glm::vec2': TypeCodegen(
        'Vec2',
        '{{ {a}.x, {a}.y }}',
        '{{ float _v[2] = {{ {a}.x, {a}.y }}; if (!Assisi::Core::Reflect::ReadFloatArray(j, _comp, "{f}", 2, _v)) return false; {a} = {{ _v[0], _v[1] }}; }}'),
    'glm::vec3': TypeCodegen(
        'Vec3',
        '{{ {a}.x, {a}.y, {a}.z }}',
        '{{ float _v[3] = {{ {a}.x, {a}.y, {a}.z }}; if (!Assisi::Core::Reflect::ReadFloatArray(j, _comp, "{f}", 3, _v)) return false; {a} = {{ _v[0], _v[1], _v[2] }}; }}'),
    'glm::vec4': TypeCodegen(
        'Vec4',
        '{{ {a}.x, {a}.y, {a}.z, {a}.w }}',
        '{{ float _v[4] = {{ {a}.x, {a}.y, {a}.z, {a}.w }}; if (!Assisi::Core::Reflect::ReadFloatArray(j, _comp, "{f}", 4, _v)) return false; {a} = {{ _v[0], _v[1], _v[2], _v[3] }}; }}'),
    'glm::quat': TypeCodegen(
        'Quat',
        '{{ {a}.w, {a}.x, {a}.y, {a}.z }}',
        '{{ float _v[4] = {{ {a}.w, {a}.x, {a}.y, {a}.z }}; if (!Assisi::Core::Reflect::ReadFloatArray(j, _comp, "{f}", 4, _v)) return false; {a} = glm::quat{{ _v[0], _v[1], _v[2], _v[3] }}; }}'),
    # glm::mat4 — 16 floats in column-major order (m[col][row]). Serialized as a
    # flat JSON array of 16; glm::mat4's 16-scalar constructor consumes the same
    # column-major order, so the round-trip is exact.
    'glm::mat4': TypeCodegen(
        'Mat4',
        '{{ {a}[0][0], {a}[0][1], {a}[0][2], {a}[0][3], {a}[1][0], {a}[1][1], {a}[1][2], {a}[1][3], {a}[2][0], {a}[2][1], {a}[2][2], {a}[2][3], {a}[3][0], {a}[3][1], {a}[3][2], {a}[3][3] }}',
        '{{ float _v[16] = {{ {a}[0][0], {a}[0][1], {a}[0][2], {a}[0][3], {a}[1][0], {a}[1][1], {a}[1][2], {a}[1][3], {a}[2][0], {a}[2][1], {a}[2][2], {a}[2][3], {a}[3][0], {a}[3][1], {a}[3][2], {a}[3][3] }}; if (!Assisi::Core::Reflect::ReadFloatArray(j, _comp, "{f}", 16, _v)) return false; {a} = glm::mat4{{ _v[0], _v[1], _v[2], _v[3], _v[4], _v[5], _v[6], _v[7], _v[8], _v[9], _v[10], _v[11], _v[12], _v[13], _v[14], _v[15] }}; }}'),
    # ECS::Entity — serialized through SceneSerializer, whose active context
    # decides whether the reference is a name (a level file), an index within a
    # moved set (entity migration) or a packed handle (an undo payload). Accepts
    # both qualified and unqualified spellings.
    'ECS::Entity': TypeCodegen(
        'EntityRef',
        'Assisi::Runtime::SceneSerializer::EntityToRef({a})',
        '{{ const nlohmann::json* _r = nullptr; if (Assisi::Core::Reflect::FindField(j, "{f}", _r)) {a} = Assisi::Runtime::SceneSerializer::RefToEntity(*_r); else {a} = Assisi::ECS::NullEntity; }}'),
    'Assisi::ECS::Entity': TypeCodegen(
        'EntityRef',
        'Assisi::Runtime::SceneSerializer::EntityToRef({a})',
        '{{ const nlohmann::json* _r = nullptr; if (Assisi::Core::Reflect::FindField(j, "{f}", _r)) {a} = Assisi::Runtime::SceneSerializer::RefToEntity(*_r); else {a} = Assisi::ECS::NullEntity; }}'),
    # ECS::InstanceId — a uint32 underneath and its own type on purpose, so the
    # codec can tell "which blueprint instance" from every other unsigned integer
    # and translate it to the instance's baseNetId on the wire. JSON carries the
    # raw number: a file has one id space, and an instance id in a save is only
    # ever read back by the same machine that wrote it. Both spellings accepted.
    # The bare spelling too: unlike ECS::Entity, whose fields all live in other
    # namespaces, InstanceId's own header declares one inside Assisi::ECS.
    'InstanceId': TypeCodegen(
        'InstanceRef',
        '{a}.value',
        '{{ std::uint32_t _n = {a}.value; if (!Assisi::Core::Reflect::ReadUInt32(j, _comp, "{f}", _n)) return false; {a} = Assisi::ECS::InstanceId{{ _n }}; }}'),
    'ECS::InstanceId': TypeCodegen(
        'InstanceRef',
        '{a}.value',
        '{{ std::uint32_t _n = {a}.value; if (!Assisi::Core::Reflect::ReadUInt32(j, _comp, "{f}", _n)) return false; {a} = Assisi::ECS::InstanceId{{ _n }}; }}'),
    'Assisi::ECS::InstanceId': TypeCodegen(
        'InstanceRef',
        '{a}.value',
        '{{ std::uint32_t _n = {a}.value; if (!Assisi::Core::Reflect::ReadUInt32(j, _comp, "{f}", _n)) return false; {a} = Assisi::ECS::InstanceId{{ _n }}; }}'),
    # Core::ShortString — a small fixed-capacity inline string (e.g. an entity
    # Name). Serialized as a JSON string of its view; Assign() re-imposes the
    # capacity on load. Same codegen as AssetPath but a distinct FieldType so the
    # editor renders a plain text box (no asset-browse button). Accepts every
    # spelling.
    'ShortString': TypeCodegen(
        'String',
        'std::string({a}.View())',
        '{{ std::string _s; if (!Assisi::Core::Reflect::ReadString(j, _comp, "{f}", _s)) return false; if (j.contains("{f}")) {a}.Assign(_s); }}'),
    'Core::ShortString': TypeCodegen(
        'String',
        'std::string({a}.View())',
        '{{ std::string _s; if (!Assisi::Core::Reflect::ReadString(j, _comp, "{f}", _s)) return false; if (j.contains("{f}")) {a}.Assign(_s); }}'),
    'Assisi::Core::ShortString': TypeCodegen(
        'String',
        'std::string({a}.View())',
        '{{ std::string _s; if (!Assisi::Core::Reflect::ReadString(j, _comp, "{f}", _s)) return false; if (j.contains("{f}")) {a}.Assign(_s); }}'),
    # Core::EntityName — the wider inline string an entity's name lives in. Same
    # codegen as ShortString, but its own FieldType: the binary codec reads into
    # the buffer by capacity, so a name decoded as a String would truncate.
    # Accepts every spelling.
    'EntityName': TypeCodegen(
        'EntityName',
        'std::string({a}.View())',
        '{{ std::string _s; if (!Assisi::Core::Reflect::ReadString(j, _comp, "{f}", _s)) return false; if (j.contains("{f}")) {a}.Assign(_s); }}'),
    'Core::EntityName': TypeCodegen(
        'EntityName',
        'std::string({a}.View())',
        '{{ std::string _s; if (!Assisi::Core::Reflect::ReadString(j, _comp, "{f}", _s)) return false; if (j.contains("{f}")) {a}.Assign(_s); }}'),
    'Assisi::Core::EntityName': TypeCodegen(
        'EntityName',
        'std::string({a}.View())',
        '{{ std::string _s; if (!Assisi::Core::Reflect::ReadString(j, _comp, "{f}", _s)) return false; if (j.contains("{f}")) {a}.Assign(_s); }}'),
    # Core::AssetPath — a fixed-capacity virtual asset path. Serialized as a JSON
    # string of its view; Assign() re-imposes the length limit on load. Accepts
    # both qualified and unqualified names.
    'AssetPath': TypeCodegen(
        'AssetPath',
        'std::string({a}.View())',
        '{{ std::string _s; if (!Assisi::Core::Reflect::ReadString(j, _comp, "{f}", _s)) return false; if (j.contains("{f}")) {a}.Assign(_s); }}'),
    'Assisi::Core::AssetPath': TypeCodegen(
        'AssetPath',
        'std::string({a}.View())',
        '{{ std::string _s; if (!Assisi::Core::Reflect::ReadString(j, _comp, "{f}", _s)) return false; if (j.contains("{f}")) {a}.Assign(_s); }}'),
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
        '{{ const nlohmann::json* _r = nullptr; if (Assisi::Core::Reflect::FindField(j, "{f}", _r)) {a} = Assisi::Core::DeserializeAssetId(*_r); }}'),
    'Core::AssetId': TypeCodegen(
        'AssetId',
        'Assisi::Core::SerializeAssetId({a})',
        '{{ const nlohmann::json* _r = nullptr; if (Assisi::Core::Reflect::FindField(j, "{f}", _r)) {a} = Assisi::Core::DeserializeAssetId(*_r); }}'),
    'Assisi::Core::AssetId': TypeCodegen(
        'AssetId',
        'Assisi::Core::SerializeAssetId({a})',
        '{{ const nlohmann::json* _r = nullptr; if (Assisi::Core::Reflect::FindField(j, "{f}", _r)) {a} = Assisi::Core::DeserializeAssetId(*_r); }}'),
    # std::vector<Core::AssetId> — MeshRenderer's per-slot material overrides.
    'std::vector<AssetId>':               _ASSET_ID_VECTOR,
    'std::vector<Core::AssetId>':         _ASSET_ID_VECTOR,
    'std::vector<Assisi::Core::AssetId>': _ASSET_ID_VECTOR,
    # Reflect::ComponentMask — a set of replicable component types, held as a
    # bitset but serialized as an array of component *names*: the bit index is a
    # replicable ordinal, which reshuffles whenever any component is added,
    # renamed, or has its capability flipped, so persisting bits would silently
    # re-aim an exclusion at the wrong component. Accepts every spelling.
    'ComponentMask':                       _COMPONENT_MASK,
    'Reflect::ComponentMask':              _COMPONENT_MASK,
    'Core::Reflect::ComponentMask':        _COMPONENT_MASK,
    'Assisi::Core::Reflect::ComponentMask': _COMPONENT_MASK,
}

# Field types whose codegen calls the Core AssetId JSON helpers; a generated file
# with any such field must include the helper header.
_ASSET_ID_TYPES = {
    'AssetId', 'Core::AssetId', 'Assisi::Core::AssetId',
    'std::vector<AssetId>', 'std::vector<Core::AssetId>', 'std::vector<Assisi::Core::AssetId>',
}

# Same idea for ComponentMask: its codegen calls the Core mask JSON helpers, so a
# generated file carrying one must include ComponentMaskJson.hpp.
_COMPONENT_MASK_TYPES = {
    'ComponentMask', 'Reflect::ComponentMask',
    'Core::Reflect::ComponentMask', 'Assisi::Core::Reflect::ComponentMask',
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
