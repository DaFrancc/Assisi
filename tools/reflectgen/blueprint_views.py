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
"""

import json
import keyword
from pathlib import Path

# Reserved words that are not in Python's `keyword` module but would still
# collide in the generated C++.
_CXX_KEYWORDS = {
    'alignas', 'alignof', 'asm', 'auto', 'bool', 'catch', 'char', 'char8_t',
    'char16_t', 'char32_t', 'concept', 'const', 'consteval', 'constexpr',
    'constinit', 'const_cast', 'decltype', 'default', 'delete', 'do', 'double',
    'dynamic_cast', 'enum', 'explicit', 'export', 'extern', 'float', 'friend',
    'goto', 'inline', 'int', 'long', 'mutable', 'namespace', 'new', 'noexcept',
    'nullptr', 'operator', 'private', 'protected', 'public', 'register',
    'reinterpret_cast', 'requires', 'short', 'signed', 'sizeof', 'static',
    'static_assert', 'static_cast', 'struct', 'switch', 'template', 'this',
    'thread_local', 'throw', 'typedef', 'typeid', 'typename', 'union',
    'unsigned', 'using', 'virtual', 'void', 'volatile', 'wchar_t',
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
            return json.load(handle)
    except FileNotFoundError:
        raise ViewError(f"'{source}' does not exist under {root}")
    except json.JSONDecodeError as error:
        raise ViewError(f"'{source}' is not readable JSON: {error}")


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


def _check_identifier(name, source, path):
    if not name.isidentifier() or keyword.iskeyword(name):
        raise ViewError(
            f"'{source}' member '{path}' cannot be a field name: '{name}' is not "
            'a valid C++ identifier')
    if name in _CXX_KEYWORDS:
        raise ViewError(
            f"'{source}' member '{path}' cannot be a field name: '{name}' is a "
            'C++ keyword')
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
        lines.append(f'struct {type_name}; // {source}')

    lines.append('')
    lines.append('} // namespace Assisi::Blueprints')
    lines.append('')
    lines.append('namespace Assisi::Runtime')
    lines.append('{')

    for type_name, source, tree, members in views:
        qualified = f'Blueprints::{type_name}'
        lines.append('')
        lines.append(f'/// @brief The members of `{source}`, nested as the file nests them.')
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
        lines.append(f'    static constexpr std::string_view kSource = "{source}";')
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
            lines.append(f'    view.{field} = FindMember(scene, table, view.instanceId, "{path}");')
        lines.append('}')

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
        if not type_name.isidentifier() or type_name in _CXX_KEYWORDS:
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
