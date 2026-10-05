#!/usr/bin/env python3
"""Moves levels and blueprints from the physics descriptors to the physics
components.

  RigidBodyDescriptor  ->  Collider, plus a RigidBody unless it was static
  CharacterDescriptor  ->  Character
  Bounce               ->  the entity's Collider.restitution, set to its rebound

and drops the CharacterMove, CharacterState and Bounce systems, whose work the
physics step now does itself, from every `systems` list: a level naming a
system that does not exist fails to load.

A moving trigger becomes a kinematic RigidBody that may not sleep, which is the
sensor the descriptor built for it. Collider sizes are left as they are: they
were already relative to the entity's scale.

An entity left with a RigidBody or a Character and a Parent is printed, since
neither may have one and only a person can say which should give way.

Every number is written back exactly as it was read, and the indentation, key
order and final newline are kept, so a file is changed only where a component
is. Running it twice changes nothing the second time.

    scripts/migrate-physics-components.py [path ...]

Paths are files or directories; the default is assets/.
"""

import json
import sys
from pathlib import Path

EXTENSIONS = {".alvl", ".abp"}
RETIRED_SYSTEMS = {"CharacterMove", "CharacterState", "Bounce"}
RETIRED_COMPONENTS = ("RigidBodyDescriptor", "CharacterDescriptor", "Bounce")

# Bounce's rebound when a file leaves it out.
DEFAULT_REBOUND = "1.0"

# Values as the files store them: an enum by its integer.
TRIGGER_CHANNEL = 2
KINEMATIC_MOTION = 1

COLLIDER_FIELDS = ("shape", "halfExtents", "radius", "halfHeight", "collidesWith", "channel")


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


def collider_and_rigid_body(descriptor):
    """The Collider and the RigidBody (or None) a descriptor becomes."""
    if descriptor is None:
        return None, None
    collider = {key: value for key, value in descriptor.items() if key in COLLIDER_FIELDS}
    if descriptor.get("isStatic", False):
        return collider, None
    rigid_body = {}
    if descriptor.get("enableCCD", False):
        rigid_body["ccd"] = True
    channel = descriptor.get("channel")
    if channel is not None and int(channel) == TRIGGER_CHANNEL:
        rigid_body["allowSleep"] = False
        rigid_body["motion"] = Number(str(KINEMATIC_MOTION))
    return collider, dict(sorted(rigid_body.items()))


def replace_keys(owner, old, new_items):
    """Replaces @p old in @p owner by @p new_items, keeping the keys sorted if
    they were, and otherwise putting the new ones where the old one was."""
    keys = list(owner.keys())
    was_sorted = keys == sorted(keys)
    rebuilt = {}
    for key, value in owner.items():
        if key == old:
            rebuilt.update(new_items)
        else:
            rebuilt[key] = value
    if was_sorted:
        rebuilt = dict(sorted(rebuilt.items()))
    owner.clear()
    owner.update(rebuilt)


def migrate_components(components):
    """Migrates one components object. True when it changed."""
    changed = False
    if "RigidBodyDescriptor" in components:
        collider, rigid_body = collider_and_rigid_body(components["RigidBodyDescriptor"])
        new_items = {"Collider": collider}
        if rigid_body is not None or collider is None:
            new_items["RigidBody"] = rigid_body
        replace_keys(components, "RigidBodyDescriptor", new_items)
        changed = True
    if "CharacterDescriptor" in components:
        replace_keys(components, "CharacterDescriptor", {"Character": components["CharacterDescriptor"]})
        changed = True
    if "Bounce" in components:
        bounce = components.pop("Bounce")
        collider = components.get("Collider")
        if isinstance(bounce, dict) and isinstance(collider, dict):
            set_key(collider, "restitution", bounce.get("rebound", Number(DEFAULT_REBOUND)))
        changed = True
    return changed


def set_key(owner, key, value):
    """Sets @p key in @p owner, keeping the keys sorted if they were."""
    was_sorted = list(owner.keys()) == sorted(owner.keys())
    owner[key] = value
    if was_sorted:
        rebuilt = dict(sorted(owner.items()))
        owner.clear()
        owner.update(rebuilt)


def migrate(value):
    """Migrates every components object and systems list under @p value. True
    when anything changed."""
    changed = False
    if isinstance(value, dict):
        for key, item in list(value.items()):
            if key == "systems" and isinstance(item, list):
                kept = [name for name in item if name not in RETIRED_SYSTEMS]
                if kept != item:
                    value[key] = kept
                    changed = True
            elif isinstance(item, dict) and any(name in item for name in RETIRED_COMPONENTS):
                changed = migrate_components(item) or changed
                changed = migrate(item) or changed
            else:
                changed = migrate(item) or changed
    elif isinstance(value, list):
        for item in value:
            changed = migrate(item) or changed
    return changed


def parented_bodies(document):
    """Names of entities with a RigidBody or a Character and a Parent."""
    names = []
    for entity in document.get("entities", []):
        components = entity.get("components", {})
        moving = components.get("RigidBody") is not None or components.get("Character") is not None
        if moving and components.get("Parent") is not None:
            names.append(entity.get("name", "?"))
    return names


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
        text = path.read_text(encoding="utf-8")
        migrated = migrate_text(text)
        if migrated is not None:
            path.write_text(migrated, encoding="utf-8")
            print(f"migrated {path}")
        document = parse(migrated if migrated is not None else text)
        for name in parented_bodies(document):
            print(f"needs a hand fix: {path}: '{name}' has a RigidBody or a Character and a Parent")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
