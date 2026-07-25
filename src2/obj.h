#ifndef OBJ_H
#define OBJ_H
#include "vec.h"
#include "render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Minimal Wavefront .obj loader. Reads `v` vertices and `f` polygon faces
// (triangles, quads or larger; texture/normal indices like `a/b/c` are ignored)
// and derives the unique edge set with per-edge face adjacency, which is what
// the structure-based renderer needs. Enough for hand-authored models like a
// cube; not a full spec implementation.

// All of a model's arrays live in one Arena, so the whole thing is released
// with a single arena_destroy(). The pointers below just index into it.
typedef struct ObjModel{
	Arena arena;
	Vec3f* vertices;   int vertex_count;
	int  (*edges)[4];  int edge_count;    // {a, b, faceL, faceR}; faceR = -1 if open
	unsigned char* edge_crease;           // per-edge: 1 if a sharp/feature edge
	int*   face_verts; int face_vert_count;
	int*   face_begin; int face_count;    // face_begin has face_count+1 entries
}ObjModel;

// Two faces sharing an edge are a "crease" when their planes differ by more than
// this angle; the test uses |dot(n1,n2)| so face winding does not matter.
#define OBJ_CREASE_COS 0.88f   // ~28 degrees

// Grow a malloc'd array to hold at least `need` elements of `elem` bytes.
static void* obj_grow(void* p, int* cap, int need, size_t elem){
	if (need <= *cap) return p;
	int c = *cap ? *cap : 16;
	while (c < need) c *= 2;
	*cap = c;
	return realloc(p, (size_t)c * elem);
}

static uint32_t obj_edge_hash(int lo, int hi){
	uint32_t h = (uint32_t)lo * 73856093u ^ (uint32_t)hi * 19349663u;
	return h ^ (h >> 16);
}

// Insert edge (a,b)->face f into a hash-indexed edge set (open addressing with
// linear probing). `E` is preallocated to an upper bound, `table` maps a slot
// to an edge index (-1 = empty), `mask` = table size - 1 (a power of two).
static void obj_edge_add(int (*E)[4], int* ec, int* table, int mask, int a, int b, int f){
	int lo = a < b ? a : b, hi = a < b ? b : a;
	uint32_t slot = obj_edge_hash(lo, hi) & (uint32_t)mask;
	while (table[slot] != -1){
		int idx = table[slot];
		if (E[idx][0] == lo && E[idx][1] == hi){
			if (E[idx][3] < 0) E[idx][3] = f;
			return;
		}
		slot = (slot + 1) & (uint32_t)mask;
	}
	E[*ec][0] = lo; E[*ec][1] = hi; E[*ec][2] = f; E[*ec][3] = -1;
	table[slot] = *ec;
	(*ec)++;
}

// Load `path` into `m`. Returns 0 on success, -1 on failure.
static int obj_load(const char* path, ObjModel* m){
	FILE* fp = fopen(path, "r");
	if (!fp) return -1;

	Vec3f* V = NULL; int vc = 0, vcap = 0;
	int* FV = NULL;  int fvc = 0, fvcap = 0;
	int* FB = NULL;  int fc = 0, fbcap = 0;
	FB = (int*)obj_grow(FB, &fbcap, 1, sizeof(int)); FB[0] = 0;

	char line[1024];
	while (fgets(line, sizeof(line), fp)){
		if (line[0] == 'v' && line[1] == ' '){
			Vec3f p;
			if (sscanf(line + 2, "%f %f %f", &p.x, &p.y, &p.z) == 3){
				V = (Vec3f*)obj_grow(V, &vcap, vc + 1, sizeof(Vec3f));
				V[vc++] = p;
			}
		}else if (line[0] == 'f' && line[1] == ' '){
			char* tok = strtok(line + 2, " \t\r\n");
			while (tok){
				int idx = atoi(tok);   // leading integer of "i", "i/j", "i//k"...
				if (idx != 0){
					int zero = idx > 0 ? idx - 1 : vc + idx;  // 1-based / negative
					FV = (int*)obj_grow(FV, &fvcap, fvc + 1, sizeof(int));
					FV[fvc++] = zero;
				}
				tok = strtok(NULL, " \t\r\n");
			}
			FB = (int*)obj_grow(FB, &fbcap, fc + 2, sizeof(int));
			FB[++fc] = fvc;
		}
	}
	fclose(fp);

	// Derive edges from each face's polygon boundary, deduplicated with a hash
	// table so this stays ~O(total face corners) even on large meshes. Unique
	// edges can't exceed the face-corner count (fvc), so E needs no growth and
	// the table (>= 2x that, power of two) never fills.
	int ecap = fvc > 0 ? fvc : 1;
	int (*E)[4] = (int(*)[4])malloc(sizeof(int[4]) * (size_t)ecap);
	int tsize = 8;
	while (tsize < ecap * 2) tsize <<= 1;
	int* table = (int*)malloc(sizeof(int) * (size_t)tsize);
	for (int i = 0; i < tsize; i++) table[i] = -1;
	int ec = 0;
	for (int f = 0; f < fc; f++){
		int b0 = FB[f], b1 = FB[f + 1], cnt = b1 - b0;
		for (int k = 0; k < cnt; k++)
			obj_edge_add(E, &ec, table, tsize - 1, FV[b0 + k], FV[b0 + (k + 1) % cnt], f);
	}
	free(table);

	if (vc == 0 || fc == 0){
		free(V); free(FV); free(FB); free(E);
		return -1;
	}

	// Flag sharp/feature edges from the dihedral angle between adjacent faces.
	// |dot| of the two (unoriented) geometric normals is ~1 when coplanar and
	// smaller as the fold sharpens; boundary edges are always features.
	size_t nf = fc > 0 ? (size_t)fc : 0;
	Vec3f* fn = (Vec3f*)malloc(sizeof(Vec3f) * nf);
	for (int f = 0; f < fc; f++){
		int b0 = FB[f];
		Vec3f a = V[FV[b0]], b = V[FV[b0 + 1]], c = V[FV[b0 + 2]];
		fn[f] = vec3f_normalize(vec3f_cross(vec3f_sub(b, a), vec3f_sub(c, a)));
	}
	unsigned char* crease = (unsigned char*)malloc((size_t)ec);
	for (int i = 0; i < ec; i++){
		int fL = E[i][2], fR = E[i][3];
		if (fR < 0){ crease[i] = 1; continue; }
		float d = fabsf(vec3f_dot(fn[fL], fn[fR]));
		crease[i] = d < OBJ_CREASE_COS ? 1 : 0;
	}
	free(fn);

	// Pack the final arrays into a single arena so obj_free() is one call. The
	// temporary parse buffers above are released right after the copy.
	size_t cap = sizeof(Vec3f) * (size_t)vc
	           + sizeof(int[4]) * (size_t)ec
	           + (size_t)ec
	           + sizeof(int) * (size_t)fvc
	           + sizeof(int) * (size_t)(fc + 1)
	           + 6 * ARENA_DEFAULT_ALIGN;
	Arena ar = arena_create(cap);
	Vec3f*        AV = (Vec3f*)       arena_alloc(&ar, sizeof(Vec3f) * (size_t)vc);
	int         (*AE)[4] = (int(*)[4])arena_alloc(&ar, sizeof(int[4]) * (size_t)ec);
	unsigned char* AC = (unsigned char*)arena_alloc(&ar, (size_t)ec);
	int*          AFV = (int*)        arena_alloc(&ar, sizeof(int) * (size_t)fvc);
	int*          AFB = (int*)        arena_alloc(&ar, sizeof(int) * (size_t)(fc + 1));
	if (!AV || !AE || !AC || !AFV || !AFB){
		arena_destroy(&ar);
		free(V); free(FV); free(FB); free(E); free(crease);
		return -1;
	}
	memcpy(AV,  V,      sizeof(Vec3f) * (size_t)vc);
	memcpy(AE,  E,      sizeof(int[4]) * (size_t)ec);
	memcpy(AC,  crease, (size_t)ec);
	memcpy(AFV, FV,     sizeof(int) * (size_t)fvc);
	memcpy(AFB, FB,     sizeof(int) * (size_t)(fc + 1));
	free(V); free(FV); free(FB); free(E); free(crease);

	m->arena = ar;
	m->vertices = AV;        m->vertex_count = vc;
	m->edges = AE;           m->edge_count = ec;
	m->edge_crease = AC;
	m->face_verts = AFV;     m->face_vert_count = fvc;
	m->face_begin = AFB;     m->face_count = fc;
	return 0;
}

// Center the model on the origin and scale so its largest half-extent is 1, so
// any model sits where the renderer expects a roughly unit object. A model
// already in [-1,1] (like the cube) is left effectively unchanged.
static void obj_normalize(ObjModel* m){
	if (m->vertex_count <= 0) return;
	Vec3f mn = m->vertices[0], mx = m->vertices[0];
	for (int i = 1; i < m->vertex_count; i++){
		Vec3f v = m->vertices[i];
		mn.x = fminf(mn.x, v.x); mn.y = fminf(mn.y, v.y); mn.z = fminf(mn.z, v.z);
		mx.x = fmaxf(mx.x, v.x); mx.y = fmaxf(mx.y, v.y); mx.z = fmaxf(mx.z, v.z);
	}
	Vec3f c = {(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f};
	float half = fmaxf((mx.x - mn.x), fmaxf((mx.y - mn.y), (mx.z - mn.z))) * 0.5f;
	if (half <= 0.0f) return;
	float s = 1.0f / half;
	for (int i = 0; i < m->vertex_count; i++){
		m->vertices[i].x = (m->vertices[i].x - c.x) * s;
		m->vertices[i].y = (m->vertices[i].y - c.y) * s;
		m->vertices[i].z = (m->vertices[i].z - c.z) * s;
	}
}

static void obj_free(ObjModel* m){
	arena_destroy(&m->arena);   // releases every array at once
	memset(m, 0, sizeof(*m));
}

// A borrowed view of the model as an Object3D (points into the ObjModel).
static Object3D obj_as_object3d(const ObjModel* m, float distance){
	Object3D o;
	o.vertices = m->vertices;    o.vertex_count = m->vertex_count;
	o.edges = (const int(*)[4])m->edges;  o.edge_count = m->edge_count;
	o.edge_crease = m->edge_crease;
	o.face_verts = m->face_verts;
	o.face_begin = m->face_begin;
	o.face_count = m->face_count;
	o.distance = distance;
	return o;
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
