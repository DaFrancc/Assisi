#!/usr/bin/env python3
"""Splits the movement settings out of every Character in levels and
blueprints.

  walkSpeed, crouchSpeedScale, jumpSpeed, gravityScale,
  coyoteTime, jumpBufferTime                              ->  CharacterLocomotion
  friction, stopSpeed, groundAcceleration, airAcceleration,
  airWishSpeedCap, bunnyHopSpeedCap, bunnyHop             ->  SourceMovement

A Character that held these moved the way SourceMovement does, so every one
gains a SourceMovement and a CharacterLocomotion and moves as it did. The
CharacterLocomotion takes the Source preset when its walk speed, crouch speed
scale, jump speed and gravity scale are that preset's, and Custom otherwise, so
the values it held are the ones read.

Every number is written back exactly as it was read, and the indentation, key
order and final newline are kept, so a file is changed only where a Character
is. A Character beside a movement component or a CharacterLocomotion is left
alone, so running it twice changes nothing the second time.

    scripts/migrate-character-movement.py [path ...]

Paths are files or directories; the default is assets/.
"""

import json
import struct
import sys
from pathlib import Path

EXTENSIONS = {".alvl", ".abp"}

LOCOMOTION_FIELDS = ("walkSpeed", "crouchSpeedScale", "jumpSpeed", "gravityScale", "coyoteTime", "jumpBufferTime")
SOURCE_FIELDS = ("friction", "stopSpeed", "groundAcceleration", "airAcceleration", "airWishSpeedCap",
                 "bunnyHopSpeedCap", "bunnyHop")
ALREADY_SPLIT = ("CharacterLocomotion", "SourceMovement", "ArcadeMovement")

# LocomotionPreset as the files store it: by its integer.
SOURCE_PRESET = "0"
CUSTOM_PRESET = "2"

# The Source preset's values, which were also the Character's defaults, so a
# field a file left out held them.
SOURCE_LOCOMOTION = {"walkSpeed": 3.62, "crouchSpeedScale": 0.333, "jumpSpeed": 3.05, "gravityScale": 1.165}


class Number(str):
    """A number's text as it appeared in the file, written back untouched."""


def parse(text):
    return json.loads(text, parse_float=Number, parse_int=Number)


def write(value, depth=0):
    """The layout json.dumps(indent=2) produces, with numbers as read."""
    pad = "  " * (depth + 1)
    end = "  " * depth
    if isinstance(value, dict):
        if not value:
            return "{}"
        items = [f"{pad}{json.dumps(key)}: {write(item, depth + 1)}" for key, item in value.items()]
        return "{\n" + ",\n".join(items) + "\n" + end + "}"
    if isinstance(value, list):
        if not value:
            return "[]"
        items = [pad + write(item, depth + 1) for item in value]
        return "[\n" + ",\n".join(items) + "\n" + end + "]"
    if isinstance(value, Number):
        return str(value)
    return json.dumps(value)


def as_float32(value):
    """@p value rounded to the float the engine stores it as."""
    return struct.unpack("f", struct.pack("f", float(value)))[0]


def is_source_preset(locomotion):
    """Whether @p locomotion's preset fields are the Source preset's."""
    return all(as_float32(locomotion.get(field, default)) == as_float32(default)
               for field, default in SOURCE_LOCOMOTION.items())


def split_character(components):
    """Moves @p components' Character's movement fields into a
    CharacterLocomotion and a SourceMovement beside it, keeping the keys sorted
    if they were."""
    character = components["Character"]
    locomotion = {field: character.pop(field) for field in LOCOMOTION_FIELDS if field in character}
    source = {field: character.pop(field) for field in SOURCE_FIELDS if field in character}
    locomotion["preset"] = Number(SOURCE_PRESET if is_source_preset(locomotion) else CUSTOM_PRESET)

    was_sorted = list(components.keys()) == sorted(components.keys())
    components["CharacterLocomotion"] = dict(sorted(locomotion.items()))
    components["SourceMovement"] = dict(sorted(source.items()))
    if was_sorted:
        rebuilt = dict(sorted(components.items()))
        components.clear()
        components.update(rebuilt)


def migrate(value):
    """Splits every Character under @p value. True when anything changed."""
    changed = False
    if isinstance(value, dict):
        character = value.get("Character")
        if isinstance(character, dict) and not any(name in value for name in ALREADY_SPLIT):
            split_character(value)
            changed = True
        for item in value.values():
            changed = migrate(item) or changed
    elif isinstance(value, list):
        for item in value:
            changed = migrate(item) or changed
    return changed


def migrate_text(text):
    """The migrated text, or None when nothing needed migrating."""
    document = parse(text)
    if not migrate(document):
        return None
    return write(document) + ("\n" if text.endswith("\n") else "")


def files_under(paths):
    for path in paths:
        if path.is_dir():
            yield from sorted(p for p in path.rglob("*") if p.suffix in EXTENSIONS)
        elif path.suffix in EXTENSIONS:
            yield path


def main(argv):
    paths = [Path(arg) for arg in argv] or [Path("assets")]
    for path in files_under(paths):
        migrated = migrate_text(path.read_text(encoding="utf-8"))
        if migrated is not None:
            path.write_text(migrated, encoding="utf-8")
            print(f"migrated {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
