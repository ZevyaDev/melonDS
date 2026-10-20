---
name: melonds-screenshot
description: Capture the DS screens to a PNG file, and collect per-frame memory values with subscribe. Use to see what the game is showing, to verify an input did something, or to sample variables every frame.
---

# Screenshots and frame sampling

    screenshot shot.png                  both screens, 256x384 (top above bottom), RGB
    screenshot top.png --screen top      also bottom; --alpha writes RGBA
    -> {"file", "width", "height", "frame", "console"}

The PNG shows the last completed frame. Open it with an image viewer tool (Read can display PNGs). Take one after
every `input` that should change the screen, and name files by step (`step03_menu.png`).

## Sample memory each frame

    subscribe --range 0x02200000:4 --range PlayerPos:12:9 --frames 30 [--every 2]

Runs the frames itself and prints one JSON line per frame event:
`{"console", "frame", "data": ["hex of range 1", "hex of range 2"], ...}`. Range syntax `ADDR:LEN[:CPU]`.
Decode the hex yourself (little-endian), or use `watch` for single reads. The subscription is removed when the
command ends.
