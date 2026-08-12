# Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc").
"""Generation of InstanceView<T> specializations from .abp files.

A blueprint's member list is flat at runtime — `body`, `car/wheel_fl` — and this
turns it back into the nested aggregate the game writes against
(docs/blueprint-system-concept.md section 7).

The one thing to be careful about: the member *names* produced here must match
the ones Blueprint.cpp produces byte for byte, because a field whose name is not
a real member compiles fine and resolves to NullEntity at runtime — the silent
failure this whole system exists to remove. Three rules carry the naming, and
all three are mirrored from FlattenInto/FlattenInstance:

  * a nested instance contributes `instanceName + "/"` to the prefix of
    everything it declares, at every depth;
  * `removed` on a nested instance cascades to descendants — removing `body`
    removes `body/bolt` too (Blueprint.cpp IsMemberRemoved);
  * duplicate declarations are checked on the *full* prefixed path, not per file.

The runtime typed-spawn test is the guard on that agreement: it spawns a real
nested blueprint and resolves every generated field, so a divergence fails a
test rather than returning a null handle to a game.

The same argument makes this file refuse what Blueprint.cpp refuses — today the
file version and a nested instance's non-uniform scale. A file this accepts and
the loader rejects is the silent failure in its worst form: the build succeeds,
the call sites compile, and every spawn of that blueprint is empty forever with
nothing pointing at the file. The manifest cross-check cannot cover it, because
it only compares blueprints that already loaded. Where a refusal is mirrored,
its leniency is mirrored with it: refusing a file the loader would have taken
fails a build over a legal blueprint, which is the same divergence pointing the
other way.
"""

import json
from pathlib import Path

# Every word C++ reserves, and the only authority here on what is reserved.
#
# This used to be "the ones Python's `keyword` module misses", with
# `keyword.iskeyword` asked alongside it. That split the table between two
# authorities and got both halves wrong. It was too lax, because Python's list
# is not a subset of C++'s and was never going to cover the difference: `case`
# is a soft keyword there, `true` and `false` are spelled capitalised, and the
# alternative tokens (`bitand`, `xor_eq`) and the coroutine keywords have no
# Python counterpart at all — so all fourteen walked through and became
# `ECS::Entity case;` in a file marked "Do not edit", failing the build at the
# generated line instead of naming the .abp and the member. It was also too
# strict, because Python reserves words C++ does not: `lambda`, `pass` and
# `yield` are ordinary identifiers in the generated header, and the loader takes
# a file that uses them, so refusing one failed a build over a legal blueprint —
# the same divergence the module docstring argues against, pointing the other
# way.
#
# So: [lex.key] Table 5 in full, plus the [lex.digraph] alternative tokens,
# which are spelled like identifiers and behave as keywords. Nothing is left out
# on the grounds that another language happens to reserve it too.
_CXX_KEYWORDS = {
    'alignas', 'alignof', 'and', 'and_eq', 'asm', 'auto', 'bitand', 'bitor',
    'bool', 'break', 'case', 'catch', 'char', 'char8_t', 'char16_t', 'char32_t',
    'class', 'compl', 'concept', 'const', 'consteval', 'constexpr', 'constinit',
    'const_cast', 'continue', 'co_await', 'co_return', 'co_yield', 'decltype',
    'default', 'delete', 'do', 'double', 'dynamic_cast', 'else', 'enum',
    'explicit', 'export', 'extern', 'false', 'float', 'for', 'friend', 'goto',
    'if', 'inline', 'int', 'long', 'mutable', 'namespace', 'new', 'noexcept',
    'not', 'not_eq', 'nullptr', 'operator', 'or', 'or_eq', 'private',
    'protected', 'public', 'register', 'reinterpret_cast', 'requires', 'return',
    'short', 'signed', 'sizeof', 'static', 'static_assert', 'static_cast',
    'struct', 'switch', 'template', 'this', 'thread_local', 'throw', 'true',
    'try', 'typedef', 'typeid', 'typename', 'union', 'unsigned', 'using',
    'virtual', 'void', 'volatile', 'wchar_t', 'while', 'xor', 'xor_eq',
}

# The one field every view has, so a member may not take the name.
_RESERVED_FIELDS = {'instanceId'}


class ViewError(Exception):
    """A blueprint that cannot produce a view. Always a build error."""


class _FlattenState:
    def __init__(self, root, source):
        self.root = root
        self.members = []
        self.declared = set()
        self.stack = [source]
        self.sources = [source]


def _read(root, source):
    path = Path(root) / source
    try:
        with open(path, 'r', encoding='utf-8') as handle:
            doc = json.load(handle)
    except FileNotFoundError:
        raise ViewError(f"'{source}' does not exist under {root}")
    except json.JSONDecodeError as error:
        raise ViewError(f"'{source}' is not readable JSON: {error}")

    _check_version(doc, source)
    return doc


def _check_version(doc, source):
    """Blueprint.cpp ReadFile: version 2 or the loader refuses the file.

    Here rather than at the runtime, because the alternative is the worst
    failure this system has. A version the loader will not read still generates
    a full view, so the build succeeds, every call site compiles, and then
    GetBlueprintDefinition hands back nothing and every spawn of the blueprint
    is empty forever — with nothing naming the file that caused it. The manifest
    cross-check cannot catch it either: it only compares files that already
    loaded.
    """
    if not isinstance(doc, dict):
        return  # Not an object at all; _flatten_into says so with more context.

    if 'version' not in doc:
        raise ViewError(
            f"'{source}' declares no version; the runtime loader reads version 2 only, "
            'and would refuse to spawn it')

    # A number, and equal to 2. The loader's `value("version", 0)` truncates, so
    # a 2.0 lands on 2 there and is taken here for the same reason; a bool is not
    # a JSON number to nlohmann's is_number, and `True == 2` is false anyway.
    version = doc['version']
    if isinstance(version, bool) or not isinstance(version, (int, float)) or version != 2:
        raise ViewError(
            f"'{source}' is version {json.dumps(version)}; the runtime loader reads version 2 "
            'only, and would refuse to spawn it')


def _instance_scale(entry):
    """Blueprint.cpp TransformFromJson, for the one field the loader judges.

    Deliberately as lenient as the C++: the slots are checked before any of them
    is read, and one that is not a number leaves the *whole* field at the
    Transform default rather than half-read. Being stricter here would fail the
    build on a file that loads.
    """
    transform = entry.get('transform')
    if not isinstance(transform, dict):
        return (1.0, 1.0, 1.0)

    scale = transform.get('scale')
    if not isinstance(scale, list) or len(scale) != 3:
        return (1.0, 1.0, 1.0)
    for slot in scale:
        # bool is an int in Python; it is not a number to nlohmann's is_number.
        if isinstance(slot, bool) or not isinstance(slot, (int, float)):
            return (1.0, 1.0, 1.0)
    return tuple(float(slot) for slot in scale)


def _has_uniform_scale(scale):
    """Blueprint.cpp HasUniformScale: relative, so scale-independent.

    The tolerance is for a hand-typed 1.0000001, not to let a real
    non-uniformity through. Computed in double here and in float there, which
    only parts them on a value already sitting on the tolerance itself.
    """
    tolerance = 1e-5
    x, y, z = scale
    mean = (abs(x) + abs(y) + abs(z)) / 3.0
    if mean <= 0.0:
        return x == y and y == z
    return abs(x - y) / mean < tolerance and abs(y - z) / mean < tolerance


def _is_removed(member_name, removed):
    """Blueprint.cpp IsMemberRemoved: exact, or a '/'-delimited descendant."""
    for path in removed:
        if member_name == path:
            return True
        if member_name.startswith(path) and member_name[len(path):len(path) + 1] == '/':
            return True
    return False


def _flatten_instance(state, entry, source, prefix):
    if not isinstance(entry, dict):
        raise ViewError(f"'{source}' has an instance that is not an object")

    name = entry.get('name')
    child_source = entry.get('source')
    if not isinstance(name, str) or not name:
        raise ViewError(f"'{source}' has an instance with no name")
    if not isinstance(child_source, str) or not child_source:
        raise ViewError(f"'{source}' instance '{name}' names no source")

    # Same refusal as the loader's, and for the same reason: a cycle would
    # otherwise recurse until the stack ran out.
    if child_source in state.stack:
        chain = ' -> '.join(state.stack + [child_source])
        raise ViewError(f'instance cycle: {chain}')

    # The loader's refusal, at the point the loader makes it — at every depth,
    # because that is where FlattenInstance checks. The view it would otherwise
    # generate is a full one for a blueprint that never spawns.
    scale = _instance_scale(entry)
    if not _has_uniform_scale(scale):
        raise ViewError(
            f"'{source}' instance '{name}' has a non-uniform scale "
            f'({scale[0]}, {scale[1]}, {scale[2]}); an instance may only translate, rotate, '
            'or scale uniformly, and the runtime loader would refuse to spawn it')

    child_prefix = prefix + name + '/'
    first = len(state.members)

    state.stack.append(child_source)
    if child_source not in state.sources:
        state.sources.append(child_source)
    _flatten_into(state, _read(state.root, child_source), child_source, child_prefix)
    state.stack.pop()

    removed = entry.get('removed')
    if not isinstance(removed, list):
        return
    removed = [path for path in removed if isinstance(path, str)]
    if not removed:
        return

    kept = []
    for index, path in enumerate(state.members):
        drop = (index >= first and path.startswith(child_prefix)
                and _is_removed(path[len(child_prefix):], removed))
        if drop:
            state.declared.discard(path)
        else:
            kept.append(path)
    state.members = kept


def _flatten_into(state, doc, source, prefix):
    if not isinstance(doc, dict):
        raise ViewError(f"'{source}' is not a JSON object")

    entities = doc.get('entities')
    if isinstance(entities, list):
        for entity in entities:
            if not isinstance(entity, dict):
                raise ViewError(f"'{source}' has an entity that is not an object")
            name = entity.get('name')
            if not isinstance(name, str) or not name:
                raise ViewError(f"'{source}' has an entity with no name")
            # The loader does not ban this, but a view cannot represent it: an
            # entity literally named 'a/b' is indistinguishable from member 'b'
            # of a nested instance 'a' once both are flattened.
            if '/' in name:
                raise ViewError(
                    f"'{source}' declares an entity named '{name}'; '/' is how "
                    'nesting is spelled, so it cannot appear in an entity name')

            full = prefix + name
            if full in state.declared:
                raise ViewError(f"'{source}' declares two members named '{full}'")
            state.declared.add(full)
            state.members.append(full)

    instances = doc.get('instances')
    if isinstance(instances, list):
        for entry in instances:
            _flatten_instance(state, entry, source, prefix)


def flatten_member_names(root, source):
    """The member names a spawn of `source` will produce, in the loader's order.

    Returns (members, sources) — the second being every file read, which the
    build needs as the dependency list so editing a nested blueprint
    regenerates the view.
    """
    state = _FlattenState(root, source)
    _flatten_into(state, _read(root, source), source, '')
    return state.members, state.sources


def _is_usable_name(name):
    """A name the generator can emit into C++ as written.

    The single decision behind both the member-name check and the type-name
    check, so a word banned as one is banned as the other. Those drifted once:
    the type name was tested against `_CXX_KEYWORDS` alone while member names
    were also asked of Python's `keyword` module, and `--blueprint
    class=car.abp` emitted `struct class;`. The two differ only in what they say
    when they refuse, which is why that is all `_check_identifier` decides for
    itself.
    """
    return name.isidentifier() and name not in _CXX_KEYWORDS


def _check_identifier(name, source, path):
    if not _is_usable_name(name):
        reason = ('is a C++ keyword' if name in _CXX_KEYWORDS
                  else 'is not a valid C++ identifier')
        raise ViewError(
            f"'{source}' member '{path}' cannot be a field name: '{name}' {reason}")
    if name in _RESERVED_FIELDS:
        raise ViewError(
            f"'{source}' member '{path}' cannot be a field name: every view "
            f"already has an '{name}'")


def build_tree(members, source):
    """Nest the flat member list, rejecting anything two fields could share a name.

    A dict whose values are either None (an entity field) or another dict (a
    nested group). Ordered by first appearance, which is the loader's order.
    """
    tree = {}
    kinds = {}  # full path -> 'entity' | 'group', for the error message

    for path in members:
        parts = path.split('/')
        node = tree
        walked = []
        for part in parts[:-1]:
            walked.append(part)
            joined = '/'.join(walked)
            _check_identifier(part, source, joined)
            existing = node.get(part)
            if existing is None and part in node:
                raise ViewError(
                    f"'{source}' has both an entity and a nested instance named "
                    f"'{joined}'; they would be one field")
            if existing is None:
                node[part] = {}
                kinds[joined] = 'group'
            node = node[part]

        leaf = parts[-1]
        _check_identifier(leaf, source, path)
        if leaf in node:
            raise ViewError(
                f"'{source}' has both an entity and a nested instance named "
                f"'{path}'; they would be one field")
        node[leaf] = None
        kinds[path] = 'entity'

    return tree


# Every character C++ reads as syntax inside a string literal, and how to spell
# it as itself.
_CXX_STRING_ESCAPES = {'\\': r'\\', '"': r'\"', '\n': r'\n', '\r': r'\r', '\t': r'\t'}


def _cxx_escaped(text):
    r"""`text` with everything a C++ literal would read as syntax spelled out.

    The one authority on this, because the alternative is escaping at some emit
    sites and not others, which is the same file's B21 lesson about splitting a
    rule between two places.

    Source paths need it. They are input — whatever `--blueprint
    Car=blueprints\car.abp` was given — with none of the constraints member
    names have, and every Windows path is full of backslashes: emitted as
    written, `blueprints\car.abp` carries `\c`, which no C++ escape names, and
    `assets\test.abp` carries a tab in the middle of a path the runtime then
    fails to find. A quote ends the literal outright and takes the rest of the
    header with it.

    Control characters go out in octal rather than hex because a hex escape has
    no length limit: `\x1` followed by a digit of the path swallows it and
    changes the value, while `\001` is three digits and stops.

    Comments get the same treatment. The hazard there is only a newline, which
    ends the `//` and leaves the rest of the path standing where code goes, but
    one function answering for both is the point.
    """
    out = []
    for character in text:
        escape = _CXX_STRING_ESCAPES.get(character)
        if escape is not None:
            out.append(escape)
        elif character < ' ' or character == '\x7f':
            out.append(f'\\{ord(character):03o}')
        else:
            out.append(character)
    return ''.join(out)


def _cxx_literal(text):
    """`text` as a C++ string literal whose value is `text` exactly."""
    return f'"{_cxx_escaped(text)}"'


def _render_fields(node, indent):
    pad = ' ' * indent
    lines = []
    entities = [name for name, child in node.items() if child is None]
    if entities:
        lines.append(f'{pad}ECS::Entity {", ".join(entities)};')
    for name, child in node.items():
        if child is None:
            continue
        # The nested group carries no id of its own: there is one instance.
        lines.append(f'{pad}struct')
        lines.append(f'{pad}{{')
        lines.extend(_render_fields(child, indent + 4))
        lines.append(f'{pad}}} {name};')
    return lines


def render_instance_views(views):
    """`views` is a list of (type_name, source, tree, members), already validated."""
    lines = [
        '// Generated by reflectgen. Do not edit — regeneration clobbers this file,',
        '// which is what keeps a method from ever being added to a view.',
        '#pragma once',
        '',
        '#include <Assisi/ECS/Entity.hpp>',
        '#include <Assisi/ECS/InstanceId.hpp>',
        '#include <Assisi/ECS/Scene.hpp>',
        '#include <Assisi/Runtime/Blueprint.hpp>',
        '#include <Assisi/Runtime/InstanceView.hpp>',
        '',
        '#include <span>',
        '#include <string_view>',
        '',
        'namespace Assisi::Blueprints',
        '{',
    ]

    if views:
        lines.append('')
        lines.append('// Tag types. Incomplete on purpose: they name a blueprint and are')
        lines.append('// never instantiated, so there is nothing to construct by mistake.')
    for type_name, source, _, _ in views:
        lines.append(f'struct {type_name}; // {_cxx_escaped(source)}')

    lines.append('')
    lines.append('} // namespace Assisi::Blueprints')
    lines.append('')
    lines.append('namespace Assisi::Runtime')
    lines.append('{')

    for type_name, source, tree, members in views:
        qualified = f'Blueprints::{type_name}'
        lines.append('')
        lines.append(f'/// @brief The members of `{_cxx_escaped(source)}`, nested as the file nests them.')
        lines.append('///')
        lines.append('/// Only `instanceId` may outlive the call that produced this; the handles')
        lines.append('/// are for right now.')
        lines.append(f'template <> struct InstanceView<{qualified}>')
        lines.append('{')
        # Move-only, per the receipt rule: a view lives in the scope of the call
        # that produced it. This is what costs the type its aggregate-ness, which
        # is why the fields are initialized here rather than by aggregate init.
        lines.append('    InstanceView()                                = default;')
        lines.append('    InstanceView(const InstanceView &)            = delete;')
        lines.append('    InstanceView &operator=(const InstanceView &) = delete;')
        lines.append('    InstanceView(InstanceView &&)                 = default;')
        lines.append('    InstanceView &operator=(InstanceView &&)      = default;')
        lines.append('')
        lines.append('    ECS::InstanceId instanceId{};')
        body = _render_fields(tree, 4)
        if body:
            lines.append('')
            lines.extend(body)
        lines.append('};')

        lines.append('')
        lines.append(f'template <> struct InstanceViewTraits<{qualified}>')
        lines.append('{')
        lines.append(f'    static constexpr std::string_view kSource = {_cxx_literal(source)};')
        lines.append('};')

        lines.append('')
        lines.append('/// Resolves every field by name. A free function rather than a method,')
        lines.append('/// because the view carries no behaviour of its own.')
        lines.append(f'inline void FillInstanceView(InstanceView<{qualified}> &view, ECS::Scene &scene,')
        lines.append('                             const InstanceTable &table)')
        lines.append('{')
        if not members:
            lines.append('    (void)view;')
            lines.append('    (void)scene;')
            lines.append('    (void)table;')
        for path in members:
            field = path.replace('/', '.')
            lines.append(f'    view.{field} = FindMember(scene, table, view.instanceId, {_cxx_literal(path)});')
        lines.append('}')

    # The manifest: the member names this generator believes each blueprint has,
    # in order. It exists to be *checked* — the same names are produced a second
    # time at run time by Blueprint.cpp's FlattenInto, and a divergence between
    # the two would resolve a field to NullEntity in code that compiles and
    # spawns fine. TestInstanceViews walks this list against
    # GetBlueprintDefinition, so every opted-in blueprint is covered the moment
    # it is opted in, rather than only the ones somebody wrote a case for.
    #
    # Order is part of the claim, not incidental: a member's index is what its
    # NetId is assigned from.
    lines.append('')
    lines.append('/// One opted-in blueprint and the members this build generated for it.')
    lines.append('struct GeneratedInstanceView')
    lines.append('{')
    lines.append('    std::string_view                  source;')
    lines.append('    std::span<const std::string_view> members;')
    lines.append('};')
    lines.append('')

    for type_name, source, _, members in views:
        joined = ', '.join(_cxx_literal(path) for path in members)
        lines.append(f'inline constexpr std::string_view kMembersOf{type_name}[] = {{{joined}}};'
                     if members else
                     f'inline constexpr std::span<const std::string_view> kMembersOf{type_name}{{}};')

    lines.append('')
    lines.append('/// Every view this build generated, for the cross-check against the loader.')
    lines.append('inline constexpr GeneratedInstanceView kGeneratedInstanceViews[] = {')
    for type_name, source, _, _ in views:
        lines.append(f'    {{{_cxx_literal(source)}, kMembersOf{type_name}}},')
    lines.append('};')

    lines.append('')
    lines.append('} // namespace Assisi::Runtime')
    lines.append('')
    return '\n'.join(lines)


def generate(root, specs):
    """`specs` is a list of (type_name, source). Returns (text, dependencies)."""
    seen_types = {}
    views = []
    dependencies = []

    for type_name, source in specs:
        if not _is_usable_name(type_name):
            raise ViewError(f"'{type_name}' is not a valid C++ type name")
        if type_name in seen_types:
            raise ViewError(
                f"two blueprints are both opted in as '{type_name}': "
                f"'{seen_types[type_name]}' and '{source}'")
        seen_types[type_name] = source

        members, sources = flatten_member_names(root, source)
        views.append((type_name, source, build_tree(members, source), members))
        for dependency in sources:
            if dependency not in dependencies:
                dependencies.append(dependency)

    return render_instance_views(views), dependencies
