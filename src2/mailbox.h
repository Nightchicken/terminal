#ifndef MAILBOX_H
#define MAILBOX_H
#include "raster.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

// Thread-safe single-slot handoff of a raster_data stream from the render
// thread to the present thread. Capacity 1: the producer blocks until the
// consumer has taken the previous frame, which paces rendering to the terminal.
// Both sides copy the cell array through the slot, so neither holds the lock
// during CPU work or terminal I/O.

typedef struct RasterMailbox{
	RasterCell* cells;   // owned copy of the published frame
	int count;
	int cap;
	int full;
	int closed;
	pthread_mutex_t lock;
	pthread_cond_t can_produce;
	pthread_cond_t can_consume;
}RasterMailbox;

static inline void mailbox_init(RasterMailbox* m){
	m->cells = NULL;
	m->count = m->cap = 0;
	m->full = m->closed = 0;
	pthread_mutex_init(&m->lock, NULL);
	pthread_cond_init(&m->can_produce, NULL);
	pthread_cond_init(&m->can_consume, NULL);
}

static inline void mailbox_destroy(RasterMailbox* m){
	free(m->cells);
	pthread_mutex_destroy(&m->lock);
	pthread_cond_destroy(&m->can_produce);
	pthread_cond_destroy(&m->can_consume);
}

static inline void mailbox_close(RasterMailbox* m){
	pthread_mutex_lock(&m->lock);
	m->closed = 1;
	pthread_cond_broadcast(&m->can_produce);
	pthread_cond_broadcast(&m->can_consume);
	pthread_mutex_unlock(&m->lock);
}

// Producer: block until the slot is free, then copy the stream in.
static inline void mailbox_publish(RasterMailbox* m, const raster_data* rd){
	pthread_mutex_lock(&m->lock);
	while (m->full && !m->closed)
		pthread_cond_wait(&m->can_produce, &m->lock);
	if (m->closed){ pthread_mutex_unlock(&m->lock); return; }
	if (rd->count > m->cap){
		m->cells = (RasterCell*)realloc(m->cells, sizeof(RasterCell) * (size_t)rd->count);
		m->cap = rd->count;
	}
	memcpy(m->cells, rd->cells, sizeof(RasterCell) * (size_t)rd->count);
	m->count = rd->count;
	m->full = 1;
	pthread_cond_signal(&m->can_consume);
	pthread_mutex_unlock(&m->lock);
}

// Consumer: block until a frame is available, then copy it into `dst` (grown as
// needed). Returns 1 on success, 0 once the mailbox is closed and drained.
static inline int mailbox_consume(RasterMailbox* m, raster_data* dst, int* dst_cap){
	pthread_mutex_lock(&m->lock);
	while (!m->full && !m->closed)
		pthread_cond_wait(&m->can_consume, &m->lock);
	if (!m->full && m->closed){ pthread_mutex_unlock(&m->lock); return 0; }
	if (m->count > *dst_cap){
		dst->cells = (RasterCell*)realloc(dst->cells, sizeof(RasterCell) * (size_t)m->count);
		*dst_cap = m->count;
	}
	memcpy(dst->cells, m->cells, sizeof(RasterCell) * (size_t)m->count);
	dst->count = m->count;
	m->full = 0;
	pthread_cond_signal(&m->can_produce);
	pthread_mutex_unlock(&m->lock);
	return 1;
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
