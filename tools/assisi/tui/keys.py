"""What the bytes a terminal sends mean, as key names.

A key is its character ("j", "D", ":", " ") or a name: "up", "down", "left",
"right", "home", "end", "pageup", "pagedown", "insert", "delete", "tab",
"shift+tab", "enter", "esc", "backspace" and "ctrl+<letter>". Everything here
is pure, so the decoding is tested on byte strings without a terminal.
"""

from __future__ import annotations

from typing import Dict, List, Optional, Tuple

ESCAPE = 0x1B

# The delete character, which terminals send for the backspace key.
DELETE = 0x7F

# What follows ESC to start a control sequence (CSI) or a single shift (SS3).
CSI_START = ord("[")
SS3_START = ord("O")

# A control sequence is ESC [, parameter bytes in this range, then one final
# byte in the range after it.
CSI_PARAMETERS = range(0x30, 0x40)
CSI_FINALS = range(0x40, 0x7F)

# The final byte of a sequence, for keys that need no parameter. Modifiers
# arrive as parameters ("1;5C" is ctrl+right) and are not told apart.
FINAL_KEYS: Dict[str, str] = {
    "A": "up", "B": "down", "C": "right", "D": "left",
    "H": "home", "F": "end", "Z": "shift+tab",
}

# The first parameter of a sequence ending in "~".
TILDE_KEYS: Dict[str, str] = {
    "1": "home", "2": "insert", "3": "delete", "4": "end",
    "5": "pageup", "6": "pagedown", "7": "home", "8": "end",
}

# The control characters with a name of their own rather than ctrl+<letter>.
CONTROL_KEYS: Dict[int, str] = {0x09: "tab", 0x0A: "enter", 0x0D: "enter", 0x08: "backspace", DELETE: "backspace"}

# Ctrl+A to Ctrl+Z send 1 to 26.
CTRL_FIRST = 0x01
CTRL_LAST = 0x1A

# Anything below this that is not named above is a control character no key
# of ours uses.
FIRST_PRINTABLE = 0x20

# The lead byte of a UTF-8 character says how many bytes it has.
UTF8_LENGTHS = ((0x80, 1), (0xE0, 2), (0xF0, 3), (0xF8, 4))

# What Windows sends after a 0 or 0xE0 prefix, as a scan code character.
WINDOWS_SCAN_KEYS: Dict[str, str] = {
    "H": "up", "P": "down", "M": "right", "K": "left",
    "G": "home", "O": "end", "I": "pageup", "Q": "pagedown",
    "R": "insert", "S": "delete", "\x0f": "shift+tab",
}
WINDOWS_PREFIXES = ("\x00", "\xe0")


def control_key(code: int) -> Optional[str]:
    """The name of the control character @p code, or None for one we ignore."""
    if code in CONTROL_KEYS:
        return CONTROL_KEYS[code]
    if CTRL_FIRST <= code <= CTRL_LAST:
        return "ctrl+" + chr(ord("a") + code - CTRL_FIRST)
    return None


def char_key(character: str) -> Optional[str]:
    """The key one character is, as Windows hands them over one at a time."""
    code = ord(character)
    if code == ESCAPE:
        return "esc"
    if code < FIRST_PRINTABLE or code == DELETE:
        return control_key(code)
    return character


def windows_key(scan: str) -> Optional[str]:
    """The key a Windows scan code names, read after a prefix character."""
    return WINDOWS_SCAN_KEYS.get(scan)


def sequence_key(body: str) -> Optional[str]:
    """The key a CSI or SS3 sequence names, from what follows its introducer."""
    final = body[-1]
    if final == "~":
        return TILDE_KEYS.get(body[:-1].split(";")[0])
    return FINAL_KEYS.get(final)


def utf8_length(lead: int) -> int:
    for limit, length in UTF8_LENGTHS:
        if lead < limit:
            return length
    return 1


def escape_length(buffer: bytes, start: int) -> int:
    """How many bytes the escape sequence at @p start takes, 0 when it is not
    complete yet, or 1 when the ESC starts no sequence."""
    if start + 1 >= len(buffer):
        return 0
    introducer = buffer[start + 1]
    if introducer == SS3_START:
        return 3 if start + 2 < len(buffer) else 0
    if introducer != CSI_START:
        return 1
    index = start + 2
    while index < len(buffer) and buffer[index] in CSI_PARAMETERS:
        index += 1
    if index >= len(buffer):
        return 0
    return index - start + 1 if buffer[index] in CSI_FINALS else 1


def decode(buffer: bytes) -> Tuple[List[str], bytes]:
    """The keys in @p buffer, and the bytes at its end that do not make a whole
    key yet. A lone ESC is held back, since it may start a sequence whose rest
    has not arrived; flush says what held-back bytes are once nothing follows."""
    names: List[str] = []
    index = 0
    while index < len(buffer):
        code = buffer[index]
        if code == ESCAPE:
            length = escape_length(buffer, index)
            if length == 0:
                break
            if length == 1:
                names.append("esc")
            else:
                name = sequence_key(buffer[index + 2:index + length].decode("ascii"))
                if name is not None:
                    names.append(name)
            index += length
            continue
        if code < FIRST_PRINTABLE or code == DELETE:
            name = control_key(code)
            if name is not None:
                names.append(name)
            index += 1
            continue
        length = utf8_length(code)
        if index + length > len(buffer):
            break
        names.append(buffer[index:index + length].decode("utf-8", errors="replace"))
        index += length
    return names, buffer[index:]


def flush(pending: bytes) -> List[str]:
    """What bytes held back by decode mean once no more are coming: an ESC on
    its own was the escape key, and half a character is nothing."""
    return ["esc"] if pending[:1] == bytes([ESCAPE]) else []
