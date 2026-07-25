---
id: userDocs
aliases: []
tags: []
---
# User Guide — `render` commands

`render` turns 3D models, images, and animated GIFs into coloured ASCII art —
either live in your terminal or saved to a `.txt` / `.gif` file.


## Live 3D model viewer

```sh
render                 # the built-in teapot
render models/cube.obj
render models/teapot.obj
```

Displays the model as a rotating, coloured ASCII line drawing at ~60 fps. The
renderer draws only the outline, silhouette, and sharp (crease) edges, so even a
dense mesh reads cleanly.

### Controls

| Key | Action |
|-----|--------|
| `↑` / `k` | Spin up (rotate about X) |
| `↓` / `j` | Spin down |
| `→` / `l` | Spin right (rotate about Y) |
| `←` / `h` | Spin left |
| `Space`   | Pause / resume rotation |
| `0`       | Reset to the default spin |
| `q` / `Q` / `Esc` | Quit |

Arrow keys and the `hjkl` keys *add* to the current spin, so tapping builds up
speed; `0` resets it. The view auto-fits when you resize the terminal.

---

## Live image viewer

```sh
render photo.jpg
render drawing.png
```

Renders the image as structure-based ASCII, fitted to your terminal. The picture
is static, so it only re-renders when you resize the window.

Press `q`, `Q`, or `Esc` to quit.

---

## Live GIF playback

```sh
render animation.gif
```

Plays an animated GIF as coloured ASCII, looping forever, honoring each frame's
delay. The grid re-fits to the terminal every frame, so resizing works live.

Press `q`, `Q`, or `Esc` to quit.

---

## Save an image to a text file

Render once (no terminal UI) and write a plain ASCII grid to a file:

```sh
render photo.jpg out.txt              # size from the terminal (or 100×50)
render photo.jpg out.txt 120 60       # explicit columns × rows
```

- `cols` / `rows` set the character grid size. If omitted, the current terminal
  size is used; if that can't be determined (e.g. output is piped), it falls back
  to 100×50.
- The output is uncoloured text — one line per row, spaces for blank cells.

### Auto resolution

Let the renderer choose the resolution that best represents the picture (it
minimizes the paper's matching-energy metric — usually a smaller, cleaner grid):

```sh
render photo.jpg out.txt auto         # search up to the terminal width (≤200)
render photo.jpg out.txt auto 120     # search up to 120 columns
```

It prints the search (each candidate size and its energy `E`) to stderr and writes
the best result to `out.txt`.

---

## Convert a GIF to an ASCII-art GIF

Produce a new animated GIF where every frame is rendered as coloured ASCII (drawn
with an embedded 8×16 font):

```sh
render in.gif out.gif                 # 80 columns, aspect-preserving height
render in.gif out.gif 120             # 120 columns
render in.gif out.gif 120 60          # explicit columns × rows
```

- `cols` defaults to 80 (max 400); `rows` defaults to an aspect-preserving height.
- Per-frame delays and looping are preserved.
- Progress (`frame N/total`) is printed to stderr; the final size is reported when
  done.

The output GIF is self-contained (no external tools needed) and can be viewed in
any image viewer or browser.

---

## Render a 3D model to a GIF

Produce an animated GIF of a model tumbling in place (headless — no terminal UI):

```sh
render model.obj out.gif                    # 80 cols, auto height, 24 frames
render models/teapot.obj teapot.gif 120 60  # explicit columns × rows
render models/cube.obj cube.gif 80 40 36    # …and an explicit frame count
```

- `cols` defaults to 80 (max 400); `rows` defaults to `cols / 2` (terminal cells
  are ~2× tall, so half the columns keeps the aspect roughly square); `frames`
  defaults to 24.
- The spin is a **seamless tumble** — one full rotation about X and two about Y —
  so the animation loops cleanly with no visible jump.
- Frames are drawn with the same AISS structure matching as the live viewer, then
  blitted with the embedded 8×16 font. The GIF loops forever and is self-contained.
- Progress (`frame N/total`) prints to stderr; the final size is reported when done.

---

## Exit codes & messages

- Success returns `0`; a load/parse/write failure prints an `error:` line to
  stderr and returns `1`.
- Informational output for the file-writing commands (search progress, final
  dimensions) goes to **stderr**, so it won't pollute a redirected `.txt`.
