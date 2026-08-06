#!/usr/bin/env python3
# Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc").
"""Tests for InstanceView<T> generation from .abp files.

The property worth defending: the field names this emits must be the member
names Blueprint.cpp produces. A field that is not a real member compiles fine
and resolves to NullEntity at runtime — the silent do-nothing failure the whole
blueprint system exists to remove — so the flattening rules are pinned here
rather than left to the one runtime test that would catch them late.

Three of these mirror rules that live in C++ and could drift:
`nesting prefixes every member`, `a removal cascades to descendants`, and
`duplicate declarations are checked on the full path`. If Blueprint.cpp's
FlattenInto changes, these are what should fail.

Run directly (`python test_blueprint_views.py`) or via ctest.
"""

import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))  # tools/reflectgen

import blueprint_views  # noqa: E402
from blueprint_views import ViewError  # noqa: E402


def _entity(name):
    return {"name": name, "components": {}}


class ViewTestCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def write(self, name, doc):
        (self.root / name).write_text(json.dumps(doc), encoding="utf-8")

    def car(self):
        self.write("car.abp", {"version": 2,
                               "entities": [_entity("body"), _entity("wheel_fl"), _entity("wheel_fr")]})

    def members(self, source):
        return blueprint_views.flatten_member_names(self.root, source)[0]

    def generate(self, *specs):
        return blueprint_views.generate(self.root, list(specs))[0]


class TestFlattening(ViewTestCase):
    def test_flat_file_keeps_declared_order(self):
        self.car()
        self.assertEqual(self.members("car.abp"), ["body", "wheel_fl", "wheel_fr"])

    def test_nesting_prefixes_every_member(self):
        self.car()
        self.write("lot.abp", {"version": 2,
                               "entities": [_entity("sign")],
                               "instances": [{"name": "car", "source": "car.abp"}]})
        self.assertEqual(self.members("lot.abp"),
                         ["sign", "car/body", "car/wheel_fl", "car/wheel_fr"])

    def test_nesting_is_recursive(self):
        self.car()
        self.write("lot.abp", {"version": 2, "instances": [{"name": "car", "source": "car.abp"}]})
        self.write("city.abp", {"version": 2, "instances": [{"name": "lot", "source": "lot.abp"}]})
        self.assertEqual(self.members("city.abp"),
                         ["lot/car/body", "lot/car/wheel_fl", "lot/car/wheel_fr"])

    def test_removal_drops_exactly_the_named_member(self):
        self.car()
        self.write("lot.abp", {"version": 2,
                               "instances": [{"name": "car", "source": "car.abp",
                                              "removed": ["wheel_fr"]}]})
        self.assertEqual(self.members("lot.abp"), ["car/body", "car/wheel_fl"])

    def test_removal_cascades_to_descendants(self):
        # Blueprint.cpp IsMemberRemoved: removing 'car' takes 'car/body' with it.
        # Getting this wrong emits fields for members that will not exist.
        self.car()
        self.write("lot.abp", {"version": 2, "entities": [_entity("sign")],
                               "instances": [{"name": "car", "source": "car.abp"}]})
        self.write("city.abp", {"version": 2,
                                "instances": [{"name": "lot", "source": "lot.abp",
                                               "removed": ["car"]}]})
        self.assertEqual(self.members("city.abp"), ["lot/sign"])

    def test_removal_does_not_match_a_name_prefix(self):
        # 'wheel' must not remove 'wheel_fl': the cascade is '/'-delimited.
        self.car()
        self.write("lot.abp", {"version": 2,
                               "instances": [{"name": "car", "source": "car.abp",
                                              "removed": ["wheel"]}]})
        self.assertEqual(self.members("lot.abp"),
                         ["car/body", "car/wheel_fl", "car/wheel_fr"])

    def test_two_instances_of_one_file_are_separate_members(self):
        self.car()
        self.write("lot.abp", {"version": 2,
                               "instances": [{"name": "left", "source": "car.abp"},
                                             {"name": "right", "source": "car.abp"}]})
        self.assertIn("left/body", self.members("lot.abp"))
        self.assertIn("right/body", self.members("lot.abp"))

    def test_dependencies_include_every_file_read(self):
        self.car()
        self.write("lot.abp", {"version": 2, "instances": [{"name": "car", "source": "car.abp"}]})
        _, sources = blueprint_views.flatten_member_names(self.root, "lot.abp")
        # The build depends on both, or editing car.abp leaves a stale view.
        self.assertEqual(sorted(sources), ["car.abp", "lot.abp"])


class TestRefusals(ViewTestCase):
    def assertRefused(self, source, fragment):
        with self.assertRaises(ViewError) as caught:
            blueprint_views.generate(self.root, [("T", source)])
        self.assertIn(fragment, str(caught.exception))

    def test_entity_and_nested_instance_of_one_name(self):
        # The collision grouping exists to make visible: both would be `.car`.
        self.car()
        self.write("bad.abp", {"version": 2, "entities": [_entity("car")],
                               "instances": [{"name": "car", "source": "car.abp"}]})
        self.assertRefused("bad.abp", "both an entity and a nested instance")

    def test_cxx_keyword_as_member_name(self):
        self.write("bad.abp", {"version": 2, "entities": [_entity("operator")]})
        self.assertRefused("bad.abp", "C++ keyword")

    def test_member_named_like_the_id_field(self):
        self.write("bad.abp", {"version": 2, "entities": [_entity("instanceId")]})
        self.assertRefused("bad.abp", "already has an")

    def test_member_name_that_is_not_an_identifier(self):
        self.write("bad.abp", {"version": 2, "entities": [_entity("front wheel")]})
        self.assertRefused("bad.abp", "not a valid C++ identifier")

    def test_slash_in_an_entity_name(self):
        # Indistinguishable from nesting once flattened.
        self.write("bad.abp", {"version": 2, "entities": [_entity("a/b")]})
        self.assertRefused("bad.abp", "cannot appear in an entity name")

    def test_instance_cycle(self):
        self.write("a.abp", {"version": 2, "instances": [{"name": "n", "source": "b.abp"}]})
        self.write("b.abp", {"version": 2, "instances": [{"name": "m", "source": "a.abp"}]})
        self.assertRefused("a.abp", "instance cycle")

    def test_duplicate_member_names_in_one_file(self):
        self.write("bad.abp", {"version": 2, "entities": [_entity("body"), _entity("body")]})
        self.assertRefused("bad.abp", "declares two members named")

    def test_missing_file(self):
        self.assertRefused("gone.abp", "does not exist")

    def test_unreadable_json(self):
        (self.root / "bad.abp").write_text("{ not json", encoding="utf-8")
        self.assertRefused("bad.abp", "not readable JSON")

    def test_two_blueprints_opted_in_under_one_type_name(self):
        self.car()
        self.write("lot.abp", {"version": 2, "entities": [_entity("sign")]})
        with self.assertRaises(ViewError) as caught:
            blueprint_views.generate(self.root, [("Car", "car.abp"), ("Car", "lot.abp")])
        self.assertIn("both opted in as", str(caught.exception))


class TestRendering(ViewTestCase):
    def test_nested_members_become_a_nested_struct(self):
        self.car()
        self.write("lot.abp", {"version": 2, "entities": [_entity("sign")],
                               "instances": [{"name": "car", "source": "car.abp"}]})
        text = self.generate(("Lot", "lot.abp"))
        self.assertIn("} car;", text)
        self.assertIn("ECS::Entity sign;", text)
        # The nested group carries no id of its own: there is one instance.
        self.assertEqual(text.count("ECS::InstanceId instanceId{};"), 1)

    def test_a_top_level_member_and_a_nested_one_do_not_collide(self):
        # The whole reason for grouping rather than flattening to car_body.
        self.car()
        self.write("lot.abp", {"version": 2, "entities": [_entity("car_body")],
                               "instances": [{"name": "car", "source": "car.abp"}]})
        text = self.generate(("Lot", "lot.abp"))
        self.assertIn("ECS::Entity car_body;", text)
        self.assertIn('view.car_body = FindMember(scene, table, view.instanceId, "car_body");', text)
        self.assertIn('view.car.body = FindMember(scene, table, view.instanceId, "car/body");', text)

    def test_fill_resolves_every_member_by_its_runtime_name(self):
        self.car()
        text = self.generate(("Car", "car.abp"))
        for name in ("body", "wheel_fl", "wheel_fr"):
            self.assertIn(f'view.{name} = FindMember(scene, table, view.instanceId, "{name}");', text)

    def test_the_view_is_move_only(self):
        self.car()
        text = self.generate(("Car", "car.abp"))
        self.assertIn("InstanceView(const InstanceView &)            = delete;", text)
        self.assertIn("InstanceView(InstanceView &&)                 = default;", text)

    def test_traits_carry_the_source_so_a_typed_spawn_needs_no_string(self):
        self.car()
        text = self.generate(("Car", "car.abp"))
        self.assertIn('static constexpr std::string_view kSource = "car.abp";', text)

    def test_the_tag_type_is_only_declared(self):
        # Incomplete on purpose: it names a blueprint and is never instantiated.
        self.car()
        text = self.generate(("Car", "car.abp"))
        self.assertIn("struct Car; // car.abp", text)

    def test_a_blueprint_with_no_members_still_generates(self):
        self.write("empty.abp", {"version": 2, "entities": []})
        text = self.generate(("Empty", "empty.abp"))
        self.assertIn("template <> struct InstanceView<Blueprints::Empty>", text)
        self.assertIn("(void)view;", text)  # or the fill would warn on unused args


if __name__ == "__main__":
    unittest.main(verbosity=2)
