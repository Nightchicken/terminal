#ifndef RASTER_H
#define RASTER_H
#include "vec.h"
#include "aiss.h"
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Data-oriented rasterizer (see ../PLAN.md).
//
// Streams of {position, color, char} structs flow through three render entry
// points into a single global raster target, which is then read out as a
// `raster_data` stream (cells ordered row-major, so horizontal neighbours are
// adjacent) and emitted by a run-length ANSI printer. Nothing here knows about
// "objects": producers fill flat arrays, the renders transform them, the
// printer walks the result.
//
//   render_char : screen-space 2D coords + glyph      -> raster_data
//   render2D    : object-space 2D coords              -> raster_data
//   render3D    : camera-space 3D coords (projected)  -> raster_data

// --- stream element structs (the data) -------------------------------------

typedef struct Data2D{
	Vec2f position;   // meaning depends on the render entry point (see above)
	Vec3f color;      // linear RGB in [0,1]
	char  c;
}Data2D;

typedef struct Data3D{
	Vec3f position;   // camera space: camera at origin, looking down +z
	Vec3f color;
	char  c;
}Data3D;

// One occupied screen cell in the output stream.
typedef struct RasterCell{
	short x, y;
	Vec3f color;
	char  c;
}RasterCell;

// The output stream: a contiguous run of cells and how many are live.
typedef struct raster_data{
	RasterCell* cells;
	int count;
}raster_data;

// --- the global raster target ----------------------------------------------
// A dense grid the renders splat into, plus the scratch the readout fills. It
// is process-global on purpose: there is one render thread and one screen, so
// the render entry points keep the plan's (array, size) signatures with no
// context argument to thread through.

typedef struct RasterTarget{
	int w, h, cap;
	char*  gc;    // glyph per cell, 0 = empty
	Vec3f* gcol;  // colour per cell
	float* gz;    // depth per cell (smaller = nearer)
	RasterCell* out;
	int out_cap;
}RasterTarget;

static RasterTarget g_raster;

// Shared AISS glyph codebook, built once on first use (see aiss.h).
static AissBook g_aiss_book;
static int      g_aiss_ready = 0;

static inline const AissBook* aiss_book_ensure(void){
	if (!g_aiss_ready){ aiss_book_init(&g_aiss_book); g_aiss_ready = 1; }
	return &g_aiss_book;
}

// Resize (grow-only) and clear the target for a new frame.
static void raster_begin(int w, int h){
	int need = w * h;
	if (need > g_raster.cap){
		g_raster.gc   = (char*) realloc(g_raster.gc,   (size_t)need);
		g_raster.gcol = (Vec3f*)realloc(g_raster.gcol, sizeof(Vec3f) * (size_t)need);
		g_raster.gz   = (float*)realloc(g_raster.gz,   sizeof(float) * (size_t)need);
		g_raster.cap  = need;
	}
	g_raster.w = w;
	g_raster.h = h;
	memset(g_raster.gc, 0, (size_t)need);
	for (int i = 0; i < need; i++) g_raster.gz[i] = FLT_MAX;
}

// Depth-tested splat of one cell; the shared core of every render entry point.
static inline void raster_put(int x, int y, Vec3f color, char c, float z){
	if (c == 0 || x < 0 || y < 0 || x >= g_raster.w || y >= g_raster.h) return;
	int i = y * g_raster.w + x;
	if (z <= g_raster.gz[i]){
		g_raster.gz[i]  = z;
		g_raster.gc[i]  = c;
		g_raster.gcol[i] = color;
	}
}

// Read the target out as a row-major stream of the occupied cells.
static raster_data raster_extract(void){
	int need = g_raster.w * g_raster.h;
	if (need > g_raster.out_cap){
		g_raster.out = (RasterCell*)realloc(g_raster.out, sizeof(RasterCell) * (size_t)need);
		g_raster.out_cap = need;
	}
	int k = 0;
	for (int y = 0; y < g_raster.h; y++){
		for (int x = 0; x < g_raster.w; x++){
			int i = y * g_raster.w + x;
			if (g_raster.gc[i]){
				g_raster.out[k].x = (short)x;
				g_raster.out[k].y = (short)y;
				g_raster.out[k].color = g_raster.gcol[i];
				g_raster.out[k].c = g_raster.gc[i];
				k++;
			}
		}
	}
	raster_data rd = {g_raster.out, k};
	return rd;
}

// Perspective projection shared by render3D and any 3D producer, so a producer
// can predict where a point lands. Horizontal scale is doubled because terminal
// cells are about twice as tall as they are wide.
static inline Vec2f raster_project(Vec3f p, int w, int h, float* out_z){
	float s = fminf(0.85f * (float)h, 0.42f * (float)w);
	float invz = 1.0f / p.z;
	Vec2f r;
	r.x = (float)w * 0.5f + p.x * invz * (s * 2.0f);
	r.y = (float)h * 0.5f - p.y * invz * s;
	if (out_z) *out_z = p.z;
	return r;
}

#define RASTER_PI 3.14159265358979323846f

// Line glyph whose shape matches a screen-space direction (y grows downward).
// Shared by the 3D edge renderer and the 2D image edge renderer.
static inline char raster_glyph_for_dir(float dx, float dy){
	float ang = atan2f(-dy, dx);
	if (ang < 0.0f) ang += RASTER_PI;
	float deg = ang * 180.0f / RASTER_PI;
	if (deg < 22.5f || deg >= 157.5f) return '-';
	if (deg < 67.5f)                  return '/';
	if (deg < 112.5f)                 return '|';
	return '\\';
}

// --- the three render entry points -----------------------------------------

// Lowest level: 2D coordinates that are already screen cells, plus a glyph.
static inline raster_data render_char(Data2D* array, int size){
	for (int i = 0; i < size; i++)
		raster_put((int)lroundf(array[i].position.x),
		           (int)lroundf(array[i].position.y),
		           array[i].color, array[i].c, 0.0f);
	return raster_extract();
}

// 2D object coordinates in normalized [-1,1] (y up), fitted to the screen.
static inline raster_data render2D(Data2D* array, int size){
	float cx = (float)g_raster.w * 0.5f, cy = (float)g_raster.h * 0.5f;
	float s = 0.9f * fminf(cy, cx * 0.5f);
	for (int i = 0; i < size; i++){
		int x = (int)lroundf(cx + array[i].position.x * s * 2.0f);
		int y = (int)lroundf(cy - array[i].position.y * s);
		raster_put(x, y, array[i].color, array[i].c, 0.0f);
	}
	return raster_extract();
}

// 3D camera-space coordinates: project, depth-test, splat.
static inline raster_data render3D(Data3D* array, int size){
	for (int i = 0; i < size; i++){
		if (array[i].position.z <= 0.0f) continue;   // behind the camera
		float z;
		Vec2f sp = raster_project(array[i].position, g_raster.w, g_raster.h, &z);
		raster_put((int)lroundf(sp.x), (int)lroundf(sp.y), array[i].color, array[i].c, z);
	}
	return raster_extract();
}

// --- structure-based 3D path (AISS per-cell matching) ----------------------
// Instead of choosing a glyph per projected point from its edge direction, the
// projected line structure is splatted into a sub-cell coverage buffer; then
// each occupied character cell has its local coverage matched to the codebook
// glyph whose shape follows it (aiss.h). This is the paper's grid-cell matching
// applied to the 3D renderer's screen-space line structure.

#define RASTER_CSX 4   // sub-cells per character cell, horizontal
#define RASTER_CSY 8   // sub-cells per character cell, vertical

static float* g_cov = NULL;             // sub-cell ink coverage
static int    g_cov_cap = 0, g_cov_w = 0, g_cov_h = 0;

// Clear the coverage buffer (grow-only) for a new structured frame. The caller
// still calls raster_begin(): gc/gz/gcol are reused as a per-cell occupancy +
// depth + colour buffer, coverage is the extra sub-cell shape channel.
static void raster_cov_begin(int w, int h){
	int need = (w * RASTER_CSX) * (h * RASTER_CSY);
	if (need > g_cov_cap){
		g_cov = (float*)realloc(g_cov, sizeof(float) * (size_t)need);
		g_cov_cap = need;
	}
	g_cov_w = w * RASTER_CSX;
	g_cov_h = h * RASTER_CSY;
	memset(g_cov, 0, sizeof(float) * (size_t)need);
}

// Splat one projected point: deposit sub-cell coverage (bilinear) and, at the
// character cell it lands in, keep the nearest colour + mark it occupied.
static inline void raster_cov_splat(float fx, float fy, Vec3f color, float z){
	int cx = (int)fx, cy = (int)fy;
	if (cx >= 0 && cy >= 0 && cx < g_raster.w && cy < g_raster.h){
		int i = cy * g_raster.w + cx;
		if (z <= g_raster.gz[i]){
			g_raster.gz[i]   = z;
			g_raster.gcol[i] = color;
			g_raster.gc[i]   = 1;            // occupancy marker, resolved later
		}
	}
	float sx = fx * RASTER_CSX, sy = fy * RASTER_CSY;
	int x0 = (int)floorf(sx - 0.5f), y0 = (int)floorf(sy - 0.5f);
	for (int yy = y0; yy <= y0 + 1; yy++){
		if (yy < 0 || yy >= g_cov_h) continue;
		float wy = 1.0f - fabsf(sy - ((float)yy + 0.5f));
		if (wy <= 0.0f) continue;
		for (int xx = x0; xx <= x0 + 1; xx++){
			if (xx < 0 || xx >= g_cov_w) continue;
			float wx = 1.0f - fabsf(sx - ((float)xx + 0.5f));
			if (wx <= 0.0f) continue;
			g_cov[yy * g_cov_w + xx] += wx * wy;
		}
	}
}

// Ink sampler over one character cell's coverage, in cell-normalized (u,v).
// u,v may reach outside [0,1]; the read clamps into the neighbouring cells'
// sub-cells, which is exactly the cross-cell context AISS tolerates.
typedef struct CovInkCtx{ int cellx, celly; }CovInkCtx;

static float raster_cov_ink(void* ctx, float u, float v){
	CovInkCtx* c = (CovInkCtx*)ctx;
	float sx = (float)(c->cellx * RASTER_CSX) + u * RASTER_CSX;
	float sy = (float)(c->celly * RASTER_CSY) + v * RASTER_CSY;
	int x = (int)floorf(sx), y = (int)floorf(sy);
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x >= g_cov_w) x = g_cov_w - 1;
	if (y >= g_cov_h) y = g_cov_h - 1;
	float g = g_cov[y * g_cov_w + x];
	return g < 1.0f ? g : 1.0f;
}

// Resolve every occupied cell to its best-matched glyph, then read out.
static raster_data raster_cov_resolve(const AissBook* book){
	for (int y = 0; y < g_raster.h; y++){
		for (int x = 0; x < g_raster.w; x++){
			int i = y * g_raster.w + x;
			if (!g_raster.gc[i]) continue;         // no coverage here
			CovInkCtx ctx = { x, y };
			AissFeature f;
			aiss_build(&f, raster_cov_ink, &ctx);
			if (f.mass < 0.75f){ g_raster.gc[i] = 0; continue; }  // too faint
			g_raster.gc[i] = aiss_match_fast(book, &f);
		}
	}
	return raster_extract();
}

// Structure-based 3D entry point: project + splat coverage, then AISS-match.
static raster_data render3D_structured(Data3D* array, int size){
	const AissBook* book = aiss_book_ensure();
	raster_cov_begin(g_raster.w, g_raster.h);
	for (int i = 0; i < size; i++){
		if (array[i].position.z <= 0.0f) continue;   // behind the camera
		float z;
		Vec2f sp = raster_project(array[i].position, g_raster.w, g_raster.h, &z);
		raster_cov_splat(sp.x, sp.y, array[i].color, z);
	}
	return raster_cov_resolve(book);
}

// --- run-length ANSI printer -----------------------------------------------
// Emits one cursor-position escape per run start, one truecolor escape per
// colour change, then the run of glyphs. The cells stream is row-major, so a
// run is simply a maximal span of same-row cells with contiguous x.

typedef struct AnsiBuf{
	char* data;
	size_t len, cap;
}AnsiBuf;

static void ansi_need(AnsiBuf* b, size_t extra){
	if (b->len + extra + 1 > b->cap){
		size_t cap = b->cap ? b->cap * 2 : 4096;
		while (cap < b->len + extra + 1) cap *= 2;
		b->data = (char*)realloc(b->data, cap);
		b->cap = cap;
	}
}

static inline void ansi_puts(AnsiBuf* b, const char* s){
	size_t n = strlen(s);
	ansi_need(b, n);
	memcpy(b->data + b->len, s, n);
	b->len += n;
}

static inline void ansi_putc(AnsiBuf* b, char c){
	ansi_need(b, 1);
	b->data[b->len++] = c;
}

static inline int color_to_255(float v){
	int i = (int)(v * 255.0f + 0.5f);
	return i < 0 ? 0 : (i > 255 ? 255 : i);
}

// Build the ANSI byte stream for one frame into `out` (len reset, then filled).
static void raster_build_ansi(const raster_data* rd, AnsiBuf* out){
	char seq[48];
	out->len = 0;
	ansi_puts(out, "\x1b[2J");   // clear so vacated cells don't ghost

	int prev_x = -2, prev_y = -2;
	int pr = -1, pg = -1, pb = -1;   // last colour emitted, -1 = none yet

	for (int i = 0; i < rd->count; i++){
		const RasterCell* c = &rd->cells[i];
		if (c->y != prev_y || c->x != prev_x + 1){
			snprintf(seq, sizeof(seq), "\x1b[%d;%dH", c->y + 1, c->x + 1);
			ansi_puts(out, seq);
		}
		int r = color_to_255(c->color.x);
		int g = color_to_255(c->color.y);
		int b = color_to_255(c->color.z);
		if (r != pr || g != pg || b != pb){
			snprintf(seq, sizeof(seq), "\x1b[38;2;%d;%d;%dm", r, g, b);
			ansi_puts(out, seq);
			pr = r; pg = g; pb = b;
		}
		ansi_putc(out, c->c);
		prev_x = c->x;
		prev_y = c->y;
	}
	ansi_puts(out, "\x1b[0m");
	out->data[out->len] = '\0';
}

// --- plain-text writer -----------------------------------------------------
// Write the raster as a plain glyph grid (no colour): one line per row, spaces
// for empty cells, trailing blanks trimmed. `rd->cells` is row-major with x
// ascending within a row (from raster_extract), so a single forward walk fills
// each line. Suitable for saving the ASCII image straight to a .txt file.
static void raster_write_text(const raster_data* rd, int w, int h, FILE* f){
	(void)w;
	int idx = 0;
	for (int y = 0; y < h; y++){
		int last = -1;
		while (idx < rd->count && rd->cells[idx].y == y){
			int x = rd->cells[idx].x;
			for (int s = last + 1; s < x; s++) fputc(' ', f);
			fputc(rd->cells[idx].c, f);
			last = x;
			idx++;
		}
		fputc('\n', f);
	}
}

#endif

/*
 * MIT License
 * Copyright (c) 2026 Matthew Kabat
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
