#!/usr/bin/env python3
"""reflect_codegen — emits the .generated.cpp registration code for reflectgen.

Consumes the parsed model (reflect_parser) and the type table (reflect_types) and
produces the C++ that registers each component/asset with the reflection
registries. This is also where default-deny lives: generate_cpp refuses (raises)
on any non-transient field it cannot serialize, on an EntityRef in an AASSET, on
out-of-range AFIELD bounds, or on a replication annotation that could not mean
anything (ACOMP(replicated, transient), AFIELD(norep) outside a replicated
component) — a silently dropped field would lose data on every save, and a
silently ignored wire annotation would leak or withhold state, both of which are
worse than a build error.
"""

from typing import Optional

from reflect_parser import FieldInfo, ComponentInfo
from reflect_types import (TypeCodegen, TYPES, UNSUPPORTED_TYPES,
                           _ASSET_ID_TYPES, _ENTITY_REF_TYPES)


def _indent(text: str, spaces: int) -> str:
    pad = ' ' * spaces
    return '\n'.join(pad + line if line.strip() else line for line in text.splitlines())


# FieldType enum values that accept AFIELD(min=/max=) bounds, mapped to the
# value range a bound may take. Floats use None ("any value"). Integer ranges
# are capped at ±2^24 — FieldMeta stores bounds as float, and that is the
# largest span where every integer is exactly representable, so a bound never
# silently shifts when it round-trips through the metadata.
NUMERIC_BOUND_RANGES: dict[str, Optional[tuple[int, int]]] = {
    'Float':  None,
    'Double': None,
    'Int':    (-2**24, 2**24),
    'Int32':  (-2**24, 2**24),
    'UInt32': (0, 2**24),
}


def _validate_bounds(f: FieldInfo, tc: Optional[TypeCodegen]) -> tuple[Optional[float], Optional[float]]:
    """Validate AFIELD(min=/max=) hints against the field's type.

    Everything wrong here is a hard generation error — a typo like
    AFIELD(min=O), a negative bound on an unsigned field, or a bound on a
    bool silently dropping the clamp is exactly the kind of quiet
    data-shape bug this generator refuses to emit.
    """
    raw_min = f.args.get('min')
    raw_max = f.args.get('max')
    if raw_min is None and raw_max is None:
        return None, None

    if tc is None or tc.enum_value not in NUMERIC_BOUND_RANGES:
        raise ValueError(f'AFIELD(min=/max=) on field "{f.name}" of type '
                         f'{f.cpp_type!r}: bounds only apply to numeric fields')

    allowed = NUMERIC_BOUND_RANGES[tc.enum_value]

    def parse(raw: Optional[str], key: str) -> Optional[float]:
        if raw is None:
            return None
        try:
            value = float(raw)
        except ValueError:
            raise ValueError(f'AFIELD({key}={raw!r}) on field "{f.name}" is not a number')
        if allowed is not None:
            lo, hi = allowed
            if not value.is_integer():
                raise ValueError(f'AFIELD({key}={raw}) on integer field "{f.name}" '
                                 f'must be a whole number')
            if not lo <= value <= hi:
                raise ValueError(f'AFIELD({key}={raw}) on field "{f.name}" is outside '
                                 f'the supported {f.cpp_type} bound range [{lo}, {hi}]')
        return value

    vmin = parse(raw_min, 'min')
    vmax = parse(raw_max, 'max')
    if vmin is not None and vmax is not None and vmin > vmax:
        raise ValueError(f'AFIELD on field "{f.name}": min={vmin:g} exceeds max={vmax:g}')
    return vmin, vmax


def _field_tc(f: FieldInfo) -> Optional[TypeCodegen]:
    """The codegen for a field. An AENUM enum synthesizes one that (de)serializes
    through its underlying integer (int64 on the wire, cast back to the enum);
    every other type comes from the TYPES table. Returns None for an unsupported
    type — the signal _check_unsupported turns into a hard error."""
    if f.enum_info is not None:
        return TypeCodegen(
            'Enum',
            'static_cast<std::int64_t>({a})',
            'if (j.contains("{f}")) {a} = static_cast<' + f.enum_info.fqn +
            '>(j.at("{f}").get<std::int64_t>());')
    return TYPES.get(f.cpp_type)


def _gen_field_meta(f: FieldInfo) -> str:
    tc        = _field_tc(f)
    ftype     = f'Assisi::Core::Reflect::FieldType::{tc.enum_value}' if tc else 'Assisi::Core::Reflect::FieldType::Unknown'
    transient = 'true' if f.args.has('transient') else 'false'
    norep     = 'true' if f.args.has('norep') else 'false'
    vmin, vmax = _validate_bounds(f, tc)

    bounds_active   = vmin is not None or vmax is not None
    enum_active     = f.enum_info is not None
    listener_active = f.radio is not None and f.radio.source != ''

    # FieldMeta's trailing members are positional (bounds, then the enum block —
    # enumConstants/enumSize/enumSigned — then the radio trio), so emitting a later
    # block forces every earlier block to be emitted at its default. Blocks that no
    # field needs are omitted, keeping unannotated fields at the short,
    # golden-stable initializer form.
    tail: list[str] = []

    if bounds_active or enum_active or listener_active:
        has_min = 'true' if vmin is not None else 'false'
        has_max = 'true' if vmax is not None else 'false'
        min_v   = f'{vmin}f' if vmin is not None else '0.f'
        max_v   = f'{vmax}f' if vmax is not None else '0.f'
        tail += [has_min, has_max, min_v, max_v]

    if enum_active or listener_active:
        if enum_active:
            consts = ', '.join(f'{{ "{n}", {v} }}' for n, v in f.enum_info.constants)
            tail.append(f'{{ {consts} }}')
            tail.append(str(f.enum_info.size))
            tail.append('true' if f.enum_info.is_signed else 'false')
        else:
            # Non-enum listener: empty enumConstants, size 0 (marks "not an enum").
            tail += ['{}', '0', 'false']

    if listener_active:
        values = ', '.join(str(v) for v in f.radio.values)
        tail += [
            f'"{f.radio.source}"',
            f'{{ {values} }}',
            f'Assisi::Core::Reflect::RadioBehavior::{f.radio.behavior}',
        ]

    base = f'{{ "{f.name}", {ftype}, offsetof(T, {f.name}), {transient}, {norep}'
    return base + ' }' if not tail else base + ', ' + ', '.join(tail) + ' }'


def _gen_flag_tail(serializable: bool, comp: ComponentInfo) -> str:
    """The trailing bool flags of a ComponentMeta initializer: serializable, then
    tracksChanges and replicated when the annotations ask for them.

    They are positional and defaulted, so a later flag forces every earlier one
    to be emitted — and an unannotated component stays at the short, one-line
    form the golden output pins. ACOMP(replicated) implies tracked (an untracked
    component's change tick reads as "unchanged" forever, so it would replicate
    once at spawn and then go silent); the implication lives here rather than in
    the parser so the annotation stays the single source of truth.
    """
    values = ['true' if serializable else 'false']
    names  = ['serializable']
    if comp.args.has('tracked') or comp.args.has('replicated'):
        values.append('true')
        names.append('tracksChanges')
    if comp.args.has('replicated'):
        values.append('true')
        names.append('replicated')

    lines = []
    for index, (value, name) in enumerate(zip(values, names)):
        text = value + (',' if index + 1 < len(values) else '')
        lines.append(text.ljust(11) + f'// {name}')
    return '\n        '.join(lines)


def _is_serializable(f: FieldInfo) -> bool:
    """A non-transient field reflectgen has codegen for (a TYPES entry or an enum)."""
    return not f.args.has('transient') and _field_tc(f) is not None


def _gen_serialize(fields: list[FieldInfo]) -> str:
    # Default-deny (enforced in generate_cpp) guarantees every non-transient
    # field has codegen, so there is no unsupported branch to emit.
    serializable = [f for f in fields if _is_serializable(f)]

    if not serializable:
        # Nothing to serialize — suppress unused-parameter warning.
        return '(void)ptr;\nreturn nlohmann::json{};'

    lines = ['const auto& c = *static_cast<const T*>(ptr);', 'return nlohmann::json{']
    for f in serializable:
        expr = _field_tc(f).serialize.format(a=f'c.{f.name}', f=f.name)
        lines.append(f'    {{ "{f.name}", {expr} }},')
    lines.append('};')
    return '\n'.join(lines)


def _gen_deserialize(fields: list[FieldInfo]) -> str:
    serializable = [f for f in fields if _is_serializable(f)]

    lines = [
        'auto& scene = *static_cast<Assisi::ECS::Scene*>(scene_ptr);',
        'Assisi::ECS::Entity e{entity_index, entity_gen};',
        'T comp{};',
    ]

    if not serializable:
        lines.append('(void)j;')
    else:
        for f in serializable:
            lines.append(_field_tc(f).deserialize.format(f=f.name, a=f'comp.{f.name}'))

    lines.append('(void)scene.Add(e, comp);')
    return '\n'.join(lines)


def _gen_deserialize_asset(fields: list[FieldInfo]) -> str:
    """Deserialize for an AASSET: write fields into a caller-owned instance
    (out_ptr), no scene/entity machinery. Per-field 'if present' so absent keys
    leave the instance's current value untouched (forward-compat)."""
    serializable = [f for f in fields if _is_serializable(f)]

    if not serializable:
        return '(void)j;\n(void)out_ptr;'

    lines = ['auto& a = *static_cast<T*>(out_ptr);']
    for f in serializable:
        lines.append(_field_tc(f).deserialize.format(f=f.name, a=f'a.{f.name}'))
    return '\n'.join(lines)


def _check_asset_fields(components: list[ComponentInfo], header_name: str) -> None:
    for comp in components:
        if not comp.is_asset:
            continue
        for f in comp.fields:
            if f.args.has('transient'):
                continue
            if f.cpp_type in _ENTITY_REF_TYPES:
                raise ValueError(
                    f"{header_name}: asset '{comp.name}' field '{f.name}' is an "
                    f"EntityRef, which AASSET types cannot serialize (no scene to "
                    f"resolve against). Remove it or mark it AFIELD(transient).")


def _check_replication(components: list[ComponentInfo], header_name: str) -> None:
    """Reject annotation combinations that would silently mean nothing.

    Wire gating is opt-in, so every mistake here fails the same way — the field
    or component quietly does not replicate — which is exactly the failure a
    generator should refuse to emit rather than leave for a live session to
    reveal. AFIELD(norep) outside a replicated component is the poster child: it
    reads as "keep this off the wire" while nothing about the type is on the wire
    to begin with.
    """
    for comp in components:
        where = f"{header_name}: {'asset' if comp.is_asset else 'component'} '{comp.name}'"

        if comp.args.has('replicated'):
            if comp.is_asset:
                raise ValueError(
                    f"{where} is marked AASSET(replicated), but assets do not replicate — "
                    f"replication is a per-entity, per-component protocol. Remove the flag.")
            if comp.args.has('transient'):
                raise ValueError(
                    f"{where} is marked ACOMP(replicated, transient). A transient component "
                    f"registers for a ComponentId only and has no serialization hooks, so "
                    f"there is nothing to put on the wire. Pick one.")

        for f in comp.fields:
            if not f.args.has('norep'):
                continue
            field_where = f"{header_name}: field '{comp.name}::{f.name}'"
            if f.args.has('transient'):
                raise ValueError(
                    f"{field_where} is marked AFIELD(transient, norep). A transient field is "
                    f"already excluded from every codec; norep says nothing further. Drop norep.")
            if not comp.args.has('replicated'):
                raise ValueError(
                    f"{field_where} is marked AFIELD(norep), but '{comp.name}' is not "
                    f"ACOMP(replicated) — nothing about it crosses the wire, so the annotation "
                    f"would silently do nothing. Mark the component replicated, or drop norep.")


def _gen_asset_block(comp: ComponentInfo) -> str:
    fqn      = '::'.join(comp.namespaces + [comp.name]) if comp.namespaces else comp.name
    var_name = f'_reflectgen_{comp.name}'
    field_metas = ',\n            '.join(_gen_field_meta(f) for f in comp.fields)
    serialize   = _indent(_gen_serialize(comp.fields), 12)
    deserialize = _indent(_gen_deserialize_asset(comp.fields), 12)

    return f"""\
// ── {comp.name} {'─' * max(0, 74 - len(comp.name))}
// AASSET: standalone asset type, registered with AssetTypeRegistry.
static const bool {var_name} = []() -> bool
{{
    using T = {fqn};
    Assisi::Core::Reflect::AssetTypeRegistry::Instance().Register({{
        "{comp.name}",
        typeid(T),
        {{
            {field_metas}
        }},
        [](const void* ptr) -> nlohmann::json
        {{
{serialize}
        }},
        [](const nlohmann::json& j, void* out_ptr)
        {{
{deserialize}
        }},
    }});
    return true;
}}();

"""


def generate_cpp(components: list[ComponentInfo], include_path: str) -> str:
    # Default-deny is enforced here (not only in main) so every path that emits
    # code — the CLI and direct callers such as the golden tests — refuses an
    # unserializable field rather than silently dropping it.
    _check_unsupported(components, include_path)
    _check_asset_fields(components, include_path)
    _check_replication(components, include_path)

    component_infos = [c for c in components if not c.is_asset]
    asset_infos     = [c for c in components if c.is_asset]

    has_entity_refs = any(
        f.cpp_type in _ENTITY_REF_TYPES
        for comp in component_infos
        if not comp.args.has('transient')  # id-only components serialize nothing
        for f in comp.fields
        if not f.args.has('transient')
    )

    # AssetId fields (in components or assets) call the Core AssetId JSON helpers,
    # so a generated file that has any must include their header.
    has_asset_ids = any(
        f.cpp_type in _ASSET_ID_TYPES
        for comp in components
        if not comp.args.has('transient')
        for f in comp.fields
        if not f.args.has('transient')
    )

    # Enum fields (de)serialize through a std::int64_t cast, which needs <cstdint>.
    has_enums = any(
        f.enum_info is not None
        for comp in components
        if not comp.args.has('transient')
        for f in comp.fields
        if not f.args.has('transient')
    )

    # Includes are conditional on what the header actually declares. An
    # asset-only header (e.g. Geometry's MaterialData) must NOT pull in
    # ComponentRegistry / ECS::Scene — its home module does not link ECS.
    includes = []
    if component_infos:
        includes.append('#include <Assisi/Core/Reflect/ComponentRegistry.hpp>')
        includes.append('#include <Assisi/ECS/Scene.hpp>')
        if has_entity_refs:
            includes.append('#include <Assisi/Runtime/SceneSerializer.hpp>')
    if asset_infos:
        includes.append('#include <Assisi/Core/Reflect/AssetTypeRegistry.hpp>')
    if has_asset_ids:
        includes.append('#include <Assisi/Core/AssetIdJson.hpp>')
    if has_enums:
        includes.append('#include <cstdint>')
    includes.append(f'#include <{include_path}>')
    include_block = '\n'.join(includes)

    blocks = []
    blocks.append(f"""\
// AUTO-GENERATED by reflectgen — do not edit.
// Source: {include_path}

{include_block}

namespace
{{
""")

    for comp in component_infos:
        fqn      = '::'.join(comp.namespaces + [comp.name]) if comp.namespaces else comp.name
        var_name = f'_reflectgen_{comp.name}'

        serial_yes = _gen_flag_tail(True, comp)
        serial_no  = _gen_flag_tail(False, comp)

        # ACOMP(transient): register only to receive a stable ComponentId (so a
        # Scene can store the type) with no serialization hooks. serializable is
        # false and every hook is null; consumers gate on ComponentMeta::
        # serializable. See RigidBody / DestroyTag.
        if comp.args.has('transient'):
            blocks.append(f"""\
// ── {comp.name} {'─' * max(0, 74 - len(comp.name))}
// ACOMP(transient): id-only registration, not serialized.
static const bool {var_name} = []() -> bool
{{
    using T = {fqn};
    Assisi::Core::Reflect::ComponentRegistry::Instance().Register({{
        "{comp.name}",
        typeid(T),
        {{}},      // fields: none reflected
        nullptr,   // serialize
        nullptr,   // addToScene
        nullptr,   // iterateEntities
        nullptr,   // getByEntity
        nullptr,   // construct
        nullptr,   // getMutable
        {serial_no}
    }});
    return true;
}}();

""")
            continue

        field_metas = ',\n            '.join(_gen_field_meta(f) for f in comp.fields)
        serialize   = _indent(_gen_serialize(comp.fields), 12)
        deserialize = _indent(_gen_deserialize(comp.fields), 12)

        blocks.append(f"""\
// ── {comp.name} {'─' * max(0, 74 - len(comp.name))}
static const bool {var_name} = []() -> bool
{{
    using T = {fqn};
    Assisi::Core::Reflect::ComponentRegistry::Instance().Register({{
        "{comp.name}",
        typeid(T),
        {{
            {field_metas}
        }},
        [](const void* ptr) -> nlohmann::json
        {{
{serialize}
        }},
        [](void* scene_ptr, uint32_t entity_index, uint32_t entity_gen, const nlohmann::json& j)
        {{
{deserialize}
        }},
        [](void* scene_ptr, std::function<void(uint32_t, uint32_t, const void*)> cb)
        {{
            auto& scene = *static_cast<Assisi::ECS::Scene*>(scene_ptr);
            for (auto [e, comp] : scene.Query<T>())
                cb(e.index, e.generation, &comp);
        }},
        [](void* scene_ptr, uint32_t entity_index, uint32_t entity_gen) -> const void*
        {{
            auto& scene = *static_cast<Assisi::ECS::Scene*>(scene_ptr);
            return scene.Get<T>(Assisi::ECS::Entity{{entity_index, entity_gen}});
        }},
        [](void* scene_ptr, uint32_t entity_index, uint32_t entity_gen) -> void*
        {{
            // Scene::Add rejects a duplicate rather than replacing it, so an
            // entity that already has this component is reset in place. Both
            // paths stamp the change tick for a tracked type.
            auto& scene = *static_cast<Assisi::ECS::Scene*>(scene_ptr);
            Assisi::ECS::Entity e{{entity_index, entity_gen}};
            if (T* existing = scene.GetMut<T>(e))
            {{
                *existing = T{{}};
                return existing;
            }}
            return scene.Add<T>(e, T{{}});
        }},
        [](void* scene_ptr, uint32_t entity_index, uint32_t entity_gen) -> void*
        {{
            // GetMut, not Get: this is the writing accessor, so it stamps.
            auto& scene = *static_cast<Assisi::ECS::Scene*>(scene_ptr);
            return scene.GetMut<T>(Assisi::ECS::Entity{{entity_index, entity_gen}});
        }},
        {serial_yes}
    }});
    return true;
}}();

""")

    for comp in asset_infos:
        blocks.append(_gen_asset_block(comp))

    blocks.append('} // namespace\n')
    return ''.join(blocks)


def _check_unsupported(components: list[ComponentInfo], header_name: str) -> None:
    """Default-deny: fail generation if any non-transient AFIELD has a type
    reflectgen cannot (de)serialize (i.e. absent from TYPES).

    A silently-skipped field round-trips to nothing — every save drops it —
    which is far worse than a build error. Mark the field AFIELD(transient) if
    it is runtime-only, or add its codegen to TYPES. UNSUPPORTED_TYPES supplies
    a richer reason string when one is known.
    """
    for comp in components:
        if comp.args.has('transient'):
            continue  # id-only component: its fields are never serialized
        for f in comp.fields:
            if f.args.has('transient'):
                continue
            if _field_tc(f) is None:
                reason = UNSUPPORTED_TYPES.get(f.cpp_type, 'no codegen for this type')
                raise ValueError(
                    f"{header_name}: field '{comp.name}::{f.name}' has type "
                    f"'{f.cpp_type}', which reflectgen cannot serialize ({reason}). "
                    f"Add its codegen to TYPES, mark it AFIELD(transient), or (for an "
                    f"enum class) annotate it AENUM().")
