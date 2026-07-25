# Developer Documentation — `src/` and `src2/`

This repository contains two independent C programs, each in its own directory.
Both are header-only in style: nearly all logic lives in `.h` files that are
`#include`d by a single `main.c` translation unit (no separate compilation units,
no linker step beyond the standard libraries).

| Directory | Program | Purpose |
|-----------|---------|---------|
| `src/`    | (unbuilt sketch) | A small threaded terminal "board" demo — the seed of a Tetris-style game. |
| `src2/`   | `render` | The real project: a data-oriented ASCII-art renderer for 3D models, images, and GIFs, based on the "Structure-based ASCII Art" paper (Xu et al., SIGGRAPH 2010). |

The `Makefile` builds **only `src2`** into the `render` binary:

```
make        # cc -O2 -std=c11 -Wall -Wextra src2/main.c -lpthread -lm -o render
make run    # build then ./render (defaults to models/teapot.obj)
make clean
```

---

## `src/` — threaded terminal board demo

An early, self-contained experiment. It opens the terminal in raw mode and runs a
20×20 bordered board with three worker threads. It is a work-in-progress skeleton
(the piece / game logic is stubbed), kept for reference; it is **not** wired into
the Makefile.

### Files

- **`vec.h`** — Small vector types and math. Both `float` (`Vec2f/Vec3f/Vec4f`)
  and `double` (`Vec2d/Vec3d/Vec4d`) variants, with `add`, `scale`, `dot`,
  `sub`, and `cross`. Shared, header-only.

- **`arena.h`** — A bump allocator (`Arena`). `arena_create` mallocs a fixed
  block; `arena_alloc` / `arena_alloc_aligned` hand out aligned slices;
  `arena_reset` rewinds to reuse the block each frame; `arena_destroy` frees it.
  Used so the per-frame render scratch is one allocation, not many.

- **`terminal.h`** — Two things in one header:
  1. *Raw-mode terminal control* — ANSI escape macros (`TERM_CLEAR`, `TERM_HOME`,
     alternate screen, hide cursor) plus `term_init` / `term_restore` /
     `term_size` / `term_write`. `term_init` switches the tty to raw mode with
     `VMIN=0, VTIME=1` so `read()` returns within ~100 ms (letting threads poll a
     `running` flag).
  2. *A 2D render path* — the `data_2d` / `data_3d` cell structs, an in-place
     Hoare-partition quicksort (`order`) that sorts cells row-major, `organize`
     which groups sorted cells into horizontal **runs** (arena-backed), and
     `printScreen` which emits one cursor-move + truecolor escape per run.

- **`main.c`** — The demo entry point. `genBoard` fills a `data_2d` array with the
  board border. Three pthreads share a mutex-guarded `thread_args`:
  - `drawThread` — ~60 fps: sort → organize into runs → clear screen → print.
  - `userInputs` — blocking `read()` of stdin into the shared input buffer.
  - `gameLoop`   — polls the input buffer; `q` sets `running = 0` to quit.

### Data flow

```
board cells (data_2d[])
    → order()      sort row-major (y, then x)
    → organize()   group into horizontal runs (arena)
    → printScreen() ANSI cursor + truecolor per run → terminal
```

---

## `src2/` — the `render` ASCII-art renderer

A data-oriented renderer that turns 3D models, still images, and animated GIFs
into coloured ASCII art. The distinguishing feature is **structure-based glyph
matching**: rather than picking a character by average brightness, each character
cell's local *line structure* is matched to the glyph whose shape best follows it,
using the Alignment-Insensitive Shape Similarity (AISS) metric from the Xu 2010
paper.

The design keeps a strict *producer → transform → render → present* pipeline.
Producers fill flat arrays of `{position, color, char}` structs; the rasterizer
splats them into one global grid; a run-length ANSI printer (or a font blitter, or
a text writer) reads the grid out.

### Build & entry

`main.c` includes every other header and dispatches on the command-line argument's
file extension (see [`userDocs.md`](userDocs.md) for the user-facing command
reference):

| Input | Path taken | Function in `main.c` |
|-------|-----------|----------------------|
| `*.obj`            | Live 3D model viewer (threaded, rotating)     | `run_model` |
| image (`.png`, `.jpg`, …), no 2nd arg | Live still-image viewer      | `run_image` |
| `*.gif`, no 2nd arg | Live GIF playback in the terminal            | `run_gif_play` |
| `model.obj out.gif [cols rows frames]` | Tumbling model → ASCII-art GIF (headless) | `run_obj_to_gif` |
| `image out.txt [cols rows]`     | One-shot render to a text file   | `run_image_to_file` |
| `image out.txt auto [maxcols]`  | Auto-pick the optimal resolution | `run_image_to_file_auto` |
| `in.gif out.gif [cols rows]`    | Convert GIF → ASCII-art GIF      | `run_gif_to_gif` (in `gifascii.h`) |

`run_obj_to_gif` is the offline sibling of `run_model`: it reuses the render
engine's structure extraction (`render_build_stream`) and AISS matching
(`render3D_structured`), but drives rotation by frame index for a seamless
tumble (one full X turn + two full Y turns) and writes frames through
`ascii_cells_to_rgb` + `gifout.h` instead of the terminal. Because
`render3D_structured` is synchronous, it needs neither the mailbox nor threads.

`main.c` also installs `SIGINT`/`SIGTERM` handlers that clear the global
`g_running` flag, and registers `term_restore` with `atexit` so the terminal is
always restored on exit.

### Files

Foundation (shared, header-only):

- **`vec.h`** — Vector types and math (same family as `src/`, float + double).
- **`mat4.h`** — Column-major 4×4 matrices: identity, multiply, translate,
  `rotate_x/y/z`, point/direction transforms, and `vec3f_normalize`. Drives the
  3D model rotation.
- **`arena.h`** — Bump allocator (see `src/` description; identical role).
- **`terminal.h`** — Raw-mode + alternate-screen control (`term_init` /
  `term_restore` / `term_size`). Restore is idempotent (safe from `atexit` and
  signal handlers).
- **`mailbox.h`** — A thread-safe, single-slot handoff (`RasterMailbox`) of a
  finished frame from the render thread to the present thread. Capacity 1, so the
  producer blocks until the consumer has taken the previous frame — this paces
  rendering to terminal I/O. Both sides copy through the slot so neither holds the
  lock during CPU work or writes.

The structure-matching core:

- **`aiss.h`** — The heart of the project. A faithful implementation of the
  paper's shape-matching metric:
  - A shape lives in a normalized cell; `AISS_SPX`×`AISS_SPY` sample points each
    accumulate a **log-polar histogram** (`AISS_NA` angular × `AISS_NR` radial
    bins) of surrounding "ink". Log-radial spacing makes nearby structure dominate,
    giving misalignment tolerance; angular bins give orientation awareness.
  - `aiss_build` builds a feature vector from any ink sampler (`AissInk`
    callback). `aiss_distance` is the paper's Eq. 1 normalized histogram distance.
  - A **codebook** of glyphs (`g_glyph_defs`) is defined by centreline stroke
    segments (`AissSeg`), rasterized analytically to one-pixel width and
    pre-built at every translation offset in `aiss_book_init`. Matching
    (`aiss_match_ex`) tries each glyph at several translations and keeps the
    closest — a cheap stand-in for the paper's deformation step.
  - `aiss_match_fast` (center + axis offsets) is used by the real-time 3D path;
    `aiss_match` (full translation search) by the one-shot image path.

- **`raster.h`** — The data-oriented rasterizer and all output formats. One
  process-global `RasterTarget` grid (one render thread, one screen). Key pieces:
  - **Stream structs** — `Data2D` (screen-space) and `Data3D` (camera-space)
    `{position, color, char}` elements; `RasterCell` / `raster_data` for the
    output stream.
  - **Three render entry points** — `render_char` (coords are already screen
    cells), `render2D` (normalized [-1,1] object coords), `render3D`
    (camera-space, projected + depth-tested). All funnel through `raster_put`
    (a depth-tested splat) and `raster_extract` (row-major readout).
  - **Structure-based 3D path** — `render3D_structured` splats projected line
    structure into a sub-cell **coverage buffer**, then `raster_cov_resolve`
    AISS-matches each occupied cell to a glyph. This applies the paper's grid-cell
    matching to the live 3D renderer.
  - **`raster_project`** — perspective projection (horizontal scale doubled
    because terminal cells are ~2× tall as wide). `raster_glyph_for_dir` picks a
    line glyph from a screen-space direction (used as a fast fallback).
  - **Output writers** — `raster_build_ansi` (run-length truecolor ANSI for the
    terminal), `raster_write_text` (plain glyph grid to a `FILE*`).

Input producers:

- **`obj.h`** — A minimal Wavefront `.obj` loader. Reads `v` vertices and `f`
  polygon faces, derives the unique edge set with per-edge face adjacency (via a
  hash table), and flags **crease/feature edges** from the dihedral angle between
  adjacent faces. `obj_normalize` centers and unit-scales the model. `ObjModel`
  packs all arrays into one arena (freed in a single `obj_free`).
  `obj_as_object3d` exposes it as the renderer's borrowed `Object3D` view.

- **`image.h`** — 2D image → structure-based ASCII, plus `stb_image.h`
  (`STB_IMAGE_IMPLEMENTATION` lives here). Loads and decodes an image, converts to
  grayscale, optionally Gaussian pre-smooths (to kill texture/dither noise on
  photos), runs a Sobel filter, and applies **non-maximum suppression** to thin
  edges to one pixel. `image_to_stream_e` fits the image to a `cols×rows` grid,
  builds a per-cell edge field, AISS-matches each cell to a glyph, and averages
  cell colour. It also returns the normalized objective **energy E** (mean match
  distance), which — because it is resolution-independent — enables automatic
  resolution selection. `keep_frac`/`magp` tune density and noise robustness
  (still-image defaults keep the whole outline; GIF/photo settings keep only the
  strongest contours).

- **`gifascii.h`** — Animated GIF → ASCII → animated GIF. Decodes every frame
  (`stbi_load_gif_from_memory`), runs each through the same AISS matcher as the
  still path, blits the matched glyph grid into an RGB frame with the embedded
  font, and appends it to an output GIF, preserving per-frame delays and looping.
  `run_gif_to_gif` is the driver.

- **`font8x16.h`** — An 8×16 bitmap font for printable ASCII 32–126 (generated
  from DejaVu Sans Mono by `tools/genfont.py`). Used to rasterize matched glyphs
  into GIF frames.

- **`gifout.h`** — A dependency-free animated GIF89a writer: LZW compression, a
  fixed 256-colour palette (6×6×6 web cube + grey ramp), nearest-colour
  quantization, and NETSCAPE2.0 looping. API: `gif_begin` / `gif_add_frame` /
  `gif_end`.

- **`stb_image.h`** — The public-domain stb single-file image loader (vendored,
  unmodified).

- **`render.h`** — The 3D render engine that ties it together. `RenderEngine`
  holds the mailbox, control state (spin speeds, pause), the object, and arena-
  backed scratch (camera-space verts, projected verts, per-face visibility, and
  the `Data3D` stream). Two threads:
  - `render_thread` — each frame: advance rotation by `dt`, transform + project
    vertices, compute face visibility, extract the **visible line structure**
    (boundary, silhouette, and crease edges only — smooth interior edges are
    dropped so a dense mesh reads as an outline) into the `Data3D` stream, call
    `render3D_structured`, and publish the frame to the mailbox.
  - `render_present` — drains the mailbox, builds run-length ANSI, writes it to
    the terminal.
  Control mutators (`render_add_spin`, `render_toggle_pause`, `render_reset_spin`)
  are called from `main.c`'s input thread under the engine's control mutex.

### 3D data flow

```
models/*.obj
  obj_load / obj_normalize        parse verts + faces, derive creased edges
  → Object3D (borrowed view)
render_thread (60 fps):
  rotate (mat4) + raster_project  transform, project vertices
  render_build_stream             emit visible edges as a Data3D stream
  render3D_structured             splat coverage → AISS-match cells (raster.h)
  mailbox_publish                 hand the frame to the present thread
render_present:
  raster_build_ansi → write()     run-length truecolor ANSI → terminal
```

### 2D / image data flow

```
image file (stb_image)
  image_load                      decode → gray → blur → Sobel → NMS-thinned edges
  image_to_stream_e               fit to grid, AISS-match each cell → Data2D stream
  render_char                     splat cells into the raster target (raster.h)
  → raster_build_ansi (live) | raster_write_text (.txt) | font blit + gifout (.gif)
```

### Design notes

- **Header-only.** Implementations live in `.h` files and reuse the shared
  `src2/` libraries (`Vec3f`, `Arena`, etc.). See the repo's memory note on this
  convention.
- **Data-oriented.** Producers never know about "objects" or "the screen"; they
  fill flat struct arrays. The rasterizer is the only place that owns pixels.
- **Global raster target on purpose.** There is exactly one render thread and one
  screen, so the render entry points keep clean `(array, size)` signatures with no
  context to thread through.
- **Arenas everywhere.** Per-frame and per-model scratch are single allocations,
  reset or destroyed as a unit — no per-object malloc churn.

See also the top-level `PLAN.md`, `RENDERER.md`, `STRUCTURE_BASED.md`, and
`FUNCTIONS.md` for the original design notes, and `papers/` for the source paper.
