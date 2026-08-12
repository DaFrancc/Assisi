#!/usr/bin/env python3
"""reflect_codegen — emits the .generated.cpp registration code for reflectgen.

Consumes the parsed model (reflect_parser) and the type table (reflect_types) and
produces the C++ that registers each component/asset with the reflection
registries. This is also where default-deny lives: generate_cpp refuses (raises)
on any non-transient field it cannot serialize, on an EntityRef in an AASSET, on
out-of-range AFIELD bounds, or on a replication annotation that could not mean
anything (ACOMP(replicable, transient), AFIELD(norep) outside a replicable
component) — a silently dropped field would lose data on every save, and a
silently ignored wire annotation would leak or withhold state, both of which are
worse than a build error.

The spelling is `replicable`, not `replicated`, and the difference is the whole
point: the annotation grants a *capability* (this type has a wire form), while
whether any given entity actually sends it is policy decided elsewhere — the
Replicated marker's exclusion mask, and the game's neverReplicate list. See
docs/replication-optin-plan-v1.md. The old spelling is rejected by name rather
than ignored, because "unknown flag" and "this flag changed meaning" are
different problems for a reader to debug.
"""

from typing import Optional

from reflect_parser import FieldInfo, ComponentInfo, MessageInfo
from reflect_types import (TypeCodegen, TYPES, UNSUPPORTED_TYPES,
                           _ASSET_ID_TYPES, _COMPONENT_MASK_TYPES, _ENTITY_REF_TYPES)


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
            '{{ std::int64_t _n = static_cast<std::int64_t>({a}); '
            'if (!Assisi::Core::Reflect::ReadInt64(j, _comp, "{f}", _n)) return false; '
            '{a} = static_cast<' + f.enum_info.fqn + '>(_n); }}')
    return TYPES.get(f.cpp_type)


def _gen_field_meta(f: FieldInfo) -> str:
    tc        = _field_tc(f)
    ftype     = f'Assisi::Core::Reflect::FieldType::{tc.enum_value}' if tc else 'Assisi::Core::Reflect::FieldType::Unknown'
    transient = 'true' if f.args.has('transient') else 'false'
    norep     = 'true' if f.args.has('norep') else 'false'
    vmin, vmax = _validate_bounds(f, tc)

    bounds_active     = vmin is not None or vmax is not None
    enum_active       = f.enum_info is not None
    listener_active   = f.radio is not None and f.radio.source != ''
    controlled_active = f.args.has('controlled')

    # FieldMeta's trailing members are positional — bounds, then the enum block
    # (enumConstants/enumSize/enumSigned), then the radio trio, then controlled —
    # so emitting any block forces every *earlier* block to be emitted at its
    # default. Blocks nobody needs are omitted, which keeps an unannotated field
    # at the short, golden-stable initializer form.
    tail: list[str] = []

    if bounds_active or enum_active or listener_active or controlled_active:
        has_min = 'true' if vmin is not None else 'false'
        has_max = 'true' if vmax is not None else 'false'
        min_v   = f'{vmin}f' if vmin is not None else '0.f'
        max_v   = f'{vmax}f' if vmax is not None else '0.f'
        tail += [has_min, has_max, min_v, max_v]

    if enum_active or listener_active or controlled_active:
        if enum_active:
            consts = ', '.join(f'{{ "{n}", {v} }}' for n, v in f.enum_info.constants)
            tail.append(f'{{ {consts} }}')
            tail.append(str(f.enum_info.size))
            tail.append('true' if f.enum_info.is_signed else 'false')
        else:
            # Not an enum: empty enumConstants, size 0 (which marks "not an enum").
            tail += ['{}', '0', 'false']

    if listener_active or controlled_active:
        if listener_active:
            values = ', '.join(str(v) for v in f.radio.values)
            tail += [
                f'"{f.radio.source}"',
                f'{{ {values} }}',
                f'Assisi::Core::Reflect::RadioBehavior::{f.radio.behavior}',
            ]
        else:
            tail += ['""', '{}', 'Assisi::Core::Reflect::RadioBehavior::None']

    if controlled_active:
        tail.append('true')

    base = f'{{ "{f.name}", {ftype}, offsetof(T, {f.name}), {transient}, {norep}'
    return base + ' }' if not tail else base + ', ' + ', '.join(tail) + ' }'


def _gen_flag_tail(serializable: bool, comp: ComponentInfo) -> str:
    """The trailing bool flags of a ComponentMeta initializer: serializable, then
    tracksChanges and replicable when the annotations ask for them.

    They are positional and defaulted, so a later flag forces every earlier one
    to be emitted — and an unannotated component stays at the short, one-line
    form the golden output pins. ACOMP(replicable) implies tracked (an untracked
    component's change tick reads as "unchanged" forever, so it would replicate
    once at spawn and then go silent); the implication lives here rather than in
    the parser so the annotation stays the single source of truth.

    Writing both — ACOMP(replicable, tracked) — is legal and not redundant. The
    implication says replication needs the ticks; the explicit word says some
    *local* system needs them too, so that dropping `replicable` later cannot
    silently strip tracking out from under it. Transform is the live case:
    PropagateTransforms wants its ticks whether or not anything is networked.
    """
    values = ['true' if serializable else 'false']
    names  = ['serializable']
    if comp.args.has('tracked') or comp.args.has('replicable'):
        values.append('true')
        names.append('tracksChanges')
    if comp.args.has('replicable'):
        values.append('true')
        names.append('replicable')

    lines = []
    for index, (value, name) in enumerate(zip(values, names)):
        text = value + (',' if index + 1 < len(values) else '')
        lines.append(text.ljust(11) + f'// {name}')
    return '\n        '.join(lines)


def _is_serializable(f: FieldInfo) -> bool:
    """A non-transient field reflectgen has codegen for (a TYPES entry or an enum)."""
    return not f.args.has('transient') and _field_tc(f) is not None


# A message's EntityRef fields, for JSON only.
#
# The component path routes an EntityRef through Runtime::SceneSerializer, which
# resolves it against the scene being saved. A message has no scene and is never
# saved — its JSON form exists for tests, tooling, and log lines — so it carries
# the handle's two halves verbatim instead. The *binary* path is unaffected and
# still translates through NetIds, which is the only form that means anything
# across the wire.
MESSAGE_ENTITY_REF = TypeCodegen(
    'EntityRef',
    'nlohmann::json{{ {{ "index", {a}.index }}, {{ "generation", {a}.generation }} }}',
    'if (j.contains("{f}") && j.at("{f}").is_object()) {a} = Assisi::ECS::Entity{{ '
    'j.at("{f}").value("index", 0u), j.at("{f}").value("generation", 0u) }};')


def _message_field_tc(f: FieldInfo) -> Optional[TypeCodegen]:
    """Like _field_tc, but for a field of a message."""
    if f.cpp_type in _ENTITY_REF_TYPES:
        return MESSAGE_ENTITY_REF
    return _field_tc(f)


def _gen_message_serialize(fields: list[FieldInfo]) -> str:
    serializable = [f for f in fields if _is_serializable(f)]
    if not serializable:
        return '(void)ptr;\nreturn nlohmann::json{};'
    lines = ['const auto& c = *static_cast<const T*>(ptr);', 'return nlohmann::json{']
    for f in serializable:
        expr = _message_field_tc(f).serialize.format(a=f'c.{f.name}', f=f.name)
        lines.append(f'    {{ "{f.name}", {expr} }},')
    lines.append('};')
    return '\n'.join(lines)


def _gen_message_deserialize(fields: list[FieldInfo], name: str) -> str:
    serializable = [f for f in fields if _is_serializable(f)]
    if not serializable:
        return '(void)j;\n(void)out_ptr;\nreturn true;'
    lines = [f'constexpr const char* _comp = "{name}";',
             '(void)_comp;',
             'auto& a = *static_cast<T*>(out_ptr);']
    for f in serializable:
        lines.append(_message_field_tc(f).deserialize.format(f=f.name, a=f'a.{f.name}'))
    lines.append('return true;')
    return '\n'.join(lines)


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


def _gen_deserialize(fields: list[FieldInfo], name: str) -> str:
    """The addToScene body. Returns false without touching the scene when a field
    is present but unreadable — the component is never half-applied, because every
    field lands on a local `comp` and only a complete one reaches Scene::Add."""
    serializable = [f for f in fields if _is_serializable(f)]

    lines = [
        f'constexpr const char* _comp = "{name}";',
        '(void)_comp;',
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
    lines.append('return true;')
    return '\n'.join(lines)


def _gen_deserialize_asset(fields: list[FieldInfo], name: str) -> str:
    """Deserialize for an AASSET: write fields into a caller-owned instance
    (out_ptr), no scene/entity machinery. Per-field 'if present' so absent keys
    leave the instance's current value untouched (forward-compat).

    Unlike the component path this writes straight into the caller's instance, so
    a false return can leave the fields before the bad one already applied. The
    caller is handing in an instance it owns and is expected to drop it."""
    serializable = [f for f in fields if _is_serializable(f)]

    if not serializable:
        return '(void)j;\n(void)out_ptr;\nreturn true;'

    lines = [f'constexpr const char* _comp = "{name}";',
             '(void)_comp;',
             'auto& a = *static_cast<T*>(out_ptr);']
    for f in serializable:
        lines.append(_field_tc(f).deserialize.format(f=f.name, a=f'a.{f.name}'))
    lines.append('return true;')
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
    reveal. AFIELD(norep) outside a replicable component is the poster child: it
    reads as "keep this off the wire" while nothing about the type is on the wire
    to begin with.

    The retired `replicated` spelling is rejected here by name. It would
    otherwise parse as an unknown flag and be ignored, silently un-replicating a
    component that used to travel — the worst possible outcome for a rename whose
    entire purpose was to stop conflating capability with policy.
    """
    for comp in components:
        where = f"{header_name}: {'asset' if comp.is_asset else 'component'} '{comp.name}'"

        if comp.args.has('replicated'):
            raise ValueError(
                f"{where} is marked '{'AASSET' if comp.is_asset else 'ACOMP'}(replicated)', which "
                f"was renamed to 'replicable'. The flag grants a capability — this type *can* "
                f"cross the wire — while whether a given entity sends it is policy, held by the "
                f"Replicated marker's exclusion mask and the game's neverReplicate list. Write "
                f"'replicable'. See docs/replication-optin-plan-v1.md.")

        if comp.args.has('replicable'):
            if comp.is_asset:
                raise ValueError(
                    f"{where} is marked AASSET(replicable), but assets do not replicate — "
                    f"replication is a per-entity, per-component protocol. Remove the flag.")
            if comp.args.has('transient'):
                raise ValueError(
                    f"{where} is marked ACOMP(replicable, transient). A transient component "
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
            if not comp.args.has('replicable'):
                raise ValueError(
                    f"{field_where} is marked AFIELD(norep), but '{comp.name}' is not "
                    f"ACOMP(replicable) — nothing about it crosses the wire, so the annotation "
                    f"would silently do nothing. Mark the component replicable, or drop norep.")


def _check_messages(messages: list, header_name: str) -> None:
    """Reject message annotations that could not mean anything.

    The first two rejections are about field annotations whose whole purpose is
    to distinguish disk from wire — a distinction a message does not have,
    because a message *is* its wire form and is never saved anywhere. The third
    is about an annotation the dispatch site could not act on.
    """
    for msg in messages:
        # An event that is not `independent` is scoped by the entity it is
        # about: relevancy sends it to whoever can see that entity, and holds it
        # for whoever has not been told about it yet. With no entity reference
        # there is nothing to scope by, so the declaration is asking for two
        # incompatible things and the fix is one word either way.
        if msg.direction == 'event' and not msg.args.has('independent'):
            if not any(f.cpp_type in _ENTITY_REF_TYPES for f in msg.fields):
                raise ValueError(
                    f"{header_name}: message '{msg.name}' is an event that names no entity, but it is not "
                    f"marked independent. Relevancy scopes an event by the entity it is about — give it an "
                    f"EntityRef field, or write AMSG(event, {msg.reliability}, independent) if it genuinely "
                    f"concerns no entity (a round banner, a chat line).")

        for f in msg.fields:
            where = f"{header_name}: field '{msg.name}::{f.name}'"
            if f.args.has('norep'):
                raise ValueError(
                    f"{where} is marked AFIELD(norep), but '{msg.name}' is a message — it exists "
                    f"only to cross the wire, so a field that never crosses it is a field that "
                    f"does nothing. Remove the field, or remove norep.")
            if f.args.has('transient'):
                raise ValueError(
                    f"{where} is marked AFIELD(transient), but a message has no persistent form "
                    f"to be excluded from — nothing ever saves one. Remove the field, or remove "
                    f"transient.")
            if f.args.has('controlled'):
                if msg.direction != 'intent':
                    raise ValueError(
                        f"{where} is marked AFIELD(controlled), but '{msg.name}' is an event. "
                        f"The annotation means 'the sender must control this entity', and the "
                        f"sender of an event is the server, which controls everything by "
                        f"definition. It only applies to AMSG(intent, ...).")
                if f.cpp_type not in _ENTITY_REF_TYPES:
                    raise ValueError(
                        f"{where} is marked AFIELD(controlled) but its type is '{f.cpp_type}'. "
                        f"Only an EntityRef field can name an entity for the dispatch site to "
                        f"check control of.")


def _check_controlled_outside_messages(components: list, header_name: str) -> None:
    """AFIELD(controlled) on a component field would silently mean nothing.

    There is no sender for a component — it is state, and state has no dispatch
    site to reject it at — so the annotation would read as a rule being enforced
    while nothing enforced it.
    """
    for comp in components:
        for f in comp.fields:
            if f.args.has('controlled'):
                raise ValueError(
                    f"{header_name}: field '{comp.name}::{f.name}' is marked AFIELD(controlled), "
                    f"but '{comp.name}' is not a message. The annotation is a rule the intent "
                    f"dispatch site enforces about a sender, and a component has no sender.")


def _gen_message_block(msg: MessageInfo) -> str:
    var_name    = f'_reflectgen_msg_{msg.name}'
    field_metas = ',\n            '.join(_gen_field_meta(f) for f in msg.fields)
    serialize   = _indent(_gen_message_serialize(msg.fields), 12)
    deserialize = _indent(_gen_message_deserialize(msg.fields, msg.name), 12)

    direction   = 'Intent' if msg.direction == 'intent' else 'Event'
    reliability = 'Reliable' if msg.reliability == 'reliable' else 'Unreliable'
    independent = 'true' if msg.args.has('independent') else 'false'

    return f"""\
// ── {msg.name} {'─' * max(0, 74 - len(msg.name))}
// AMSG({msg.direction}, {msg.reliability}{', independent' if msg.args.has('independent') else ''})
static const bool {var_name} = []() -> bool
{{
    using T = {msg.fqn};
    Assisi::Core::Reflect::MessageRegistry::Instance().Register({{
        "{msg.name}",
        typeid(T),
        {{
            {field_metas}
        }},
        Assisi::Core::Reflect::MessageDirection::{direction},
        Assisi::Core::Reflect::MessageReliability::{reliability},
        {independent},
        Assisi::Core::Reflect::kInvalidMessageId, // id: assigned at finalize
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


def gen_message_forward(msg: MessageInfo) -> str:
    """A global-scope forward declaration of one message type.

    Forward-declared rather than included because specializing on an incomplete
    type is legal, and including the real headers would close a cycle: a header
    that declares handlers already includes the dispatch header the traits are
    part of.
    """
    return (''.join(f'namespace {ns} {{ ' for ns in msg.namespaces)
            + f'struct {msg.name}; '
            + ''.join('} ' for _ in msg.namespaces)).rstrip()


def gen_message_traits(msg: MessageInfo) -> str:
    """The compile-time half of one message's declaration.

    What makes `SendIntent(SomeEvent{...})` a compile error at the call site
    rather than a dropped packet at the receive site — and what makes passing a
    struct that was never declared a message fail with an incomplete type
    instead of silently encoding nothing.

    Emitted into a whole-tree header rather than beside the registration,
    because a specialization is only useful where the *call* is.
    """
    direction   = 'Intent' if msg.direction == 'intent' else 'Event'
    reliability = 'Reliable' if msg.reliability == 'reliable' else 'Unreliable'
    independent = 'true' if msg.args.has('independent') else 'false'

    return f"""\
template <>
struct MessageTraits<::{msg.fqn}>
{{
    static constexpr MessageDirection   direction   = MessageDirection::{direction};
    static constexpr MessageReliability reliability = MessageReliability::{reliability};
    static constexpr bool               independent = {independent};
}};

"""


def _gen_asset_block(comp: ComponentInfo) -> str:
    fqn      = '::'.join(comp.namespaces + [comp.name]) if comp.namespaces else comp.name
    var_name = f'_reflectgen_{comp.name}'
    field_metas = ',\n            '.join(_gen_field_meta(f) for f in comp.fields)
    serialize   = _indent(_gen_serialize(comp.fields), 12)
    deserialize = _indent(_gen_deserialize_asset(comp.fields, comp.name), 12)

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


def _gen_handler_block(handler) -> str:
    """Bind one AMSG_HANDLER declaration to its message type.

    Emitted **inside the handler's own namespace**, and that is the whole answer
    to "which function does this call". The message type is spelled exactly as
    the declaration spelled it and is resolved in exactly the scope the
    declaration was resolved in, so the binding names the same type the author
    named — there is no second lookup that could find something else. The
    handler itself is then written fully qualified and anchored at global scope,
    so nothing nearer can shadow it, and the address is taken through an
    explicit signature cast, so an overload set collapses to precisely the
    declared shape at compile time.

    Two handlers for one message type is caught by the whole-tree check, which is
    the only scope where that question can be asked — see reflectgen.py's
    --check-handlers.
    """
    open_ns  = ''.join(f'namespace {ns} {{ ' for ns in handler.namespaces)
    close_ns = ''.join('} ' for _ in handler.namespaces)
    scope    = '::'.join(['', *handler.namespaces, handler.name]) if handler.namespaces else f'::{handler.name}'
    var      = f'_reflectgen_bind_{handler.name}_{handler.message.replace("::", "_")}'

    return f"""\
// ── handler: {handler.name} → {handler.message} {'─' * max(0, 40 - len(handler.name) - len(handler.message))}
{open_ns}namespace {{
const bool {var} = []() -> bool
{{
    // Resolved in the declaration's own scope, so it names the same type the
    // declaration named — never whatever a second lookup elsewhere might find.
    using MsgT = {handler.message};
    Assisi::NetSync::MessageDispatch::Instance().Bind<MsgT>(
        static_cast<void (*)(Assisi::NetSync::NetContext &, const MsgT &)>(&{scope}));
    return true;
}}();
}} {close_ns}

"""


def gen_system_registration(system) -> str:
    """One ASYSTEM declaration's catalog entry.

    The declaration *is* the registration, and it lands in the module's generated
    OBJECT library — which cmake/AssisiReflect.cmake pulls fully into the final
    link precisely so a static initializer nobody references still runs. That is
    what replaces `registerGameSystems`: linking a module registers its systems.

    The function is wrapped in a lambda with an explicit fully-qualified call
    rather than taken by address, for the same reason handler binding is: nothing
    about which function runs should depend on name lookup.
    """
    names   = lambda values: '{' + ', '.join(f'"{v}"' for v in values) + '}'
    context = 'RenderContext' if system.is_render else 'SystemContext'
    run     = ('nullptr' if system.is_render else
               f'[](Assisi::App::SystemContext &ctx) {{ {system.fqn}(ctx); }}')
    render  = (f'[](Assisi::App::RenderContext &ctx) {{ {system.fqn}(ctx); }}' if system.is_render
               else 'nullptr')

    return f"""\
// ── {system.name} (system) {'─' * max(0, 65 - len(system.name))}
static const bool _reflectgen_system_{system.function} = []() -> bool
{{
    Assisi::App::SystemCatalog::Instance().Register({{
        "{system.name}",
        Assisi::App::SystemPhase::{system.phase if not system.is_render else 'Update'},
        {str(system.is_render).lower()},   // render phase — runs through RunRender, not Run
        {run},
        {render},
        {names(system.after)},
        {names(system.before)},
        {str(system.active_world_only).lower()},
    }});
    return true;
}}();

"""


def generate_cpp(components: list[ComponentInfo], include_path: str, messages: Optional[list] = None,
                 handlers: Optional[list] = None, systems: Optional[list] = None) -> str:
    messages = messages or []
    handlers = handlers or []
    systems  = systems or []

    # Default-deny is enforced here (not only in main) so every path that emits
    # code — the CLI and direct callers such as the golden tests — refuses an
    # unserializable field rather than silently dropping it.
    # Before the serialization checks, because "a view may not be stored" is the
    # more specific complaint and the one worth reading first.
    _check_no_instance_views(components, include_path)
    _check_no_instance_views(messages, include_path)
    _check_unsupported(components, include_path)
    _check_unsupported(messages, include_path)
    _check_asset_fields(components, include_path)
    _check_replication(components, include_path)
    _check_messages(messages, include_path)
    _check_controlled_outside_messages(components, include_path)

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

    # Likewise ComponentMask fields, which route through the Core mask helpers
    # that own the bit <-> component-name translation.
    has_component_masks = any(
        f.cpp_type in _COMPONENT_MASK_TYPES
        for comp in components
        if not comp.args.has('transient')
        for f in comp.fields
        if not f.args.has('transient')
    )

    # Enum fields (de)serialize through a std::int64_t cast, which needs <cstdint>.
    has_enums = any(
        f.enum_info is not None
        for comp in [*components, *messages]
        if not comp.args.has('transient')
        for f in comp.fields
        if not f.args.has('transient')
    )

    # Includes are conditional on what the header actually declares. An
    # asset-only header (e.g. Geometry's MaterialData) must NOT pull in
    # ComponentRegistry / ECS::Scene — its home module does not link ECS.
    includes = []
    # Unconditional: every deserialize body reads its fields through these, and
    # every kind of registration (component, asset, message) emits one.
    includes.append('#include <Assisi/Core/Reflect/JsonRead.hpp>')
    if component_infos:
        includes.append('#include <Assisi/Core/Reflect/ComponentRegistry.hpp>')
        includes.append('#include <Assisi/ECS/Scene.hpp>')
        if has_entity_refs:
            includes.append('#include <Assisi/Runtime/SceneSerializer.hpp>')
    if asset_infos:
        includes.append('#include <Assisi/Core/Reflect/AssetTypeRegistry.hpp>')
    if messages:
        includes.append('#include <Assisi/Core/Reflect/MessageRegistry.hpp>')
    if handlers:
        includes.append('#include <Assisi/NetSync/MessageDispatch.hpp>')
    if systems:
        includes.append('#include <Assisi/App/SystemCatalog.hpp>')
    if has_asset_ids:
        includes.append('#include <Assisi/Core/AssetIdJson.hpp>')
    if has_component_masks:
        includes.append('#include <Assisi/Core/Reflect/ComponentMaskJson.hpp>')
    if has_enums:
        includes.append('#include <cstdint>')
    includes.append(f'#include <{include_path}>')
    include_block = '\n'.join(includes)

    # Emitted only when there is a field to assert on, so a header that reflects
    # nothing but id-only components does not carry a declaration it never uses.
    view_ban = _gen_view_ban([*components, *messages])
    view_fwd = ('\n// Declared, never defined — the real one lives in '
                '<Assisi/Runtime/InstanceView.hpp>.\n'
                'namespace Assisi::Runtime { template <typename T> struct InstanceView; }\n'
                ) if view_ban else ''

    blocks = []
    blocks.append(f"""\
// AUTO-GENERATED by reflectgen — do not edit.
// Source: {include_path}

{include_block}
{view_fwd}
namespace
{{
{view_ban}""")

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
        deserialize = _indent(_gen_deserialize(comp.fields, comp.name), 12)

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

    for msg in messages:
        blocks.append(_gen_message_block(msg))

    blocks.append('} // namespace\n')

    # Handler bindings sit *outside* the file's anonymous namespace because each
    # one opens the handler's own namespace to resolve the message type there.
    # They keep their own inner anonymous namespace, so the registrar objects are
    # still internal to this translation unit.
    for handler in handlers:
        blocks.append('\n' + _gen_handler_block(handler))

    for system in systems:
        blocks.append('\n' + gen_system_registration(system))

    return ''.join(blocks)


def _gen_view_ban(owners: list) -> str:
    """The InstanceView storage ban, restated for the compiler.

    _check_no_instance_views matches spellings, and a spelling can lie: an alias
    declared in a header this one includes arrives at a regex parser as an
    ordinary word. decltype cannot be fooled that way, so every reflected field —
    transient ones included, since the objection is to storing a view at all —
    carries the question to the compiler as well.

    The primary template is redeclared rather than included: it is a declaration,
    it costs no include and no module dependency (Geometry reflects types and
    does not link Runtime), and if it ever stops matching the real one in
    <Assisi/Runtime/InstanceView.hpp>, this fails to compile saying so.
    """
    asserts = []
    for owner in owners:
        fqn = '::'.join(owner.namespaces + [owner.name]) if owner.namespaces else owner.name
        for f in owner.fields:
            # No message: the failing expression already names the field, and
            # the rule is four lines above it in this same file.
            asserts.append(
                f'static_assert(!_reflectgen_is_instance_view<decltype({fqn}::{f.name})>);')
    if not asserts:
        return ''

    body = '\n'.join(asserts)
    return f"""\
// ── The InstanceView storage ban {'─' * 45}
// Also checked by reflectgen, which matches spellings — and a spelling can lie:
// an alias declared in another header reaches the generator as an ordinary word,
// where decltype sees the type itself. A stored view is a member list that goes
// stale; keep the ECS::InstanceId and re-resolve with FindInstance<T>. Transient
// fields are asserted too: the objection is to storing one at all.
template <typename T> inline constexpr bool _reflectgen_is_instance_view = false;
template <typename T>
inline constexpr bool _reflectgen_is_instance_view<Assisi::Runtime::InstanceView<T>> = true;

{body}

"""


def _check_no_instance_views(components: list[ComponentInfo], header_name: str) -> None:
    """Default-deny an InstanceView field anywhere in a reflected type.

    Stronger than _check_unsupported below, and deliberately so: that one lets a
    field through once it is marked AFIELD(transient), and skips a transient
    component's fields entirely. Neither annotation is a way through here,
    because the problem is not that a view cannot be serialized — it is that a
    view *stored* anywhere is a member list that goes stale, which is the failure
    the whole blueprint design is built to prevent
    (docs/blueprint-system-concept.md §7). A view lives in the scope of the call
    that produced it; only the instance id may outlive it, and an ECS::InstanceId
    field is the supported way to say so.

    The ban is on the type, not on the spelling: an alias, an alias of an alias,
    and a struct that holds a view are all caught. The parser resolves those
    spellings within the header and records the chain in FieldInfo.view_via,
    which this quotes; the static_assert emitted alongside the registration
    covers the half no regex can see, an alias declared in some other header.
    """
    for comp in components:
        for f in comp.fields:
            bare = f.cpp_type.replace(' ', '')
            if 'InstanceView<' in bare:
                detail = ''
            elif f.view_via:
                detail = f" ({f.view_via})"
            else:
                continue
            raise ValueError(
                f"{header_name}: field '{comp.name}::{f.name}' has type "
                f"'{f.cpp_type}'{detail}. A reflected type may not hold an "
                f"InstanceView: the handles in one go stale, so storing it is a "
                f"member list by another name. Keep the ECS::InstanceId instead "
                f"and re-resolve with FindInstance<T> when you need the members.")


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
