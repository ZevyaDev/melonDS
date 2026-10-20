"""Expand an input sequence string into one entry per frame.

Grammar (a superset of ramtools.parse_sequence, which parses every token that has no touch part):

    "A*4 w20 Down*30"       tokens split on spaces or commas
    A, A+B, Up+Left         keys held together ("+"); names are case-insensitive
    *N                      frames to hold (default 4)
    wN                      N frames with nothing pressed
    touch:X,Y  touch:X:Y    pen down at X (0-255), Y (0-191); combines with keys: A+touch:10,20*5

A touch that is not repeated in the next token is released: the first frame after it carries one
release point. A sequence that ends while touching gets one extra trailing release frame, so the pen
is never left down.
"""
import re
from dataclasses import dataclass

from ..client import KEYS
from ..ramtools import parse_sequence

TOUCH = re.compile(r"touch:(\d+)[,:](\d+)", re.IGNORECASE)
KEY_NAMES = {k.lower(): k for k in KEYS}


@dataclass
class Frame:
    buttons: int = 0
    touch: list | None = None      # None = unchanged, else [(offset, x, y, down)]


def _keys_mask(names):
    mask = 0
    for name in names:
        if name == "":
            continue
        key = KEY_NAMES.get(name.lower())
        if key is None:
            raise ValueError(f"unknown key {name!r} (keys: {', '.join(KEYS)})")
        mask |= 1 << KEYS.index(key)
    return mask


def expand(spec):
    """Returns a list of Frame."""
    text = TOUCH.sub(lambda m: f"touch:{m.group(1)}:{m.group(2)}", spec)
    frames = []
    pen = None                       # (x, y) while the pen is down
    for token in text.replace(",", " ").split():
        keys_part, star, count_part = token.partition("*")
        touch = None
        key_names = []
        for part in keys_part.split("+"):
            if part.lower().startswith("touch"):
                m = re.fullmatch(r"touch:(\d+):(\d+)", part, re.IGNORECASE)
                if not m:
                    raise ValueError(f"bad touch {part!r}; use touch:X,Y")
                touch = (int(m.group(1)), int(m.group(2)))
                if touch[0] > 255 or touch[1] > 191:
                    raise ValueError(f"touch {touch} is outside 0-255 x 0-191")
            else:
                key_names.append(part)
        try:
            if touch is None:
                (names, count), = parse_sequence(token)
                mask = _keys_mask(names)
            else:
                count = int(count_part or 4)
                mask = _keys_mask(key_names)
        except ValueError as e:
            if "unknown key" in str(e) or "touch" in str(e):
                raise
            raise ValueError(f"bad token {token!r}: {e}") from None
        if count < 0:
            raise ValueError(f"bad token {token!r}: negative frame count")
        for i in range(count):
            if touch is not None:
                frames.append(Frame(mask, [(0, touch[0], touch[1], 1)]))
            elif pen is not None and i == 0:
                frames.append(Frame(mask, [(0, pen[0], pen[1], 0)]))
            else:
                frames.append(Frame(mask, None))
        if touch is not None and count:
            pen = touch
        elif pen is not None and count:
            pen = None
    if pen is not None:
        frames.append(Frame(0, [(0, pen[0], pen[1], 0)]))
    return frames
