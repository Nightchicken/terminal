#ifndef IMAGE_H
#define IMAGE_H
#include "vec.h"
#include "raster.h"
#include "arena.h"
#include <math.h>
#include <stdlib.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// 2D image -> structure-based ASCII. An image is loaded, reduced to grayscale
// and Sobel-filtered; then for each terminal cell the local edge structure is
// matched to the character whose shape follows it, using the alignment-
// insensitive shape similarity (AISS) metric of Xu 2010 (aiss.h). This is the
// paper's exact problem: divide the edge image into grid cells and best-match
// each cell with a character. A per-cell structure tensor is still accumulated,
// but only to threshold weak cells to blank (so the output stays an oriented
// line drawing, not a tone ramp) and to average cell colour. The glyph itself
// comes from AISS matching the cell's edge field against the codebook, which
// captures corners and curves the old single-angle selection could not.

typedef struct Image{
	int w, h;
	unsigned char* rgb;   // w*h*3
	int own_rgb;          // 1 if rgb was stb-allocated here and image_free owns it
	Arena arena;          // backs gray/gx/gy/mag, released in one destroy
	float* gray;          // w*h luminance in [0,1]
	float* gx;            // w*h Sobel x
	float* gy;            // w*h Sobel y
	float* mag;           // w*h thinned edge magnitude (non-max suppressed)
}Image;

static inline float img_gray_at(const Image* im, int x, int y){
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x >= im->w) x = im->w - 1;
	if (y >= im->h) y = im->h - 1;
	return im->gray[y * im->w + x];
}

// Build the edge fields (grayscale, Sobel, NMS-thinned magnitude) from an
// already-decoded w*h*3 RGB buffer. `own_rgb` records whether image_free should
// release `rgb` (true for a single stb-loaded image, false for a GIF frame that
// is a slice of a larger buffer freed elsewhere). Returns 0 on success.
static int image_build_blur(Image* im, unsigned char* data, int w, int h, int own_rgb, int blur){
	im->w = w; im->h = h; im->rgb = data; im->own_rgb = own_rgb;

	// gray/gx/gy/mag share one arena so image_free() releases them together.
	size_t npx = (size_t)w * (size_t)h;
	im->arena = arena_create(4 * sizeof(float) * npx + 4 * ARENA_DEFAULT_ALIGN);
	im->gray = (float*)arena_alloc(&im->arena, sizeof(float) * npx);
	im->gx   = (float*)arena_alloc(&im->arena, sizeof(float) * npx);
	im->gy   = (float*)arena_alloc(&im->arena, sizeof(float) * npx);
	im->mag  = (float*)arena_alloc(&im->arena, sizeof(float) * npx);
	if (!im->gray || !im->gx || !im->gy || !im->mag){
		arena_destroy(&im->arena);
		if (own_rgb) stbi_image_free(data);
		return -1;
	}

	for (int i = 0; i < w * h; i++){
		float r = data[i * 3 + 0] / 255.0f;
		float g = data[i * 3 + 1] / 255.0f;
		float b = data[i * 3 + 2] / 255.0f;
		im->gray[i] = 0.299f * r + 0.587f * g + 0.114f * b;
	}

	// Gaussian pre-smoothing (Canny step 1): `blur` passes of a separable 1-2-1
	// kernel on the grayscale before Sobel. This removes fine texture, GIF dither
	// and JPEG-style noise so that only real object contours survive as edges --
	// essential for photographic frames, where otherwise every textured region
	// reads as an edge and the subject is buried. Clean line art uses blur=0/1.
	if (blur > 0){
		float* tmp = (float*)malloc(sizeof(float) * (size_t)w * (size_t)h);
		if (tmp){
			for (int pass = 0; pass < blur; pass++){
				for (int y = 0; y < h; y++)
					for (int x = 0; x < w; x++){
						float l = im->gray[y * w + (x > 0 ? x - 1 : 0)];
						float c = im->gray[y * w + x];
						float r = im->gray[y * w + (x < w - 1 ? x + 1 : w - 1)];
						tmp[y * w + x] = 0.25f * l + 0.5f * c + 0.25f * r;
					}
				for (int y = 0; y < h; y++)
					for (int x = 0; x < w; x++){
						float u = tmp[(y > 0 ? y - 1 : 0) * w + x];
						float c = tmp[y * w + x];
						float d = tmp[(y < h - 1 ? y + 1 : h - 1) * w + x];
						im->gray[y * w + x] = 0.25f * u + 0.5f * c + 0.25f * d;
					}
			}
			free(tmp);
		}
	}

	for (int y = 0; y < h; y++){
		for (int x = 0; x < w; x++){
			float tl = img_gray_at(im, x-1, y-1), tc = img_gray_at(im, x, y-1), tr = img_gray_at(im, x+1, y-1);
			float ml = img_gray_at(im, x-1, y),                                 mr = img_gray_at(im, x+1, y);
			float bl = img_gray_at(im, x-1, y+1), bc = img_gray_at(im, x, y+1), br = img_gray_at(im, x+1, y+1);
			im->gx[y * w + x] = (tr + 2*mr + br) - (tl + 2*ml + bl);
			im->gy[y * w + x] = (bl + 2*bc + br) - (tl + 2*tc + tr);
		}
	}

	// Non-maximum suppression: keep each pixel's magnitude only if it is a local
	// maximum along the gradient direction, thinning edges toward one-pixel
	// centrelines. Xu 2010 matches one-pixel-width structure, so a thick contour
	// must not fill a whole cell (which would match a dense glyph like '*').
	for (int y = 0; y < h; y++){
		for (int x = 0; x < w; x++){
			int idx = y * w + x;
			float gx = im->gx[idx], gy = im->gy[idx];
			float m = sqrtf(gx * gx + gy * gy);
			if (m < 1e-4f){ im->mag[idx] = 0.0f; continue; }
			float nx = gx / m, ny = gy / m;   // gradient direction (across edge)
			float fx = (float)x + nx, fy = (float)y + ny;
			float bx = (float)x - nx, by = (float)y - ny;
			int fxi = (int)(fx + 0.5f), fyi = (int)(fy + 0.5f);
			int bxi = (int)(bx + 0.5f), byi = (int)(by + 0.5f);
			float mf = 0.0f, mb = 0.0f;
			if (fxi >= 0 && fyi >= 0 && fxi < w && fyi < h){
				float a = im->gx[fyi * w + fxi], b = im->gy[fyi * w + fxi];
				mf = sqrtf(a * a + b * b);
			}
			if (bxi >= 0 && byi >= 0 && bxi < w && byi < h){
				float a = im->gx[byi * w + bxi], b = im->gy[byi * w + bxi];
				mb = sqrtf(a * a + b * b);
			}
			im->mag[idx] = (m >= mf && m >= mb) ? m : 0.0f;
		}
	}
	return 0;
}

// Default blur wrapper (mild smoothing; negligible on clean line art).
static inline int image_build(Image* im, unsigned char* data, int w, int h, int own_rgb){
	return image_build_blur(im, data, w, h, own_rgb, 1);
}

// Load `path`, decode to RGB, and build the edge fields. Returns 0 on success.
static int image_load(const char* path, Image* im){
	int w, h, n;
	unsigned char* data = stbi_load(path, &w, &h, &n, 3);
	if (!data) return -1;
	return image_build_blur(im, data, w, h, 1, 1);
}

static void image_free(Image* im){
	if (im->rgb && im->own_rgb) stbi_image_free(im->rgb);   // only if we own it
	arena_destroy(&im->arena);                              // gray + gx + gy in one call
	im->rgb = NULL; im->gray = im->gx = im->gy = NULL;
}

// Per-cell structure tensor + colour accumulator.
typedef struct ImgCell{
	double a, b, c;        // sum gx^2, gx*gy, gy^2
	double r, g, bl;       // colour sums
	int cnt;
}ImgCell;

// AISS ink sampler over one image cell's edge field, in cell-normalized (u,v).
// (u,v) maps into the cell's pixel block; values outside [0,1] reach into the
// neighbouring pixels (clamped), giving the cross-cell context AISS tolerates.
typedef struct ImgInkCtx{
	const Image* im;
	float px0, py0, pxs, pys;   // cell pixel origin + span
	float invmax;               // 1 / max edge magnitude (normalization)
}ImgInkCtx;

static float img_cell_ink(void* ctx, float u, float v){
	ImgInkCtx* k = (ImgInkCtx*)ctx;
	int x = (int)(k->px0 + u * k->pxs + 0.5f);
	int y = (int)(k->py0 + v * k->pys + 0.5f);
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x >= k->im->w) x = k->im->w - 1;
	if (y >= k->im->h) y = k->im->h - 1;
	float m = k->im->mag[y * k->im->w + x] * k->invmax;
	return m < 1.0f ? m : 1.0f;
}

// Fit the image into cols x rows (preserving aspect, accounting for 2:1 cells),
// detect edges per cell, and emit a Data2D stream of matched glyphs. Returns the
// number of points written. When `out_energy` is non-NULL it receives the
// objective energy E = mean AISS matching distance over non-empty cells (paper
// Eq. 6 with the deformation term = 1), which is normalized and so comparable
// across text resolutions -- the basis for automatic resolution selection.
// `keep_frac` is the fraction of edged cells to keep (density: ~0.85 keeps a
// clean line drawing's whole outline; ~0.2 keeps only the strongest contours of
// a busy photographic frame). `magp` is the edge-magnitude percentile used to
// normalize the ink field (robust to a few very bright edges).
static int image_to_stream_e(const Image* im, int cols, int rows, Data2D* out, int cap,
                             double* out_energy, double keep_frac, float magp){
	if (out_energy) *out_energy = 1e30;
	if (im->w < 1 || im->h < 1) return 0;

	// Desired columns/rows so the picture keeps its aspect; a cell is ~2x tall.
	float target = 2.0f * (float)im->w / (float)im->h;
	int fw, fh;
	if ((float)cols / (float)rows > target){ fh = rows; fw = (int)(target * (float)rows + 0.5f); }
	else                                   { fw = cols; fh = (int)((float)cols / target + 0.5f); }
	if (fw < 1) fw = 1;
	if (fh < 1) fh = 1;
	if (fw > cols) fw = cols;
	if (fh > rows) fh = rows;
	int ox = (cols - fw) / 2, oy = (rows - fh) / 2;

	ImgCell* cell = (ImgCell*)calloc((size_t)(fw * fh), sizeof(ImgCell));

	for (int j = 0; j < fh; j++){
		int py0 = j * im->h / fh, py1 = (j + 1) * im->h / fh;
		if (py1 <= py0) py1 = py0 + 1;
		for (int y = py0; y < py1 && y < im->h; y++){
			for (int i = 0; i < fw; i++){
				int px0 = i * im->w / fw, px1 = (i + 1) * im->w / fw;
				if (px1 <= px0) px1 = px0 + 1;
				ImgCell* cc = &cell[j * fw + i];
				for (int x = px0; x < px1 && x < im->w; x++){
					float gx = im->gx[y * im->w + x], gy = im->gy[y * im->w + x];
					cc->a += (double)gx * gx;
					cc->b += (double)gx * gy;
					cc->c += (double)gy * gy;
					cc->r += im->rgb[(y * im->w + x) * 3 + 0];
					cc->g += im->rgb[(y * im->w + x) * 3 + 1];
					cc->bl += im->rgb[(y * im->w + x) * 3 + 2];
					cc->cnt++;
				}
			}
		}
	}

	// Robust scales via percentiles instead of the absolute maximum. On
	// photographic frames a few extremely strong edges (e.g. bright caption
	// text) would otherwise set the scale and threshold every softer edge -- the
	// subject -- away. Percentiles are resistant to those outliers, so the actual
	// content shows up. Histograms (256 bins) give the percentile cheaply.

	// (1) Magnitude scale for the AISS ink field: edges near/above the P-th
	//     percentile saturate to solid ink; the caption just clips.
	float maxmag = 1e-6f;
	for (int p = 0; p < im->w * im->h; p++)
		if (im->mag[p] > maxmag) maxmag = im->mag[p];
	{
		int mhist[256] = {0}; long nz = 0;
		for (int p = 0; p < im->w * im->h; p++){
			float mg = im->mag[p];
			if (mg > 1e-4f){ int bkt = (int)(mg / maxmag * 255.0f); mhist[bkt]++; nz++; }
		}
		if (nz > 0){
			long target = (long)((double)nz * (double)magp), acc = 0;
			for (int bkt = 0; bkt < 256; bkt++){
				acc += mhist[bkt];
				if (acc >= target){ maxmag = (bkt + 0.5f) / 256.0f * maxmag; break; }
			}
		}
	}
	if (maxmag < 1e-6f) maxmag = 1e-6f;
	float invmax = 1.0f / (0.55f * maxmag);   // <1 saturates so edges read solid

	// (2) Cell threshold as a coverage target: keep the strongest fraction of
	//     the cells that carry any edge, so the frame is filled with real
	//     structure whatever the absolute contrast. A percentile of the edged
	//     cells' energies gives that threshold.
	double maxen = 1e-12;
	for (int k = 0; k < fw * fh; k++){
		if (cell[k].cnt == 0) continue;
		double en = (cell[k].a + cell[k].c) / cell[k].cnt;
		if (en > maxen) maxen = en;
	}
	double thresh = 0.0;
	{
		int ehist[256] = {0}; long en_nz = 0;
		for (int k = 0; k < fw * fh; k++){
			if (cell[k].cnt == 0) continue;
			double en = (cell[k].a + cell[k].c) / cell[k].cnt;
			if (en > maxen * 1e-4){ int bkt = (int)(en / maxen * 255.0); if (bkt > 255) bkt = 255; ehist[bkt]++; en_nz++; }
		}
		if (en_nz > 0){
			long target = (long)((double)en_nz * (1.0 - keep_frac)), acc = 0;
			for (int bkt = 0; bkt < 256; bkt++){
				acc += ehist[bkt];
				if (acc >= target){ thresh = (double)bkt / 255.0 * maxen; break; }
			}
		}
	}
	const AissBook* book = aiss_book_ensure();

	double energy_sum = 0.0;   // sum of per-cell AISS match distances
	int    energy_k   = 0;     // number of non-empty (matched) cells
	int n = 0;
	for (int j = 0; j < fh && n < cap; j++){
		int py0 = j * im->h / fh, py1 = (j + 1) * im->h / fh;
		if (py1 <= py0) py1 = py0 + 1;
		for (int i = 0; i < fw && n < cap; i++){
			ImgCell* cc = &cell[j * fw + i];
			if (cc->cnt == 0) continue;
			double a = cc->a / cc->cnt, b = cc->b / cc->cnt, c = cc->c / cc->cnt;
			double en = a + c;
			(void)b;
			if (en <= thresh || en <= 1e-6) continue;

			// Match the cell's local edge structure to the best-fitting glyph
			// via alignment-insensitive shape similarity (Xu 2010, aiss.h).
			int px0 = i * im->w / fw, px1 = (i + 1) * im->w / fw;
			if (px1 <= px0) px1 = px0 + 1;
			ImgInkCtx ink = { im, (float)px0, (float)py0,
			                  (float)(px1 - px0), (float)(py1 - py0), invmax };
			AissFeature f;
			aiss_build(&f, img_cell_ink, &ink);
			if (f.mass < 0.75f) continue;
			float dist;
			char glyph = aiss_match_ex(book, &f, AISS_NOFF, &dist);
			energy_sum += dist;
			energy_k++;

			// Block-average colour, brightened so dark edges stay visible.
			float r = (float)(cc->r / cc->cnt) / 255.0f;
			float g = (float)(cc->g / cc->cnt) / 255.0f;
			float bl = (float)(cc->bl / cc->cnt) / 255.0f;
			float mx = fmaxf(r, fmaxf(g, bl));
			if (mx > 1e-4f && mx < 0.5f){ float s = 0.5f / mx; r *= s; g *= s; bl *= s; }

			out[n].position = (Vec2f){(float)(ox + i), (float)(oy + j)};
			out[n].color = (Vec3f){r, g, bl};
			out[n].c = glyph;
			n++;
		}
	}

	free(cell);
	if (out_energy) *out_energy = energy_k > 0 ? energy_sum / energy_k : 1e30;
	return n;
}

// Still-image defaults: keep most of the outline, robust normalization.
#define IMG_KEEP_STILL 0.85
#define IMG_MAGP_STILL 0.90f

// Back-compatible wrapper: still-image density, no energy computed.
static inline int image_to_stream(const Image* im, int cols, int rows, Data2D* out, int cap){
	return image_to_stream_e(im, cols, rows, out, cap, NULL, IMG_KEEP_STILL, IMG_MAGP_STILL);
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
