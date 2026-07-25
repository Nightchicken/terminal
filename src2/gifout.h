#ifndef GIFOUT_H
#define GIFOUT_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Minimal animated GIF89a writer with LZW compression and a fixed 256-colour
// global palette (6x6x6 web cube + a grey ramp). Truecolour frames are mapped
// to the nearest palette entry. This is all that is needed to save the ASCII
// renderer's coloured output as an animation; there is no external dependency.
//
//   gif_begin(g, path, w, h, loop_forever)
//   gif_add_frame(g, rgb, delay_centiseconds)   // rgb is w*h*3 bytes
//   gif_end(g)

typedef struct GifOut{
	FILE* f;
	int   w, h;
	unsigned char pal[256 * 3];
	// LZW / sub-block bit writer state
	unsigned char block[255];
	int  blocklen;
	unsigned int acc;
	int  accbits;
	// scratch
	unsigned char* idx;      // w*h palette indices
	int*  hkey;              // LZW hash: key = (prefix<<8)|suffix, -1 empty
	int*  hval;              // LZW hash: assigned code
}GifOut;

#define GIF_HASH_BITS 13
#define GIF_HASH_SIZE (1 << GIF_HASH_BITS)

// --- palette ----------------------------------------------------------------

static void gif__build_palette(unsigned char* pal){
	int p = 0;
	for (int r = 0; r < 6; r++)
		for (int g = 0; g < 6; g++)
			for (int b = 0; b < 6; b++){
				pal[p*3+0] = (unsigned char)(r * 51);
				pal[p*3+1] = (unsigned char)(g * 51);
				pal[p*3+2] = (unsigned char)(b * 51);
				p++;
			}
	for (int i = 0; p < 256; i++, p++){
		int v = i * 255 / 39;
		pal[p*3+0] = pal[p*3+1] = pal[p*3+2] = (unsigned char)v;
	}
}

static inline int gif__quantize(int r, int g, int b){
	int r6 = (r * 5 + 127) / 255;
	int g6 = (g * 5 + 127) / 255;
	int b6 = (b * 5 + 127) / 255;
	return r6 * 36 + g6 * 6 + b6;   // index into the 6x6x6 cube
}

// --- bit / sub-block writer -------------------------------------------------

static void gif__flush_block(GifOut* g){
	if (g->blocklen > 0){
		fputc(g->blocklen, g->f);
		fwrite(g->block, 1, (size_t)g->blocklen, g->f);
		g->blocklen = 0;
	}
}

static void gif__put_bits(GifOut* g, int code, int nbits){
	g->acc |= (unsigned int)code << g->accbits;
	g->accbits += nbits;
	while (g->accbits >= 8){
		g->block[g->blocklen++] = (unsigned char)(g->acc & 0xff);
		g->acc >>= 8;
		g->accbits -= 8;
		if (g->blocklen == 255) gif__flush_block(g);
	}
}

// --- LZW --------------------------------------------------------------------

static void gif__lzw(GifOut* g, const unsigned char* idx, int npix){
	const int minCode = 8;                 // 256-colour global palette
	const int clearCode = 1 << minCode;    // 256
	const int eoiCode = clearCode + 1;     // 257
	int codeSize = minCode + 1;            // 9
	int next = clearCode + 2;              // 258

	for (int i = 0; i < GIF_HASH_SIZE; i++) g->hkey[i] = -1;

	fputc(minCode, g->f);                  // LZW minimum code size byte
	g->blocklen = 0; g->acc = 0; g->accbits = 0;

	gif__put_bits(g, clearCode, codeSize);
	int prefix = idx[0];
	for (int i = 1; i < npix; i++){
		int c = idx[i];
		int key = (prefix << 8) | c;
		int slot = (key * 2654435761u) & (GIF_HASH_SIZE - 1);
		while (g->hkey[slot] != -1 && g->hkey[slot] != key)
			slot = (slot + 1) & (GIF_HASH_SIZE - 1);

		if (g->hkey[slot] == key){          // (prefix,c) already in table
			prefix = g->hval[slot];
			continue;
		}
		gif__put_bits(g, prefix, codeSize); // emit prefix, add new entry
		g->hkey[slot] = key;
		g->hval[slot] = next;
		next++;
		if (next == 4096){
			gif__put_bits(g, clearCode, codeSize);
			for (int k = 0; k < GIF_HASH_SIZE; k++) g->hkey[k] = -1;
			next = clearCode + 2;
			codeSize = minCode + 1;
		}else if (next == (1 << codeSize) + 1 && codeSize < 12){
			// Bump one code later than plain LZW: the decoder adds no entry for
			// the first code after a clear, so its table trails the encoder's by
			// one. Increasing when the just-assigned code == 2^codeSize keeps the
			// read/write widths in lockstep (matches giflib / gif.h).
			codeSize++;
		}
		prefix = c;
	}
	gif__put_bits(g, prefix, codeSize);
	gif__put_bits(g, eoiCode, codeSize);

	if (g->accbits > 0){                    // flush remaining bits
		g->block[g->blocklen++] = (unsigned char)(g->acc & 0xff);
		g->acc = 0; g->accbits = 0;
		if (g->blocklen == 255) gif__flush_block(g);
	}
	gif__flush_block(g);
	fputc(0x00, g->f);                      // sub-block terminator
}

// --- public API -------------------------------------------------------------

static int gif_begin(GifOut* g, const char* path, int w, int h, int loop_forever){
	memset(g, 0, sizeof *g);
	g->f = fopen(path, "wb");
	if (!g->f) return -1;
	g->w = w; g->h = h;
	g->idx  = (unsigned char*)malloc((size_t)w * (size_t)h);
	g->hkey = (int*)malloc(sizeof(int) * GIF_HASH_SIZE);
	g->hval = (int*)malloc(sizeof(int) * GIF_HASH_SIZE);
	if (!g->idx || !g->hkey || !g->hval){
		free(g->idx); free(g->hkey); free(g->hval); fclose(g->f); g->f = NULL;
		return -1;
	}
	gif__build_palette(g->pal);

	fwrite("GIF89a", 1, 6, g->f);
	// logical screen descriptor
	fputc(w & 0xff, g->f); fputc((w >> 8) & 0xff, g->f);
	fputc(h & 0xff, g->f); fputc((h >> 8) & 0xff, g->f);
	fputc(0xF7, g->f);   // global colour table, 256 entries (2^(7+1))
	fputc(0, g->f);      // background colour index
	fputc(0, g->f);      // pixel aspect ratio
	fwrite(g->pal, 1, sizeof g->pal, g->f);

	if (loop_forever){   // NETSCAPE2.0 application extension: loop forever
		fputc(0x21, g->f); fputc(0xFF, g->f); fputc(0x0B, g->f);
		fwrite("NETSCAPE2.0", 1, 11, g->f);
		fputc(0x03, g->f); fputc(0x01, g->f);
		fputc(0x00, g->f); fputc(0x00, g->f);   // loop count 0 = forever
		fputc(0x00, g->f);
	}
	return 0;
}

static void gif_add_frame(GifOut* g, const unsigned char* rgb, int delay_cs){
	int npix = g->w * g->h;
	for (int i = 0; i < npix; i++)
		g->idx[i] = (unsigned char)gif__quantize(rgb[i*3+0], rgb[i*3+1], rgb[i*3+2]);

	// graphic control extension (per-frame delay)
	fputc(0x21, g->f); fputc(0xF9, g->f); fputc(0x04, g->f);
	fputc(0x00, g->f);                                   // no transparency/disposal
	fputc(delay_cs & 0xff, g->f); fputc((delay_cs >> 8) & 0xff, g->f);
	fputc(0x00, g->f);                                   // transparent index
	fputc(0x00, g->f);                                   // block terminator

	// image descriptor (full frame, no local colour table)
	fputc(0x2C, g->f);
	fputc(0, g->f); fputc(0, g->f);                      // left
	fputc(0, g->f); fputc(0, g->f);                      // top
	fputc(g->w & 0xff, g->f); fputc((g->w >> 8) & 0xff, g->f);
	fputc(g->h & 0xff, g->f); fputc((g->h >> 8) & 0xff, g->f);
	fputc(0x00, g->f);                                   // no local table, no interlace

	gif__lzw(g, g->idx, npix);
}

static void gif_end(GifOut* g){
	if (!g->f) return;
	fputc(0x3B, g->f);   // trailer
	fclose(g->f);
	free(g->idx); free(g->hkey); free(g->hval);
	g->f = NULL; g->idx = NULL; g->hkey = NULL; g->hval = NULL;
}

#endif
