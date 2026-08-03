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
            ["SampleAllTypes", "SampleRef", "SampleEmpty", "SampleTransient", "SampleRadio",
             "SampleReplicated"],
        )
        self.assertNotIn("GhostComponent", self.by_name)

    def test_acomp_transient_flag_is_captured(self):
        self.assertTrue(self.by_name["SampleTransient"].args.has("transient"))
        self.assertFalse(self.by_name["SampleEmpty"].args.has("transient"))

    def test_acomp_tracked_flag_is_captured(self):
        self.assertTrue(self.by_name["SampleAllTypes"].args.has("tracked"))
        self.assertFalse(self.by_name["SampleRef"].args.has("tracked"))

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
        self.assertEqual(fields["paths"], "std::vector<Assisi::Core::AssetPath>")
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

    def test_asset_path_vector_serializes_as_a_string_array(self):
        components = reflectgen.parse_header(FIXTURES / "Sample.hpp")
        cpp = reflectgen.generate_cpp(components, SAMPLE_INCLUDE)
        # Field metadata carries the new enum value.
        self.assertIn('"paths", Assisi::Core::Reflect::FieldType::AssetPathVector', cpp)
        # Serialize builds a JSON array from each path's View().
        self.assertIn("nlohmann::json::array()", cpp)
        self.assertIn("_arr.push_back(std::string(_p.View()))", cpp)
        # Deserialize clears then rebuilds (shorter saved arrays shrink the vector).
        self.assertIn("comp.paths.clear()", cpp)
        self.assertIn("comp.paths.push_back(_p)", cpp)

    def test_short_string_serializes_as_a_string(self):
        comps = _parse_source(
            "namespace N {\nACOMP()\nstruct C {\n"
            "  AFIELD() Assisi::Core::ShortString label;\n"
            "};\n}\n"
        )
        cpp = reflectgen.generate_cpp(comps, "N/C.hpp")
        self.assertIn('"label", Assisi::Core::Reflect::FieldType::String', cpp)
        self.assertIn("std::string(c.label.View())", cpp)          # serialize
        self.assertIn('comp.label.Assign(j.at("label").get<std::string>())', cpp)  # deserialize

    def test_asset_id_serializes_via_the_core_helpers(self):
        comps = _parse_source(
            "namespace N {\nACOMP()\nstruct Ref {\n"
            "  AFIELD() Assisi::Core::AssetId mesh;\n"
            "  AFIELD() std::vector<Assisi::Core::AssetId> slots;\n"
            "};\n}\n"
        )
        cpp = reflectgen.generate_cpp(comps, "N/Ref.hpp")
        # Field metadata carries the new enum values.
        self.assertIn('"mesh", Assisi::Core::Reflect::FieldType::AssetId', cpp)
        self.assertIn('"slots", Assisi::Core::Reflect::FieldType::AssetIdVector', cpp)
        # Serialize/deserialize route through the Core AssetId JSON helpers.
        self.assertIn("Assisi::Core::SerializeAssetId(c.mesh)", cpp)
        self.assertIn("comp.mesh = Assisi::Core::DeserializeAssetId(", cpp)
        self.assertIn("Assisi::Core::DeserializeAssetId(_e)", cpp)  # vector element
        # An AssetId field pulls in the helper header.
        self.assertIn("#include <Assisi/Core/AssetIdJson.hpp>", cpp)

    def test_no_asset_id_include_without_asset_id_fields(self):
        comps = _parse_source(
            "namespace N {\nACOMP()\nstruct Plain { AFIELD() int32_t x = 0; };\n}\n"
        )
        self.assertNotIn("AssetIdJson", reflectgen.generate_cpp(comps, "N/Plain.hpp"))

    def test_acomp_transient_emits_id_only_registration(self):
        components = reflectgen.parse_header(FIXTURES / "Sample.hpp")
        cpp = reflectgen.generate_cpp(components, SAMPLE_INCLUDE)
        # The transient component is registered (for its id) but marked
        # non-serializable with null hooks, and its field is not (de)serialized.
        self.assertIn('"SampleTransient"', cpp)
        self.assertIn("false      // serializable", cpp)
        self.assertNotIn("c.ignored", cpp)
        self.assertNotIn("comp.ignored", cpp)
        # Normal components carry the serializable=true marker.
        self.assertIn("true       // serializable", cpp)

    def test_entity_ref_pulls_in_scene_serializer_include(self):
        with_ref = reflectgen.parse_header(FIXTURES / "Sample.hpp")
        self.assertIn(
            "#include <Assisi/Runtime/SceneSerializer.hpp>",
            reflectgen.generate_cpp(with_ref, SAMPLE_INCLUDE),
        )

        without_ref = _parse_source(
            "namespace N {\nACOMP()\nstruct Plain { AFIELD() int32_t x = 0; };\n}\n"
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

    def test_min_max_bounds_are_emitted_in_field_meta(self):
        components = _parse_source(
            "namespace N {\nACOMP()\nstruct C {\n"
            "    AFIELD(min = 0) float radius = 10.f;\n"
            "    AFIELD(min = -1, max = 1) float bias = 0.f;\n"
            "    AFIELD() float plain = 0.f;\n"
            "};\n}\n"
        )
        cpp = reflectgen.generate_cpp(components, "N/C.hpp")
        # min-only: hasMin, hasMax, minValue, maxValue appended.
        self.assertIn('offsetof(T, radius), false, false, true, false, 0.0f, 0.f', cpp)
        # min and max together.
        self.assertIn('offsetof(T, bias), false, false, true, true, -1.0f, 1.0f', cpp)
        # Unannotated fields keep the short (golden-stable) initializer.
        self.assertIn('offsetof(T, plain), false, false }', cpp)

    def test_non_numeric_bound_is_a_hard_error(self):
        components = _parse_source(
            "namespace N {\nACOMP()\nstruct C { AFIELD(min = zero) float radius = 1.f; };\n}\n"
        )
        with self.assertRaises(ValueError):
            reflectgen.generate_cpp(components, "N/C.hpp")

    def test_integer_bounds_are_emitted(self):
        components = _parse_source(
            "namespace N {\nACOMP()\nstruct C { AFIELD(min = 0, max = 100) int32_t count = 1; };\n}\n"
        )
        cpp = reflectgen.generate_cpp(components, "N/C.hpp")
        self.assertIn("offsetof(T, count), false, false, true, true, 0.0f, 100.0f", cpp)

    def _assert_bound_rejected(self, field_decl: str):
        components = _parse_source(
            "namespace N {\nACOMP()\nstruct C { " + field_decl + " };\n}\n"
        )
        with self.assertRaises(ValueError):
            reflectgen.generate_cpp(components, "N/C.hpp")

    def test_negative_bound_on_unsigned_field_is_a_hard_error(self):
        self._assert_bound_rejected("AFIELD(min = -1) uint32_t count = 0;")

    def test_fractional_bound_on_integer_field_is_a_hard_error(self):
        self._assert_bound_rejected("AFIELD(min = 0.5) int32_t count = 1;")

    def test_bound_on_non_numeric_field_is_a_hard_error(self):
        self._assert_bound_rejected("AFIELD(min = 0) bool enabled = true;")
        self._assert_bound_rejected("AFIELD(max = 1) glm::vec3 dir{};")

    def test_min_greater_than_max_is_a_hard_error(self):
        self._assert_bound_rejected("AFIELD(min = 2, max = 1) float radius = 1.f;")

    def test_integer_bound_beyond_float_exact_range_is_a_hard_error(self):
        # FieldMeta stores bounds as float; integers beyond 2^24 stop being
        # exactly representable, so the generator refuses them.
        self._assert_bound_rejected("AFIELD(max = 4294967295) uint32_t count = 0;")


class ParserEdgeCaseTest(unittest.TestCase):
    """Fragile-parser surfaces the single golden fixture does not exercise:
    namespace-stack tracking, brace-balancing, pointer stripping, and the
    default-deny hard failure for genuinely unknown types."""

    def test_nested_namespace_blocks_compose(self):
        # The golden uses the collapsed `namespace A::B` form; this is the
        # separate-blocks path through the namespace stack.
        components = _parse_source(
            "namespace Outer {\n"
            "namespace Inner {\n"
            "ACOMP()\n"
            "struct C { AFIELD() int32_t a = 0; };\n"
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
            "struct First { AFIELD() int32_t a = 0; };\n"
            "struct Plain { int untracked = 0; };\n"
            "ACOMP()\n"
            "struct Second { AFIELD() int32_t b = 0; };\n"
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
            "    AFIELD() int32_t a = 1;\n"
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
            "struct C { AFIELD(transient) Foo* cache = nullptr; AFIELD() int32_t a = 0; };\n"
            "}\n"
        )
        cache = next(f for f in components[0].fields if f.name == "cache")
        self.assertEqual(cache.cpp_type, "Foo")  # '*' stripped
        self.assertTrue(cache.args.has("transient"))

        cpp = reflectgen.generate_cpp(components, "N/C.hpp")
        # In the metadata table (unknown type, transient), but never (de)serialized.
        self.assertIn("offsetof(T, cache), true, false", cpp)
        self.assertNotIn("c.cache", cpp)
        self.assertNotIn("comp.cache", cpp)

    def test_unknown_type_is_a_hard_error(self):
        # reflectgen is default-deny: a non-transient field whose type is in
        # neither TYPES nor anything else is a hard generation error, not a
        # silent skip — a skipped field drops on every save. Both the standalone
        # check and generate_cpp (which enforces it) must raise.
        components = _parse_source(
            "namespace N {\nACOMP()\nstruct C { AFIELD() SomeType data = {}; };\n}\n"
        )
        with self.assertRaises(ValueError):
            reflectgen._check_unsupported(components, "C.hpp")
        with self.assertRaises(ValueError):
            reflectgen.generate_cpp(components, "N/C.hpp")

    def test_transient_unknown_type_is_still_allowed(self):
        # Transient fields are never serialized, so an unknown type is fine.
        components = _parse_source(
            "namespace N {\nACOMP()\n"
            "struct C { AFIELD(transient) SomeType data = {}; AFIELD() int32_t a = 0; };\n}\n"
        )
        cpp = reflectgen.generate_cpp(components, "N/C.hpp")  # must not raise
        self.assertIn("Assisi::Core::Reflect::FieldType::Unknown", cpp)  # in the meta table
        self.assertNotIn("c.data", cpp)  # but never (de)serialized


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


class AssetTypeTest(unittest.TestCase):
    """AASSET: standalone asset types register with AssetTypeRegistry, not
    ComponentRegistry, and emit no scene/entity machinery."""

    _MAT = (
        "#include <Assisi/Core/AssetPath.hpp>\n"
        "#include <vector>\n"
        "namespace Assisi::Geometry {\n"
        "AASSET()\n"
        "struct MaterialData {\n"
        "    AFIELD() glm::vec4 BaseColorFactor{1,1,1,1};\n"
        "    AFIELD() float MetallicFactor = 1.f;\n"
        "    AFIELD() Assisi::Core::AssetPath BaseColorTexture;\n"
        "    AFIELD() std::vector<Assisi::Core::AssetPath> layers;\n"
        "    AFIELD(transient) int32_t cache = 0;\n"
        "};\n}\n"
    )

    def test_aasset_is_flagged_as_asset(self):
        comps = _parse_source(self._MAT)
        self.assertEqual(len(comps), 1)
        self.assertTrue(comps[0].is_asset)
        self.assertEqual(comps[0].namespaces, ["Assisi", "Geometry"])

    def test_aasset_registers_with_asset_registry_not_component(self):
        cpp = reflectgen.generate_cpp(_parse_source(self._MAT), "Assisi/Geometry/MaterialData.hpp")
        self.assertIn("AssetTypeRegistry::Instance().Register", cpp)
        self.assertNotIn("ComponentRegistry", cpp)
        # Asset-only header must not drag in ECS (its module may not link it).
        self.assertNotIn("Assisi/ECS/Scene.hpp", cpp)
        self.assertNotIn("scene_ptr", cpp)
        # Deserialize writes into the caller's instance, not a scene.
        self.assertIn("void* out_ptr", cpp)
        self.assertIn("auto& a = *static_cast<T*>(out_ptr)", cpp)
        self.assertIn("a.MetallicFactor = j.at(\"MetallicFactor\")", cpp)
        # Transient field is in the meta table but never (de)serialized.
        self.assertIn('"cache", Assisi::Core::Reflect::FieldType::Int32', cpp)
        self.assertNotIn("a.cache", cpp)
        self.assertNotIn("c.cache", cpp)

    def test_aasset_entity_ref_field_is_rejected(self):
        src = (
            "#include <Assisi/ECS/Entity.hpp>\n"
            "namespace N {\nAASSET()\nstruct Bad { AFIELD() ECS::Entity owner = ECS::NullEntity; };\n}\n"
        )
        with self.assertRaises(ValueError):
            reflectgen.generate_cpp(_parse_source(src), "N/Bad.hpp")

    def test_component_and_asset_in_one_header_pull_both_includes(self):
        src = (
            "#include <Assisi/Core/AssetPath.hpp>\n"
            "namespace N {\n"
            "ACOMP()\nstruct C { AFIELD() int32_t a = 0; };\n"
            "AASSET()\nstruct A { AFIELD() float b = 0.f; };\n"
            "}\n"
        )
        cpp = reflectgen.generate_cpp(_parse_source(src), "N/Mixed.hpp")
        self.assertIn("ComponentRegistry", cpp)
        self.assertIn("AssetTypeRegistry", cpp)


class ReplicationAnnotationTest(unittest.TestCase):
    """ACOMP(replicable) / AFIELD(norep) — the wire gate.

    Every rejection here has the same motive: the mistake's natural failure mode
    is silence (a component that quietly does not replicate, a field that quietly
    does), which a live session reveals late and confusingly, so the generator
    refuses instead.
    """

    def _sample(self):
        return reflectgen.parse_header(FIXTURES / "Sample.hpp")

    def test_replicable_component_emits_tracks_changes_and_replicable(self):
        cpp = reflectgen.generate_cpp(self._sample(), SAMPLE_INCLUDE)
        # replicable implies tracked, so both trailing flags are emitted — the
        # implication is what stops a replicable component reporting change tick
        # 0 forever and going silent after its spawn.
        self.assertIn("true,      // serializable\n"
                      "        true,      // tracksChanges\n"
                      "        true       // replicable", cpp)

    def test_replicable_with_explicit_tracked_is_accepted(self):
        # Not redundancy: one change-tick lane, two readers. The implication
        # serves replication; the explicit word records that a local system wants
        # the ticks too, so dropping `replicable` later cannot silently strip
        # them. ECS::Transform is the live case.
        src = ("namespace N {\nACOMP(replicable, tracked)\n"
               "struct C { AFIELD() int32_t a = 0; };\n}\n")
        cpp = reflectgen.generate_cpp(_parse_source(src), "N/C.hpp")
        self.assertIn("true       // replicable", cpp)

    def test_the_retired_replicated_spelling_is_rejected_by_name(self):
        # It would otherwise parse as an unknown flag and be silently ignored,
        # un-replicating a component that used to travel.
        src = "namespace N {\nACOMP(replicated)\nstruct C { AFIELD() int32_t a = 0; };\n}\n"
        with self.assertRaises(ValueError) as caught:
            reflectgen.generate_cpp(_parse_source(src), "N/C.hpp")
        self.assertIn("renamed to 'replicable'", str(caught.exception))

    def test_unmarked_component_emits_neither_flag(self):
        cpp = reflectgen.generate_cpp(self._sample(), SAMPLE_INCLUDE)
        # SampleRef is plain ACOMP(): its registration keeps the short form.
        self.assertIn("true       // serializable", cpp)

    def test_norep_field_is_flagged_but_still_serialized_to_disk(self):
        cpp = reflectgen.generate_cpp(self._sample(), SAMPLE_INCLUDE)
        # In the metadata table with norep = true (transient = false)...
        self.assertIn('{ "serverOnly", Assisi::Core::Reflect::FieldType::Int32, '
                      'offsetof(T, serverOnly), false, true }', cpp)
        # ...and in both JSON bodies, because norep is a *wire* exclusion only.
        self.assertIn("c.serverOnly", cpp)
        self.assertIn("comp.serverOnly", cpp)

    def test_replicable_with_transient_is_rejected(self):
        src = "namespace N {\nACOMP(replicable, transient)\nstruct C { AFIELD() int32_t a = 0; };\n}\n"
        with self.assertRaises(ValueError) as caught:
            reflectgen.generate_cpp(_parse_source(src), "N/C.hpp")
        self.assertIn("nothing to put on the wire", str(caught.exception))

    def test_replicable_on_an_asset_is_rejected(self):
        src = "namespace N {\nAASSET(replicable)\nstruct A { AFIELD() float b = 0.f; };\n}\n"
        with self.assertRaises(ValueError) as caught:
            reflectgen.generate_cpp(_parse_source(src), "N/A.hpp")
        self.assertIn("assets do not replicate", str(caught.exception))

    def test_norep_outside_a_replicable_component_is_rejected(self):
        src = "namespace N {\nACOMP()\nstruct C { AFIELD(norep) int32_t a = 0; };\n}\n"
        with self.assertRaises(ValueError) as caught:
            reflectgen.generate_cpp(_parse_source(src), "N/C.hpp")
        self.assertIn("silently do nothing", str(caught.exception))

    def test_norep_with_transient_is_rejected(self):
        src = ("namespace N {\nACOMP(replicable)\n"
               "struct C { AFIELD(transient, norep) int32_t a = 0; };\n}\n")
        with self.assertRaises(ValueError) as caught:
            reflectgen.generate_cpp(_parse_source(src), "N/C.hpp")
        self.assertIn("Drop norep", str(caught.exception))


class EnumTest(unittest.TestCase):
    _SRC = (
        "#include <cstdint>\n"
        "namespace N {\n"
        "AENUM()\nenum class Shape : uint32_t { Box, Sphere, Capsule = 5, Cylinder };\n"
        "ACOMP()\nstruct Body { AFIELD() Shape shape = Shape::Sphere; AFIELD() int32_t n = 0; };\n"
        "}\n"
    )

    def test_enum_field_is_resolved_with_constants(self):
        comp = _parse_source(self._SRC)[0]
        shape = comp.fields[0]
        self.assertIsNotNone(shape.enum_info)
        self.assertEqual(shape.enum_info.fqn, "N::Shape")
        self.assertEqual(shape.enum_info.constants,
                         [("Box", 0), ("Sphere", 1), ("Capsule", 5), ("Cylinder", 6)])

    def test_enum_field_generates_meta_and_roundtrip(self):
        cpp = reflectgen.generate_cpp(_parse_source(self._SRC), "N/Body.hpp")
        self.assertIn("FieldType::Enum", cpp)
        self.assertIn('{ "Capsule", 5 }', cpp)
        self.assertIn("static_cast<std::int64_t>(c.shape)", cpp)
        self.assertIn("static_cast<N::Shape>(j.at(\"shape\").get<std::int64_t>())", cpp)
        self.assertIn("#include <cstdint>", cpp)

    def test_non_integer_enumerator_is_rejected(self):
        src = ("#include <cstdint>\nnamespace N {\n"
               "AENUM()\nenum class E : uint32_t { A = someExpr };\n"
               "ACOMP()\nstruct C { AFIELD() E e = E::A; };\n}\n")
        with self.assertRaises(ValueError):
            _parse_source(src)

    def _enum_size(self, underlying_decl: str):
        """Parse `enum class E <underlying_decl> { A, B }` and return its EnumInfo."""
        src = (f"#include <cstdint>\nnamespace N {{\n"
               f"AENUM()\nenum class E {underlying_decl} {{ A, B }};\n"
               f"ACOMP()\nstruct C {{ AFIELD() E e = E::A; }};\n}}\n")
        return _parse_source(src)[0].fields[0].enum_info

    def test_default_underlying_is_4_byte_signed(self):
        info = self._enum_size("")
        self.assertEqual((info.size, info.is_signed), (4, True))

    def test_underlying_widths_and_signedness(self):
        cases = {
            ": std::uint8_t":  (1, False),
            ": int8_t":        (1, True),
            ": uint16_t":      (2, False),
            ": std::int16_t":  (2, True),
            ": uint32_t":      (4, False),
            ": int":           (4, True),
            ": unsigned":      (4, False),
            ": std::uint64_t": (8, False),
            ": int64_t":       (8, True),
        }
        for decl, expected in cases.items():
            info = self._enum_size(decl)
            self.assertEqual((info.size, info.is_signed), expected, decl)

    def test_enum_size_is_emitted_in_field_meta(self):
        cpp = reflectgen.generate_cpp(_parse_source(
            "#include <cstdint>\nnamespace N {\n"
            "AENUM()\nenum class E : std::uint8_t { A, B };\n"
            "ACOMP()\nstruct C { AFIELD() E e = E::A; };\n}\n"), "N/C.hpp")
        # ... enumConstants }, size, signed
        self.assertIn('{ { "A", 0 }, { "B", 1 } }, 1, false', cpp)

    def test_platform_dependent_long_underlying_is_rejected(self):
        with self.assertRaises(ValueError):
            self._enum_size(": long")
        with self.assertRaises(ValueError):
            self._enum_size(": unsigned long")


class RadioTest(unittest.TestCase):
    """AFIELD(radio ...): a broadcaster enum (AFIELD(radioBroadcast)) plus listener
    fields (AFIELD(radioListen = { source, value, behavior })) whose editor
    visibility follows it. A field may be both, forming a chain. Every misuse is a
    hard build failure."""

    # `mode` broadcasts, `sub` is a broadcaster-listener of it — a two-link chain.
    def _src(self, body: str) -> str:
        return (
            "#include <cstdint>\n"
            "namespace N {\n"
            "AENUM()\nenum class Mode { Off, Low, High };\n"
            "AENUM()\nenum class Sub { A, B };\n"
            "ACOMP()\nstruct C {\n" + body + "\n};\n}\n"
        )

    def _fields(self, body: str):
        comp = _parse_source(self._src(body))[0]
        return {f.name: f for f in comp.fields}

    # ── resolution ──────────────────────────────────────────────────────────

    def test_broadcaster_field_is_flagged(self):
        f = self._fields("AFIELD(radioBroadcast) Mode mode = Mode::Off;")["mode"]
        self.assertTrue(f.radio.is_broadcast)
        self.assertEqual(f.radio.source, "")

    def test_single_value_listener_resolves(self):
        f = self._fields(
            "AFIELD(radioBroadcast) Mode mode = Mode::Off;\n"
            "AFIELD(radioListen = {source = mode, value = High, behavior = vanish}) float x = 0.f;"
        )["x"]
        self.assertEqual(f.radio.source, "mode")
        self.assertEqual(f.radio.values, [2])
        self.assertEqual(f.radio.behavior, "Vanish")

    def test_value_set_listener_resolves(self):
        f = self._fields(
            "AFIELD(radioBroadcast) Mode mode = Mode::Off;\n"
            "AFIELD(radioListen = {source = mode, value = {Low, High}, behavior = grey}) int32_t n = 0;"
        )["n"]
        self.assertEqual(f.radio.values, [1, 2])
        self.assertEqual(f.radio.behavior, "Grey")

    def test_field_can_be_both_broadcaster_and_listener(self):
        fields = self._fields(
            "AFIELD(radioBroadcast) Mode mode = Mode::Off;\n"
            "AFIELD(radioBroadcast, radioListen = {source = mode, value = High, behavior = vanish}) Sub sub = Sub::A;\n"
            "AFIELD(radioListen = {source = sub, value = B, behavior = grey}) int32_t n = 0;"
        )
        sub = fields["sub"]
        self.assertTrue(sub.radio.is_broadcast)  # broadcasts to `n`
        self.assertEqual(sub.radio.source, "mode")  # and listens to `mode`
        self.assertEqual(fields["n"].radio.source, "sub")

    def test_listener_emits_radio_members_in_field_meta(self):
        comps = _parse_source(self._src(
            "AFIELD(radioBroadcast) Mode mode = Mode::Off;\n"
            "AFIELD(radioListen = {source = mode, value = {Low, High}, behavior = grey}) int32_t n = 0;"
        ))
        cpp = reflectgen.generate_cpp(comps, "N/C.hpp")
        # Non-enum listener: defaulted bounds + empty enum block (size 0), then the trio.
        self.assertIn(
            'offsetof(T, n), false, false, false, false, 0.f, 0.f, {}, 0, false, "mode", { 1, 2 }, '
            "Assisi::Core::Reflect::RadioBehavior::Grey",
            cpp,
        )
        # The broadcaster enum stays an ordinary enum field (no radio members),
        # now carrying its width (default int -> 4, signed).
        self.assertIn(
            'offsetof(T, mode), false, false, false, false, 0.f, 0.f, '
            '{ { "Off", 0 }, { "Low", 1 }, { "High", 2 } }, 4, true }',
            cpp,
        )

    def test_both_roles_emit_enum_constants_and_radio_members(self):
        # A broadcaster-listener enum carries BOTH its enumConstants and the trio.
        cpp = reflectgen.generate_cpp(_parse_source(self._src(
            "AFIELD(radioBroadcast) Mode mode = Mode::Off;\n"
            "AFIELD(radioBroadcast, radioListen = {source = mode, value = High, behavior = vanish}) Sub sub = Sub::A;"
        )), "N/C.hpp")
        self.assertIn(
            'offsetof(T, sub), false, false, false, false, 0.f, 0.f, { { "A", 0 }, { "B", 1 } }, 4, true, '
            '"mode", { 2 }, Assisi::Core::Reflect::RadioBehavior::Vanish',
            cpp,
        )

    def test_bound_and_radio_coexist_on_one_field(self):
        cpp = reflectgen.generate_cpp(_parse_source(self._src(
            "AFIELD(radioBroadcast) Mode mode = Mode::Off;\n"
            "AFIELD(min = 0, radioListen = {source = mode, value = High, behavior = vanish}) int32_t n = 0;"
        )), "N/C.hpp")
        self.assertIn(
            'offsetof(T, n), false, false, true, false, 0.0f, 0.f, {}, 0, false, "mode", { 2 }, '
            "Assisi::Core::Reflect::RadioBehavior::Vanish",
            cpp,
        )

    # ── hard-fail validation ────────────────────────────────────────────────

    def _assert_rejected(self, body: str):
        with self.assertRaises(ValueError):
            _parse_source(self._src(body))

    def test_broadcast_on_non_enum_is_rejected(self):
        self._assert_rejected("AFIELD(radioBroadcast) float notAnEnum = 0.f;")

    def test_listen_to_missing_field_is_rejected(self):
        self._assert_rejected(
            "AFIELD(radioListen = {source = ghost, value = High, behavior = grey}) float x = 0.f;")

    def test_listen_to_non_enum_field_is_rejected(self):
        self._assert_rejected(
            "AFIELD() float src = 0.f;\n"
            "AFIELD(radioListen = {source = src, value = High, behavior = grey}) float x = 0.f;")

    def test_listen_to_non_broadcaster_enum_is_rejected(self):
        # Target is an enum but not itself marked AFIELD(radioBroadcast).
        self._assert_rejected(
            "AFIELD() Mode mode = Mode::Off;\n"
            "AFIELD(radioListen = {source = mode, value = High, behavior = grey}) float x = 0.f;")

    def test_listen_to_self_is_rejected(self):
        self._assert_rejected(
            "AFIELD(radioBroadcast, radioListen = {source = x, value = High, behavior = grey}) Mode x = Mode::Off;")

    def test_direct_cycle_is_rejected(self):
        # Two broadcaster-listeners following each other: a <-> b.
        self._assert_rejected(
            "AFIELD(radioBroadcast, radioListen = {source = b, value = B, behavior = grey}) Sub a = Sub::A;\n"
            "AFIELD(radioBroadcast, radioListen = {source = a, value = A, behavior = grey}) Sub b = Sub::A;")

    def test_unknown_enumerator_value_is_rejected(self):
        self._assert_rejected(
            "AFIELD(radioBroadcast) Mode mode = Mode::Off;\n"
            "AFIELD(radioListen = {source = mode, value = Sideways, behavior = grey}) float x = 0.f;")

    def test_value_set_with_one_unknown_member_is_rejected(self):
        self._assert_rejected(
            "AFIELD(radioBroadcast) Mode mode = Mode::Off;\n"
            "AFIELD(radioListen = {source = mode, value = {Low, Nope}, behavior = grey}) float x = 0.f;")

    def test_bad_behavior_is_rejected(self):
        self._assert_rejected(
            "AFIELD(radioBroadcast) Mode mode = Mode::Off;\n"
            "AFIELD(radioListen = {source = mode, value = High, behavior = sparkle}) float x = 0.f;")

    def test_missing_behavior_key_is_rejected(self):
        self._assert_rejected(
            "AFIELD(radioBroadcast) Mode mode = Mode::Off;\n"
            "AFIELD(radioListen = {source = mode, value = High}) float x = 0.f;")

    def test_unknown_radio_key_is_rejected(self):
        self._assert_rejected(
            "AFIELD(radioBroadcast) Mode mode = Mode::Off;\n"
            "AFIELD(radioListen = {source = mode, value = High, behavior = grey, extra = 1}) float x = 0.f;")

    def test_non_object_listen_spec_is_rejected(self):
        self._assert_rejected(
            "AFIELD(radioBroadcast) Mode mode = Mode::Off;\n"
            "AFIELD(radioListen = mode) float x = 0.f;")


class MalformedMacroTest(unittest.TestCase):
    """A misused reflection macro must fail generation loudly, never be silently
    skipped — a dropped field would lose its data on every save."""

    def test_afield_without_a_field_declaration_raises(self):
        src = "namespace N {\nACOMP()\nstruct Bad { AFIELD() };\n}\n"
        with self.assertRaises(ValueError):
            _parse_source(src)

    def test_afield_on_a_method_raises(self):
        # AFIELD marking something that isn't a plain 'Type name;' field.
        src = "namespace N {\nACOMP()\nstruct Bad { AFIELD() void step() {} };\n}\n"
        with self.assertRaises(ValueError):
            _parse_source(src)

    def test_aenum_not_followed_by_enum_raises(self):
        src = "namespace N {\nAENUM()\nstruct NotAnEnum { int x; };\n}\n"
        with self.assertRaises(ValueError):
            _parse_source(src)

    def test_unsupported_field_type_still_raises(self):
        src = "namespace N {\nACOMP()\nstruct C { AFIELD() SomeUnknownType t; };\n}\n"
        with self.assertRaises(ValueError):
            reflectgen.generate_cpp(_parse_source(src), "N/C.hpp")


def _parse_full(source: str):
    """Parse an inline header string, returning (components, messages, handlers)."""
    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / "Temp.hpp"
        path.write_text(source, encoding="utf-8")
        return reflectgen.parse_header_full(path)


class MessageGrammarTest(unittest.TestCase):
    """AMSG(direction, reliability[, flags]) — mandatory, positional, ordered.

    Every way of getting the grammar wrong is a build error that names the rule,
    which is the whole argument for making both arguments mandatory: the
    declaration states the entire wire contract with nothing to memorise, and a
    default that changed later could never silently reclassify a message written
    under the old one.
    """

    def test_both_arguments_are_recorded(self):
        src = ("namespace N {\nAMSG(event, unreliable, independent)\n"
               "struct Boom { AFIELD() uint32_t target = 0; };\n}\n")
        _, messages, _ = _parse_full(src)
        self.assertEqual(len(messages), 1)
        self.assertEqual(messages[0].direction, "event")
        self.assertEqual(messages[0].reliability, "unreliable")
        self.assertTrue(messages[0].args.has("independent"))
        self.assertEqual(messages[0].fqn, "N::Boom")

    def test_missing_both_arguments_is_rejected(self):
        src = "namespace N {\nAMSG()\nstruct Boom { AFIELD() uint32_t t = 0; };\n}\n"
        with self.assertRaises(ValueError) as caught:
            _parse_full(src)
        self.assertIn("no arguments", str(caught.exception))

    def test_missing_reliability_is_rejected(self):
        src = "namespace N {\nAMSG(event)\nstruct Boom { AFIELD() uint32_t t = 0; };\n}\n"
        with self.assertRaises(ValueError) as caught:
            _parse_full(src)
        self.assertIn("only one argument", str(caught.exception))

    def test_swapped_arguments_are_named_as_swapped(self):
        # The mistake the ordering rule exists to catch, and the fix is one
        # transposition — so the error says so rather than reporting an unknown
        # argument twice.
        src = "namespace N {\nAMSG(reliable, event)\nstruct Boom { AFIELD() uint32_t t = 0; };\n}\n"
        with self.assertRaises(ValueError) as caught:
            _parse_full(src)
        self.assertIn("wrong way round", str(caught.exception))
        self.assertIn("AMSG(event, reliable)", str(caught.exception))

    def test_unknown_direction_is_rejected(self):
        src = "namespace N {\nAMSG(broadcast, reliable)\nstruct Boom { AFIELD() uint32_t t = 0; };\n}\n"
        with self.assertRaises(ValueError) as caught:
            _parse_full(src)
        self.assertIn("not a direction", str(caught.exception))

    def test_unknown_reliability_is_rejected(self):
        src = "namespace N {\nAMSG(event, maybe)\nstruct Boom { AFIELD() uint32_t t = 0; };\n}\n"
        with self.assertRaises(ValueError) as caught:
            _parse_full(src)
        self.assertIn("not a reliability", str(caught.exception))

    def test_unknown_flag_is_rejected(self):
        src = ("namespace N {\nAMSG(event, reliable, buffered)\n"
               "struct Boom { AFIELD() uint32_t t = 0; };\n}\n")
        with self.assertRaises(ValueError) as caught:
            _parse_full(src)
        self.assertIn("unknown AMSG flag", str(caught.exception))

    def test_amsg_on_a_non_struct_is_rejected(self):
        # Silently walking past it would leave the author believing a type is on
        # the wire when nothing registered it.
        src = "namespace N {\nAMSG(event, reliable)\nvoid NotAStruct();\n}\n"
        with self.assertRaises(ValueError) as caught:
            _parse_full(src)
        self.assertIn("not followed by a 'struct'", str(caught.exception))

    def test_norep_in_a_message_is_rejected(self):
        src = ("namespace N {\nAMSG(event, reliable)\n"
               "struct Boom { AFIELD(norep) uint32_t t = 0; };\n}\n")
        _, messages, _ = _parse_full(src)
        with self.assertRaises(ValueError) as caught:
            reflectgen.generate_cpp([], "N/Boom.hpp", messages)
        self.assertIn("only to cross the wire", str(caught.exception))

    def test_transient_in_a_message_is_rejected(self):
        src = ("namespace N {\nAMSG(event, reliable)\n"
               "struct Boom { AFIELD(transient) uint32_t t = 0; };\n}\n")
        _, messages, _ = _parse_full(src)
        with self.assertRaises(ValueError) as caught:
            reflectgen.generate_cpp([], "N/Boom.hpp", messages)
        self.assertIn("no persistent form", str(caught.exception))

    def test_a_message_registers_with_its_grammar(self):
        src = ("namespace N {\nAMSG(intent, reliable)\n"
               "struct Go { AFIELD() uint32_t target = 0; };\n}\n")
        _, messages, _ = _parse_full(src)
        cpp = reflectgen.generate_cpp([], "N/Go.hpp", messages)
        self.assertIn("MessageRegistry::Instance().Register", cpp)
        self.assertIn("MessageDirection::Intent", cpp)
        self.assertIn("MessageReliability::Reliable", cpp)
        self.assertIn("using T = N::Go;", cpp)


class ControlledFieldTest(unittest.TestCase):
    """AFIELD(controlled): "the sender must control this entity".

    It marks the *subject* of an intent, as distinct from any other entity the
    message merely mentions. Without the distinction the dispatch site could
    either check nothing or check every reference, and checking every reference
    would forbid a client from ever naming an entity it does not own — which is
    most of them.
    """

    def test_controlled_reaches_the_field_metadata(self):
        src = ("namespace N {\nAMSG(intent, reliable)\n"
               "struct Go { AFIELD(controlled) Assisi::ECS::Entity pawn; };\n}\n")
        _, messages, _ = _parse_full(src)
        cpp = reflectgen.generate_cpp([], "N/Go.hpp", messages)
        # Emitted last in FieldMeta's positional tail, which forces every earlier
        # block to its default — so the presence of the trailing `true` is what
        # the dispatch site reads.
        self.assertIn('offsetof(T, pawn), false, false, false, false, 0.f, 0.f, {}, 0, false, "", {}, '
                      'Assisi::Core::Reflect::RadioBehavior::None, true', cpp)

    def test_controlled_on_an_event_is_rejected(self):
        # The sender of an event is the server, which controls everything by
        # definition, so the annotation could not mean anything.
        src = ("namespace N {\nAMSG(event, reliable)\n"
               "struct Boom { AFIELD(controlled) Assisi::ECS::Entity what; };\n}\n")
        _, messages, _ = _parse_full(src)
        with self.assertRaises(ValueError) as caught:
            reflectgen.generate_cpp([], "N/Boom.hpp", messages)
        self.assertIn("is an event", str(caught.exception))

    def test_controlled_on_a_non_entity_field_is_rejected(self):
        src = ("namespace N {\nAMSG(intent, reliable)\n"
               "struct Go { AFIELD(controlled) int32_t pawn = 0; };\n}\n")
        _, messages, _ = _parse_full(src)
        with self.assertRaises(ValueError) as caught:
            reflectgen.generate_cpp([], "N/Go.hpp", messages)
        self.assertIn("Only an EntityRef field", str(caught.exception))

    def test_controlled_on_a_component_is_rejected(self):
        # A component has no sender, so there would be nothing to enforce the
        # rule against — and an annotation that reads like a rule while
        # enforcing nothing is worse than no annotation.
        src = ("namespace N {\nACOMP()\n"
               "struct C { AFIELD(controlled) Assisi::ECS::Entity e; };\n}\n")
        with self.assertRaises(ValueError) as caught:
            reflectgen.generate_cpp(_parse_source(src), "N/C.hpp")
        self.assertIn("a component has no sender", str(caught.exception))


class MessageTraitsTest(unittest.TestCase):
    """The compile-time facts that make a wrong-direction send a build error."""

    def test_traits_carry_the_grammar(self):
        src = ("namespace N {\nAMSG(event, reliable, independent)\n"
               "struct Boom { AFIELD() uint32_t t = 0; };\n}\n")
        _, messages, _ = _parse_full(src)
        traits = reflectgen.gen_message_traits(messages[0])
        self.assertIn("MessageTraits<::N::Boom>", traits)
        self.assertIn("MessageDirection::Event", traits)
        self.assertIn("MessageReliability::Reliable", traits)
        self.assertIn("independent = true", traits)

    def test_the_type_is_forward_declared_at_global_scope(self):
        # Forward-declared rather than included, because specializing on an
        # incomplete type is legal and including the real header would close a
        # cycle through the dispatch header.
        src = ("namespace A { namespace B {\nAMSG(intent, unreliable)\n"
               "struct Go { AFIELD() uint32_t t = 0; };\n} }\n")
        _, messages, _ = _parse_full(src)
        self.assertEqual(reflectgen.gen_message_forward(messages[0]),
                         "namespace A { namespace B { struct Go; } }")


class MessageHandlerTest(unittest.TestCase):
    """AMSG_HANDLER() declarations, and the binding they generate."""

    _SRC = (
        "namespace Game {\n"
        "AMSG(intent, reliable)\n"
        "struct Fire { AFIELD() uint32_t weapon = 0; };\n"
        "AMSG_HANDLER() void HandleFire(NetContext &ctx, const Fire &msg);\n"
        "}\n"
    )

    def test_handler_is_found_with_its_namespace_and_message(self):
        _, _, handlers = _parse_full(self._SRC)
        self.assertEqual(len(handlers), 1)
        self.assertEqual(handlers[0].name, "HandleFire")
        self.assertEqual(handlers[0].namespaces, ["Game"])
        self.assertEqual(handlers[0].message, "Fire")
        self.assertEqual(handlers[0].fqn, "::Game::HandleFire")

    def test_binding_is_emitted_in_the_declarations_own_namespace(self):
        _, messages, handlers = _parse_full(self._SRC)
        cpp = reflectgen.generate_cpp([], "Game/Fire.hpp", messages, handlers)
        # The message type is resolved where the declaration resolved it...
        self.assertIn("namespace Game {", cpp)
        self.assertIn("using MsgT = Fire;", cpp)
        # ...the handler is named from global scope, so nothing nearer can
        # shadow it...
        self.assertIn("&::Game::HandleFire", cpp)
        # ...and the signature is pinned, so an overload set cannot drift.
        self.assertIn("static_cast<void (*)(Assisi::NetSync::NetContext &, const MsgT &)>", cpp)

    def test_a_non_void_handler_is_rejected(self):
        src = ("namespace Game {\nAMSG(intent, reliable)\nstruct Fire { AFIELD() uint32_t w = 0; };\n"
               "AMSG_HANDLER() bool HandleFire(NetContext &ctx, const Fire &msg);\n}\n")
        with self.assertRaises(ValueError) as caught:
            _parse_full(src)
        self.assertIn("returns void", str(caught.exception))

    def test_a_handler_without_a_context_is_not_matched(self):
        # The signature is fixed, and a declaration that does not have it is not
        # a handler at all — which surfaces as "no handler for this message"
        # rather than as a silently wrong binding.
        src = ("namespace Game {\nAMSG(intent, reliable)\nstruct Fire { AFIELD() uint32_t w = 0; };\n"
               "AMSG_HANDLER() void HandleFire(const Fire &msg);\n}\n")
        _, _, handlers = _parse_full(src)
        self.assertEqual(handlers, [])

    def test_two_handlers_for_one_message_is_a_build_error(self):
        with tempfile.TemporaryDirectory() as d:
            first = Path(d) / "A.hpp"
            first.write_text(
                "namespace Game {\n"
                "AMSG(intent, reliable)\n"
                "struct Fire { AFIELD() uint32_t w = 0; };\n"
                "AMSG_HANDLER() void HandleFire(NetContext &ctx, const Fire &msg);\n"
                "}\n", encoding="utf-8")
            second = Path(d) / "B.hpp"
            second.write_text(
                "namespace Other {\n"
                "AMSG_HANDLER() void AlsoHandleFire(NetContext &ctx, const Game::Fire &msg);\n"
                "}\n", encoding="utf-8")

            with self.assertRaises(ValueError) as caught:
                reflectgen.check_message_handlers([first, second])
            message = str(caught.exception)
            self.assertIn("two handlers are declared", message)
            # Both sites are named, because "one of these is wrong" is not
            # actionable without knowing which two.
            self.assertIn("HandleFire", message)
            self.assertIn("AlsoHandleFire", message)

    def test_a_handler_for_an_unknown_message_is_a_build_error(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "A.hpp"
            path.write_text(
                "namespace Game {\n"
                "AMSG_HANDLER() void HandleNothing(NetContext &ctx, const Missing &msg);\n"
                "}\n", encoding="utf-8")
            with self.assertRaises(ValueError) as caught:
                reflectgen.check_message_handlers([path])
            self.assertIn("names no AMSG message type", str(caught.exception))

    def test_an_ambiguous_message_name_is_a_build_error(self):
        # The same struct name in two namespaces, and a handler declared in
        # neither. Guessing which one it meant is exactly what the rule forbids.
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "A.hpp"
            path.write_text(
                "namespace A {\nAMSG(intent, reliable)\nstruct Fire { AFIELD() uint32_t w = 0; };\n}\n"
                "namespace B {\nAMSG(intent, reliable)\nstruct Fire { AFIELD() uint32_t w = 0; };\n}\n"
                "namespace C {\n"
                "AMSG_HANDLER() void HandleFire(NetContext &ctx, const Fire &msg);\n"
                "}\n", encoding="utf-8")
            with self.assertRaises(ValueError) as caught:
                reflectgen.check_message_handlers([path])
            self.assertIn("ambiguous", str(caught.exception))

    def test_an_enclosing_namespace_disambiguates(self):
        # ...and when the handler *is* declared inside one of the candidates,
        # that is not ambiguity — it is the same rule C++ itself would use.
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "A.hpp"
            path.write_text(
                "namespace A {\nAMSG(intent, reliable)\nstruct Fire { AFIELD() uint32_t w = 0; };\n"
                "AMSG_HANDLER() void HandleFire(NetContext &ctx, const Fire &msg);\n}\n"
                "namespace B {\nAMSG(intent, reliable)\nstruct Fire { AFIELD() uint32_t w = 0; };\n}\n",
                encoding="utf-8")
            summary = reflectgen.check_message_handlers([path])
            self.assertIn("A::Fire -> A::HandleFire", summary)
            self.assertIn("B::Fire -> (no handler)", summary)


class IncludePathTest(unittest.TestCase):
    def test_detects_path_after_include_segment(self):
        p = Path("modules") / "Runtime" / "include" / "Assisi" / "Runtime" / "Foo.hpp"
        self.assertEqual(reflectgen._detect_include_path(p), "Assisi/Runtime/Foo.hpp")

    def test_falls_back_to_filename_without_include_segment(self):
        self.assertEqual(reflectgen._detect_include_path(Path("src") / "Foo.hpp"), "Foo.hpp")


if __name__ == "__main__":
    unittest.main()
