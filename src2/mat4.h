#ifndef MAT4_H
#define MAT4_H
#include "vec.h"
#include <math.h>

// Column-major 4x4 matrix math, built on top of vec.h's Vec3f. Header-only.
// Element at (row, col) lives at m[col*4 + row], matching OpenGL convention.

typedef struct Mat4{
	float m[16];
}Mat4;

static inline Mat4 mat4_identity(void){
	Mat4 r = {{0}};
	r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
	return r;
}

// r = a * b (apply b first, then a).
static inline Mat4 mat4_mul(Mat4 a, Mat4 b){
	Mat4 r;
	for (int col = 0; col < 4; col++){
		for (int row = 0; row < 4; row++){
			float sum = 0.0f;
			for (int k = 0; k < 4; k++)
				sum += a.m[k * 4 + row] * b.m[col * 4 + k];
			r.m[col * 4 + row] = sum;
		}
	}
	return r;
}

static inline Mat4 mat4_translate(float x, float y, float z){
	Mat4 r = mat4_identity();
	r.m[12] = x; r.m[13] = y; r.m[14] = z;
	return r;
}

static inline Mat4 mat4_rotate_x(float rad){
	float c = cosf(rad), s = sinf(rad);
	Mat4 r = mat4_identity();
	r.m[5] = c;  r.m[6] = s;
	r.m[9] = -s; r.m[10] = c;
	return r;
}

static inline Mat4 mat4_rotate_y(float rad){
	float c = cosf(rad), s = sinf(rad);
	Mat4 r = mat4_identity();
	r.m[0] = c;  r.m[2] = -s;
	r.m[8] = s;  r.m[10] = c;
	return r;
}

static inline Mat4 mat4_rotate_z(float rad){
	float c = cosf(rad), s = sinf(rad);
	Mat4 r = mat4_identity();
	r.m[0] = c;  r.m[1] = s;
	r.m[4] = -s; r.m[5] = c;
	return r;
}

// Transform a position (implicit w = 1): applies rotation and translation.
static inline Vec3f mat4_xform_point(Mat4 a, Vec3f v){
	return (Vec3f){
		a.m[0] * v.x + a.m[4] * v.y + a.m[8]  * v.z + a.m[12],
		a.m[1] * v.x + a.m[5] * v.y + a.m[9]  * v.z + a.m[13],
		a.m[2] * v.x + a.m[6] * v.y + a.m[10] * v.z + a.m[14],
	};
}

// Transform a direction (implicit w = 0): rotation/scale only, no translation.
static inline Vec3f mat4_xform_dir(Mat4 a, Vec3f v){
	return (Vec3f){
		a.m[0] * v.x + a.m[4] * v.y + a.m[8]  * v.z,
		a.m[1] * v.x + a.m[5] * v.y + a.m[9]  * v.z,
		a.m[2] * v.x + a.m[6] * v.y + a.m[10] * v.z,
	};
}

static inline Vec3f vec3f_normalize(Vec3f a){
	float len = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
	if (len < 1e-8f) return (Vec3f){0.0f, 0.0f, 0.0f};
	float inv = 1.0f / len;
	return (Vec3f){a.x * inv, a.y * inv, a.z * inv};
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
