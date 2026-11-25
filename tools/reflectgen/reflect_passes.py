"""reflect_passes — APASS: render passes declared on their structs.

A pass is a struct marked APASS(...) whose Record method's signature says what
it reads and writes:

    APASS(name = "sky", after = "draw-scene")
    struct SkyPass
    {
        bool Wanted(const SceneFrame &frame) const;
        bool Record(Recorder &rec, const SceneFrame &frame, const ViewFrame &view,
                    Reads<SceneDepth, CloudMap> in, Writes<SceneColor, SkyPass> out);
    };

One header holds one graph: its passes, the struct that carries the objects
its owner shares with them (APASSIMPORTS), and the struct its passes are
initialised from. This module reads them, orders the passes, refuses an order
that cannot be right, and writes the C++ that records them: straight-line code
in the order found, with every fact read here asserted against the real types.

Python reads only the annotations and Record's declaration, both in one rigid
shape; whether a pass has a hook (Wanted, Release, Initialize,
OnTargetsChanged) is the compiler's to say, through concepts in the generated
code, which also refuses a hook of the right name and the wrong shape.
"""

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

from reflect_parser import _namespaces_at, _split_args, strip_comments

# The annotation and the struct it marks, with no base class: a pass is its own
# state, hooks and Record, and nothing it inherits is read here.
_APASS_RE = re.compile(r'\bAPASS\s*\((?P<args>[^)]*)\)\s*struct\s+(?P<struct>\w+)\s*\{')
_APASS_TOKEN_RE = re.compile(r'\bAPASS\s*\(')
_IMPORTS_RE = re.compile(r'\bAPASSIMPORTS\s*\(\s*\)\s*struct\s+(?P<struct>\w+)\s*\{')
_IMPORTS_TOKEN_RE = re.compile(r'\bAPASSIMPORTS\s*\(')
_VIEW_IMPORTS_RE = re.compile(r'\bAPASSVIEWIMPORTS\s*\(\s*\)\s*struct\s+(?P<struct>\w+)\s*\{')
_VIEW_IMPORTS_TOKEN_RE = re.compile(r'\bAPASSVIEWIMPORTS\s*\(')
_DEFINE_LINE_RE = re.compile(r'\s*#\s*define\s*$')
# Record, declared and not defined in the struct, returning void or bool.
_RECORD_RE = re.compile(r'\b(?P<ret>void|bool)\s+Record\s*\((?P<params>[^)]*)\)\s*;')
_RECORD_TOKEN_RE = re.compile(r'\bRecord\s*\(')
# One shared object an imports struct holds: `Type &name;`.
_IMPORT_MEMBER_RE = re.compile(r'^\s*(?P<type>\w+)\s*&\s*(?P<name>\w+)\s*;\s*$')
_IDENTIFIER_RE = re.compile(r'^\w+$')
# A pass name: lower-case words joined by dashes, as the profiler shows it.
_PASS_NAME_RE = re.compile(r'^[a-z][a-z0-9]*(-[a-z0-9]+)*$')

_APASS_KEYS = {'name', 'after', 'before', 'timers'}
_APASS_FLAGS = {'base', 'untimed'}

# The view a per-view pass is recorded for. Its frame type is the graph's.
_VIEW_FRAME = 'ViewFrame'

# What a per-view pass's resource is called inside the graph when the type is
# one each view owns, so the view's copy and the world's are told apart.
_VIEW_KEY = 'view:'


@dataclass
class PassInfo:
    """One APASS struct, as its annotation and Record declare it."""
    struct: str
    name: str
    after: list
    before: list
    base: bool
    untimed: bool          # opens a GPU marker but no timer: the passes it calls time themselves
    timers: list           # the GPU timers it opens, in order; [name] unless declared
    returns_bool: bool     # Record returns whether the pass stayed on
    recorder: bool         # Record takes the Recorder
    view: bool             # Record takes the view: replayed per view
    reads: list
    writes: list
    namespaces: list
    header: str
    line: int
    # reads and writes as the graph tells resources apart: a type each view
    # owns, read by a per-view pass, carries _VIEW_KEY.
    read_keys: list = field(default_factory=list)
    write_keys: list = field(default_factory=list)

    @property
    def member(self) -> str:
        """Its member in the generated pass set: the struct's name, lower camel
        case, without a trailing Pass."""
        stem = self.struct[:-len('Pass')] if self.struct.endswith('Pass') and self.struct != 'Pass' else self.struct
        return stem[0].lower() + stem[1:]


@dataclass
class ImportsInfo:
    """The APASSIMPORTS struct: each shared object's type and member."""
    struct: str
    members: dict = field(default_factory=dict)   # type -> member name
    # The APASSVIEWIMPORTS struct, on the world's imports only: what each view
    # owns. None on a view's own.
    view: Optional['ImportsInfo'] = None


@dataclass
class Graph:
    """A header's passes, in the order they run, and what they hand on."""
    prefix: str
    passes: list               # PassInfo, in run order
    imports: ImportsInfo
    produced: list             # resources the frame's passes produce, in first-writer order
    namespaces: list
    header: str
    # What each view owns, and the resources its passes produce: empty for a
    # graph with no per-view pass.
    view_imports: ImportsInfo = field(default_factory=lambda: ImportsInfo(struct=''))
    view_produced: list = field(default_factory=list)

    @property
    def frame_passes(self) -> list:
        return [p for p in self.passes if not p.view]

    @property
    def view_passes(self) -> list:
        return [p for p in self.passes if p.view]


def _member_of(type_name: str) -> str:
    return type_name[0].lower() + type_name[1:]


def _line_of(text: str, position: int) -> int:
    return text.count('\n', 0, position) + 1


def _is_macro_definition(text: str, position: int) -> bool:
    line_start = text.rfind('\n', 0, position) + 1
    return _DEFINE_LINE_RE.match(text[line_start:position]) is not None


def _split_params(params: str) -> list:
    """Record's parameters, split at commas outside template brackets."""
    result, current, depth = [], [], 0
    for ch in params:
        if ch == '<':
            depth += 1
        elif ch == '>':
            depth -= 1
        if ch == ',' and depth == 0:
            result.append(''.join(current).strip())
            current = []
        else:
            current.append(ch)
    tail = ''.join(current).strip()
    if tail:
        result.append(tail)
    return result


def _struct_body(text: str, open_brace: int) -> tuple:
    """The body of the struct whose '{' is at @p open_brace, and where it ends."""
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[open_brace + 1:i], i
    raise ValueError('a pass struct is never closed')


def _parse_types(listed: str, where: str, kind: str) -> list:
    types = [t.strip() for t in listed.split(',') if t.strip()]
    if not types:
        raise ValueError(f"{where} lists nothing in {kind}<>; leave the parameter out instead.")
    for t in types:
        if not _IDENTIFIER_RE.match(t):
            raise ValueError(
                f"{where} lists '{t}' in {kind}<>. A resource is named by a bare identifier, the "
                f"same spelling everywhere, so the order can be worked out by name: bring the type "
                f"into the pass header's namespace with a using-declaration.")
    duplicates = sorted({t for t in types if types.count(t) > 1})
    if duplicates:
        raise ValueError(f"{where} lists {', '.join(duplicates)} twice in {kind}<>.")
    return types


def _parse_record(body: str, struct: str, prefix: str, where: str) -> tuple:
    matches = list(_RECORD_RE.finditer(body))
    tokens = list(_RECORD_TOKEN_RE.finditer(body))
    if len(matches) != 1 or len(tokens) != 1:
        raise ValueError(
            f"{where} needs exactly one Record, declared in the struct and defined in the .cpp: "
            f"void Record([Recorder &rec,] const {prefix}Frame &frame[, const {_VIEW_FRAME} &view]"
            f"[, Reads<...> in][, Writes<...> out]); — or returning bool, false turning the pass off.")
    match = matches[0]
    params = _split_params(match.group('params'))

    shape = [
        ('recorder', re.compile(r'^Recorder\s*&\s*\w*$')),
        ('frame', re.compile(r'^const\s+' + re.escape(prefix) + r'Frame\s*&\s*\w*$')),
        ('view', re.compile(r'^const\s+' + _VIEW_FRAME + r'\s*&\s*\w*$')),
        ('reads', re.compile(r'^Reads\s*<(?P<types>[^>]*)>\s*\w*$')),
        ('writes', re.compile(r'^Writes\s*<(?P<types>[^>]*)>\s*\w*$')),
    ]
    found: dict = {}
    position = 0
    for param in params:
        while position < len(shape) and not shape[position][1].match(param):
            position += 1
        if position == len(shape):
            raise ValueError(
                f"{where}'s Record takes '{param}', which is not part of a pass's signature, or comes "
                f"out of order. The parameters are, in this order: Recorder &, const {prefix}Frame &, "
                f"const {_VIEW_FRAME} &, Reads<...>, Writes<...>; only the frame is required.")
        found[shape[position][0]] = shape[position][1].match(param)
        position += 1
    if 'frame' not in found:
        raise ValueError(f"{where}'s Record does not take const {prefix}Frame &, which every pass does.")

    reads = _parse_types(found['reads'].group('types'), where, 'Reads') if 'reads' in found else []
    writes = _parse_types(found['writes'].group('types'), where, 'Writes') if 'writes' in found else []
    both = sorted(set(reads) & set(writes))
    if both:
        raise ValueError(
            f"{where} lists {', '.join(both)} in both Reads<> and Writes<>. Writes<> may read what "
            f"it lists, so name each once, in Writes<>.")
    return (match.group('ret') == 'bool', 'recorder' in found, 'view' in found, reads, writes)


def _quoted(value: str, key: str, where: str) -> str:
    value = value.strip()
    if len(value) < 2 or value[0] != '"' or value[-1] != '"':
        raise ValueError(f"{where} writes '{key} = {value}'. A pass name is quoted: {key} = \"{value.strip(chr(34))}\".")
    inner = value[1:-1]
    if not _PASS_NAME_RE.match(inner):
        raise ValueError(
            f"{where} names '{inner}' in {key}. Pass and timer names are lower-case words joined by "
            f"dashes, as the profiler shows them.")
    return inner


def parse_passes(text: str, path: Path, prefix: str) -> tuple:
    """Every APASS struct and the APASSIMPORTS struct in a comment-stripped
    header. Refuses any annotation in a shape this does not read, rather than
    skipping it."""
    passes: list = []
    declared = set()
    for match in _APASS_RE.finditer(text):
        declared.add(match.start())
        line = _line_of(text, match.start())
        struct = match.group('struct')
        where = f"{path.name}:{line}: pass {struct}"

        name, after, before, timers, flags = '', [], [], [], set()
        for arg in _split_args(match.group('args')):
            arg = arg.strip()
            if not arg:
                continue
            if '=' in arg:
                key, _, value = arg.partition('=')
                key, value = key.strip(), value.strip()
                if key not in _APASS_KEYS:
                    raise ValueError(f"{where} sets unknown key '{key}'. Recognised: {', '.join(sorted(_APASS_KEYS))}.")
                if key == 'timers':
                    if not (value.startswith('{') and value.endswith('}')):
                        raise ValueError(f"{where} writes 'timers = {value}'. Timers are a braced list: timers = {{\"a\", \"b\"}}.")
                    timers = [_quoted(v, key, where) for v in _split_args(value[1:-1]) if v.strip()]
                elif key == 'name':
                    name = _quoted(value, key, where)
                elif key == 'after':
                    after.append(_quoted(value, key, where))
                else:
                    before.append(_quoted(value, key, where))
                continue
            if arg not in _APASS_FLAGS:
                raise ValueError(f"{where} carries unknown flag '{arg}'. Recognised: {', '.join(sorted(_APASS_FLAGS))}.")
            flags.add(arg)

        if not name:
            raise ValueError(
                f"{where} has no name. Every pass needs one: APASS(name = \"something\"). It is the "
                f"name the profiler shows, and what after and before refer to.")
        untimed = 'untimed' in flags
        if untimed and timers:
            raise ValueError(f"{where} is untimed and lists timers. An untimed pass opens no timer of its own.")
        if untimed and 'base' in flags:
            raise ValueError(
                f"{where} is both base and untimed. A base pass is found in a capture by its timer, "
                f"and an untimed pass has none.")
        timers = timers or [name]
        duplicate_timers = sorted({t for t in timers if timers.count(t) > 1})
        if duplicate_timers:
            raise ValueError(f"{where} lists the timer {', '.join(duplicate_timers)} twice.")
        base = 'base' in flags
        if base and name not in timers:
            raise ValueError(
                f"{where} is a base pass whose timers do not include its name. The empty-scene check "
                f"looks for a base pass by its name, so one of its timers carries it.")

        body, _ = _struct_body(text, match.end() - 1)
        returns_bool, recorder, view, reads, writes = _parse_record(body, struct, prefix, where)
        if base and returns_bool:
            raise ValueError(
                f"{where} is a base pass whose Record returns bool. A base pass records every frame, so it "
                f"cannot turn itself off: Record returns void.")
        passes.append(PassInfo(struct=struct, name=name, after=after, before=before, base=base, untimed=untimed,
                               timers=timers,
                               returns_bool=returns_bool, recorder=recorder, view=view, reads=reads,
                               writes=writes, namespaces=_namespaces_at(text, match.start()), header=str(path),
                               line=line))

    for token in _APASS_TOKEN_RE.finditer(text):
        if token.start() not in declared and not _is_macro_definition(text, token.start()):
            raise ValueError(
                f"{path.name}:{_line_of(text, token.start())}: this APASS is not followed by a struct "
                f"reflectgen reads, so the pass would never run. Write it as APASS(...) struct Name {{ ... }}; "
                f"with no base class.")

    imports = _parse_imports(text, path, _IMPORTS_RE, _IMPORTS_TOKEN_RE, 'APASSIMPORTS')
    view_imports = _parse_imports(text, path, _VIEW_IMPORTS_RE, _VIEW_IMPORTS_TOKEN_RE, 'APASSVIEWIMPORTS')
    if view_imports.members and not any(p.view for p in passes):
        raise ValueError(f"{path.name}: {view_imports.struct} lists what each view owns, and no pass is "
                         f"recorded per view.")
    for p in passes:
        p.read_keys = [_key_of(t, p, imports, view_imports) for t in p.reads]
        p.write_keys = [_key_of(t, p, imports, view_imports) for t in p.writes]
    imports.view = view_imports
    return passes, imports


def _parse_imports(text: str, path: Path, struct_re, token_re, macro: str) -> ImportsInfo:
    """The one struct @p macro marks, if the header has it: each shared
    object's type and member."""
    matches = list(struct_re.finditer(text))
    tokens = [t for t in token_re.finditer(text) if not _is_macro_definition(text, t.start())]
    if len(matches) != len(tokens):
        raise ValueError(f"{path.name}: an {macro} is not followed by a struct reflectgen reads: "
                         f"{macro}() struct Name {{ Type &member; ... }};")
    if len(matches) > 1:
        raise ValueError(f"{path.name}: one graph has one {macro} struct, and this header has several.")
    imports = ImportsInfo(struct='')
    if matches:
        match = matches[0]
        imports.struct = match.group('struct')
        body, _ = _struct_body(text, match.end() - 1)
        for statement in body.split(';'):
            statement = statement.strip()
            if not statement:
                continue
            member = _IMPORT_MEMBER_RE.match(statement + ';')
            if member is None:
                raise ValueError(
                    f"{path.name}: {imports.struct} holds '{statement}'. It holds only references to the "
                    f"shared objects its passes reach, one per line: Type &member;")
            if member.group('type') in imports.members:
                raise ValueError(f"{path.name}: {imports.struct} holds {member.group('type')} twice.")
            imports.members[member.group('type')] = member.group('name')
    return imports


def _key_of(t: str, p: PassInfo, imports: ImportsInfo, view_imports: ImportsInfo) -> str:
    """How the graph names @p t as @p p uses it: the view's own copy when @p p
    is recorded per view and each view owns one, else the type itself."""
    if t not in view_imports.members:
        return t
    if p.view:
        return _VIEW_KEY + t
    if t in imports.members:
        return t
    raise ValueError(
        f"{p.name} runs once a frame and uses {t}, which each view owns for itself "
        f"({view_imports.struct}). With two views there would be two, and nothing to say which it means.")


def _is_import(key: str, imports: ImportsInfo) -> bool:
    """Whether @p key is an object the graph's owner shares, the world's or a
    view's, rather than one a pass produces."""
    return key in imports.members or key.startswith(_VIEW_KEY)


def _edges(passes: list, imports: ImportsInfo) -> tuple:
    """Every ordering edge, with why: (from, to) -> reason. Refuses a produced
    resource with no writer or several, and a read nothing writes."""
    by_name = {p.name: p for p in passes}
    writers: dict = {}
    readers: dict = {}
    for p in passes:
        for t in p.write_keys:
            writers.setdefault(t, []).append(p)
        for t in p.read_keys:
            readers.setdefault(t, []).append(p)

    for t, rs in readers.items():
        if t not in writers and not _is_import(t, imports):
            raise ValueError(
                f"{rs[0].name} reads {t}, which no pass writes. A resource reaches a pass from the pass "
                f"that writes it, or from {imports.struct or 'the imports struct'} when its owner shares it.")
    for t, ws in writers.items():
        if not _is_import(t, imports) and len(ws) > 1:
            raise ValueError(
                f"{t} is written by {' and '.join(w.name for w in ws)}. A resource the passes produce has "
                f"one writer, so turning that pass off can empty it without erasing another's work: "
                f"split it, or have its owner share it through {imports.struct or 'the imports struct'}.")

    edges: dict = {}
    for t, ws in writers.items():
        for w in ws:
            for r in readers.get(t, []):
                if r is not w:
                    edges.setdefault((w.name, r.name), f"{w.name} writes {t}, which {r.name} reads")
    for p in passes:
        for target in p.after + p.before:
            if target not in by_name:
                raise ValueError(f"{p.name} is ordered against '{target}', which no pass is named.")
            if target == p.name:
                raise ValueError(f"{p.name} is ordered against itself.")
        for target in p.after:
            edges.setdefault((target, p.name), f"{p.name} is after {target}")
        for target in p.before:
            edges.setdefault((p.name, target), f"{p.name} is before {target}")
    return edges, writers


def _find_cycle(names: list, edges: dict) -> list:
    successors: dict = {n: [] for n in names}
    for (a, b) in edges:
        successors[a].append(b)
    colour: dict = {n: 0 for n in names}
    stack: list = []

    def visit(n: str) -> Optional[list]:
        colour[n] = 1
        stack.append(n)
        for m in successors[n]:
            if colour[m] == 1:
                return stack[stack.index(m):] + [m]
            if colour[m] == 0:
                found = visit(m)
                if found:
                    return found
        stack.pop()
        colour[n] = 2
        return None

    for n in names:
        if colour[n] == 0:
            found = visit(n)
            if found:
                return found
    return []


def order_passes(passes: list, imports: ImportsInfo) -> list:
    """The passes in the order they run: every edge respected, ties broken by
    declaration order. Refuses a cycle, naming the resource behind each step,
    and two writers of a shared object with no order between them."""
    seen: dict = {}
    for p in passes:
        if p.name in seen:
            raise ValueError(f"two passes are named '{p.name}': {seen[p.name].struct} and {p.struct}.")
        seen[p.name] = p
    structs = [p.struct for p in passes]
    duplicate_structs = sorted({s for s in structs if structs.count(s) > 1})
    if duplicate_structs:
        raise ValueError(f"the struct {', '.join(duplicate_structs)} is marked APASS twice.")

    edges, writers = _edges(passes, imports)
    names = [p.name for p in passes]
    index = {n: i for i, n in enumerate(names)}
    incoming = {n: 0 for n in names}
    successors: dict = {n: [] for n in names}
    for (a, b) in edges:
        incoming[b] += 1
        successors[a].append(b)

    order: list = []
    ready = sorted((n for n in names if incoming[n] == 0), key=index.get)
    while ready:
        n = ready.pop(0)
        order.append(n)
        for m in successors[n]:
            incoming[m] -= 1
            if incoming[m] == 0:
                ready.append(m)
        ready.sort(key=index.get)
    if len(order) != len(names):
        cycle = _find_cycle(names, edges)
        steps = '\n'.join(f"  {edges[(cycle[i], cycle[i + 1])]}" for i in range(len(cycle) - 1))
        raise ValueError(f"the passes cannot be ordered, because each of these must come before the next:\n{steps}")

    position = {n: i for i, n in enumerate(order)}
    reach: dict = {n: set() for n in names}
    for n in reversed(order):
        for m in successors[n]:
            reach[n] |= {m} | reach[m]
    for t, ws in writers.items():
        if not _is_import(t, imports):
            continue
        for i, a in enumerate(ws):
            for b in ws[i + 1:]:
                # A frame pass and a view pass are ordered already: the
                # frame's passes all record before the views'.
                if a.view != b.view:
                    continue
                if b.name not in reach[a.name] and a.name not in reach[b.name]:
                    raise ValueError(
                        f"{a.name} and {b.name} both write {t.removeprefix(_VIEW_KEY)}, and nothing orders "
                        f"them. Say which goes first with after = \"...\" or before = \"...\".")

    by_name = {p.name: p for p in passes}
    for p in passes:
        if p.view:
            continue
        for t in p.read_keys:
            for w in writers.get(t, []):
                if w.view and not _is_import(t, imports):
                    raise ValueError(
                        f"{p.name} runs once a frame but reads {t}, which {w.name} writes once per view. "
                        f"With two views there would be two, and nothing to say which it means.")
    # Every pass run once a frame records before any view's: the views come
    # after the frame's work, each in turn.
    for (a, b), why in edges.items():
        if by_name[a].view and not by_name[b].view:
            raise ValueError(
                f"{b} runs once a frame and must come after {a}, which runs once per view ({why}). "
                f"The frame's passes all record before the views'.")
    in_order = [by_name[n] for n in sorted(names, key=position.get)]
    return [p for p in in_order if not p.view] + [p for p in in_order if p.view]


def build_graph(header: Path, prefix: str) -> Graph:
    text = strip_comments(header.read_text(encoding='utf-8'))
    passes, imports = parse_passes(text, header, prefix)
    if not passes:
        raise ValueError(f"{header.name} declares no APASS struct.")
    namespaces = {tuple(p.namespaces) for p in passes}
    if len(namespaces) != 1:
        raise ValueError(f"{header.name}: every pass of one graph is declared in the same namespace.")
    ordered = order_passes(passes, imports)
    produced: list = []
    view_produced: list = []
    for p in ordered:
        for t in p.write_keys:
            into = view_produced if p.view else produced
            if not _is_import(t, imports) and t not in into:
                into.append(t)
    return Graph(prefix=prefix, passes=ordered, imports=imports, produced=produced,
                 namespaces=list(passes[0].namespaces), header=str(header),
                 view_imports=imports.view or ImportsInfo(struct=''), view_produced=view_produced)


def _resource_ref(t: str, graph: Graph) -> str:
    """Where the walk finds the resource the graph calls @p t: the world's
    imports, the view's, what the frame's passes produced, or what this view's
    produced."""
    if t in graph.imports.members:
        return f"imports.{graph.imports.members[t]}"
    if t.startswith(_VIEW_KEY):
        return f"viewImports.{graph.view_imports.members[t.removeprefix(_VIEW_KEY)]}"
    if t in graph.view_produced:
        return f"viewPasses.produced.{_member_of(t)}"
    return f"produced.{_member_of(t)}"


def _record_type(p: PassInfo, prefix: str) -> str:
    params = []
    if p.recorder:
        params.append('Render::Recorder &')
    params.append(f'const {prefix}Frame &')
    if p.view:
        params.append(f'const {_VIEW_FRAME} &')
    if p.reads:
        params.append(f"Render::Reads<{', '.join(p.reads)}>")
    if p.writes:
        params.append(f"Render::Writes<{', '.join(p.writes)}>")
    ret = 'bool' if p.returns_bool else 'void'
    return f"{ret} ({p.struct}::*)({', '.join(params)})"


def _run_params(graph: Graph) -> str:
    """Run's parameters, or RunFrame's in a graph with views."""
    return (f"{graph.imports.struct} &imports, nvrhi::ICommandList *commandList, "
            f"const {graph.prefix}Frame &frame")


def _run_view_params(graph: Graph) -> str:
    """RunView's parameters: the view's own passes and imports, then the frame
    and the view."""
    g = graph.prefix
    params = f"{g}ViewPasses &viewPasses, {graph.imports.struct} &imports, "
    if graph.view_imports.struct:
        params += f"{graph.view_imports.struct} &viewImports, "
    return params + f"nvrhi::ICommandList *commandList, const {g}Frame &frame, const {_VIEW_FRAME} &view"


def _timer_count(passes: list) -> int:
    return sum(len(p.timers) for p in passes if not p.untimed)


def _render_view_header(graph: Graph, include: str) -> str:
    """The header of a graph with passes recorded per view: the frame's passes
    on the graph, and each view's in an object of its own, kept by whoever
    owns the view."""
    g = graph.prefix
    ns = '::'.join(graph.namespaces)
    lines = [
        f"/* Generated by reflectgen from {include}. Do not edit. */",
        "#pragma once",
        "",
        f"#include <{include}>",
        "",
        "#include <nvrhi/nvrhi.h>",
        "",
        "#include <cstdint>",
        "",
        f"namespace {ns}",
        "{",
        "",
        f"/// One of each {g} pass recorded once a frame, in the order they run.",
        f"struct {g}PassSet",
        "{",
    ]
    lines += [f"    {p.struct} {p.member};" for p in graph.frame_passes]
    lines += [
        "};",
        "",
        f"/// What the {g} passes recorded once a frame produce, each written by one pass and empty while it is off.",
        f"struct {g}Produced",
        "{",
    ]
    lines += [f"    {t} {_member_of(t)};" for t in graph.produced]
    lines += [
        "};",
        "",
        f"/// One of each {g} pass recorded per view, in the order they run.",
        f"struct {g}ViewPassSet",
        "{",
    ]
    lines += [f"    {p.struct} {p.member};" for p in graph.view_passes]
    lines += [
        "};",
        "",
        f"/// What one view's {g} passes produce, each written by one pass and empty while it is off.",
        f"struct {g}ViewProduced",
        "{",
    ]
    lines += [f"    {t} {_member_of(t)};" for t in graph.view_produced]
    lines += [
        "};",
        "",
        f"/// The GPU timers the frame's {g} passes and one view's can open.",
        f"inline constexpr std::uint32_t k{g}PassTimerCount = {_timer_count(graph.passes)};",
        "",
        f"class {g}PassGraph;",
        "",
        f"/// One view's own {g} passes and what they produce. Kept from frame to frame by",
        "/// whoever owns the view, so what a pass remembers belongs to that view alone.",
        f"class {g}ViewPasses",
        "{",
        "public:",
        f"    {g}ViewPassSet passes;",
        f"    {g}ViewProduced produced;",
        "",
        "private:",
        f"    friend class {g}PassGraph;",
        f"    bool _usable[{len(graph.view_passes)}] = {{}};",
        "};",
        "",
        f"/// The {g} passes, recorded in the order their declarations fix: the frame's",
        "/// once, then each view's in turn.",
        f"class {g}PassGraph",
        "{",
        "public:",
        "    /// Each frame pass's Initialize, then its OnTargetsChanged with @p targets. A",
        "    /// pass whose Initialize fails stays off.",
        f"    void Initialize(const {g}PassInit &init, const nvrhi::FramebufferInfo &targets);",
        "    /// Each frame pass's OnTargetsChanged, for a scene target of a new format or sample count.",
        "    void OnTargetsChanged(const nvrhi::FramebufferInfo &targets);",
        "    /// Each of a view's passes' Initialize, then its OnTargetsChanged with @p targets.",
        f"    void InitializeView({g}ViewPasses &viewPasses, const {g}PassInit &init,",
        "                        const nvrhi::FramebufferInfo &targets);",
        "    /// Each of a view's passes' OnTargetsChanged.",
        f"    void OnViewTargetsChanged({g}ViewPasses &viewPasses, const nvrhi::FramebufferInfo &targets);",
        "    /// Record every pass run once a frame, in order, into @p commandList.",
        f"    void RunFrame({_run_params(graph)});",
        "    /// Record one view's passes, in order, into @p commandList, after RunFrame.",
        f"    void RunView({_run_view_params(graph)});",
        "",
        f"    {g}PassSet passes;",
        f"    {g}Produced produced;",
        "",
        "private:",
        f"    bool _usable[{len(graph.frame_passes)}] = {{}};",
        "};",
        "",
        f"}} // namespace {ns}",
        "",
    ]
    return '\n'.join(lines)


def render_header(graph: Graph, include: str) -> str:
    if graph.view_passes:
        return _render_view_header(graph, include)
    g = graph.prefix
    ns = '::'.join(graph.namespaces)
    timer_count = sum(len(p.timers) for p in graph.passes if not p.untimed)
    lines = [
        f"/* Generated by reflectgen from {include}. Do not edit. */",
        "#pragma once",
        "",
        f"#include <{include}>",
        "",
        "#include <nvrhi/nvrhi.h>",
        "",
        "#include <cstdint>",
        "",
        f"namespace {ns}",
        "{",
        "",
        f"/// One of each {g} pass, in the order they run.",
        f"struct {g}PassSet",
        "{",
    ]
    lines += [f"    {p.struct} {p.member};" for p in graph.passes]
    lines += [
        "};",
        "",
        f"/// What the {g} passes produce, each written by one pass and empty while it is off.",
        f"struct {g}Produced",
        "{",
    ]
    lines += [f"    {t} {_member_of(t)};" for t in graph.produced]
    lines += [
        "};",
        "",
        f"/// The GPU timers the {g} passes can open in one frame.",
        f"inline constexpr std::uint32_t k{g}PassTimerCount = {timer_count};",
        "",
        f"/// The {g} passes, recorded in the order their declarations fix.",
        f"class {g}PassGraph",
        "{",
        "public:",
        "    /// Each pass's Initialize, then its OnTargetsChanged with @p targets. A pass",
        "    /// whose Initialize fails stays off.",
        f"    void Initialize(const {g}PassInit &init, const nvrhi::FramebufferInfo &targets);",
        "    /// Each pass's OnTargetsChanged, for a scene target of a new format or sample count.",
        "    void OnTargetsChanged(const nvrhi::FramebufferInfo &targets);",
        "    /// Record every pass, in order, into @p commandList.",
        f"    void Run({_run_params(graph)});",
        "",
        f"    {g}PassSet passes;",
        f"    {g}Produced produced;",
        "",
        "private:",
        f"    bool _usable[{len(graph.passes)}] = {{}};",
        "};",
        "",
        f"}} // namespace {ns}",
        "",
    ]
    return '\n'.join(lines)


def render_source(graph: Graph, generated_header: str) -> str:
    g = graph.prefix
    ns = '::'.join(graph.namespaces)
    frame_types = lambda p: f'{g}Frame, {_VIEW_FRAME}' if p.view else f'{g}Frame'
    out = [
        f"/* Generated by reflectgen from {graph.header.split('include/')[-1]}. Do not edit. */",
        "",
        f"#include <{generated_header}>",
        "",
        "#include <Assisi/Chiara/Profile.hpp>",
        "#include <Assisi/Render/FramePass.hpp>",
        "",
        "#include <type_traits>",
        "",
        f"namespace {ns}",
        "{",
        "namespace",
        "{",
    ]
    for p in graph.passes:
        timer_list = ', '.join(f'"{t}"' for t in p.timers)
        out.append(f"constexpr const char *k{p.struct}Timers[] = {{{timer_list}}};")
    out.append("")
    for p in graph.passes:
        # A per-view pass's Wanted may take the view too, or the frame alone.
        wanted_fits = f"Render::WantedFits<{p.struct}, {g}Frame>"
        wanted_shape = f"bool Wanted(const {g}Frame &) const"
        if p.view:
            wanted_fits += f" || Render::WantedFits<{p.struct}, {frame_types(p)}>"
            wanted_shape = f"bool Wanted(const {g}Frame &[, const {_VIEW_FRAME} &]) const"
        out += [
            f"static_assert(std::is_same_v<decltype(&{p.struct}::Record), {_record_type(p, g)}>,",
            f"              \"{p.struct}::Record is not declared the way reflectgen read it\");",
            f"static_assert(!Render::HasWanted<{p.struct}> || {wanted_fits},",
            f"              \"{p.struct}::Wanted must be {wanted_shape}\");",
            f"static_assert(!Render::HasRelease<{p.struct}> || Render::ReleaseFits<{p.struct}>,",
            f"              \"{p.struct}::Release must be void Release()\");",
            f"static_assert(!Render::HasInitialize<{p.struct}> || Render::InitializeFits<{p.struct}, {g}PassInit>,",
            f"              \"{p.struct}::Initialize must be bool Initialize(const {g}PassInit &)\");",
            f"static_assert(!Render::HasOnTargetsChanged<{p.struct}> || Render::OnTargetsChangedFits<{p.struct}>,",
            f"              \"{p.struct}::OnTargetsChanged must be bool OnTargetsChanged(const nvrhi::FramebufferInfo &)\");",
        ]
        if p.base:
            out += [
                f"static_assert(!Render::HasWanted<{p.struct}>,",
                f"              \"{p.struct} is a base pass, which every frame records, so it has no Wanted\");",
            ]
    out += ["} // namespace", ""]

    if graph.view_passes:
        out += _render_view_walks(graph)
    else:
        out += _render_hooks(graph, graph.passes, "Initialize", "OnTargetsChanged", "passes", "_usable", "")
        out += [f"void {g}PassGraph::Run({_run_params(graph)})", "{"]
        for i, p in enumerate(graph.passes):
            out += _walk_block(graph, p, f"passes.{p.member}", f"_usable[{i}]")
        out += ["}", ""]
    out += [f"}} // namespace {ns}", ""]
    return '\n'.join(out)


def _render_hooks(graph: Graph, passes: list, initialize: str, targets_changed: str, set_ref: str, usable: str,
                  view_param: str) -> list:
    """Initialize and OnTargetsChanged over @p passes, which live in @p set_ref
    and are switched on in @p usable; @p view_param is the view's parameter
    for a view's hooks, empty for the frame's."""
    g = graph.prefix
    out = [f"void {g}PassGraph::{initialize}({view_param}const {g}PassInit &init, const nvrhi::FramebufferInfo &targets)",
           "{"]
    for i, p in enumerate(passes):
        out.append(f"    {usable}[{i}] = Render::PassInitialize({set_ref}.{p.member}, init);")
    view_arg = 'viewPasses, ' if view_param else ''
    out += [f"    {targets_changed}({view_arg}targets);", "}", ""]
    out += [f"void {g}PassGraph::{targets_changed}({view_param}const nvrhi::FramebufferInfo &targets)", "{"]
    for i, p in enumerate(passes):
        out.append(f"    {usable}[{i}] = {usable}[{i}] && Render::PassOnTargetsChanged({set_ref}.{p.member}, targets);")
    out += ["}", ""]
    return out


def _render_view_walks(graph: Graph) -> list:
    """A graph with views: the frame's hooks and walk, then a view's."""
    g = graph.prefix
    out = _render_hooks(graph, graph.frame_passes, "Initialize", "OnTargetsChanged", "passes", "_usable", "")
    out += _render_hooks(graph, graph.view_passes, "InitializeView", "OnViewTargetsChanged", "viewPasses.passes",
                         "viewPasses._usable", f"{g}ViewPasses &viewPasses, ")
    out += [f"void {g}PassGraph::RunFrame({_run_params(graph)})", "{"]
    for i, p in enumerate(graph.frame_passes):
        out += _walk_block(graph, p, f"passes.{p.member}", f"_usable[{i}]")
    out += ["}", ""]
    out += [f"void {g}PassGraph::RunView({_run_view_params(graph)})", "{"]
    for i, p in enumerate(graph.view_passes):
        out += _walk_block(graph, p, f"viewPasses.passes.{p.member}", f"viewPasses._usable[{i}]")
    out += ["}", ""]
    return out


def _walk_block(graph: Graph, p: PassInfo, pass_ref: str, usable: str) -> list:
    """The walk's recording of @p p, held at @p pass_ref and switched on at
    @p usable: recorded when wanted, released and its outputs emptied when off."""
    frame_args = 'frame, view' if p.view else 'frame'
    produced_outputs = [t for t in p.write_keys if not _is_import(t, graph.imports)]

    def off(indent: str) -> list:
        """A pass that is off releases what it holds and empties what it produced."""
        return ([f"{indent}Render::PassRelease({pass_ref});"] +
                [f"{indent}{_resource_ref(t, graph)} = {{}};" for t in produced_outputs])

    summary = []
    if p.reads:
        summary.append(f"reads {', '.join(p.reads)}")
    if p.writes:
        summary.append(f"writes {', '.join(p.writes)}")
    out = [f"    // {p.name} ({'per view' if p.view else 'per frame'}){': ' + '; '.join(summary) if summary else ''}"]
    args = []
    if p.recorder:
        args.append('rec')
    args.append(frame_args)
    if p.reads:
        args.append(f"Render::Reads<{', '.join(p.reads)}>{{{', '.join(_resource_ref(t, graph) for t in p.read_keys)}}}")
    if p.writes:
        args.append(f"Render::Writes<{', '.join(p.writes)}>{{{', '.join(_resource_ref(t, graph) for t in p.write_keys)}}}")
    call = f"{pass_ref}.Record({', '.join(args)})"
    if p.untimed:
        timing = 'Untimed'
    elif p.base and len(p.timers) == 1:
        timing = 'Eager'
    else:
        timing = 'Lazy'
    out += [
        "    {",
        f"        ASSISI_PROFILE_SCOPE(\"{p.name}\");",
        f"        if ({usable} && Render::PassWanted({pass_ref}, {frame_args}))",
        "        {",
        f"            Render::Recorder rec{{commandList, k{p.struct}Timers, Render::RecorderTiming::{timing}}};",
    ]
    if p.returns_bool:
        out += [f"            if (!{call})", "            {"] + off("                ") + ["            }"]
    else:
        if not p.recorder:
            out.append("            (void)rec;")
        out.append(f"            {call};")
    out += ["        }", "        else", "        {"] + off("            ") + ["        }", "    }"]
    return out


def write_if_changed(path: Path, text: str) -> None:
    """Write @p text unless @p path already holds it, so a regeneration that
    changes nothing recompiles nothing."""
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding='utf-8') == text:
        return
    path.write_text(text, encoding='utf-8')
