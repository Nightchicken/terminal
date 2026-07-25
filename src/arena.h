#ifndef ARENA_H
#define ARENA_H
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef struct{
	size_t current;
	size_t capacity;
	unsigned char* data;
}Arena;

#define ARENA_DEFAULT_ALIGN (sizeof(void*) > 8 ? sizeof(void*) : 8)

Arena arena_create(size_t capacity){
	Arena a;
	a.current = 0;
	a.capacity = capacity;
	a.data = (unsigned char*)malloc(capacity);
	if (!a.data) a.capacity = 0;
	return a;
}

size_t arena_align_up(size_t n, size_t align){
	return (n + (align - 1)) & ~(align - 1);
}

void* arena_alloc_aligned(Arena* self, size_t size, size_t align){
	size_t offset = arena_align_up(self->current, align);
	if (offset + size > self->capacity) return NULL;
	void* ptr = self->data + offset;
	self->current = offset + size;
	return ptr;
}

void* arena_alloc(Arena* self, size_t size){
	return arena_alloc_aligned(self, size, ARENA_DEFAULT_ALIGN);
}

void* arena_calloc(Arena* self, size_t size){
	void* ptr = arena_alloc(self, size);
	if (ptr) memset(ptr, 0, size);
	return ptr;
}

void arena_reset(Arena* self){
	self->current = 0;
}

void arena_destroy(Arena* self){
	free(self->data);
	self->data = NULL;
	self->current = 0;
	self->capacity = 0;
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
