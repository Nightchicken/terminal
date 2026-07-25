#ifndef AISS_H
#define AISS_H
#include <math.h>
#include <string.h>
#include <float.h>

// Alignment-Insensitive Shape Similarity (AISS) matcher.
//
// A faithful implementation of the shape-matching core of Xu, Zhang & Wong,
// "Structure-based ASCII Art" (SIGGRAPH 2010) -- see ../papers. The paper's key
// contribution is a similarity metric that tolerates misalignment while still
// accounting for position and orientation, so a grid cell of line structure can
// be matched to the character whose *shape* follows it (not just its average
// tone or a single dominant angle).
//
// The metric (paper Sec. 4, Fig. 6):
//   * A shape lives in a normalized cell (u,v) in [0,1]^2, (0,0) top-left.
//   * N sample points are placed on a regular grid over the cell -> position
//     awareness / translation (paper: "N points regularly sampled in a grid").
//   * At each sample point a log-polar histogram of the surrounding "grayness"
//     is accumulated: NA angular bins x NR radial bins, radial spacing in log
//     space so nearby structure dominates -> misalignment tolerance, and the
//     angular bins give orientation awareness.
//   * The per-point histograms are concatenated into one feature vector.
//   * Distance between two shapes S, S' is (paper Eq. 1):
//         D = (1/(n+n')) * sum_i || h_i - h'_i ||
//     where h_i is the i-th sample point's histogram and n, n' are the total
//     grayness of each shape (the normalization counteracts absolute grayness).
//
// A cell of image/edge structure is matched by building its feature the same
// way and picking the codebook glyph with the smallest D. The deformation /
// simulated-annealing half of the paper (moving vector vertices to raise the
// match rate) is intentionally not implemented here: this renderer consumes
// raster edge fields and 3D line structure in real time, so only the per-cell
// AISS matching applies.

// --- descriptor configuration ----------------------------------------------
// Kept modest so per-cell matching stays cheap enough for the 60fps 3D path;
// the image path is one-shot so cost there is irrelevant.

#define AISS_NA    12                       // angular bins (paper: 12)
#define AISS_NR    5                        // radial bins in log space (paper: 5)
#define AISS_SPX   3                         // sample points across the cell (u)
#define AISS_SPY   6                         // sample points down the cell (v)
#define AISS_NP    (AISS_SPX * AISS_SPY)     // total sample points
#define AISS_BINS  (AISS_NA * AISS_NR)       // histogram bins per sample point
#define AISS_DIM   (AISS_NP * AISS_BINS)     // full feature dimension
#define AISS_R     0.45f                     // log-polar radius (~half the cell)
#define AISS_RMIN  0.06f                     // inner radius of the log mapping
#define AISS_WHALF 6                         // window sampled on a (2W+1)^2 grid

static const float AISS_PI = 3.14159265358979323846f;

typedef struct AissFeature{
	float h[AISS_DIM];
	float mass;                              // total grayness n (Eq. 1 denom)
}AissFeature;

// Grayness sampler: returns ink in [0,1] at normalized cell coord (u,v). u,v may
// fall outside [0,1] when a log-polar window near the border reaches into
// neighbouring content; samplers clamp or return 0 as appropriate.
typedef float (*AissInk)(void* ctx, float u, float v);

// Build the concatenated log-polar feature for a shape.
static void aiss_build(AissFeature* f, AissInk ink, void* ctx){
	memset(f->h, 0, sizeof f->h);
	float mass = 0.0f;
	float logR = logf(AISS_R / AISS_RMIN);

	int wi = 0;
	for (int sy = 0; sy < AISS_SPY; sy++){
		float pv = (sy + 0.5f) / AISS_SPY;
		for (int sx = 0; sx < AISS_SPX; sx++, wi++){
			float pu = (sx + 0.5f) / AISS_SPX;
			float* hh = f->h + wi * AISS_BINS;

			for (int qy = -AISS_WHALF; qy <= AISS_WHALF; qy++){
				float dv = (float)qy / AISS_WHALF * AISS_R;
				for (int qx = -AISS_WHALF; qx <= AISS_WHALF; qx++){
					float du = (float)qx / AISS_WHALF * AISS_R;
					float r = sqrtf(du * du + dv * dv);
					if (r > AISS_R) continue;

					float g = ink(ctx, pu + du, pv + dv);
					if (g <= 0.0f) continue;

					int rb;
					if (r < AISS_RMIN) rb = 0;
					else{
						rb = (int)(AISS_NR * logf(r / AISS_RMIN) / logR);
						if (rb < 0) rb = 0;
						if (rb >= AISS_NR) rb = AISS_NR - 1;
					}
					float ang = atan2f(dv, du) + AISS_PI;      // [0, 2pi)
					int ab = (int)(AISS_NA * ang / (2.0f * AISS_PI));
					if (ab < 0) ab = 0;
					if (ab >= AISS_NA) ab = AISS_NA - 1;

					hh[rb * AISS_NA + ab] += g;
					mass += g;
				}
			}
		}
	}
	f->mass = mass;
}

// Paper Eq. 1: normalized sum of per-sample-point histogram distances.
static float aiss_distance(const AissFeature* a, const AissFeature* b){
	float denom = a->mass + b->mass;
	if (denom <= 1e-6f) return FLT_MAX;
	float acc = 0.0f;
	for (int wi = 0; wi < AISS_NP; wi++){
		const float* ha = a->h + wi * AISS_BINS;
		const float* hb = b->h + wi * AISS_BINS;
		float s = 0.0f;
		for (int k = 0; k < AISS_BINS; k++){
			float d = ha[k] - hb[k];
			s += d * d;
		}
		acc += sqrtf(s);
	}
	return acc / denom;
}

// --- glyph codebook --------------------------------------------------------
// Each character is described by its centreline strokes (polyline segments) in
// the normalized cell, mirroring the paper's "ignore font thickness via
// centreline extraction, rasterize every glyph to one-pixel width". The strokes
// are sampled analytically (distance-to-segment falloff) so the codebook is
// resolution independent and built once at startup.

typedef struct AissSeg{ float u0, v0, u1, v1; }AissSeg;

typedef struct AissGlyphDef{
	char c;
	const AissSeg* segs;
	int nseg;
}AissGlyphDef;

#define AISS_THICK 0.12f   // stroke half-width used when rasterizing glyphs

static const AissSeg g_seg_dash[]  = {{0.08f,0.50f,0.92f,0.50f}};
static const AissSeg g_seg_under[] = {{0.00f,0.92f,1.00f,0.92f}};
static const AissSeg g_seg_pipe[]  = {{0.50f,0.05f,0.50f,0.95f}};
static const AissSeg g_seg_fwd[]   = {{0.90f,0.05f,0.10f,0.95f}};
static const AissSeg g_seg_bwd[]   = {{0.10f,0.05f,0.90f,0.95f}};
static const AissSeg g_seg_lpar[]  = {{0.66f,0.05f,0.40f,0.50f},{0.40f,0.50f,0.66f,0.95f}};
static const AissSeg g_seg_rpar[]  = {{0.34f,0.05f,0.60f,0.50f},{0.60f,0.50f,0.34f,0.95f}};
static const AissSeg g_seg_lt[]    = {{0.78f,0.12f,0.28f,0.50f},{0.28f,0.50f,0.78f,0.88f}};
static const AissSeg g_seg_gt[]    = {{0.22f,0.12f,0.72f,0.50f},{0.72f,0.50f,0.22f,0.88f}};
static const AissSeg g_seg_caret[] = {{0.12f,0.70f,0.50f,0.22f},{0.50f,0.22f,0.88f,0.70f}};
static const AissSeg g_seg_vee[]   = {{0.12f,0.30f,0.50f,0.78f},{0.50f,0.78f,0.88f,0.30f}};
static const AissSeg g_seg_eq[]    = {{0.08f,0.40f,0.92f,0.40f},{0.08f,0.62f,0.92f,0.62f}};
static const AissSeg g_seg_tilde[] = {{0.08f,0.55f,0.35f,0.45f},{0.35f,0.45f,0.62f,0.58f},{0.62f,0.58f,0.90f,0.48f}};
static const AissSeg g_seg_plus[]  = {{0.50f,0.22f,0.50f,0.78f},{0.16f,0.50f,0.84f,0.50f}};
static const AissSeg g_seg_ex[]    = {{0.18f,0.20f,0.82f,0.80f},{0.82f,0.20f,0.18f,0.80f}};
static const AissSeg g_seg_star[]  = {{0.50f,0.20f,0.50f,0.80f},{0.22f,0.32f,0.78f,0.68f},{0.78f,0.32f,0.22f,0.68f}};
static const AissSeg g_seg_quote[] = {{0.50f,0.06f,0.44f,0.32f}};
static const AissSeg g_seg_dot[]   = {{0.46f,0.84f,0.54f,0.86f}};
static const AissSeg g_seg_comma[] = {{0.52f,0.74f,0.42f,0.94f}};
static const AissSeg g_seg_seven[] = {{0.12f,0.14f,0.88f,0.14f},{0.88f,0.14f,0.36f,0.94f}};
static const AissSeg g_seg_ell[]   = {{0.26f,0.06f,0.26f,0.90f},{0.26f,0.90f,0.82f,0.90f}};
static const AissSeg g_seg_tee[]   = {{0.10f,0.16f,0.90f,0.16f},{0.50f,0.16f,0.50f,0.90f}};
static const AissSeg g_seg_wye[]   = {{0.16f,0.10f,0.50f,0.50f},{0.84f,0.10f,0.50f,0.50f},{0.50f,0.50f,0.50f,0.92f}};
static const AissSeg g_seg_jay[]   = {{0.68f,0.08f,0.68f,0.82f},{0.68f,0.82f,0.46f,0.94f},{0.46f,0.94f,0.24f,0.82f}};
static const AissSeg g_seg_arr[]   = {{0.32f,0.34f,0.32f,0.90f},{0.32f,0.44f,0.62f,0.34f}};
static const AissSeg g_seg_cee[]   = {{0.72f,0.40f,0.42f,0.36f},{0.42f,0.36f,0.30f,0.60f},{0.30f,0.60f,0.44f,0.78f},{0.44f,0.78f,0.72f,0.72f}};
static const AissSeg g_seg_oh[]    = {{0.50f,0.32f,0.68f,0.50f},{0.68f,0.50f,0.50f,0.68f},{0.50f,0.68f,0.32f,0.50f},{0.32f,0.50f,0.50f,0.32f}};

#define AISS_GLYPH(ch, arr) { (ch), (arr), (int)(sizeof(arr)/sizeof((arr)[0])) }

static const AissGlyphDef g_glyph_defs[] = {
	AISS_GLYPH('-',  g_seg_dash),  AISS_GLYPH('_',  g_seg_under),
	AISS_GLYPH('|',  g_seg_pipe),  AISS_GLYPH('/',  g_seg_fwd),
	AISS_GLYPH('\\', g_seg_bwd),   AISS_GLYPH('(',  g_seg_lpar),
	AISS_GLYPH(')',  g_seg_rpar),  AISS_GLYPH('<',  g_seg_lt),
	AISS_GLYPH('>',  g_seg_gt),    AISS_GLYPH('^',  g_seg_caret),
	AISS_GLYPH('v',  g_seg_vee),   AISS_GLYPH('=',  g_seg_eq),
	AISS_GLYPH('~',  g_seg_tilde), AISS_GLYPH('+',  g_seg_plus),
	AISS_GLYPH('x',  g_seg_ex),    AISS_GLYPH('*',  g_seg_star),
	AISS_GLYPH('\'', g_seg_quote), AISS_GLYPH('.',  g_seg_dot),
	AISS_GLYPH(',',  g_seg_comma), AISS_GLYPH('7',  g_seg_seven),
	AISS_GLYPH('L',  g_seg_ell),   AISS_GLYPH('T',  g_seg_tee),
	AISS_GLYPH('Y',  g_seg_wye),   AISS_GLYPH('J',  g_seg_jay),
	AISS_GLYPH('r',  g_seg_arr),   AISS_GLYPH('c',  g_seg_cee),
	AISS_GLYPH('o',  g_seg_oh),
};

#define AISS_MAXG ((int)(sizeof(g_glyph_defs)/sizeof(g_glyph_defs[0])))

// Each glyph is pre-built at a small set of translations. Matching takes the
// best-aligned variant, which realizes the paper's misalignment tolerance /
// local deformation (slide the character to where it best fits the cell)
// without the global simulated-annealing optimization. Index 0 is centred; 1-4
// are near axis shifts; 5-8 are diagonal shifts; 9-12 are wide axis shifts that
// let a line pinned near a cell edge (v~=0 or v~=1) still reach a straight glyph
// instead of a spurious junction glyph.
#define AISS_NOFF 13
#define AISS_NAXIS 5   // center + 4 near-axis offsets (used by the fast 3D matcher)

static const float g_off_du[AISS_NOFF] = { 0.00f, 0.13f,-0.13f, 0.00f, 0.00f, 0.13f,-0.13f, 0.13f,-0.13f, 0.00f, 0.00f, 0.28f,-0.28f };
static const float g_off_dv[AISS_NOFF] = { 0.00f, 0.00f, 0.00f, 0.19f,-0.19f, 0.19f, 0.19f,-0.19f,-0.19f, 0.38f,-0.38f, 0.00f, 0.00f };

typedef struct AissBook{
	AissFeature feat[AISS_MAXG * AISS_NOFF];   // [glyph][offset]
	char        ch[AISS_MAXG];
	int         n;                             // number of glyphs
}AissBook;

static inline float aiss_seg_dist(float u, float v, AissSeg s){
	float dx = s.u1 - s.u0, dy = s.v1 - s.v0;
	float len2 = dx * dx + dy * dy;
	float t = (len2 > 1e-9f) ? ((u - s.u0) * dx + (v - s.v0) * dy) / len2 : 0.0f;
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	float ex = u - (s.u0 + t * dx), ey = v - (s.v0 + t * dy);
	return sqrtf(ex * ex + ey * ey);
}

typedef struct AissGlyphInk{ const AissSeg* segs; int nseg; }AissGlyphInk;

static float aiss_glyph_ink(void* ctx, float u, float v){
	AissGlyphInk* g = (AissGlyphInk*)ctx;
	float best = 1e9f;
	for (int i = 0; i < g->nseg; i++){
		float d = aiss_seg_dist(u, v, g->segs[i]);
		if (d < best) best = d;
	}
	float x = best / AISS_THICK;
	return expf(-x * x);
}

// Rasterize every codebook glyph, at every translation, into its AISS feature.
// Call once.
static void aiss_book_init(AissBook* b){
	b->n = AISS_MAXG;
	for (int gi = 0; gi < AISS_MAXG; gi++){
		b->ch[gi] = g_glyph_defs[gi].c;
		int ns = g_glyph_defs[gi].nseg;
		for (int oi = 0; oi < AISS_NOFF; oi++){
			AissSeg tmp[8];
			for (int s = 0; s < ns; s++){
				AissSeg o = g_glyph_defs[gi].segs[s];
				o.u0 += g_off_du[oi]; o.u1 += g_off_du[oi];
				o.v0 += g_off_dv[oi]; o.v1 += g_off_dv[oi];
				tmp[s] = o;
			}
			AissGlyphInk gk = { tmp, ns };
			aiss_build(&b->feat[gi * AISS_NOFF + oi], aiss_glyph_ink, &gk);
		}
	}
}

// Best-matching glyph for a cell feature. `noff` variants per glyph are tried
// and the closest (best-aligned) is taken (paper Eq. 1, minimized over the set
// and over the allowed translations). The winning distance -- the cell's
// contribution to the objective energy E (paper Eq. 6) -- is returned in
// `*out_d` when non-NULL.
static char aiss_match_ex(const AissBook* b, const AissFeature* cell, int noff, float* out_d){
	float best = FLT_MAX;
	char  bc = ' ';
	for (int gi = 0; gi < b->n; gi++){
		const AissFeature* var = &b->feat[gi * AISS_NOFF];
		for (int oi = 0; oi < noff; oi++){
			float d = aiss_distance(cell, &var[oi]);
			if (d < best){ best = d; bc = b->ch[gi]; }
		}
	}
	if (out_d) *out_d = best;
	return bc;
}

// Full translation search (image path, one-shot: quality over speed).
static inline char aiss_match(const AissBook* b, const AissFeature* cell){
	return aiss_match_ex(b, cell, AISS_NOFF, NULL);
}

// Center + axis offsets only (3D path: cheaper, for real-time matching).
static inline char aiss_match_fast(const AissBook* b, const AissFeature* cell){
	return aiss_match_ex(b, cell, AISS_NAXIS, NULL);
}

#endif
