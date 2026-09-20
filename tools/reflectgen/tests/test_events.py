#!/usr/bin/env python3
# Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc").
"""Behavioural tests for reflectgen's AEVENT pass.

The pass exists so a file can name an event C++ names by type. What these
defend is the name: a catalog key that drifted from the type would leave every
markup file naming an event that no longer registers, and the cook is the only
thing standing between that and a shipped menu whose button does nothing.

Run directly (`python test_events.py`) or via ctest.
"""

import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))  # tools/reflectgen

import reflect_codegen  # noqa: E402
import reflect_events  # noqa: E402
import reflect_parser  # noqa: E402


class EventTest(unittest.TestCase):
    """AEVENT: the grammar, the whole-tree name check, and the emitted entry.

    An event's catalog name is its fully-qualified type name, derived rather than
    declared — the opposite of ASYSTEM, and for a reason that does not apply here.
    A system name is an invented string no compiler checks, so deriving it would
    let a C++ rename silently rename content. An event name is a type, and every
    file naming one is cooked against this catalog, so a rename fails the cook
    with the file and line that still says the old name.
    """

    SOURCE = ("namespace Game {\n"
              "AEVENT()\nstruct ResumeClicked\n{\n};\n"
              "AEVENT()\nstruct QuitRequested { };\n"
              "}\n")

    def _events(self, text):
        return reflect_events.find_events(reflect_parser.strip_comments(text), Path("T.hpp"))

    def test_grammar_reaches_the_definition(self):
        found = {e.catalog_name: e for e in self._events(self.SOURCE)}
        self.assertEqual(set(found), {"Game::ResumeClicked", "Game::QuitRequested"})
        self.assertEqual(found["Game::ResumeClicked"].name, "ResumeClicked")
        self.assertEqual(found["Game::ResumeClicked"].fqn, "::Game::ResumeClicked")

    def test_a_nested_namespace_reaches_the_catalog_name(self):
        found = self._events("namespace Assisi::App {\nAEVENT()\nstruct QuitRequested { };\n}\n")
        self.assertEqual(found[0].catalog_name, "Assisi::App::QuitRequested")

    def test_an_event_outside_any_namespace_is_named_by_its_type(self):
        found = self._events("AEVENT()\nstruct Bare { };\n")
        self.assertEqual(found[0].catalog_name, "Bare")
        self.assertEqual(found[0].fqn, "::Bare")

    def test_a_commented_out_event_is_not_found(self):
        # The walk runs over stripped text, so an event inside a comment must not
        # reach the catalog — it would register a type that is not declared.
        found = self._events("// AEVENT()\n// struct Ghost { };\n"
                             "/* AEVENT()\nstruct AlsoGhost { }; */\n")
        self.assertEqual(found, [])

    def test_arguments_are_refused(self):
        # AEVENT carries nothing today. Accepting an argument silently would let a
        # name someone expected to matter do nothing at all.
        with self.assertRaises(ValueError) as caught:
            self._events("AEVENT(name = \"Resume\")\nstruct ResumeClicked { };\n")
        self.assertIn("AEVENT", str(caught.exception))

    def test_a_class_is_refused(self):
        # Markup builds an event by naming its type, so every member has to be
        # reachable and the type has to be an aggregate.
        with self.assertRaises(ValueError) as caught:
            self._events("AEVENT()\nclass ResumeClicked { };\n")
        self.assertIn("struct", str(caught.exception))

    def test_duplicate_names_are_a_build_error_naming_both(self):
        with tempfile.TemporaryDirectory() as d:
            first = Path(d) / "A.hpp"
            first.write_text("namespace Game {\nAEVENT()\nstruct Resume { };\n}\n", encoding="utf-8")
            second = Path(d) / "B.hpp"
            second.write_text("namespace Game {\nAEVENT()\nstruct Resume { };\n}\n", encoding="utf-8")

            with self.assertRaises(ValueError) as caught:
                reflect_events.check_events([first, second])
            message = str(caught.exception)
            self.assertIn("Game::Resume", message)
            self.assertIn("A.hpp", message)
            self.assertIn("B.hpp", message)

    def test_two_events_of_the_same_name_in_different_namespaces_are_both_kept(self):
        # The name carries its namespaces, so these cannot collide — and a check
        # that flattened them would refuse a tree that is perfectly well-formed.
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "A.hpp"
            path.write_text("namespace Game {\nAEVENT()\nstruct Resume { };\n}\n"
                            "namespace Editor {\nAEVENT()\nstruct Resume { };\n}\n", encoding="utf-8")
            lines = reflect_events.check_events([path])
            self.assertIn("2 event(s)", lines[0])

    def test_the_registration_names_the_type_and_guards_construction(self):
        events = self._events(self.SOURCE)
        emitted = reflect_events.gen_event_registration(events[0])
        self.assertIn('"Game::ResumeClicked"', emitted)
        self.assertIn("::Game::ResumeClicked", emitted)
        # Markup default-constructs the event by name, so a type that cannot be
        # has to fail the build rather than the cook of a file naming it.
        self.assertIn("is_default_constructible", emitted)

    def test_a_header_with_events_includes_the_catalog(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "A.hpp"
            path.write_text(self.SOURCE, encoding="utf-8")
            cpp = reflect_codegen.generate_cpp(
                [], "A.hpp", events=reflect_events.parse_header_events(path))
            self.assertIn("Assisi/Core/EventCatalog.hpp", cpp)
            self.assertIn('"Game::QuitRequested"', cpp)


if __name__ == "__main__":
    unittest.main()
