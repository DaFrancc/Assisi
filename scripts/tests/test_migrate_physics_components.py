#!/usr/bin/env python3
"""Behavioural tests for migrate-physics-components.py.

What matters most is that every descriptor lands as the component set that
builds the same body, that a file is touched only where a component changes,
and that a second run finds nothing to do.
"""

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "migrate-physics-components.py"

_spec = importlib.util.spec_from_file_location("migrate_physics_components", SCRIPT)
migrate = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(migrate)


def level(*entities, systems=None):
    document = {"entities": list(entities), "version": 2}
    if systems is not None:
        document["systems"] = systems
    return json.dumps(document, indent=2, sort_keys=True)


def entity(name, **components):
    return {"components": components, "name": name}


def components_of(text, name):
    for item in json.loads(text)["entities"]:
        if item["name"] == name:
            return item["components"]
    raise KeyError(name)


class MigrateTest(unittest.TestCase):
    def test_a_static_descriptor_becomes_a_collider_alone(self):
        text = level(entity("wall", RigidBodyDescriptor={"halfExtents": [2.0, 1.0, 2.0], "isStatic": True}))
        components = components_of(migrate.migrate_text(text), "wall")
        self.assertEqual(components, {"Collider": {"halfExtents": [2.0, 1.0, 2.0]}})

    def test_a_moving_descriptor_becomes_a_collider_and_a_rigid_body(self):
        text = level(entity("crate", RigidBodyDescriptor={"shape": 1, "radius": 0.25, "isStatic": False,
                                                          "enableCCD": True}))
        components = components_of(migrate.migrate_text(text), "crate")
        self.assertEqual(components["Collider"], {"shape": 1, "radius": 0.25})
        self.assertEqual(components["RigidBody"], {"ccd": True})

    def test_a_descriptor_with_no_static_flag_moves(self):
        # The descriptor's default was a moving body.
        text = level(entity("ball", RigidBodyDescriptor={"shape": 1}))
        components = components_of(migrate.migrate_text(text), "ball")
        self.assertEqual(components["RigidBody"], {})

    def test_a_moving_trigger_becomes_a_kinematic_sensor_that_never_sleeps(self):
        text = level(entity("zone", RigidBodyDescriptor={"channel": 2, "isStatic": False}))
        components = components_of(migrate.migrate_text(text), "zone")
        self.assertEqual(components["Collider"], {"channel": 2})
        self.assertEqual(components["RigidBody"], {"allowSleep": False, "motion": 1})

    def test_a_static_trigger_stays_static(self):
        text = level(entity("zone", RigidBodyDescriptor={"channel": 2, "isStatic": True}))
        components = components_of(migrate.migrate_text(text), "zone")
        self.assertNotIn("RigidBody", components)

    def test_a_character_descriptor_becomes_a_character(self):
        text = level(entity("player", CharacterDescriptor={"walkSpeed": 5.0}))
        components = components_of(migrate.migrate_text(text), "player")
        self.assertEqual(components, {"Character": {"walkSpeed": 5.0}})

    def test_retired_systems_are_dropped_and_the_rest_kept_in_order(self):
        text = level(systems=["CharacterMove", "CharacterLook", "CharacterState", "CharacterEye"])
        self.assertEqual(json.loads(migrate.migrate_text(text))["systems"], ["CharacterLook", "CharacterEye"])

    def test_a_bounce_becomes_its_colliders_restitution(self):
        text = level(entity("ball", Bounce={"rebound": 1.25}, Collider={"radius": 0.5, "shape": 1},
                            RigidBody={}))
        components = components_of(migrate.migrate_text(text), "ball")
        self.assertNotIn("Bounce", components)
        self.assertEqual(components["Collider"], {"radius": 0.5, "restitution": 1.25, "shape": 1})

    def test_a_bounce_with_no_rebound_takes_its_default(self):
        text = level(entity("ball", Bounce={}, Collider={}))
        self.assertEqual(components_of(migrate.migrate_text(text), "ball")["Collider"], {"restitution": 1.0})

    def test_the_bounce_system_is_dropped(self):
        text = level(systems=["BouncerSpawn", "Bounce"])
        self.assertEqual(json.loads(migrate.migrate_text(text))["systems"], ["BouncerSpawn"])

    def test_a_removed_descriptor_in_an_override_is_removed_from_both_components(self):
        text = json.dumps({"overrides": {"crate": {"RigidBodyDescriptor": None}}}, indent=2)
        self.assertEqual(json.loads(migrate.migrate_text(text))["overrides"]["crate"],
                         {"Collider": None, "RigidBody": None})

    def test_numbers_are_written_back_exactly_as_they_were(self):
        text = ('{\n  "entities": [\n    {\n      "components": {\n        "RigidBodyDescriptor": {\n'
                '          "halfExtents": [\n            1.3829116821289063,\n            0.5,\n            1e-07\n'
                '          ],\n          "isStatic": true\n        }\n      },\n      "name": "a"\n    }\n  ],\n'
                '  "version": 2\n}')
        migrated = migrate.migrate_text(text)
        self.assertIn("1.3829116821289063", migrated)
        self.assertIn("1e-07", migrated)

    def test_the_file_keeps_its_layout(self):
        text = level(entity("wall", RigidBodyDescriptor={"isStatic": True}), systems=["Oscillate"]) + "\n"
        migrated = migrate.migrate_text(text)
        expected = level(entity("wall", Collider={}), systems=["Oscillate"]) + "\n"
        self.assertEqual(migrated, expected)

    def test_a_second_run_changes_nothing(self):
        text = level(entity("crate", RigidBodyDescriptor={"isStatic": False}),
                     entity("player", CharacterDescriptor={}), systems=["CharacterMove"])
        once = migrate.migrate_text(text)
        self.assertIsNotNone(once)
        self.assertIsNone(migrate.migrate_text(once))

    def test_a_parented_body_is_reported(self):
        text = level(entity("lid", RigidBodyDescriptor={"isStatic": False}, Parent={"parent": "box"}),
                     entity("decal", RigidBodyDescriptor={"isStatic": True}, Parent={"parent": "box"}))
        self.assertEqual(migrate.parented_bodies(migrate.parse(migrate.migrate_text(text))), ["lid"])

    def test_the_command_migrates_the_files_it_is_given(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "level.alvl"
            untouched = Path(root) / "notes.txt"
            path.write_text(level(entity("wall", RigidBodyDescriptor={"isStatic": True})), encoding="utf-8")
            untouched.write_text('{"RigidBodyDescriptor": {}}', encoding="utf-8")
            self.assertEqual(migrate.main([root]), 0)
            self.assertIn("Collider", path.read_text(encoding="utf-8"))
            self.assertEqual(untouched.read_text(encoding="utf-8"), '{"RigidBodyDescriptor": {}}')


if __name__ == "__main__":
    unittest.main()
