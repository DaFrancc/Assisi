#!/usr/bin/env python3
# Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc").
"""AEVENT(): finding event declarations, and the catalog entry each one emits.

An event reaches C++ by its type, which the compiler checks. It reaches a file
by its name, which nothing checks — so a screen written in markup says
`on_click="Game::ResumeClicked"` and something has to turn that string into a
push. This pass builds that table.

Its own module rather than three more shapes inside the parser and the
generator: the walk reads no struct body, the entry holds no fields, and
nothing here shares machinery with the component scanner beyond namespace
capture.
"""

import re
from dataclasses import dataclass
from pathlib import Path

from reflect_parser import _namespaces_at, strip_comments

# AEVENT() followed by the struct it marks. Deliberately tighter than the struct
# scanner the components use: an event reaches markup by name, so the only thing
# to extract is that name and the namespaces around it.
_AEVENT_RE = re.compile(
    r'\bAEVENT\s*\((?P<args>[^)]*)\)\s*'
    r'(?P<kind>struct|class)\s+(?P<name>\w+)'
)


@dataclass
class EventInfo:
    """One AEVENT() declaration: `struct Name { ... };`

    The catalog name is the fully-qualified type name, derived rather than
    declared — which ASYSTEM deliberately refuses to do, for a reason that does
    not reach here. A system name is an invented string no compiler checks, so
    deriving it lets a C++ rename silently rename content that fails at load. An
    event name is a type, and every file naming one is cooked against this
    catalog, so a rename fails the cook pointing at the file that still says the
    old name.
    """
    name:       str    # unqualified struct name
    namespaces: list   # enclosing namespaces at the declaration
    header:     str    # source header, for diagnostics

    @property
    def fqn(self) -> str:
        """The spelling generated code uses: leading `::`, so nothing depends on
        where the registration happens to be emitted."""
        return '::'.join(['', *self.namespaces, self.name]) if self.namespaces else f'::{self.name}'

    @property
    def catalog_name(self) -> str:
        """The spelling a markup file writes."""
        return '::'.join([*self.namespaces, self.name]) if self.namespaces else self.name


def find_events(text: str, path: Path) -> list:
    """Every AEVENT() declaration in an already-comment-stripped header.

    The fields are not read — an event's wire form is nobody's business here,
    and markup builds one by naming its type and nothing else.
    """
    events: list = []
    for match in _AEVENT_RE.finditer(text):
        where = f"{path.name}: event '{match.group('name')}'"

        if match.group('args').strip():
            raise ValueError(
                f"{where} passes arguments to AEVENT, which takes none. An event's catalog name is "
                f"its fully-qualified type name, so there is nothing to configure — and an argument "
                f"that parsed as nothing would be a name somebody expected to matter.")

        if match.group('kind') != 'struct':
            raise ValueError(
                f"{where} is a class. An event is a struct: markup names the type and builds one, "
                f"so its members have to be reachable and the type has to be an aggregate.")

        events.append(EventInfo(name=match.group('name'),
                                namespaces=_namespaces_at(text, match.start()),
                                header=str(path)))
    return events


def parse_header_events(path: Path) -> list:
    """Every AEVENT() declaration in @p path."""
    return find_events(strip_comments(path.read_text(encoding='utf-8')), path)


def gen_event_registration(event: EventInfo) -> str:
    """One AEVENT declaration's catalog entry.

    The entry holds the push rather than the type, so a caller binds one to a
    button without knowing what it is pushing. That is what lets the UI stay
    ignorant of game types while still firing them.

    The type is named with a leading `::` and constructed inside the lambda, so
    nothing about which type is pushed depends on where this lands.
    """
    return f"""\
// ── {event.catalog_name} (event) {'─' * max(0, 62 - len(event.catalog_name))}
static const bool _reflectgen_event_{event.name} = []() -> bool
{{
    // Markup builds one by name and has no arguments to give it. A type that
    // cannot be default-constructed fails here rather than at the cook of the
    // first file that names it.
    static_assert(std::is_default_constructible_v<{event.fqn}>,
                  "an AEVENT type is built by name from markup, so it must be default-constructible");
    Assisi::Core::EventCatalog::Instance().Register({{
        "{event.catalog_name}",
        [](Assisi::Core::EventQueue &events) {{ events.Push({event.fqn}{{}}); }},
    }});
    return true;
}}();

"""


def check_events(headers) -> list:
    """Duplicate catalog names — whole-tree.

    An event's name is its fully-qualified type name, so two of them can collide
    only by being declared twice in one namespace, which C++ would refuse anyway
    — except across headers that are never included together. A markup file
    naming that event would then push whichever registration won the link, which
    is to say whichever object the linker reached first.
    """
    events: list = []
    for header in headers:
        try:
            events.extend(parse_header_events(Path(header).resolve()))
        except ValueError:
            raise
        except Exception:
            # Already a hard error in the per-header pass; repeating it here
            # would bury the real message.
            continue

    by_name: dict = {}
    for event in sorted(events, key=lambda e: (e.header, e.name)):
        if event.catalog_name in by_name:
            first = by_name[event.catalog_name]
            raise ValueError(
                f"two events are named '{event.catalog_name}':\n"
                f"  {first.header}\n"
                f"  {event.header}\n"
                f"A markup file names an event by this string, so two of them cannot be told apart.")
        by_name[event.catalog_name] = event

    lines = [f'{len(by_name)} event(s)']
    for name in sorted(by_name):
        lines.append(f'  {name}')
    return lines
