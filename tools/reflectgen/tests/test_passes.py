#!/usr/bin/env python3
# Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc").
"""Behavioural tests for reflectgen's APASS graph.

The graph is the frame's order, worked out from what each pass reads and
writes and from the orders it states. What these defend is that an order that
cannot be right fails the build with a message naming the passes, instead of
recording a frame whose passes read what has not been written yet.

Run directly (`python test_passes.py`) or via ctest.
"""

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))  # tools/reflectgen

import reflect_parser  # noqa: E402
import reflect_passes  # noqa: E402

IMPORTS = (
    "APASSIMPORTS()\n"
    "struct TestImports\n"
    "{\n"
    "    Target &target;\n"
    "    Service &service;\n"
    "};\n")


def a_pass(struct, name, record, extra=''):
    """One APASS struct with @p record as its Record's parameters after the frame."""
    args = f'name = "{name}"' + (f', {extra}' if extra else '')
    return (f"APASS({args})\n"
            f"struct {struct}\n"
            "{\n"
            f"    void Record(Recorder &rec, const TestFrame &frame{record});\n"
            "};\n")


def graph_of(*passes, imports=IMPORTS):
    text = reflect_parser.strip_comments("namespace Game {\n" + imports + ''.join(passes) + "}\n")
    found, imported = reflect_passes.parse_passes(text, Path("T.hpp"), "Test")
    return reflect_passes.order_passes(found, imported)


def names(ordered):
    return [p.name for p in ordered]


class OrderTest(unittest.TestCase):
    def test_independent_passes_keep_their_declared_order(self):
        # Three, because the first pick is decided before any tie is broken.
        ordered = graph_of(a_pass("APass", "a", ", Writes<X> out"), a_pass("BPass", "b", ", Writes<Y> out"),
                           a_pass("CPass", "c", ", Writes<Z> out"))
        self.assertEqual(names(ordered), ["a", "b", "c"])

    def test_a_pass_freed_by_its_writer_waits_its_turn_behind_earlier_ones(self):
        ordered = graph_of(a_pass("WriterPass", "writer", ", Writes<X> out"), a_pass("BPass", "b", ", Writes<Y> out"),
                           a_pass("ReaderPass", "reader", ", Reads<X> in"))
        self.assertEqual(names(ordered), ["writer", "b", "reader"])

    def test_a_reader_comes_after_the_pass_that_writes_what_it_reads(self):
        ordered = graph_of(a_pass("ReaderPass", "reader", ", Reads<X> in"), a_pass("WriterPass", "writer", ", Writes<X> out"))
        self.assertEqual(names(ordered), ["writer", "reader"])

    def test_after_and_before_order_passes_no_data_connects(self):
        ordered = graph_of(a_pass("APass", "a", "", 'after = "b"'), a_pass("BPass", "b", ""),
                           a_pass("CPass", "c", "", 'before = "b"'))
        self.assertEqual(names(ordered), ["c", "b", "a"])

    def test_a_cycle_is_refused_with_the_reason_for_each_step(self):
        with self.assertRaises(ValueError) as caught:
            graph_of(a_pass("APass", "a", ", Reads<Y> in, Writes<X> out"),
                     a_pass("BPass", "b", ", Reads<X> in, Writes<Y> out"))
        message = str(caught.exception)
        self.assertIn("a writes X, which b reads", message)
        self.assertIn("b writes Y, which a reads", message)

    def test_a_produced_resource_with_two_writers_is_refused(self):
        with self.assertRaises(ValueError) as caught:
            graph_of(a_pass("APass", "a", ", Writes<X> out"), a_pass("BPass", "b", ", Writes<X> out"))
        self.assertIn("X is written by a and b", str(caught.exception))

    def test_a_read_nothing_writes_is_refused(self):
        with self.assertRaises(ValueError) as caught:
            graph_of(a_pass("APass", "a", ", Reads<X> in"))
        self.assertIn("a reads X, which no pass writes", str(caught.exception))

    def test_an_imported_object_may_be_read_without_a_writer(self):
        self.assertEqual(names(graph_of(a_pass("APass", "a", ", Reads<Service> in"))), ["a"])

    def test_two_writers_of_an_imported_object_need_an_order(self):
        with self.assertRaises(ValueError) as caught:
            graph_of(a_pass("APass", "a", ", Writes<Service> out"), a_pass("BPass", "b", ", Writes<Service> out"))
        self.assertIn("a and b both write Service", str(caught.exception))

    def test_any_path_orders_two_writers_of_an_imported_object(self):
        ordered = graph_of(a_pass("APass", "a", ", Writes<Service, X> out"), a_pass("MPass", "m", ", Reads<X> in, Writes<Y> out"),
                           a_pass("BPass", "b", ", Reads<Y> in, Writes<Service> out"))
        self.assertEqual(names(ordered), ["a", "m", "b"])

    def test_an_order_against_no_pass_is_refused(self):
        with self.assertRaises(ValueError) as caught:
            graph_of(a_pass("APass", "a", "", 'after = "ghost"'))
        self.assertIn("'ghost'", str(caught.exception))

    def test_two_passes_of_one_name_are_refused(self):
        with self.assertRaises(ValueError) as caught:
            graph_of(a_pass("APass", "a", ""), a_pass("BPass", "a", ""))
        self.assertIn("two passes are named 'a'", str(caught.exception))

    def test_a_frame_pass_may_not_read_what_a_view_pass_writes(self):
        view_pass = ("APASS(name = \"v\")\nstruct VPass\n{\n"
                     "    void Record(const TestFrame &frame, const ViewFrame &view, Writes<X> out);\n};\n")
        with self.assertRaises(ValueError) as caught:
            graph_of(view_pass, a_pass("FPass", "f", ", Reads<X> in"))
        self.assertIn("once per view", str(caught.exception))


class ShapeTest(unittest.TestCase):
    def refused(self, *passes):
        with self.assertRaises(ValueError) as caught:
            graph_of(*passes)
        return str(caught.exception)

    def test_an_apass_on_a_struct_with_a_base_is_refused_not_skipped(self):
        message = self.refused("APASS(name = \"a\")\nstruct APass : Base\n{\n"
                               "    void Record(const TestFrame &frame);\n};\n")
        self.assertIn("never run", message)

    def test_a_pass_without_a_name_is_refused(self):
        message = self.refused("APASS()\nstruct APass\n{\n    void Record(const TestFrame &frame);\n};\n")
        self.assertIn("has no name", message)

    def test_record_parameters_out_of_order_are_refused(self):
        message = self.refused("APASS(name = \"a\")\nstruct APass\n{\n"
                               "    void Record(const TestFrame &frame, Recorder &rec);\n};\n")
        self.assertIn("out of order", message)

    def test_a_record_defined_in_the_struct_is_refused(self):
        message = self.refused("APASS(name = \"a\")\nstruct APass\n{\n"
                               "    void Record(const TestFrame &frame) {}\n};\n")
        self.assertIn("exactly one Record", message)

    def test_a_qualified_resource_name_is_refused(self):
        self.assertIn("bare identifier", self.refused(a_pass("APass", "a", ", Writes<Render::X> out")))

    def test_a_resource_in_both_lists_is_refused(self):
        self.assertIn("both Reads<> and Writes<>", self.refused(a_pass("APass", "a", ", Reads<X> in, Writes<X> out")))

    def test_a_base_pass_names_one_of_its_timers(self):
        message = self.refused(a_pass("APass", "a", "", 'base, timers = {"t1", "t2"}'))
        self.assertIn("base pass", message)

    def test_names_are_lower_case_words_joined_by_dashes(self):
        self.assertIn("lower-case", self.refused(a_pass("APass", "Draw_Scene", "")))

    def test_record_may_return_whether_the_pass_stayed_on(self):
        ordered = graph_of("APASS(name = \"a\")\nstruct APass\n{\n    bool Record(const TestFrame &frame);\n};\n")
        self.assertTrue(ordered[0].returns_bool)
        self.assertFalse(ordered[0].recorder)


class EmitTest(unittest.TestCase):
    def test_the_walk_records_each_pass_in_order_with_its_resources(self):
        text = reflect_parser.strip_comments(
            "namespace Game {\n" + IMPORTS + a_pass("WriterPass", "writer", ", Writes<X> out") +
            "APASS(name = \"reader\", base)\nstruct ReaderPass\n{\n"
            "    void Record(Recorder &rec, const TestFrame &frame, Reads<X, Service> in);\n};\n}\n")
        found, imported = reflect_passes.parse_passes(text, Path("include/Game/T.hpp"), "Test")
        ordered = reflect_passes.order_passes(found, imported)
        graph = reflect_passes.Graph(prefix="Test", passes=ordered, imports=imported, produced=["X"],
                                     namespaces=["Game"], header="include/Game/T.hpp")
        source = reflect_passes.render_source(graph, "Game/T.generated.hpp")
        writer = source.index('ASSISI_PROFILE_SCOPE("writer")')
        reader = source.index('ASSISI_PROFILE_SCOPE("reader")')
        self.assertLess(writer, reader)
        self.assertIn("Render::Writes<X>{produced.x}", source)
        self.assertIn("Render::Reads<X, Service>{produced.x, imports.service}", source)
        self.assertIn("produced.x = {};", source)
        self.assertIn("Render::RecorderTiming::Eager", source[reader:])
        header = reflect_passes.render_header(graph, "Game/T.hpp")
        self.assertIn("WriterPass writer;", header)
        self.assertIn("X x;", header)

    def test_an_untimed_pass_opens_a_marker_and_counts_no_timer(self):
        text = reflect_parser.strip_comments(
            "namespace Game {\n" + IMPORTS + a_pass("HookPass", "hook", ", Writes<X> out", "untimed") +
            a_pass("TimedPass", "timed", ", Reads<X> in") + "}\n")
        found, imported = reflect_passes.parse_passes(text, Path("include/Game/T.hpp"), "Test")
        graph = reflect_passes.Graph(prefix="Test", passes=reflect_passes.order_passes(found, imported),
                                     imports=imported, produced=["X"], namespaces=["Game"],
                                     header="include/Game/T.hpp")
        source = reflect_passes.render_source(graph, "Game/T.generated.hpp")
        timed = source.index('ASSISI_PROFILE_SCOPE("timed")')
        self.assertIn("Render::RecorderTiming::Untimed", source[:timed])
        self.assertIn("Render::RecorderTiming::Lazy", source[timed:])
        self.assertIn("kTestPassTimerCount = 1;", reflect_passes.render_header(graph, "Game/T.hpp"))

    def test_a_graph_with_no_per_view_pass_takes_no_view(self):
        text = reflect_parser.strip_comments("namespace Game {\n" + IMPORTS + a_pass("APass", "a", ", Writes<X> out") + "}\n")
        found, imported = reflect_passes.parse_passes(text, Path("include/Game/T.hpp"), "Test")
        graph = reflect_passes.Graph(prefix="Test", passes=reflect_passes.order_passes(found, imported),
                                     imports=imported, produced=["X"], namespaces=["Game"],
                                     header="include/Game/T.hpp")
        self.assertNotIn("ViewFrame", reflect_passes.render_header(graph, "Game/T.hpp"))
        self.assertNotIn("ViewFrame", reflect_passes.render_source(graph, "Game/T.generated.hpp"))


class BaseTest(unittest.TestCase):
    def test_a_base_pass_cannot_turn_itself_off(self):
        with self.assertRaises(ValueError) as caught:
            graph_of("APASS(name = \"a\", base)\nstruct APass\n{\n"
                     "    bool Record(Recorder &rec, const TestFrame &frame, Writes<X> out);\n};\n")
        self.assertIn("base pass whose Record returns bool", str(caught.exception))


class UntimedTest(unittest.TestCase):
    def test_an_untimed_pass_cannot_list_timers(self):
        with self.assertRaises(ValueError) as caught:
            graph_of(a_pass("APass", "a", ", Writes<X> out", 'untimed, timers = {"a1", "a2"}'))
        self.assertIn("untimed and lists timers", str(caught.exception))

    def test_an_untimed_pass_cannot_be_base(self):
        with self.assertRaises(ValueError) as caught:
            graph_of(a_pass("APass", "a", ", Writes<X> out", "untimed, base"))
        self.assertIn("both base and untimed", str(caught.exception))


if __name__ == '__main__':
    unittest.main()
