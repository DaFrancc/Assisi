# User interface

Menus, HUDs and dialogs are drawn by **Mondrian**, the engine's own UI system.
This chapter starts with a tutorial that builds a pause menu. The sections after
it describe each part of the system in reference form:

| Section | What it covers |
|---|---|
| [Tutorial: a pause menu](ui-tutorial.md) | A working menu, from screen file to level. |
| [Screen settings](ui-screen-settings.md) | Input, pausing, hiding what's beneath and draw order. |
| [Elements and names](ui-elements.md) | The nine elements, and how nodes are named. |
| [Layout and appearance](ui-layout.md) | Lengths and units, sizes, colours, text, floating and scrolling. |
| [Controls](ui-controls.md) | Toggles, sliders, scroll areas and text fields. |
| [Button actions](ui-actions.md) | What a button does when clicked: an action or an event. |
| [Templates](ui-templates.md) | Declaring an element once and reusing it. |
| [Screens at run time](ui-run-time.md) | Lifetime, finding screens and nodes, and sharing screens between levels. |
| [What the cook checks](ui-cook-checks.md) | Every mistake that fails the cook. |

## Overview

These are the terms the rest of the chapter uses:

| Term | Meaning |
|---|---|
| **Screen** | One piece of UI: a menu, a HUD, a dialog. A screen belongs to a world and is destroyed with it. |
| **Node** | One box in a screen. A node can hold text, children, or be a control such as a button. |
| **Screen file** | A `.amdn` file describing one screen, written in an XML-style markup. |
| **Element** | One tag in a screen file, such as `<button>`. Each element becomes one node. |
| **Attribute** | A setting on an element, such as `width="grow"`. |
| **Cooking** | The build step that compiles every screen file into a binary form. |

A screen file is never read by the shipped game. The cook compiles it, and the
game loads the compiled form. Any mistake in a screen file — a misspelt
attribute, an unknown event, a bad value — fails the cook with the file, line
and column, so it is caught at build time rather than by a player.
