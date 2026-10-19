"""A frame of the TUI as a grid of styled cells, and how to put it on screen.

Styles name only the terminal's own sixteen colours and never a background, so
the developer's colour scheme decides what the TUI looks like. Every character
takes one column: the glyphs and box lines used here are one column wide in
the terminals this runs in, and a line is always rewritten whole, so a
character that is not only nudges its own line.
"""

from __future__ import annotations

import enum
from dataclasses import dataclass
from typing import List, Optional, Tuple


class Color(enum.Enum):
    """A foreground colour, as its SGR code."""

    DEFAULT = 39
    RED = 31
    GREEN = 32
    YELLOW = 33
    BLUE = 34
    MAGENTA = 35
    CYAN = 36


# The SGR codes of the attributes a style can switch on.
SGR_BOLD = 1
SGR_DIM = 2
SGR_REVERSE = 7


@dataclass(frozen=True)
class Style:
    color: Color = Color.DEFAULT
    bold: bool = False
    dim: bool = False
    reverse: bool = False

    def sgr(self) -> str:
        """The escape sequence that sets exactly this style from any other."""
        codes = [0, self.color.value]
        codes += [code for code, on in ((SGR_BOLD, self.bold), (SGR_DIM, self.dim), (SGR_REVERSE, self.reverse))
                  if on]
        return "\x1b[" + ";".join(str(code) for code in codes) + "m"


PLAIN = Style()

Cell = Tuple[str, Style]

# The box-drawing characters of a pane's border.
TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_RIGHT = "┌", "┐", "└", "┘"
HORIZONTAL, VERTICAL = "─", "│"

# How far from the corner a box's title starts.
TITLE_INDENT = 1

# The smallest box that has an inside.
MINIMUM_BOX = 2


class Canvas:
    def __init__(self, width: int, height: int):
        self.width = width
        self.height = height
        self.rows: List[List[Cell]] = [[(" ", PLAIN)] * width for _ in range(height)]

    def put(self, x: int, y: int, text: str, style: Style = PLAIN, limit: Optional[int] = None) -> int:
        """Write @p text from column @p x of row @p y, cut off at the right
        edge or before column @p limit, and return how many columns it took."""
        right = self.width if limit is None else min(limit, self.width)
        if not 0 <= y < self.height or x >= right:
            return 0
        row = self.rows[y]
        visible = text[:right - x]
        for offset, character in enumerate(visible):
            if x + offset >= 0:
                row[x + offset] = (character, style)
        return len(visible)

    def fill(self, x: int, y: int, width: int, style: Style) -> None:
        """Give @p width columns from @p x on row @p y the style @p style,
        keeping their characters: how a selected row is shown."""
        if not 0 <= y < self.height:
            return
        row = self.rows[y]
        for column in range(max(0, x), min(self.width, x + width)):
            row[column] = (row[column][0], style)

    def box(self, x: int, y: int, size: Tuple[int, int], title: str, style: Style = PLAIN) -> None:
        """A border of @p size columns by rows, with @p title set into its top."""
        width, height = size
        if width < MINIMUM_BOX or height < MINIMUM_BOX:
            return
        inner = width - MINIMUM_BOX
        self.put(x, y, TOP_LEFT + HORIZONTAL * inner + TOP_RIGHT, style)
        for row in range(y + 1, y + height - 1):
            self.put(x, row, VERTICAL, style)
            self.put(x + width - 1, row, VERTICAL, style)
        self.put(x, y + height - 1, BOTTOM_LEFT + HORIZONTAL * inner + BOTTOM_RIGHT, style)
        if title:
            self.put(x + TITLE_INDENT, y, f" {title} "[:inner], style)

    def text_lines(self) -> List[str]:
        return ["".join(character for character, _ in row) for row in self.rows]


def line_output(row: List[Cell]) -> str:
    """One row as characters and the style changes between them."""
    parts: List[str] = []
    current: Optional[Style] = None
    for character, style in row:
        if style != current:
            parts.append(style.sgr())
            current = style
        parts.append(character)
    return "".join(parts)


def render(canvas: Canvas, previous: Optional[Canvas]) -> str:
    """What to write to turn @p previous into @p canvas: the rows that differ,
    or a cleared screen and every row when there is nothing to compare with.
    Rows are full width, so nothing is erased after them: erasing to the end
    of a line whose last column was just written erases that column too."""
    whole = previous is None or (previous.width, previous.height) != (canvas.width, canvas.height)
    parts: List[str] = [f"{PLAIN.sgr()}\x1b[2J"] if whole else []
    for index, row in enumerate(canvas.rows):
        if whole or previous.rows[index] != row:
            parts.append(f"\x1b[{index + 1};1H{line_output(row)}")
    if parts:
        parts.append(PLAIN.sgr())
    return "".join(parts)
