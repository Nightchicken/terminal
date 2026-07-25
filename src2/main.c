// 3D/2D -> ASCII terminal renderer (data-oriented; see ../PLAN.md).
//
// Needs POSIX sigaction under -std=c11.
#define _POSIX_C_SOURCE 200809L
//
// main.c loads the input and dispatches: a .obj runs the 3D structure renderer
// (render.h, rotation + threads); an image runs the 2D structure-based edge
// renderer (image.h). Both draw through the shared raster path in raster.h.

#include "render.h"
#include "obj.h"
#include "image.h"
#include "gifascii.h"

#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define DEFAULT_MODEL "models/teapot.obj"

static RenderEngine g_engine;

static void on_signal(int sig){
	(void)sig;
	g_running = 0;
}

static void install_signals(void){
	struct sigaction sa = {0};
	sa.sa_handler = on_signal;
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

static int ends_with(const char* s, const char* suf){
	size_t ls = strlen(s), lf = strlen(suf);
	if (lf > ls) return 0;
	for (size_t i = 0; i < lf; i++)
		if (tolower((unsigned char)s[ls - lf + i]) != tolower((unsigned char)suf[i])) return 0;
	return 1;
}

// -- 3D model path ----------------------------------------------------------

static void* input_thread(void* arg){
	RenderEngine* e = (RenderEngine*)arg;
	const float step = 0.3f;

	while (g_running){
		unsigned char buf[16];
		ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
		if (n <= 0) continue;

		for (ssize_t i = 0; i < n; i++){
			if (buf[i] == 0x1b && i + 2 < n && buf[i + 1] == '['){
				switch (buf[i + 2]){
					case 'A': render_add_spin(e, -step, 0); break;
					case 'B': render_add_spin(e,  step, 0); break;
					case 'C': render_add_spin(e, 0,  step); break;
					case 'D': render_add_spin(e, 0, -step); break;
				}
				i += 2;
				continue;
			}
			switch (buf[i]){
				case 'q': case 'Q': case 0x1b: g_running = 0; break;
				case ' ': render_toggle_pause(e); break;
				case 'k': render_add_spin(e, -step, 0); break;
				case 'j': render_add_spin(e,  step, 0); break;
				case 'l': render_add_spin(e, 0,  step); break;
				case 'h': render_add_spin(e, 0, -step); break;
				case '0': render_reset_spin(e); break;
			}
		}
	}
	return NULL;
}

static int run_model(const char* path){
	ObjModel model;
	if (obj_load(path, &model) != 0){
		fprintf(stderr, "error: could not load model '%s'\n", path);
		return 1;
	}
	obj_normalize(&model);
	Object3D object = obj_as_object3d(&model, 4.5f);

	render_engine_init(&g_engine);
	render_engine_set_object(&g_engine, &object);

	term_init();
	atexit(term_restore);
	install_signals();

	pthread_t render_tid, input_tid;
	pthread_create(&render_tid, NULL, render_thread, &g_engine);
	pthread_create(&input_tid,  NULL, input_thread,  &g_engine);

	render_present(&g_engine);

	g_running = 0;
	pthread_join(render_tid, NULL);
	pthread_join(input_tid,  NULL);

	render_engine_destroy(&g_engine);
	term_restore();
	obj_free(&model);
	return 0;
}

// -- 2D image path ----------------------------------------------------------

static int run_image(const char* path){
	Image im;
	if (image_load(path, &im) != 0){
		fprintf(stderr, "error: could not load image '%s'\n", path);
		return 1;
	}

	term_init();
	atexit(term_restore);
	install_signals();

	Data2D* stream = NULL;
	int cap = 0;
	AnsiBuf ansi = {NULL, 0, 0};
	int pw = -1, ph = -1;

	while (g_running){
		int w, h;
		if (term_size(&w, &h) != 0){ w = 80; h = 24; }
		if (w < 1) w = 1;
		if (h < 1) h = 1;

		// The picture is static, so only re-rasterize when the terminal resizes.
		if (w != pw || h != ph){
			pw = w; ph = h;
			if (w * h > cap){
				stream = (Data2D*)realloc(stream, sizeof(Data2D) * (size_t)(w * h));
				cap = w * h;
			}
			raster_begin(w, h);
			int n = image_to_stream(&im, w, h, stream, cap);
			raster_data rd = render_char(stream, n);
			raster_build_ansi(&rd, &ansi);
			ssize_t off = 0;
			while (off < (ssize_t)ansi.len){
				ssize_t wr = write(STDOUT_FILENO, ansi.data + off, ansi.len - (size_t)off);
				if (wr <= 0) break;
				off += wr;
			}
		}

		unsigned char buf[8];
		ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));   // 100ms timeout (VTIME)
		for (ssize_t i = 0; i < r; i++)
			if (buf[i] == 'q' || buf[i] == 'Q' || buf[i] == 0x1b) g_running = 0;
	}

	free(stream);
	free(ansi.data);
	image_free(&im);
	term_restore();
	return 0;
}

// -- image -> text file (one-shot, no terminal) -----------------------------

static int run_image_to_file(const char* in, const char* outpath, int cols, int rows){
	Image im;
	if (image_load(in, &im) != 0){
		fprintf(stderr, "error: could not load image '%s'\n", in);
		return 1;
	}

	// Size the output: explicit cols/rows win; else use the terminal; else a
	// sensible default (stdout may be redirected, so term_size can fail).
	if (cols < 1 || rows < 1){
		int w, h;
		if (term_size(&w, &h) == 0 && w > 0 && h > 0){ cols = w; rows = h; }
		else { cols = 100; rows = 50; }
	}

	raster_begin(cols, rows);
	Data2D* stream = (Data2D*)malloc(sizeof(Data2D) * (size_t)(cols * rows));
	int n = image_to_stream(&im, cols, rows, stream, cols * rows);
	raster_data rd = render_char(stream, n);

	FILE* f = fopen(outpath, "w");
	if (!f){
		fprintf(stderr, "error: could not open '%s' for writing\n", outpath);
		free(stream);
		image_free(&im);
		return 1;
	}
	raster_write_text(&rd, cols, rows, f);
	fclose(f);

	free(stream);
	image_free(&im);
	fprintf(stderr, "wrote %dx%d ASCII image to '%s'\n", cols, rows, outpath);
	return 0;
}

// -- image -> text file at the automatically selected resolution ------------
// Xu 2010 Sec. 3/6: because the objective energy E is normalized, results at
// different text resolutions are directly comparable, so the system can pick
// the resolution that minimizes E. We sweep candidate widths (height derived to
// preserve aspect), match at each, and keep the lowest-E size -- the "let the
// system choose the optimal (usually small) resolution" feature.

static int run_image_to_file_auto(const char* in, const char* outpath, int maxcols){
	Image im;
	if (image_load(in, &im) != 0){
		fprintf(stderr, "error: could not load image '%s'\n", in);
		return 1;
	}

	// Upper bound on the search: explicit maxcols, else the terminal width, else
	// a sensible cap. Clamped so the scratch stays bounded.
	if (maxcols < 1){
		int w, h;
		if (term_size(&w, &h) == 0 && w > 0) maxcols = w; else maxcols = 160;
	}
	if (maxcols > 200) maxcols = 200;
	if (maxcols < 24)  maxcols = 24;

	int wmin = 16, step = 4;
	int hmax = (int)((double)maxcols * im.h / (2.0 * im.w) + 0.5);
	if (hmax < 1) hmax = 1;
	Data2D* stream = (Data2D*)malloc(sizeof(Data2D) * (size_t)(maxcols * hmax));
	if (!stream){ image_free(&im); return 1; }

	int bestW = 0, bestH = 0;
	double bestE = 1e30;
	fprintf(stderr, "auto-resolution search (minimizing AISS energy E):\n");
	for (int W = wmin; W <= maxcols; W += step){
		int H = (int)((double)W * im.h / (2.0 * im.w) + 0.5);
		if (H < 1) H = 1;
		double E;
		int n = image_to_stream_e(&im, W, H, stream, W * H, &E, IMG_KEEP_STILL, IMG_MAGP_STILL);
		if (n <= 0) continue;
		fprintf(stderr, "   %3dx%-3d  cells=%-4d  E=%.4f%s\n",
		        W, H, n, E, (E < bestE) ? "  <-- best" : "");
		if (E < bestE){ bestE = E; bestW = W; bestH = H; }
	}
	if (bestW < 1){ bestW = maxcols; bestH = hmax; }

	// Final render at the selected resolution.
	raster_begin(bestW, bestH);
	int n = image_to_stream(&im, bestW, bestH, stream, bestW * bestH);
	raster_data rd = render_char(stream, n);

	FILE* f = fopen(outpath, "w");
	if (!f){
		fprintf(stderr, "error: could not open '%s' for writing\n", outpath);
		free(stream); image_free(&im);
		return 1;
	}
	raster_write_text(&rd, bestW, bestH, f);
	fclose(f);

	free(stream);
	image_free(&im);
	fprintf(stderr, "chose %dx%d (E=%.4f); wrote ASCII image to '%s'\n",
	        bestW, bestH, bestE, outpath);
	return 0;
}

// -- animated gif -> live terminal playback ---------------------------------
// A lone `.gif` argument animates in the terminal (coloured ANSI), looping until
// 'q'/ESC. Uses the same photographic settings as GIF-to-GIF output; the grid is
// re-fit to the terminal each frame so a resize is handled live.

static int run_gif_play(const char* path){
	long len = 0;
	unsigned char* buf = gifascii_read_file(path, &len);
	if (!buf){ fprintf(stderr, "error: could not read '%s'\n", path); return 1; }

	int* delays = NULL;
	int w, h, z, comp;
	unsigned char* frames = stbi_load_gif_from_memory(buf, (int)len, &delays,
	                                                  &w, &h, &z, &comp, 4);
	free(buf);
	if (!frames || z < 1 || w < 1 || h < 1){
		fprintf(stderr, "error: '%s' is not a readable (animated) GIF\n", path);
		if (frames) stbi_image_free(frames);
		free(delays);
		return 1;
	}

	term_init();
	atexit(term_restore);
	install_signals();

	unsigned char* rgb3 = (unsigned char*)malloc((size_t)w * h * 3);
	Data2D* stream = NULL;
	int cap = 0;
	AnsiBuf ansi = {NULL, 0, 0};
	int fr = 0;

	while (g_running && rgb3){
		int cols, rows;
		if (term_size(&cols, &rows) != 0){ cols = 80; rows = 24; }
		if (cols < 1) cols = 1;
		if (rows < 1) rows = 1;
		if (cols * rows > cap){
			stream = (Data2D*)realloc(stream, sizeof(Data2D) * (size_t)(cols * rows));
			cap = cols * rows;
		}

		const unsigned char* srcf = frames + (size_t)fr * w * h * comp;
		for (int i = 0; i < w * h; i++){
			rgb3[i*3+0] = srcf[i*comp+0];
			rgb3[i*3+1] = srcf[i*comp+1];
			rgb3[i*3+2] = srcf[i*comp+2];
		}
		Image im;
		if (image_build_blur(&im, rgb3, w, h, 0, 3) == 0){
			raster_begin(cols, rows);
			int n = image_to_stream_e(&im, cols, rows, stream, cap, NULL, 0.20, 0.85f);
			raster_data rd = render_char(stream, n);
			raster_build_ansi(&rd, &ansi);
			ssize_t off = 0;
			while (off < (ssize_t)ansi.len){
				ssize_t wr = write(STDOUT_FILENO, ansi.data + off, ansi.len - (size_t)off);
				if (wr <= 0) break;
				off += wr;
			}
			image_free(&im);
		}

		double delay = delays ? (double)delays[fr] / 1000.0 : 0.1;
		if (delay < 0.02) delay = 0.1;
		double end = render_now() + delay;
		while (render_now() < end && g_running){
			unsigned char b[8];
			ssize_t r = read(STDIN_FILENO, b, sizeof b);   // returns within 100ms (VTIME)
			for (ssize_t i = 0; i < r; i++)
				if (b[i] == 'q' || b[i] == 'Q' || b[i] == 0x1b) g_running = 0;
			if (r == 0 && delay <= 0.1) break;
		}
		fr = (fr + 1) % z;
	}

	free(stream); free(rgb3); free(ansi.data);
	stbi_image_free(frames); free(delays);
	term_restore();
	return 0;
}

// -- 3D model -> animated ASCII gif (offline, headless) ---------------------
// Renders a seamless tumble (one full X turn + two full Y turns, so the last
// frame lands back on the first) of a model to an animated ASCII-art GIF. Reuses
// the live renderer's structure extraction and AISS matching, but drives the
// rotation by frame index instead of a clock and skips the terminal/threads
// entirely: render3D_structured is synchronous, so no mailbox or input thread.

static int run_obj_to_gif(const char* in, const char* out, int cols, int rows, int frames){
	ObjModel model;
	if (obj_load(in, &model) != 0){
		fprintf(stderr, "error: could not load model '%s'\n", in);
		return 1;
	}
	obj_normalize(&model);
	Object3D object = obj_as_object3d(&model, 4.5f);

	if (cols < 1)   cols = 80;
	if (cols > 400) cols = 400;
	if (rows < 1)   rows = cols / 2;   // cells are ~2x tall, so half the columns
	if (rows < 1)   rows = 1;
	if (frames < 1) frames = 24;

	int Wpx = cols * FONT_W, Hpx = rows * FONT_H;

	RenderEngine engine;
	render_engine_init(&engine);
	render_engine_set_object(&engine, &object);

	GifOut g;
	if (gif_begin(&g, out, Wpx, Hpx, 1) != 0){
		fprintf(stderr, "error: could not open '%s' for writing\n", out);
		render_engine_destroy(&engine);
		obj_free(&model);
		return 1;
	}

	unsigned char* framer = (unsigned char*)malloc((size_t)Wpx * Hpx * 3);
	if (!framer){
		fprintf(stderr, "error: out of memory\n");
		gif_end(&g);
		render_engine_destroy(&engine);
		obj_free(&model);
		return 1;
	}

	const float two_pi = 6.28318530717958647692f;
	const int   delay_cs = 8;   // ~0.08s per frame

	for (int fr = 0; fr < frames; fr++){
		float t  = (float)fr / (float)frames;
		float ax = two_pi * t;          // one full turn about X
		float ay = 2.0f * two_pi * t;   // two full turns about Y -> seamless tumble

		raster_begin(cols, rows);
		int n = render_build_stream(&engine, cols, rows, ax, ay);
		raster_data rd = render3D_structured(engine.stream, n);
		ascii_cells_to_rgb(&rd, cols, rows, framer, 0);
		gif_add_frame(&g, framer, delay_cs);

		fprintf(stderr, "\rframe %d/%d", fr + 1, frames);
		fflush(stderr);
	}

	gif_end(&g);
	free(framer);
	render_engine_destroy(&engine);
	obj_free(&model);
	fprintf(stderr, "\rwrote %d-frame %dx%d-cell ASCII GIF (%dx%d px) to '%s'\n",
	        frames, cols, rows, Wpx, Hpx, out);
	return 0;
}

// -- entry point ------------------------------------------------------------

int main(int argc, char** argv){
	const char* path = argc > 1 ? argv[1] : DEFAULT_MODEL;

	// `render <in.gif> <out.gif> [cols] [rows]` converts an animated GIF to an
	// animated ASCII-art GIF (takes precedence over the still-image file path).
	if (argc > 2 && ends_with(path, ".gif") && ends_with(argv[2], ".gif")){
		int cols = (argc > 3 && strcmp(argv[3], "auto") != 0) ? atoi(argv[3]) : 0;
		int rows = argc > 4 ? atoi(argv[4]) : 0;
		return run_gif_to_gif(path, argv[2], cols, rows);
	}

	// `render <model.obj> <out.gif> [cols] [rows] [frames]` renders a tumbling
	// spin of the model to an animated ASCII-art GIF (headless, no terminal).
	if (argc > 2 && ends_with(path, ".obj") && ends_with(argv[2], ".gif")){
		int cols   = argc > 3 ? atoi(argv[3]) : 0;
		int rows   = argc > 4 ? atoi(argv[4]) : 0;
		int frames = argc > 5 ? atoi(argv[5]) : 0;
		return run_obj_to_gif(path, argv[2], cols, rows, frames);
	}

	// `render <image> <out.txt> [cols] [rows]` renders once to a text file, or
	// `render <image> <out.txt> auto [maxcols]` picks the optimal resolution.
	if (argc > 2 && !ends_with(path, ".obj")){
		if (argc > 3 && strcmp(argv[3], "auto") == 0){
			int maxcols = argc > 4 ? atoi(argv[4]) : 0;
			return run_image_to_file_auto(path, argv[2], maxcols);
		}
		int cols = argc > 3 ? atoi(argv[3]) : 0;
		int rows = argc > 4 ? atoi(argv[4]) : 0;
		return run_image_to_file(path, argv[2], cols, rows);
	}

	if (ends_with(path, ".obj")) return run_model(path);
	if (ends_with(path, ".gif")) return run_gif_play(path);
	return run_image(path);
}
