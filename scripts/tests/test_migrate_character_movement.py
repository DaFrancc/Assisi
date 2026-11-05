#!/usr/bin/env python3
"""Behavioural tests for migrate-character-movement.py.

What matters most is that a Character moves as it did once split, that a file
is touched only where a Character is, and that a second run finds nothing to do.
"""

import importlib.util
import json
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "migrate-character-movement.py"

_spec = importlib.util.spec_from_file_location("migrate_character_movement", SCRIPT)
migrate = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(migrate)


def level(*entities):
    return json.dumps({"entities": list(entities), "version": 2}, indent=2, sort_keys=True) + "\n"


def entity(name, **components):
    return {"components": components, "name": name}


def components_of(text, name):
    for item in json.loads(text)["entities"]:
        if item["name"] == name:
            return item["components"]
    raise KeyError(name)


# A Character as the engine saved one before the split: every field written.
SAVED_CHARACTER = {
    "airAcceleration": 10.0, "airWishSpeedCap": 0.5715000033378601, "bunnyHop": 1,
    "bunnyHopSpeedCap": 1.7000000476837158, "channel": 1, "collidesWith": "All", "coyoteTime": 0.10000000149011612,
    "crouchEyeHeight": 0.8999999761581421, "crouchHalfHeight": 0.30000001192092896,
    "crouchSpeedScale": 0.3330000042915344, "eyeHeight": 1.5, "eyeSpeed": 3.0, "friction": 4.0,
    "gravityScale": 1.1649999618530273, "groundAcceleration": 10.0, "halfHeight": 0.6000000238418579,
    "jumpBufferTime": 0.15000000596046448, "jumpSpeed": 3.049999952316284, "mass": 70.0, "maxSlopeDegrees": 50.0,
    "maxStepHeight": 0.4000000059604645, "options": "All", "pushStrength": 100.0, "radius": 0.30000001192092896,
    "stopSpeed": 1.9049999713897705, "walkSpeed": 3.619999885559082,
}


class MigrateTest(unittest.TestCase):
    def test_a_saved_character_keeps_its_shape_and_gains_its_movement_beside_it(self):
        components = components_of(migrate.migrate_text(level(entity("player", Character=SAVED_CHARACTER))),
                                   "player")
        self.assertEqual(set(components["Character"]),
                         {"channel", "collidesWith", "crouchEyeHeight", "crouchHalfHeight", "eyeHeight", "eyeSpeed",
                          "halfHeight", "mass", "maxSlopeDegrees", "maxStepHeight", "options", "pushStrength",
                          "radius"})
        self.assertEqual(components["SourceMovement"],
                         {"airAcceleration": 10.0, "airWishSpeedCap": 0.5715000033378601, "bunnyHop": 1,
                          "bunnyHopSpeedCap": 1.7000000476837158, "friction": 4.0, "groundAcceleration": 10.0,
                          "stopSpeed": 1.9049999713897705})
        self.assertEqual(components["CharacterLocomotion"]["coyoteTime"], 0.10000000149011612)
        self.assertEqual(components["CharacterLocomotion"]["walkSpeed"], 3.619999885559082)

    def test_the_source_numbers_take_the_source_preset(self):
        # Written back as floats, which is how the engine saved them.
        components = components_of(migrate.migrate_text(level(entity("player", Character=SAVED_CHARACTER))),
                                   "player")
        self.assertEqual(components["CharacterLocomotion"]["preset"], 0)

    def test_a_character_that_left_its_speeds_out_had_the_source_ones(self):
        components = components_of(migrate.migrate_text(level(entity("npc", Character={"radius": 0.4}))), "npc")
        self.assertEqual(components["CharacterLocomotion"], {"preset": 0})
        self.assertEqual(components["SourceMovement"], {})
        self.assertEqual(components["Character"], {"radius": 0.4})

    def test_tuned_numbers_are_kept_and_read(self):
        components = components_of(migrate.migrate_text(level(entity("runner", Character={"walkSpeed": 6.0}))),
                                   "runner")
        self.assertEqual(components["CharacterLocomotion"], {"preset": 2, "walkSpeed": 6.0})

    def test_numbers_are_written_back_exactly_as_they_were_read(self):
        text = '{\n  "entities": [\n    {\n      "components": {\n        "Character": {\n' \
               '          "walkSpeed": 3.619999885559082\n        }\n      },\n      "name": "p"\n    }\n  ]\n}\n'
        self.assertIn('"walkSpeed": 3.619999885559082', migrate.migrate_text(text))

    def test_a_file_with_no_character_is_left_alone(self):
        self.assertIsNone(migrate.migrate_text(level(entity("crate", Collider={"radius": 0.5}))))

    def test_a_second_run_changes_nothing(self):
        once = migrate.migrate_text(level(entity("player", Character=SAVED_CHARACTER)))
        self.assertIsNone(migrate.migrate_text(once))

    def test_a_character_already_beside_a_movement_component_is_left_alone(self):
        text = level(entity("player", Character={"radius": 0.4}, ArcadeMovement={}))
        self.assertIsNone(migrate.migrate_text(text))

    def test_the_components_and_their_fields_stay_in_sorted_order(self):
        migrated = migrate.migrate_text(level(entity("player", Character=SAVED_CHARACTER, Transform={})))
        components = components_of(migrated, "player")
        self.assertEqual(list(components), ["Character", "CharacterLocomotion", "SourceMovement", "Transform"])
        for name in ("Character", "CharacterLocomotion", "SourceMovement"):
            self.assertEqual(list(components[name]), sorted(components[name]))


if __name__ == "__main__":
    unittest.main()
