#!/usr/bin/env python3
# Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc").
"""Golden-file and behavioural tests for reflectgen.

reflectgen is a regex-based C++ parser that emits engine-critical registration
code. These tests defend it against silent regex regressions: a checked-in
golden output for a fixture header that exercises every supported field type
and edge case, plus targeted behavioural cases (comment stripping, namespace
capture, transient exclusion, the unsupported-type hard fail, EntityRef include
emission).

Run directly (`python test_reflectgen.py`) or via ctest. To adopt a deliberate
change to the generated output, regenerate the golden:

    REFLECTGEN_UPDATE_GOLDEN=1 python tools/reflectgen/tests/test_reflectgen.py
"""

import os
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))  # tools/reflectgen (so `import reflectgen` works)

import reflectgen  # noqa: E402

FIXTURES = HERE / "fixtures"
GOLDEN = HERE / "golden"

# The include path the golden was generated against. Pinned here rather than
# auto-detected so the golden does not depend on the fixture's location on disk.
SAMPLE_INCLUDE = "Assisi/Testing/Sample.hpp"


def _norm(text: str) -> str:
    """Line-ending-agnostic normalisation so CRLF checkouts compare equal."""
    return text.replace("\r\n", "\n").replace("\r", "\n")


def _parse_source(source: str):
    """Parse an inline header string through a throwaway temp file."""
    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / "Temp.hpp"
        path.write_text(source, encoding="utf-8")
        return reflectgen.parse_header(path)


class GoldenTest(unittest.TestCase):
    def test_sample_matches_golden(self):
        components = reflectgen.parse_header(FIXTURES / "Sample.hpp")
        actual = reflectgen.generate_cpp(components, SAMPLE_INCLUDE)

        golden_path = GOLDEN / "Sample.generated.cpp"
        if os.environ.get("REFLECTGEN_UPDATE_GOLDEN"):
            golden_path.write_text(actual, encoding="utf-8")
            self.skipTest(f"updated golden: {golden_path}")

        expected = golden_path.read_text(encoding="utf-8")
        self.assertEqual(
            _norm(actual),
            _norm(expected),
            "Generated output drifted from the golden. If this change is "
            "intentional, regenerate with REFLECTGEN_UPDATE_GOLDEN=1.",
        )


class ParseTest(unittest.TestCase):
    def setUp(self):
        self.components = reflectgen.parse_header(FIXTURES / "Sample.hpp")
        self.by_name = {c.name: c for c in self.components}

    def test_finds_exactly_the_annotated_components(self):
        # GhostComponent lives inside a // comment and must not be parsed.
        self.assertEqual(
            [c.name for c in self.components],
            ["SampleAllTypes", "SampleRef", "SampleEmpty"],
        )
        self.assertNotIn("GhostComponent", self.by_name)

    def test_captures_nested_namespace(self):
        self.assertEqual(self.by_name["SampleAllTypes"].namespaces, ["Assisi", "Runtime"])

    def test_field_types_are_parsed(self):
        fields = {f.name: f.cpp_type for f in self.by_name["SampleAllTypes"].fields}
        self.assertEqual(fields["f"], "float")
        self.assertEqual(fields["i32"], "int32_t")
        self.assertEqual(fields["u32"], "uint32_t")
        self.assertEqual(fields["v3"], "glm::vec3")
        self.assertEqual(fields["q"], "glm::quat")
        self.assertEqual(fields["m"], "glm::mat4")
        self.assertEqual(fields["assetPath"], "Assisi::Core::AssetPath")
        self.assertEqual(self.by_name["SampleRef"].fields[0].cpp_type, "ECS::Entity")

    def test_transient_flag_is_captured(self):
        fields = {f.name: f for f in self.by_name["SampleAllTypes"].fields}
        self.assertTrue(fields["runtimeCache"].args.has("transient"))
        self.assertFalse(fields["f"].args.has("transient"))

    def test_empty_component_has_no_fields(self):
        self.assertEqual(self.by_name["SampleEmpty"].fields, [])


class CodegenTest(unittest.TestCase):
    def test_transient_field_is_excluded_from_serialization(self):
        components = reflectgen.parse_header(FIXTURES / "Sample.hpp")
        cpp = reflectgen.generate_cpp(components, SAMPLE_INCLUDE)
        # It appears in the field-metadata table (marked transient=true)...
        self.assertIn('"runtimeCache", Assisi::Core::Reflect::FieldType::Float', cpp)
        # ...but never in the serialize/deserialize bodies.
        self.assertNotIn("c.runtimeCache", cpp)
        self.assertNotIn("comp.runtimeCache", cpp)

    def test_entity_ref_pulls_in_scene_serializer_include(self):
        with_ref = reflectgen.parse_header(FIXTURES / "Sample.hpp")
        self.assertIn(
            "#include <Assisi/Runtime/SceneSerializer.hpp>",
            reflectgen.generate_cpp(with_ref, SAMPLE_INCLUDE),
        )

        without_ref = _parse_source(
            "namespace N {\nACOMP()\nstruct Plain { AFIELD() int x = 0; };\n}\n"
        )
        self.assertNotIn(
            "SceneSerializer",
            reflectgen.generate_cpp(without_ref, "N/Plain.hpp"),
        )

    def test_header_without_annotations_yields_nothing(self):
        components = _parse_source(
            "namespace N {\nstruct Plain { int x = 0; };\n}\n"
        )
        self.assertEqual(components, [])


class ParserEdgeCaseTest(unittest.TestCase):
    """Fragile-parser surfaces the single golden fixture does not exercise:
    namespace-stack tracking, brace-balancing, pointer stripping, and the
    silent-skip path for genuinely unknown (but not guarded) types."""

    def test_nested_namespace_blocks_compose(self):
        # The golden uses the collapsed `namespace A::B` form; this is the
        # separate-blocks path through the namespace stack.
        components = _parse_source(
            "namespace Outer {\n"
            "namespace Inner {\n"
            "ACOMP()\n"
            "struct C { AFIELD() int a = 0; };\n"
            "}\n"
            "}\n"
        )
        self.assertEqual(len(components), 1)
        self.assertEqual(components[0].namespaces, ["Outer", "Inner"])

    def test_plain_struct_between_components_does_not_break_namespace(self):
        # A non-ACOMP struct between two components must not leak into either,
        # and both components must still resolve to the enclosing namespace.
        components = _parse_source(
            "namespace N {\n"
            "ACOMP()\n"
            "struct First { AFIELD() int a = 0; };\n"
            "struct Plain { int untracked = 0; };\n"
            "ACOMP()\n"
            "struct Second { AFIELD() int b = 0; };\n"
            "}\n"
        )
        self.assertEqual([c.name for c in components], ["First", "Second"])
        self.assertTrue(all(c.namespaces == ["N"] for c in components))

    def test_inner_brace_in_struct_body_is_balanced(self):
        # A nested struct inside the body stresses _extract_brace_body's depth
        # counting; only the AFIELD-annotated outer field must be captured.
        components = _parse_source(
            "namespace N {\n"
            "ACOMP()\n"
            "struct Outer {\n"
            "    struct Inner { int z = 0; } nested;\n"
            "    AFIELD() int a = 1;\n"
            "};\n"
            "}\n"
        )
        self.assertEqual(len(components), 1)
        fields = [f.name for f in components[0].fields]
        self.assertEqual(fields, ["a"])  # 'z' and 'nested' are not AFIELD

    def test_pointer_field_marked_transient_is_stripped_and_excluded(self):
        components = _parse_source(
            "namespace N {\n"
            "ACOMP()\n"
            "struct C { AFIELD(transient) Foo* cache = nullptr; AFIELD() int a = 0; };\n"
            "}\n"
        )
        cache = next(f for f in components[0].fields if f.name == "cache")
        self.assertEqual(cache.cpp_type, "Foo")  # '*' stripped
        self.assertTrue(cache.args.has("transient"))

        cpp = reflectgen.generate_cpp(components, "N/C.hpp")
        # In the metadata table (unknown type, transient), but never (de)serialized.
        self.assertIn("offsetof(T, cache), true", cpp)
        self.assertNotIn("c.cache", cpp)
        self.assertNotIn("comp.cache", cpp)

    def test_unknown_unguarded_type_warns_but_does_not_fail(self):
        # A type in neither TYPES nor UNSUPPORTED_TYPES is a silent skip today
        # (FieldType::Unknown + a WARNING comment). Lock that so it can't change
        # unnoticed — the loud failure is reserved for UNSUPPORTED_TYPES.
        components = _parse_source(
            "namespace N {\nACOMP()\nstruct C { AFIELD() SomeType data = {}; };\n}\n"
        )
        reflectgen._check_unsupported(components, "C.hpp")  # must not raise

        cpp = reflectgen.generate_cpp(components, "N/C.hpp")
        self.assertIn("Assisi::Core::Reflect::FieldType::Unknown", cpp)
        self.assertIn("WARNING: unsupported type 'SomeType' for field 'data'", cpp)


class UnsupportedTypeTest(unittest.TestCase):
    """The UNSUPPORTED_TYPES guard must hard-fail rather than silently skip."""

    def _component(self, cpp_type: str, transient: bool):
        args = reflectgen.AnnotArgs()
        if transient:
            args.flags.add("transient")
        return reflectgen.ComponentInfo(
            name="Bad",
            namespaces=["N"],
            args=reflectgen.AnnotArgs(),
            fields=[reflectgen.FieldInfo(name="blob", cpp_type=cpp_type, args=args)],
        )

    def test_non_transient_unsupported_field_raises(self):
        reflectgen.UNSUPPORTED_TYPES["std::string"] = "no string codegen yet"
        try:
            with self.assertRaises(ValueError):
                reflectgen._check_unsupported([self._component("std::string", transient=False)], "Bad.hpp")
        finally:
            del reflectgen.UNSUPPORTED_TYPES["std::string"]

    def test_transient_unsupported_field_is_exempt(self):
        reflectgen.UNSUPPORTED_TYPES["std::string"] = "no string codegen yet"
        try:
            # A transient field is never (de)serialized, so an unsupported type is fine.
            reflectgen._check_unsupported([self._component("std::string", transient=True)], "Bad.hpp")
        finally:
            del reflectgen.UNSUPPORTED_TYPES["std::string"]


class IncludePathTest(unittest.TestCase):
    def test_detects_path_after_include_segment(self):
        p = Path("modules") / "Runtime" / "include" / "Assisi" / "Runtime" / "Foo.hpp"
        self.assertEqual(reflectgen._detect_include_path(p), "Assisi/Runtime/Foo.hpp")

    def test_falls_back_to_filename_without_include_segment(self):
        self.assertEqual(reflectgen._detect_include_path(Path("src") / "Foo.hpp"), "Foo.hpp")


if __name__ == "__main__":
    unittest.main()
