#ifndef RENDER_H
#define RENDER_H
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L   // clock_gettime / nanosleep under -std=c11
#endif

#include "vec.h"
#include "mat4.h"
#include "arena.h"
#include "raster.h"
#include "terminal.h"
#include "mailbox.h"

#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <math.h>

// The renderer. main.c supplies the object data (an Object3D); everything else
// -- rotation, structure extraction, projection, rasterization and drawing to
// the screen -- happens here. Two pieces run on separate threads, handing
// frames over through the engine's mailbox:
//
//   render_thread   - rotates the object, extracts its visible line structure
//                     into a Data3D stream, calls render3D, publishes the frame
//   render_present  - drains the mailbox, builds run-length ANSI, writes it
//
// `g_running` is the shared shutdown flag; it is a plain global so a signal
// handler in main.c can clear it. Every loop polls it.

static volatile sig_atomic_t g_running = 1;

// --- object data (supplied by main) ----------------------------------------
// A structure-based model: vertices, quad faces (for visibility), and edges
// tagged with their two adjacent faces. The renderer draws the edges that
// border a visible face.

typedef struct Object3D{
	const Vec3f* vertices;   int vertex_count;
	const int  (*edges)[4];  int edge_count;    // {vertA, vertB, faceL, faceR}; faceR = -1 if open
	const unsigned char* edge_crease;           // per-edge: 1 if a sharp/feature edge
	const int*  face_verts;                     // flattened face vertex indices
	const int*  face_begin;                     // face_count+1 offsets into face_verts
	int face_count;
	float distance;          // how far to push the object down +z
}Object3D;

// --- render engine ---------------------------------------------------------

typedef struct RenderEngine{
	RasterMailbox mailbox;
	pthread_mutex_t ctl;      // guards the control fields below
	float spin_x;             // rotation speed about x (rad/s)
	float spin_y;             // rotation speed about y (rad/s)
	int   paused;

	const Object3D* object;
	Arena  arena;             // backs the Data3D stream (see ../PLAN.md)
	Data3D* stream;
	int     capacity;

	Arena  scratch;           // backs cam/scr/front, freed in one call
	Vec3f* cam;               // scratch: camera-space vertices
	Vec2f* scr;               // scratch: projected vertices
	int*   front;             // scratch: per-face visibility
}RenderEngine;

static inline void render_engine_init(RenderEngine* e){
	mailbox_init(&e->mailbox);
	pthread_mutex_init(&e->ctl, NULL);
	e->spin_x = 0.7f;
	e->spin_y = 1.1f;
	e->paused = 0;
	e->object = NULL;
	e->capacity = 1 << 18;   // headroom for dense meshes (e.g. the teapot)
	e->arena = arena_create(sizeof(Data3D) * (size_t)e->capacity + 64);
	e->stream = (Data3D*)arena_alloc(&e->arena, sizeof(Data3D) * (size_t)e->capacity);
	if (!e->stream) e->capacity = 0;
	e->scratch = (Arena){0};
	e->cam = NULL;
	e->scr = NULL;
	e->front = NULL;
}

static inline void render_engine_set_object(RenderEngine* e, const Object3D* obj){
	e->object = obj;
	// cam/scr/front share one scratch arena, sized to this object and reset (so
	// a later set_object doesn't leak) rather than three separate reallocs.
	arena_destroy(&e->scratch);
	size_t sz = sizeof(Vec3f) * (size_t)obj->vertex_count
	          + sizeof(Vec2f) * (size_t)obj->vertex_count
	          + sizeof(int)   * (size_t)obj->face_count
	          + 3 * ARENA_DEFAULT_ALIGN;
	e->scratch = arena_create(sz);
	e->cam   = (Vec3f*)arena_alloc(&e->scratch, sizeof(Vec3f) * (size_t)obj->vertex_count);
	e->scr   = (Vec2f*)arena_alloc(&e->scratch, sizeof(Vec2f) * (size_t)obj->vertex_count);
	e->front = (int*)  arena_alloc(&e->scratch, sizeof(int)   * (size_t)obj->face_count);
}

static inline void render_engine_destroy(RenderEngine* e){
	mailbox_destroy(&e->mailbox);
	pthread_mutex_destroy(&e->ctl);
	arena_destroy(&e->arena);
	arena_destroy(&e->scratch);   // frees cam + scr + front together
}

// --- control mutators (called from the input thread) -----------------------

static inline void render_add_spin(RenderEngine* e, float dx, float dy){
	pthread_mutex_lock(&e->ctl);
	e->spin_x += dx;
	e->spin_y += dy;
	pthread_mutex_unlock(&e->ctl);
}

static inline void render_toggle_pause(RenderEngine* e){
	pthread_mutex_lock(&e->ctl);
	e->paused = !e->paused;
	pthread_mutex_unlock(&e->ctl);
}

static inline void render_reset_spin(RenderEngine* e){
	pthread_mutex_lock(&e->ctl);
	e->spin_x = 0.7f;
	e->spin_y = 1.1f;
	pthread_mutex_unlock(&e->ctl);
}

// --- structure extraction --------------------------------------------------

static inline Vec3f render_lerp(Vec3f a, Vec3f b, float t){
	return (Vec3f){a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

// Transform + project the object at rotation (ax, ay) and emit its visible line
// structure into the engine's Data3D stream. Returns the number of points.
static int render_build_stream(RenderEngine* e, int w, int h, float ax, float ay){
	const Object3D* obj = e->object;
	Data3D* out = e->stream;
	int cap = e->capacity, n = 0;

	Mat4 model = mat4_mul(mat4_translate(0, 0, obj->distance),
	                      mat4_mul(mat4_rotate_y(ay), mat4_rotate_x(ax)));

	for (int i = 0; i < obj->vertex_count; i++){
		e->cam[i] = mat4_xform_point(model, obj->vertices[i]);
		e->scr[i] = raster_project(e->cam[i], w, h, NULL);
	}

	// Face visibility: outward normal pointing back toward the camera. Works
	// for any convex polygon face (normal from the first 3 verts, centroid from
	// all of them); winding is irrelevant since the normal is oriented outward.
	Vec3f center = {0.0f, 0.0f, obj->distance};
	for (int f = 0; f < obj->face_count; f++){
		int b0 = obj->face_begin[f], b1 = obj->face_begin[f + 1], cnt = b1 - b0;
		Vec3f a = e->cam[obj->face_verts[b0]];
		Vec3f b = e->cam[obj->face_verts[b0 + 1]];
		Vec3f c = e->cam[obj->face_verts[b0 + 2]];
		Vec3f nrm = vec3f_cross(vec3f_sub(b, a), vec3f_sub(c, a));
		Vec3f fc = {0.0f, 0.0f, 0.0f};
		for (int k = b0; k < b1; k++) fc = vec3f_add(fc, e->cam[obj->face_verts[k]]);
		fc = vec3f_scale(fc, 1.0f / (float)cnt);
		if (vec3f_dot(nrm, vec3f_sub(fc, center)) < 0.0f) nrm = vec3f_scale(nrm, -1.0f);
		e->front[f] = vec3f_dot(nrm, fc) < 0.0f;
	}

	char* vertex_seen = (char*)calloc((size_t)obj->vertex_count, 1);
	for (int ei = 0; ei < obj->edge_count; ei++){
		const int* ed = obj->edges[ei];
		int fL = ed[2], fR = ed[3];

		// Feature-edge selection: a boundary edge whose face faces us, a
		// silhouette where a front and back face meet, or a visible sharp
		// crease. Smooth interior edges (both faces front, not a crease) are
		// dropped so a dense mesh reads as an outline, not a wire ball.
		int vis;
		if (fR < 0)                            vis = e->front[fL];
		else if (e->front[fL] != e->front[fR]) vis = 1;
		else if (e->front[fL])                 vis = obj->edge_crease[ei];
		else                                   vis = 0;
		if (!vis) continue;

		int a = ed[0], b = ed[1];
		vertex_seen[a] = vertex_seen[b] = 1;

		char glyph = raster_glyph_for_dir(e->scr[b].x - e->scr[a].x,
		                                  e->scr[b].y - e->scr[a].y);
		Vec3f dir = vec3f_sub(obj->vertices[b], obj->vertices[a]);
		Vec3f ad = {fabsf(dir.x), fabsf(dir.y), fabsf(dir.z)};
		float mx = fmaxf(ad.x, fmaxf(ad.y, ad.z));
		if (mx < 1e-6f) mx = 1.0f;
		Vec3f color = {0.25f + 0.75f * ad.x / mx,
		               0.25f + 0.75f * ad.y / mx,
		               0.25f + 0.75f * ad.z / mx};

		float dsx = e->scr[b].x - e->scr[a].x, dsy = e->scr[b].y - e->scr[a].y;
		int steps = (int)ceilf(fmaxf(fabsf(dsx), fabsf(dsy)));
		if (steps < 1) steps = 1;
		for (int i = 0; i <= steps && n < cap; i++){
			out[n].position = render_lerp(e->cam[a], e->cam[b], (float)i / (float)steps);
			out[n].color = color;
			out[n].c = glyph;
			n++;
		}
	}

	// Corner markers only for low-poly models (e.g. the cube): on a dense mesh
	// a '+' at every feature vertex would bury the line glyphs. Drawn last so
	// '+' wins the depth ties against the edges.
	if (obj->vertex_count <= 64){
		for (int i = 0; i < obj->vertex_count && n < cap; i++){
			if (!vertex_seen[i]) continue;
			out[n].position = e->cam[i];
			out[n].color = (Vec3f){1.f, 1.f, 1.f};
			out[n].c = '+';
			n++;
		}
	}
	free(vertex_seen);

	return n;
}

// --- timing helpers --------------------------------------------------------

static inline double render_now(void){
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static inline void render_sleep(double s){
	if (s <= 0.0) return;
	struct timespec ts;
	ts.tv_sec  = (time_t)s;
	ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
	nanosleep(&ts, NULL);
}

// --- render thread ---------------------------------------------------------

static void* render_thread(void* arg){
	RenderEngine* e = (RenderEngine*)arg;
	float ax = 0.0f, ay = 0.0f;
	double prev = render_now();
	const double frame_target = 1.0 / 60.0;

	while (g_running){
		double start = render_now();
		double dt = start - prev;
		prev = start;

		float sx, sy;
		int paused;
		pthread_mutex_lock(&e->ctl);
		sx = e->spin_x; sy = e->spin_y; paused = e->paused;
		pthread_mutex_unlock(&e->ctl);

		if (!paused){
			ax += sx * (float)dt;
			ay += sy * (float)dt;
		}

		int w, h;
		if (term_size(&w, &h) != 0){ w = 80; h = 24; }
		if (w < 1) w = 1;
		if (h < 1) h = 1;

		raster_begin(w, h);                              // clear the target
		int n = render_build_stream(e, w, h, ax, ay);    // object -> Data3D stream
		raster_data rd = render3D_structured(e->stream, n);  // stream -> AISS-matched raster_data
		mailbox_publish(&e->mailbox, &rd);

		render_sleep(frame_target - (render_now() - start));
	}

	mailbox_close(&e->mailbox);   // wake the present loop so it can exit
	return NULL;
}

// --- present loop (runs on the caller's thread) ----------------------------

static void render_present(RenderEngine* e){
	raster_data frame = {NULL, 0};
	int frame_cap = 0;
	AnsiBuf ansi = {NULL, 0, 0};

	while (mailbox_consume(&e->mailbox, &frame, &frame_cap)){
		raster_build_ansi(&frame, &ansi);
		ssize_t off = 0;
		while (off < (ssize_t)ansi.len){
			ssize_t wr = write(STDOUT_FILENO, ansi.data + off, ansi.len - (size_t)off);
			if (wr <= 0) break;
			off += wr;
		}
	}

	free(frame.cells);
	free(ansi.data);
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
