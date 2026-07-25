#ifndef GIFASCII_H
#define GIFASCII_H
#include "image.h"
#include "raster.h"
#include "gifout.h"
#include "font8x16.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Animated GIF -> structure-based ASCII -> animated GIF.
//
// Each input frame is run through the same AISS structure matcher as the still
// image path (image.h); the resulting grid of coloured glyphs is drawn into an
// RGB frame with the embedded 8x16 font (font8x16.h) and appended to an output
// GIF (gifout.h). Per-frame delays and looping are preserved.

// Draw one matched ASCII frame (raster cells at cols x rows) into an RGB image
// of (cols*FONT_W) x (rows*FONT_H) pixels on a solid `bg` grey background.
static void ascii_cells_to_rgb(const raster_data* rd, int cols, int rows,
                               unsigned char* rgb, int bg){
	int W = cols * FONT_W, H = rows * FONT_H;
	memset(rgb, bg, (size_t)W * H * 3);
	for (int k = 0; k < rd->count; k++){
		const RasterCell* c = &rd->cells[k];
		if (c->c < FONT_FIRST || c->c > FONT_LAST) continue;
		const unsigned char* gl = font8x16[c->c - FONT_FIRST];
		int r = color_to_255(c->color.x);
		int g = color_to_255(c->color.y);
		int b = color_to_255(c->color.z);
		int px0 = c->x * FONT_W, py0 = c->y * FONT_H;
		for (int fy = 0; fy < FONT_H; fy++){
			unsigned char bits = gl[fy];
			if (!bits) continue;
			int yy = py0 + fy;
			for (int fx = 0; fx < FONT_W; fx++){
				if (!(bits & (0x80 >> fx))) continue;
				int o = (yy * W + (px0 + fx)) * 3;
				rgb[o+0] = (unsigned char)r;
				rgb[o+1] = (unsigned char)g;
				rgb[o+2] = (unsigned char)b;
			}
		}
	}
}

// Read the whole file at `path` into a malloc'd buffer; returns NULL on failure
// and sets *out_len to the byte count on success.
static unsigned char* gifascii_read_file(const char* path, long* out_len){
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0){ fclose(f); return NULL; }
	unsigned char* buf = (unsigned char*)malloc((size_t)len);
	if (!buf){ fclose(f); return NULL; }
	if (fread(buf, 1, (size_t)len, f) != (size_t)len){ free(buf); fclose(f); return NULL; }
	fclose(f);
	*out_len = len;
	return buf;
}

// Convert an animated GIF to an animated ASCII-art GIF. `cols`/`rows` < 1 are
// auto-derived: cols defaults to 80, rows to the aspect-preserving height.
static int run_gif_to_gif(const char* in, const char* out, int cols, int rows){
	long len = 0;
	unsigned char* buf = gifascii_read_file(in, &len);
	if (!buf){ fprintf(stderr, "error: could not read '%s'\n", in); return 1; }

	int* delays = NULL;
	int w, h, z, comp;
	unsigned char* frames = stbi_load_gif_from_memory(buf, (int)len, &delays,
	                                                  &w, &h, &z, &comp, 4);
	free(buf);
	if (!frames || z < 1 || w < 1 || h < 1){
		fprintf(stderr, "error: '%s' is not a readable (animated) GIF\n", in);
		if (frames) stbi_image_free(frames);
		free(delays);
		return 1;
	}

	if (cols < 1) cols = 80;
	if (cols > 400) cols = 400;
	if (rows < 1){ rows = (int)((double)cols * h / (2.0 * w) + 0.5); }
	if (rows < 1) rows = 1;

	int Wpx = cols * FONT_W, Hpx = rows * FONT_H;

	GifOut g;
	if (gif_begin(&g, out, Wpx, Hpx, 1) != 0){
		fprintf(stderr, "error: could not open '%s' for writing\n", out);
		stbi_image_free(frames); free(delays);
		return 1;
	}

	Data2D* stream        = (Data2D*)malloc(sizeof(Data2D) * (size_t)(cols * rows));
	unsigned char* rgb3   = (unsigned char*)malloc((size_t)w * h * 3);
	unsigned char* framer = (unsigned char*)malloc((size_t)Wpx * Hpx * 3);
	if (!stream || !rgb3 || !framer){
		fprintf(stderr, "error: out of memory\n");
		free(stream); free(rgb3); free(framer);
		gif_end(&g); stbi_image_free(frames); free(delays);
		return 1;
	}

	for (int fr = 0; fr < z; fr++){
		const unsigned char* src = frames + (size_t)fr * w * h * comp;
		for (int i = 0; i < w * h; i++){       // RGBA (or RGB) frame -> packed RGB
			rgb3[i*3+0] = src[i*comp+0];
			rgb3[i*3+1] = src[i*comp+1];
			rgb3[i*3+2] = src[i*comp+2];
		}
		Image im;
		if (image_build_blur(&im, rgb3, w, h, 0, 3) != 0) continue;   // heavier denoise for photographic frames

		raster_begin(cols, rows);
		// Sparse coverage + robust normalization: keep only the strongest
		// contours so the subject reads against a clear background, rather than
		// every textured region (which buries it).
		int n = image_to_stream_e(&im, cols, rows, stream, cols * rows, NULL, 0.20, 0.85f);
		raster_data rd = render_char(stream, n);
		ascii_cells_to_rgb(&rd, cols, rows, framer, 0);

		int delay_cs = delays ? (delays[fr] + 5) / 10 : 10;   // ms -> centiseconds
		if (delay_cs < 2) delay_cs = 2;
		gif_add_frame(&g, framer, delay_cs);
		image_free(&im);

		fprintf(stderr, "\rframe %d/%d", fr + 1, z);
		fflush(stderr);
	}

	gif_end(&g);
	free(stream); free(rgb3); free(framer);
	stbi_image_free(frames); free(delays);
	fprintf(stderr, "\rwrote %d-frame %dx%d-cell ASCII GIF (%dx%d px) to '%s'\n",
	        z, cols, rows, Wpx, Hpx, out);
	return 0;
}

#endif
