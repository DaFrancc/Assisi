#!/usr/bin/env python3
"""reflect_parser — header scanning and the parse-time data model for reflectgen.

Reads a C++ header: strips comments, tracks the namespace stack, extracts the
ACOMP/AASSET/AENUM/AFIELD/AMSG/AMSG_HANDLER/ASYSTEM annotations, and resolves
enum-typed fields and the AFIELD(radio ...) references (with cycle detection).
Everything here is about *understanding* the header; nothing emits C++ (that is
reflect_codegen). The dataclasses below are the contract between the two.
"""

import re
from pathlib import Path
from dataclasses import dataclass, field
from typing import Optional

# ──────────────────────────────────────────────────────────────────────────────
# Data model
# ──────────────────────────────────────────────────────────────────────────────

@dataclass
class AnnotArgs:
    flags: set = field(default_factory=set)
    kvs:   dict = field(default_factory=dict)

    def has(self, flag: str) -> bool:
        return flag in self.flags

    def get(self, key: str, default=None):
        return self.kvs.get(key, default)


@dataclass
class EnumInfo:
    name:      str                 # unqualified, e.g. 'ColliderShape'
    fqn:       str                 # fully-qualified, e.g. 'Assisi::Physics::ColliderShape'
    constants: list                # list[tuple[str, int]] in declaration order
    size:      int  = 4            # underlying byte width: 1/2/4/8
    is_signed: bool = True         # underlying type is signed (default `int` is)


@dataclass
class RadioInfo:
    """Resolved AFIELD(radio ...) metadata for one field.

    A *broadcaster* (AFIELD(radioBroadcast) on an enum) sets is_broadcast. A
    *listener* (AFIELD(radioListen = { source, value, behavior })) sets
    source/values/behavior, with `values` already resolved from enumerator names
    to their integer values. A field can be BOTH — an enum that follows another
    broadcaster while broadcasting to its own listeners (a radio chain).
    """
    is_broadcast: bool = False     # AFIELD(radioBroadcast) flag on an enum field
    source:       str  = ''        # sibling broadcaster enum field name ('' = not a listener)
    values:       list = field(default_factory=list)  # list[int]: active enum values
    behavior:     str  = 'None'    # 'None' | 'Grey' | 'Vanish'


@dataclass
class FieldInfo:
    name:      str
    cpp_type:  str
    args:      AnnotArgs
    enum_info: Optional[EnumInfo]  = None  # set when cpp_type names an AENUM enum
    radio:     Optional[RadioInfo] = None  # set by _resolve_radio after parsing
    # Why this field's type reaches an InstanceView without spelling one — an
    # alias, an alias of an alias, a struct that holds one. Set from
    # find_view_spellings below; read by reflectgen's storage ban, which quotes it.
    view_via:  Optional[str]       = None


@dataclass
class ComponentInfo:
    name:       str
    namespaces: list   # e.g. ['Assisi', 'Runtime']
    args:       AnnotArgs
    fields:     list   # list[FieldInfo]
    is_asset:   bool = False  # True for AASSET (standalone asset), False for ACOMP


@dataclass
class MessageInfo:
    """One AMSG(direction, reliability[, extras]) struct.

    Direction and reliability are mandatory *positional* arguments in that
    order, so the declaration states the whole wire contract with no defaults to
    remember and no way for a changed default to silently reclassify an existing
    message. Everything after them is a flag.
    """
    name:        str
    namespaces:  list           # e.g. ['MyGame', 'Chat']
    direction:   str            # 'intent' | 'event'
    reliability: str            # 'reliable' | 'unreliable'
    args:        AnnotArgs      # the remaining flags, e.g. `independent`
    fields:      list           # list[FieldInfo]

    @property
    def fqn(self) -> str:
        return '::'.join(self.namespaces + [self.name]) if self.namespaces else self.name


@dataclass
class HandlerInfo:
    """One AMSG_HANDLER() declaration: `void Name(NetContext &, const T &);`

    The registration step other engines automate (Unity's generated RPC systems,
    Unreal's UHT) is automated here too — a declaration in a reflected header
    *is* the registration, and the generated table binds it by fully-qualified
    name with an explicit signature cast, so nothing about scoping is left to
    lookup rules.
    """
    name:       str    # unqualified function name
    namespaces: list   # enclosing namespaces at the declaration
    message:    str    # the message type as written, e.g. 'ChatSend' or 'A::B::ChatSend'
    header:     str    # source header, for diagnostics

    @property
    def fqn(self) -> str:
        return '::'.join(['', *self.namespaces, self.name]) if self.namespaces else f'::{self.name}'


@dataclass
class SystemInfo:
    """One ASYSTEM() declaration: `void Name(SystemContext &);`

    A system is (phase, name, function, ordering, scope) and data can supply only
    the name, so everything but the name lives here — on the function, rather than
    in a registration call elsewhere or restated in every level file. The
    declaration *is* the registration; linking a module registers its systems.
    """
    function:        str    # unqualified function name
    namespaces:      list   # enclosing namespaces at the declaration
    name:            str    # the name a file uses; defaults to function minus a trailing "System"
    phase:           str    # PreUpdate | FixedUpdate | Update | PostUpdate | Render
    after:           list   # system names this must run after
    before:          list   # system names this must run before
    active_world_only: bool
    header:          str    # source header, for diagnostics

    @property
    def fqn(self) -> str:
        return '::'.join(['', *self.namespaces, self.function]) if self.namespaces else f'::{self.function}'

    @property
    def is_render(self) -> bool:
        return self.phase == 'Render'


# ──────────────────────────────────────────────────────────────────────────────
# Annotation argument parser
# ──────────────────────────────────────────────────────────────────────────────

def _split_args(s: str) -> list[str]:
    """Split by comma, respecting quoted strings and {brace} nesting.

    Brace-awareness is what lets a single AFIELD argument carry a nested object
    with its own commas, e.g. `radio = { listen = shape, value = {A, B} }` — only
    top-level commas separate arguments. The same helper splits that nested body.
    """
    result, current, in_quote, qchar, depth = [], [], False, '', 0
    for ch in s:
        if in_quote:
            current.append(ch)
            if ch == qchar:
                in_quote = False
        elif ch in ('"', "'"):
            in_quote, qchar = True, ch
            current.append(ch)
        elif ch == '{':
            depth += 1
            current.append(ch)
        elif ch == '}':
            depth -= 1
            current.append(ch)
        elif ch == ',' and depth == 0:
            result.append(''.join(current))
            current = []
        else:
            current.append(ch)
    if current:
        result.append(''.join(current))
    return result


def parse_annot_args(content: str) -> AnnotArgs:
    """Parse the argument list inside ACOMP(...) or AFIELD(...)."""
    args = AnnotArgs()
    for token in _split_args(content):
        token = token.strip()
        if not token:
            continue
        if '=' in token:
            k, _, v = token.partition('=')
            args.kvs[k.strip()] = v.strip().strip("\"'")
        else:
            args.flags.add(token)
    return args


# ──────────────────────────────────────────────────────────────────────────────
# Comment stripping
# ──────────────────────────────────────────────────────────────────────────────

def strip_comments(text: str) -> str:
    """Remove C and C++ comments while preserving line structure."""
    result, i, n = [], 0, len(text)
    while i < n:
        if text[i:i+2] == '//':
            while i < n and text[i] != '\n':
                result.append(' ')
                i += 1
        elif text[i:i+2] == '/*':
            result.append(' ')
            result.append(' ')
            i += 2
            while i < n and text[i:i+2] != '*/':
                result.append('\n' if text[i] == '\n' else ' ')
                i += 1
            if i < n:
                result.append(' ')
                result.append(' ')
                i += 2
        else:
            result.append(text[i])
            i += 1
    return ''.join(result)


# ──────────────────────────────────────────────────────────────────────────────
# Header parser
# ──────────────────────────────────────────────────────────────────────────────

_ACOMP_RE  = re.compile(r'\bACOMP\s*\(([^)]*)\)')
_AASSET_RE = re.compile(r'\bAASSET\s*\(([^)]*)\)')
_AENUM_RE  = re.compile(r'\bAENUM\s*\(([^)]*)\)')
_AFIELD_RE = re.compile(r'\bAFIELD\s*\(([^)]*)\)')
_AMSG_RE   = re.compile(r'\bAMSG\s*\(([^)]*)\)')

# AMSG_HANDLER() followed by exactly one shape:
#   void Name(NetContext &ctx, const MessageType &msg);
# Rigid on purpose. The rigidity is what keeps this a fixed-shape pattern match
# rather than a C++ signature parser — extracting one type from one fixed
# pattern does not reopen the function-parsing problem that AMSG-as-a-struct
# exists to avoid, because the wire form still comes entirely from the struct.
# Parameter names are optional (a declaration may omit them) and the reference
# markers may sit against either token.
_AMSG_HANDLER_RE = re.compile(
    r'\bAMSG_HANDLER\s*\(\s*\)\s*'
    r'(?P<ret>[\w:]+)\s+'                       # return type (must be void)
    r'(?P<name>\w+)\s*\('
    r'\s*(?P<ctx>[\w:]+)\s*&\s*\w*\s*,'         # NetContext &ctx
    r'\s*const\s+(?P<msg>[\w:]+)\s*&\s*\w*\s*'  # const T &msg
    r'\)\s*;'
)

# ASYSTEM(phase, ...) followed by one fixed shape:
#   void Name(SystemContext &ctx);
# Rigid for the same reason AMSG_HANDLER is: extracting a phase and a name from a
# fixed pattern does not reopen the C++-signature-parsing problem, and every
# system has the same signature by construction — the context type is decided by
# the phase, not by the author.
_ASYSTEM_RE = re.compile(
    r'\bASYSTEM\s*\((?P<args>[^)]*)\)\s*'
    r'(?:inline\s+)?'                      # a header-only system is still a system
    r'(?P<ret>[\w:]+)\s+'                  # return type (must be void)
    r'(?P<fn>\w+)\s*\('
    r'\s*(?P<ctx>[\w:]+)\s*&\s*\w*\s*'     # SystemContext &ctx / RenderContext &ctx
    r'\)\s*;'
)

_ASYSTEM_PHASES = ('PreUpdate', 'FixedUpdate', 'Update', 'PostUpdate', 'Render')
_ASYSTEM_FLAGS  = {'activeWorldOnly'}
_ASYSTEM_KEYS   = {'name', 'after', 'before'}

# Both AMSG positional arguments, in the order the grammar fixes.
_AMSG_DIRECTIONS   = ('intent', 'event')
_AMSG_RELIABILITY  = ('reliable', 'unreliable')
_AMSG_EXTRA_FLAGS  = {'independent'}
_STRUCT_RE = re.compile(r'\bstruct\s+(\w+)')
# `enum class Name` / `enum struct Name`, with an optional `: underlying`. Both
# the name (group 1) and the underlying-type spelling (group 2, up to the body's
# `{`) are captured; the brace body is extracted separately.
_ENUM_RE   = re.compile(r'\benum\s+(?:class|struct)\s+(\w+)\s*(?::\s*([^{]+))?')
_NS_RE     = re.compile(r'\bnamespace\s+([\w:]+)')

# Underlying-type spelling → (byte width, is_signed). Whitespace is normalised to
# single spaces before lookup. `long` and `unsigned long` are absent on purpose,
# so their platform-dependent width fails the build (see _enum_underlying).
_ENUM_UNDERLYING: dict[str, tuple[int, bool]] = {
    'char':                   (1, True),   # impl-defined signedness; treat as signed
    'signed char':            (1, True),
    'unsigned char':          (1, False),
    'int8_t':                 (1, True),  'std::int8_t':  (1, True),
    'uint8_t':                (1, False), 'std::uint8_t': (1, False),
    'short':                  (2, True),  'short int':    (2, True),  'signed short': (2, True),
    'unsigned short':         (2, False), 'unsigned short int': (2, False),
    'int16_t':                (2, True),  'std::int16_t': (2, True),
    'uint16_t':               (2, False), 'std::uint16_t': (2, False),
    'int':                    (4, True),  'signed':       (4, True),  'signed int':   (4, True),
    'unsigned':               (4, False), 'unsigned int': (4, False),
    'int32_t':                (4, True),  'std::int32_t': (4, True),
    'uint32_t':               (4, False), 'std::uint32_t': (4, False),
    'long long':              (8, True),  'signed long long': (8, True), 'long long int': (8, True),
    'unsigned long long':     (8, False), 'unsigned long long int': (8, False),
    'int64_t':                (8, True),  'std::int64_t': (8, True),
    'uint64_t':               (8, False), 'std::uint64_t': (8, False),
}


def _enum_underlying(underlying: Optional[str], enum_name: str, header_name: str) -> tuple[int, bool]:
    """Map an `enum class` underlying-type spelling to (byte width, is_signed).

    None means no `: type` was given, so the C++ default underlying for a scoped
    enum applies: `int` (4-byte signed). An unrecognised spelling — including the
    platform-dependent `long` — is a hard build error, since the editor must know
    the exact width to read/write the field without corrupting neighbours."""
    if underlying is None:
        return 4, True
    key = ' '.join(underlying.split())
    if key not in _ENUM_UNDERLYING:
        raise ValueError(
            f"{header_name}: AENUM enum '{enum_name}' has underlying type '{key}', which "
            f"reflectgen does not support. Use a fixed-width integer type (int8_t, uint16_t, "
            f"int32_t, uint64_t, ...) or the default `int`; 'long' is platform-dependent.")
    return _ENUM_UNDERLYING[key]


def parse_enum_constants(body: str) -> list:
    """Parse an enum body into [(name, value), ...] in declaration order.

    Handles implicit auto-increment and explicit integer values (decimal or
    0x-hex, possibly negative). A non-integer initializer (e.g. referencing
    another constant or an expression) is a hard error — reflectgen needs the
    concrete value to serialize by number and to drive the editor combo.
    """
    constants: list = []
    next_value = 0
    for raw in body.split(','):
        entry = raw.strip()
        if not entry:
            continue
        if '=' in entry:
            enum_name, _, raw_value = entry.partition('=')
            enum_name = enum_name.strip()
            try:
                value = int(raw_value.strip(), 0)
            except ValueError:
                raise ValueError(f"AENUM enumerator '{enum_name}' has a non-integer "
                                 f"value '{raw_value.strip()}'; only integer literals are supported")
        else:
            enum_name = entry
            value = next_value
        constants.append((enum_name, value))
        next_value = value + 1
    return constants


# ──────────────────────────────────────────────────────────────────────────────
# Radio (declarative editor visibility) parsing + validation
# ──────────────────────────────────────────────────────────────────────────────

# behavior spelling (lower-case in the annotation) → FieldMeta enumerator.
_RADIO_BEHAVIORS = {'grey': 'Grey', 'vanish': 'Vanish'}
_RADIO_KEYS      = {'source', 'value', 'behavior'}


def _parse_value_list(raw: str) -> list[str]:
    """Parse a radio `value` — a single enumerator `E1` or a set `{E1, E2}` —
    into a list of enumerator-name strings."""
    raw = raw.strip()
    if raw.startswith('{') and raw.endswith('}'):
        return [tok.strip() for tok in _split_args(raw[1:-1]) if tok.strip()]
    return [raw] if raw else []


def parse_radio_spec(raw: str, where: str) -> dict:
    """Parse the `{ source = ..., value = ..., behavior = ... }` object of an
    AFIELD(radioListen = {...}) listener into a {key: raw_value} dict. `where`
    names the field for diagnostics. Structure errors are hard failures."""
    raw = raw.strip()
    if not (raw.startswith('{') and raw.endswith('}')):
        raise ValueError(
            f"{where}: AFIELD(radioListen = ...) must be a brace object "
            f"'{{ source = ..., value = ..., behavior = ... }}' (got: '{raw}').")
    spec: dict = {}
    for tok in _split_args(raw[1:-1]):
        tok = tok.strip()
        if not tok:
            continue
        if '=' not in tok:
            raise ValueError(
                f"{where}: AFIELD(radio) sub-argument '{tok}' is not a key = value pair.")
        key, _, value = tok.partition('=')
        spec[key.strip()] = value.strip()
    return spec


def _resolve_radio(comp: ComponentInfo, header_name: str) -> None:
    """Resolve and validate every field's AFIELD(radio ...) against its struct,
    attaching a RadioInfo to each field. A field may be a broadcaster
    (AFIELD(radioBroadcast) on an enum or a bool), a listener (AFIELD(radioListen = {
    source, value, behavior })), or both — listeners can follow a broadcaster
    that itself follows another, forming a chain.

    Every misuse is a hard build failure: a broadcaster that is neither an enum
    nor a bool, a listener naming a missing / wrong-typed / non-broadcaster field,
    an unknown enumerator, a bad behavior, or a cycle in the source chain."""
    by_name = {f.name: f for f in comp.fields}

    for f in comp.fields:
        where         = f"{header_name}: field '{comp.name}::{f.name}'"
        has_broadcast = f.args.has('radioBroadcast')   # AFIELD(radioBroadcast)
        raw_spec      = f.args.get('radioListen')      # AFIELD(radioListen={..})
        info          = RadioInfo()

        if has_broadcast:
            if f.enum_info is None and f.cpp_type != 'bool':
                raise ValueError(
                    f"{where} is marked AFIELD(radioBroadcast) but is neither an AENUM enum "
                    f"field nor a bool (its type is '{f.cpp_type}'). Only those can broadcast.")
            info.is_broadcast = True

        if raw_spec is not None:
            spec = parse_radio_spec(raw_spec, where)
            missing = _RADIO_KEYS - spec.keys()
            unknown = spec.keys() - _RADIO_KEYS
            if missing:
                raise ValueError(
                    f"{where}: AFIELD(radioListen) is missing {sorted(missing)}; it needs "
                    f"source, value, and behavior.")
            if unknown:
                raise ValueError(
                    f"{where}: AFIELD(radioListen) has unknown key(s) {sorted(unknown)}; only "
                    f"source, value, behavior are allowed.")

            source_name = spec['source']
            if source_name == f.name:
                raise ValueError(f"{where}: AFIELD(radioListen) cannot listen to itself.")
            source = by_name.get(source_name)
            if source is None:
                raise ValueError(
                    f"{where}: AFIELD(radioListen source = {source_name}) names no field in "
                    f"struct '{comp.name}'.")
            if source.enum_info is None and source.cpp_type != 'bool':
                raise ValueError(
                    f"{where}: AFIELD(radioListen) follows '{source_name}', which is neither an "
                    f"AENUM enum field nor a bool.")
            if not source.args.has('radioBroadcast'):
                raise ValueError(
                    f"{where}: AFIELD(radioListen) follows '{source_name}', which is not "
                    f"marked AFIELD(radioBroadcast).")

            value_names = _parse_value_list(spec['value'])
            if not value_names:
                raise ValueError(f"{where}: AFIELD(radioListen value = ...) is empty.")

            # A bool broadcasts the same way an enum does — it is a two-valued
            # enumeration, and requiring one to be spelled as an AENUM to drive a
            # radio is ceremony rather than meaning. `true`/`false` are its
            # enumerator names, and reach the runtime as the 1/0 a bool holds.
            if source.enum_info is None:
                const_map = {'false': 0, 'true': 1}
                source_desc = f"bool '{source_name}'"
            else:
                const_map = {name: val for name, val in source.enum_info.constants}
                source_desc = f"'{source.enum_info.name}'"
            values: list = []
            for vname in value_names:
                if vname not in const_map:
                    raise ValueError(
                        f"{where}: AFIELD(radioListen value = {vname}) is not a value "
                        f"of {source_desc} (valid: {sorted(const_map)}).")
                values.append(const_map[vname])

            behavior = _RADIO_BEHAVIORS.get(spec['behavior'].lower())
            if behavior is None:
                raise ValueError(
                    f"{where}: AFIELD(radioListen behavior = {spec['behavior']}) must be "
                    f"'grey' or 'vanish'.")

            info.source   = source_name
            info.values   = values
            info.behavior = behavior

        f.radio = info

    # Chains must be acyclic — the runtime resolves visibility by walking source
    # links, and a cycle (A follows B follows A) would never terminate. Follow
    # each listener's chain and reject a repeat.
    for start in comp.fields:
        if start.radio is None or not start.radio.source:
            continue
        seen: set = set()
        cur = start
        while cur.radio is not None and cur.radio.source:
            if cur.name in seen:
                raise ValueError(
                    f"{header_name}: AFIELD(radioListen) cycle detected in struct "
                    f"'{comp.name}' involving '{cur.name}'.")
            seen.add(cur.name)
            cur = by_name[cur.radio.source]  # existence validated above


# Field declaration: optional cv/storage-class keywords, type with optional
# namespace/template args, optional ptr/ref, name, optional default, semicolon.
# - Type modifiers (const, unsigned, etc.) can precede the base type token.
# - The pointer/ref marker may be flush against the variable name (int*foo),
#   so \s* (not \s+) separates type from name.
_FIELD_RE  = re.compile(
    r'((?:(?:const|unsigned|signed|long|short|volatile)\s+)*'  # cv/modifier keywords
    r'[\w:]+(?:\s*<[^>]*>)?'                                   # base type + optional template
    r'(?:\s*[*&])?)'                                           # optional ptr/ref
    r'\s*(\w+)'                                                # variable name (zero or more spaces after type)
    r'\s*(?:[={][^;]*)?\s*;'                                   # optional default + semicolon
)


def parse_amsg_args(content: str, struct_name: str, header_name: str) -> tuple[str, str, AnnotArgs]:
    """Parse `AMSG(direction, reliability[, extras])` into its three parts.

    The two positional arguments are mandatory and ordered, and every way of
    getting that wrong is a hard build error that names the rule. Explicitness
    over defaults, for two reasons: the declaration states the entire wire
    contract with nothing to memorise, and a default that changed later could
    never silently reclassify messages that were written under the old one.
    """
    where = f"{header_name}: message '{struct_name}'"
    grammar = ("AMSG takes two mandatory arguments in this order — direction "
               "('intent' or 'event'), then reliability ('reliable' or 'unreliable') — "
               "optionally followed by flags. For example: "
               "AMSG(event, unreliable, independent).")

    tokens = [tok.strip() for tok in _split_args(content) if tok.strip()]
    if len(tokens) < 2:
        raise ValueError(
            f"{where} is declared '{'AMSG(' + content.strip() + ')'}', which names "
            f"{'no arguments' if not tokens else 'only one argument'}. {grammar}")

    direction, reliability = tokens[0], tokens[1]

    # Swapped is called out by name rather than lumped in with "unknown", because
    # it is the mistake the ordering rule exists to catch and the fix is one
    # transposition.
    if direction in _AMSG_RELIABILITY and reliability in _AMSG_DIRECTIONS:
        raise ValueError(
            f"{where} is declared 'AMSG({direction}, {reliability})' — the two arguments are "
            f"the right way round for each other but the wrong way round for the grammar. "
            f"Write 'AMSG({reliability}, {direction})'. {grammar}")

    if direction not in _AMSG_DIRECTIONS:
        raise ValueError(
            f"{where} has '{direction}' as its first argument, which is not a direction. "
            f"{grammar}")
    if reliability not in _AMSG_RELIABILITY:
        raise ValueError(
            f"{where} has '{reliability}' as its second argument, which is not a reliability. "
            f"{grammar}")

    extras = parse_annot_args(','.join(tokens[2:]))
    unknown = extras.flags - _AMSG_EXTRA_FLAGS
    if unknown:
        raise ValueError(
            f"{where} has unknown AMSG flag(s) {sorted(unknown)}; the only flag is "
            f"'independent' (this message names no entity, so relevancy has nothing to "
            f"scope it by and nothing to hold it for).")
    if extras.kvs:
        raise ValueError(
            f"{where} has AMSG key = value argument(s) {sorted(extras.kvs)}; AMSG takes "
            f"two positional arguments and flags, nothing else.")

    return direction, reliability, extras


def find_systems(text: str, path: Path) -> list:
    """Every ASYSTEM() declaration in an already-comment-stripped header.

    A second independent walk, like find_handlers and for the same reason: one
    rigid regex over the whole text is simpler than threading a rare declaration
    through a scanner built around struct bodies.
    """
    systems: list = []
    for match in _ASYSTEM_RE.finditer(text):
        where = f"{path.name}: system '{match.group('fn')}'"

        if match.group('ret') != 'void':
            raise ValueError(
                f"{where} returns '{match.group('ret')}'. A system returns void — the scheduler has "
                f"nowhere to put a value, and anything it wanted to say belongs in a component.")

        args = _split_args(match.group('args'))
        if not args or not args[0]:
            raise ValueError(
                f"{where} has no phase. Phase is mandatory and positional: "
                f"ASYSTEM({'|'.join(_ASYSTEM_PHASES)}).")

        phase = args[0].strip()
        if phase not in _ASYSTEM_PHASES:
            raise ValueError(
                f"{where} names phase '{phase}', which is not one of "
                f"{', '.join(_ASYSTEM_PHASES)}.")

        # The context type follows from the phase rather than from the author, so
        # a mismatch is caught here instead of becoming a wrong cast at runtime.
        # This is the check the manual Register/RegisterRender split leaves to the
        # caller.
        wanted_ctx = 'RenderContext' if phase == 'Render' else 'SystemContext'
        if not match.group('ctx').endswith(wanted_ctx):
            raise ValueError(
                f"{where} is in phase {phase} and takes '{match.group('ctx')} &'. A {phase} system "
                f"takes {wanted_ctx} & — the phase decides the context, so the two cannot disagree.")

        name              = ''
        after, before     = [], []
        active_world_only = False

        for arg in args[1:]:
            arg = arg.strip()
            if not arg:
                continue
            if '=' in arg:
                key, _, value = arg.partition('=')
                key, value    = key.strip(), value.strip().strip('"')
                if key not in _ASYSTEM_KEYS:
                    raise ValueError(
                        f"{where} sets unknown key '{key}'. Recognised: "
                        f"{', '.join(sorted(_ASYSTEM_KEYS))}.")
                if not value:
                    raise ValueError(f"{where} sets '{key}' to nothing.")
                if key == 'name':
                    name = value
                elif key == 'after':
                    after.append(value)
                else:
                    before.append(value)
                continue

            if arg not in _ASYSTEM_FLAGS:
                raise ValueError(
                    f"{where} carries unknown flag '{arg}'. Recognised: "
                    f"{', '.join(sorted(_ASYSTEM_FLAGS))}. Note that RequireAny is deliberately not "
                    f"part of ASYSTEM — a gate reflectgen cannot verify, and too tight a one is a "
                    f"system that silently never runs.")
            active_world_only = True

        if active_world_only and phase == 'Render':
            raise ValueError(
                f"{where} is a Render system with activeWorldOnly. Render already runs for the world "
                f"being drawn and nothing else, so the flag would say nothing.")

        # Default: the function name with a trailing "System" stripped, because
        # `BounceSystem` in code is `Bounce` in a file and repeating the suffix in
        # every level would be noise.
        if not name:
            name = match.group('fn')
            if name.endswith('System') and len(name) > len('System'):
                name = name[: -len('System')]

        systems.append(SystemInfo(function=match.group('fn'),
                                  namespaces=_namespaces_at(text, match.start()),
                                  name=name, phase=phase, after=after, before=before,
                                  active_world_only=active_world_only, header=str(path)))
    return systems


def find_handlers(text: str, path: Path) -> list:
    """Every AMSG_HANDLER() declaration in an already-comment-stripped header.

    Namespace tracking is a second, independent walk rather than being folded
    into parse_header's state machine: handlers are found by one rigid regex over
    the whole text, and threading that through a character-by-character scanner
    designed around struct bodies would complicate the common path for a rare
    declaration.
    """
    handlers: list = []
    for match in _AMSG_HANDLER_RE.finditer(text):
        where = f"{path.name}: handler '{match.group('name')}'"
        if match.group('ret') != 'void':
            raise ValueError(
                f"{where} returns '{match.group('ret')}'. A message handler returns void — "
                f"there is nowhere for a return value to go, because a reply is state and "
                f"the state channel already carries it.")
        if not match.group('ctx').endswith('NetContext'):
            raise ValueError(
                f"{where} takes '{match.group('ctx')} &' as its first parameter, but every "
                f"handler has the same signature: void Name(NetContext &, const T &).")

        # Namespaces enclosing this declaration: count braces before it, tracking
        # the namespace stack exactly as parse_header does.
        namespaces = _namespaces_at(text, match.start())
        handlers.append(HandlerInfo(name=match.group('name'), namespaces=namespaces,
                                    message=match.group('msg'), header=str(path)))
    return handlers


def _namespaces_at(text: str, position: int) -> list:
    """The namespace stack in effect at @p position of a comment-stripped header."""
    ns_stack: list = []
    ns_depths: list = []
    depth = 0
    i = 0
    while i < position:
        ns_m = _NS_RE.match(text, i)
        if ns_m:
            j = ns_m.end()
            while j < position and text[j] in ' \t\n\r':
                j += 1
            if j < position and text[j] == '{':
                for part in ns_m.group(1).split('::'):
                    ns_stack.append(part)
                    ns_depths.append(depth)
                depth += 1
                i = j + 1
                continue
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            while ns_depths and ns_depths[-1] >= depth:
                ns_stack.pop()
                ns_depths.pop()
        i += 1
    return ns_stack


def _extract_brace_body(text: str, start: int) -> tuple[Optional[str], int]:
    """
    Find the brace-balanced body at or after `start`.
    Returns (body_content, position_after_closing_brace).
    """
    i, n = start, len(text)
    while i < n and text[i] != '{':
        i += 1
    if i >= n:
        return None, -1
    depth = 1
    i += 1
    body_start = i
    while i < n and depth > 0:
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
        i += 1
    return text[body_start:i - 1], i


def _find_fields_in_body(body: str, source_header: str) -> list[FieldInfo]:
    """Find all AFIELD-annotated fields within a struct body."""
    fields = []
    i = 0
    while i < len(body):
        m = _AFIELD_RE.search(body, i)
        if not m:
            break
        args = parse_annot_args(m.group(1))
        rest = body[m.end():]
        fm = _FIELD_RE.match(rest.lstrip())
        if not fm:
            # A malformed AFIELD would otherwise silently drop the field — and its
            # data on every save. Fail the build loudly instead of moving on.
            snippet = ' '.join(rest.lstrip()[:60].split())
            raise ValueError(
                f"{source_header}: AFIELD is not followed by a recognisable field "
                f"declaration (got: '{snippet}...'). A reflected field must be a plain "
                f"'Type name;' declaration immediately after the AFIELD(...) macro.")
        raw_type = fm.group(1).strip()
        name     = fm.group(2).strip()
        cpp_type = raw_type.replace('const ', '').replace('*', '').replace('&', '').strip()
        fields.append(FieldInfo(name=name, cpp_type=cpp_type, args=args))
        i = m.end()
    return fields


# ──────────────────────────────────────────────────────────────────────────────
# The InstanceView storage ban: spellings that reach a view without saying so
# ──────────────────────────────────────────────────────────────────────────────

# `using Name = Type;`. `using namespace X;` and `using Base::member;` carry no
# '=' and are skipped by construction.
_USING_RE   = re.compile(r'\busing\s+(\w+)\s*=\s*([^;]+);')
# `typedef Type Name;` — the name is the last identifier before the semicolon.
_TYPEDEF_RE = re.compile(r'\btypedef\s+(.+?)\s+(\w+)\s*;')
# `struct Name {`, `class Name final : Base {` — where a holder is declared.
_RECORD_RE  = re.compile(r'\b(?:struct|class)\s+(\w+)\s*(?:final\b\s*)?(?::[^{;]*)?\{')
# Every identifier in a type spelling, template arguments included.
_TYPE_NAME_RE = re.compile(r'[A-Za-z_][\w:]*')


def _reaches_a_view(spelling: str, poisoned: dict) -> Optional[str]:
    """How @p spelling reaches an InstanceView, or None if it does not.

    Returns the empty string when the spelling says so itself — that case is the
    plainer check in reflectgen and has its own message — and otherwise the
    sentence explaining the name it went through.
    """
    if 'InstanceView<' in spelling.replace(' ', ''):
        return ''
    for token in _TYPE_NAME_RE.findall(spelling):
        # A qualified spelling of a local name still names it: `N::CarView` is
        # the `CarView` this header declares.
        reason = poisoned.get(token.rsplit('::', 1)[-1])
        if reason is not None:
            return reason
    return None


def find_view_spellings(text: str) -> dict:
    """Every type name in @p text that reaches an InstanceView without being one.

    Maps the name to the sentence a build error quotes: *'CarHandle' is an alias
    for 'CarView', and 'CarView' is an alias for 'InstanceView<Car>'*. Aliases,
    aliases of aliases, and structs that hold one all land here, because the ban
    is on storing a view and none of those stops it being stored.

    Only what this header spells is visible: an alias declared in a header this
    one includes reads as an ordinary word. The static_assert reflectgen emits
    into the generated file is what covers that half.
    """
    aliases: list = []
    for m in _USING_RE.finditer(text):
        aliases.append((m.group(1), ' '.join(m.group(2).split())))
    for m in _TYPEDEF_RE.finditer(text):
        aliases.append((m.group(2), ' '.join(m.group(1).split())))

    # Member declarations only. _FIELD_RE's shape ends at a ';' with no parameter
    # list between, so a method that *returns* a view is not a struct that
    # *stores* one — the distinction the ban is about. A view declared as a local
    # inside an inline method body is over-read as storage, which is the
    # default-deny side to err on.
    records: list = []
    for m in _RECORD_RE.finditer(text):
        body, _ = _extract_brace_body(text, m.end() - 1)
        if body is None:
            continue
        records.append((m.group(1), [f.group(1).strip() for f in _FIELD_RE.finditer(body)]))

    # To a fixpoint: an alias of an alias, or a struct holding a struct holding a
    # view, is as stored as the direct spelling, and neither declaration order
    # nor depth may decide whether it is caught.
    poisoned: dict = {}
    changed = True
    while changed:
        changed = False
        for name, spelling in aliases:
            if name in poisoned:
                continue
            via = _reaches_a_view(spelling, poisoned)
            if via is None:
                continue
            poisoned[name] = (f"'{name}' is an alias for '{spelling}'"
                              + (f", and {via}" if via else ''))
            changed = True
        for name, members in records:
            if name in poisoned:
                continue
            for member in members:
                via = _reaches_a_view(member, poisoned)
                if via is None:
                    continue
                poisoned[name] = (f"'{name}' has a member of type '{member}'"
                                  + (f", and {via}" if via else ''))
                changed = True
                break
    return poisoned


def parse_header(path: Path) -> list[ComponentInfo]:
    """Parse a header file and return all ACOMP-annotated components.

    AENUM-annotated `enum class`es in the same header are collected first, and
    any component field whose type names one is resolved to it (enum_info),
    which is what lets reflectgen (de)serialize the field and emit its combo.
    """
    return parse_header_full(path)[0]


def parse_header_systems(path: Path) -> list:
    """Every ASYSTEM() declaration in @p path.

    Its own entry point rather than a fourth element of parse_header_full's
    tuple, so callers that want only components are unaffected.
    """
    return find_systems(strip_comments(path.read_text(encoding='utf-8')), path)


def parse_header_full(path: Path) -> tuple[list, list, list]:
    """Everything a header declares: (components, messages, handlers).

    parse_header is the components-only facade over this, which is all most
    callers want. Systems come from parse_header_systems.
    """
    text = strip_comments(path.read_text(encoding='utf-8'))
    components: list[ComponentInfo] = []
    messages: list = []
    # Enum lookups, keyed by both the unqualified name and the fully-qualified
    # spelling so a field can reference the enum either way.
    enums: dict[str, EnumInfo] = {}

    ns_stack:       list[str] = []
    ns_open_depths: list[int] = []
    brace_depth     = 0
    i               = 0
    n               = len(text)
    pending_acomp: Optional[AnnotArgs] = None
    pending_is_asset = False
    pending_amsg: Optional[str] = None  # the raw AMSG(...) argument text

    while i < n:
        # ── Namespace ───────────────────────────────────────────────────────
        ns_m = _NS_RE.match(text, i)
        if ns_m:
            j = ns_m.end()
            while j < n and text[j] in ' \t\n\r':
                j += 1
            if j < n and text[j] == '{':
                for p in ns_m.group(1).split('::'):
                    ns_stack.append(p)
                    ns_open_depths.append(brace_depth)
                brace_depth += 1
                i = j + 1
                continue

        # ── ACOMP / AASSET / AENUM ──────────────────────────────────────────
        acomp_m = _ACOMP_RE.match(text, i)
        if acomp_m:
            pending_acomp = parse_annot_args(acomp_m.group(1))
            pending_is_asset = False
            i = acomp_m.end()
            continue
        aasset_m = _AASSET_RE.match(text, i)
        if aasset_m:
            pending_acomp = parse_annot_args(aasset_m.group(1))
            pending_is_asset = True
            i = aasset_m.end()
            continue
        amsg_m = _AMSG_RE.match(text, i)
        if amsg_m:
            pending_amsg = amsg_m.group(1)
            i = amsg_m.end()
            continue
        aenum_m = _AENUM_RE.match(text, i)
        if aenum_m:
            # AENUM must be immediately followed (bar whitespace) by an
            # `enum class`/`enum struct` with a body — anything else is a
            # malformed annotation, which is a hard error, not a silent skip.
            j = aenum_m.end()
            while j < n and text[j] in ' \t\n\r':
                j += 1
            enum_m = _ENUM_RE.match(text, j)
            if not enum_m:
                snippet = ' '.join(text[j:j + 60].split())
                raise ValueError(
                    f"{path.name}: AENUM is not followed by an 'enum class' / 'enum struct' "
                    f"definition (got: '{snippet}...').")
            enum_name = enum_m.group(1)
            size, is_signed = _enum_underlying(enum_m.group(2), enum_name, path.name)
            body, end = _extract_brace_body(text, enum_m.end())
            if body is None:
                raise ValueError(f"{path.name}: AENUM enum '{enum_name}' has no '{{ ... }}' body.")
            fqn = '::'.join(ns_stack + [enum_name]) if ns_stack else enum_name
            info = EnumInfo(name=enum_name, fqn=fqn, constants=parse_enum_constants(body),
                            size=size, is_signed=is_signed)
            enums[enum_name] = info
            enums[fqn] = info
            i = end
            continue

        # ── Struct (only matters after AMSG) ─────────────────────────────────
        if pending_amsg is not None:
            struct_m = _STRUCT_RE.match(text, i)
            if struct_m:
                name = struct_m.group(1)
                body, end = _extract_brace_body(text, struct_m.end())
                if body is not None:
                    direction, reliability, extras = parse_amsg_args(pending_amsg, name, path.name)
                    messages.append(MessageInfo(
                        name=name,
                        namespaces=list(ns_stack),
                        direction=direction,
                        reliability=reliability,
                        args=extras,
                        fields=_find_fields_in_body(body, str(path)),
                    ))
                    pending_amsg = None
                    brace_depth += body.count('{') - body.count('}')
                    i = end
                    continue
                pending_amsg = None
            elif not text[i].isspace():
                # An AMSG followed by anything but a struct is a malformed
                # annotation, not something to walk past: a message *is* a
                # struct, and silently ignoring the macro would leave the author
                # believing a type is on the wire when nothing registered it.
                snippet = ' '.join(text[i:i + 60].split())
                raise ValueError(
                    f"{path.name}: AMSG is not followed by a 'struct' definition "
                    f"(got: '{snippet}...'). A message is a plain reflected struct — see "
                    f"Assisi/Core/Reflect/MessageMeta.hpp for why it is not a function.")

        # ── Struct (only matters after ACOMP/AASSET) ────────────────────────
        if pending_acomp is not None:
            struct_m = _STRUCT_RE.match(text, i)
            if struct_m:
                name = struct_m.group(1)
                body, end = _extract_brace_body(text, struct_m.end())
                if body is not None:
                    fields = _find_fields_in_body(body, str(path))
                    components.append(ComponentInfo(
                        name=name,
                        namespaces=list(ns_stack),
                        args=pending_acomp,
                        fields=fields,
                        is_asset=pending_is_asset,
                    ))
                    pending_acomp = None
                    pending_is_asset = False
                    brace_depth += body.count('{') - body.count('}')
                    i = end
                    continue
                else:
                    pending_acomp = None
                    pending_is_asset = False

        # ── Brace / namespace tracking ───────────────────────────────────────
        ch = text[i]
        if ch == '{':
            brace_depth += 1
        elif ch == '}':
            brace_depth -= 1
            while ns_open_depths and ns_open_depths[-1] >= brace_depth:
                ns_stack.pop()
                ns_open_depths.pop()
        i += 1

    # Resolve enum-typed fields now that every AENUM in the header is known.
    for comp in components:
        for f in comp.fields:
            f.enum_info = enums.get(f.cpp_type)
    for msg in messages:
        for f in msg.fields:
            f.enum_info = enums.get(f.cpp_type)

    # The InstanceView storage ban is on storing one, not on spelling one, so a
    # field's type is resolved through this header's aliases and holder structs
    # before reflectgen judges it. An empty reason means the type spells a view
    # outright, which is the other check's message to give.
    view_spellings = find_view_spellings(text)
    for owner in (*components, *messages):
        for f in owner.fields:
            f.view_via = _reaches_a_view(f.cpp_type, view_spellings) or None

    # Radio references resolve against sibling fields (and their enum_info), so
    # this must run after enum resolution. Any misuse raises here.
    for comp in components:
        _resolve_radio(comp, path.name)
    for msg in messages:
        _resolve_radio(msg, path.name)

    return components, messages, find_handlers(text, path)


# ──────────────────────────────────────────────────────────────────────────────
# Include-path auto-detection
# ──────────────────────────────────────────────────────────────────────────────

def _detect_include_path(header: Path) -> str:
    """
    If the path contains an 'include' segment, return everything after it.
    e.g. .../modules/Runtime/include/Assisi/Runtime/Foo.hpp -> Assisi/Runtime/Foo.hpp
    Otherwise return just the filename.
    """
    parts = header.parts
    try:
        idx = next(i for i, p in enumerate(parts) if p.lower() == 'include')
        return '/'.join(parts[idx + 1:])
    except StopIteration:
        return header.name
