/*
 * at9enc - a small ATRAC9 encoder (48 kHz, stereo, 256-sample frames, 4 frames per superframe).
 *
 * The bitstream syntax and the code tables come from the ATRAC9 decoder of FFmpeg (Rostislav Pehlivanov, LGPL 2.1+,
 * libavcodec/atrac9dec.c and atrac9tab.h); this encoder produces what that decoder reads, with the plain features only:
 * independent channels (no intensity stereo), no band extension, scale factors coded as VLC deltas, one spectral
 * precision offset per frame found by rate control.
 *
 * usage: at9enc input.wav output.at9 [bytes_per_superframe]      (input: 48 kHz, 16 bit, stereo PCM WAV)
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "at9tab_enc.h"

#define N 256				/* coefficients (and hop) per frame */
#define FRAMES_PER_SF 4
#define MAXU 30

/* ---------------- bit writer ---------------- */
typedef struct { uint8_t *b; int64_t pos; int64_t cap; } BW;

static void bw_put(BW *w, int n, uint32_t v)
{
	for (int i = n - 1; i >= 0; i--)
	{
		if (w->pos >= w->cap) { w->pos++; continue; }	/* overflow: only the length is tracked */
		if ((v >> i) & 1) w->b[w->pos >> 3] |= 0x80 >> (w->pos & 7); else w->b[w->pos >> 3] &= ~(0x80 >> (w->pos & 7));
		w->pos++;
	}
}
static void bw_align(BW *w) { while (w->pos & 7) bw_put(w, 1, 0); }

/* ---------------- tables built once ---------------- */
typedef struct { uint32_t code; uint8_t len; int sym; } Code;

static Code sfA[7][64];			/* unsigned scale factor delta codes, [len][symbol] */
static int sfAn[7];

typedef struct { int n; int sym[256]; uint32_t code[256]; uint8_t len[256]; int vals[256][8]; } CoefBook;
static CoefBook cbook[2][8][4];
static int cbValid[2][8][4];

static void build_codes(const uint8_t (*tab)[2], int n, Code *out)
{
	uint64_t code = 0;
	for (int i = 0; i < n; i++)
	{
		const int len = tab[i][1];
		out[i].sym = tab[i][0];
		out[i].len = len;
		out[i].code = (uint32_t)(code >> (32 - len));
		code += 1ULL << (32 - len);
	}
}

static int sext(int v, int bits) { return (v << (32 - bits)) >> (32 - bits); }

static void init_tables(void)
{
	/* scale factor VLCs: consecutive blocks of the sfb_a table, len 1..6 */
	const uint8_t (*tab)[2] = at9_sfb_a_tab;
	for (int i = 1; i < 7; i++)
	{
		const int sz = at9_huffman_sf_unsigned[i].size;
		build_codes(tab, sz, sfA[i]);
		sfAn[i] = sz;
		tab += sz;
	}

	/* spectral codebooks */
	const uint8_t (*ct)[2] = at9_coeffs_tab;
	for (int i = 0; i < 2; i++)
		for (int j = 2; j < 8; j++)
			for (int k = i; k < 4; k++)
			{
				const HuffmanCodebook *hf = &at9_huffman_coeffs[i][j][k];
				CoefBook *cb = &cbook[i][j][k];
				cb->n = hf->size;
				Code tmp[256];
				build_codes(ct, hf->size, tmp);
				for (int e = 0; e < hf->size; e++)
				{
					cb->sym[e] = tmp[e].sym; cb->code[e] = tmp[e].code; cb->len[e] = tmp[e].len;
					int v = tmp[e].sym;
					for (int q = 0; q < hf->value_cnt; q++)
					{
						cb->vals[e][q] = sext(v & ((1 << hf->value_bits) - 1), hf->value_bits);
						v >>= hf->value_bits;
					}
				}
				cbValid[i][j][k] = 1;
				ct += hf->size;
			}
}

/* ---------------- block parameters ---------------- */
typedef struct
{
	int band_count, qcnt;
	int gval;				/* gradient value at the high end, 0..31 */
	int gv0;				/* gradient value at the low end (>= gval) */
	int gradient[MAXU + 1];
	int gmode, gstart, gbound;		/* gradient mode (0 or 1), start unit (mode 1), boundary */
	int sf[2][MAXU + 1];
	int pc[2][MAXU], pf[2][MAXU], cbs[2][MAXU];
	float coef[2][N];
} Blk;

static void calc_gradient(Blk *b)
{
	uint8_t curve[32];
	int r0, r1, v0, v1;
	if (b->gmode == 1) { r0 = b->gstart; r1 = 31; v0 = b->gval; v1 = 31; }
	else { r0 = 0; r1 = 31; v0 = b->gv0; v1 = b->gval; }
	for (int j = 0; j < r1 - r0; j++) curve[j] = at9_tab_b_dist[(j * 48) / (r1 - r0)];
	const int values = v1 - v0;
	const int sign = 1 - 2 * (values < 0);
	const int base = v0 + sign;
	const float scale = (abs(values) - 1) / 31.0f;
	for (int i = 0; i <= b->qcnt; i++) b->gradient[i] = (i >= r0) ? v1 : v0;
	for (int i = r0; i < r1 && i <= MAXU; i++) b->gradient[i] = base + sign * ((int)(scale * curve[i - r0]));
}

static void calc_precision(Blk *b, int ch)
{
	int mask[MAXU + 1];
	memset(mask, 0, sizeof(mask));
	for (int i = 1; i < b->qcnt; i++)
	{
		const int delta = abs(b->sf[ch][i] - b->sf[ch][i - 1]) - 1;
		if (delta > 0)
		{
			const int neg = b->sf[ch][i - 1] > b->sf[ch][i];
			mask[i - neg] += delta < 5 ? delta : 5;
		}
	}
	for (int i = 0; i < b->qcnt; i++)
	{
		int p;
		if (b->gmode == 1)
		{
			p = b->sf[ch][i] + mask[i] - b->gradient[i];
			if (p >= 0) p >>= 1;
		}
		else p = b->sf[ch][i] - b->gradient[i];
		if (p < 1) p = 1;
		if (i < b->gbound) p++;
		b->pf[ch][i] = 0;
		if (p > 15) { b->pf[ch][i] = (p < 30 ? p : 30) - 15; p = 15; }
		b->pc[ch][i] = p;
	}
}

/* same as the decoder's calc_codebook_idx */
static void calc_codebook(Blk *b, int ch)
{
	int sf[MAXU + 2];
	memcpy(sf, b->sf[ch], sizeof(int) * (MAXU + 1));
	const int q = b->qcnt;
	memset(b->cbs[ch], 0, sizeof(b->cbs[ch]));
	if (q <= 1) return;
	sf[q] = sf[q - 1];
	int avg = 0;
	if (q > 12)
	{
		for (int i = 0; i < 12; i++) avg += sf[i];
		avg = (avg + 6) / 12;
	}
	for (int i = 8; i < q; i++)
	{
		const int prev = sf[i - 1], cur = sf[i], next = sf[i + 1];
		const int mn = prev < next ? prev : next;
		if ((cur - mn >= 3 || 2 * cur - prev - next >= 3))
			b->cbs[ch][i] = 1;
	}
	for (int i = 12; i < q; i++)
	{
		const int cur = sf[i];
		const int cnd = at9_q_unit_to_coeff_cnt[i] == 16;
		const int a = sf[i + 1], c = sf[i - 1];
		const int mn = a < c ? a : c;
		if (b->cbs[ch][i]) continue;
		b->cbs[ch][i] = (((cur - mn) >= 2) && (cur >= (avg - cnd)));
	}
}

/* writes the scale factors of one channel (mode 0: VLC delta offset), picking the cheapest weights/length */
static void write_sf(BW *w, Blk *b, int ch, int dry, int *bits_out)
{
	const int q = b->qcnt;
	int bestBits = 1 << 30, bestW = 0, bestLen = 3, bestBase = 0;
	for (int wi = 0; wi < 8; wi++)
	{
		int mn = 1000, mx = -1000, t[MAXU];
		for (int i = 0; i < q; i++) { t[i] = b->sf[ch][i] + at9_tab_sf_weights[wi][i]; if (t[i] < mn) mn = t[i]; if (t[i] > mx) mx = t[i]; }
		int base = mn > 31 ? 31 : mn;
		for (int len = 3; len <= 6; len++)
		{
			int ok = 1, bits = 2 + 3 + 5 + 2 + len;
			int prev = 0;
			for (int i = 0; i < q; i++)
			{
				const int v = t[i] - base;
				if (v < 0 || v >= (1 << len)) { ok = 0; break; }
				if (i > 0) bits += sfA[len][(v - prev) & ((1 << len) - 1)].len;
				prev = v;
			}
			/* the decoder looks the delta up by symbol: find its code */
			if (ok && bits < bestBits) { bestBits = bits; bestW = wi; bestLen = len; bestBase = base; }
		}
	}
	(void)dry;
	/* write */
	int t[MAXU];
	for (int i = 0; i < q; i++) t[i] = b->sf[ch][i] + at9_tab_sf_weights[bestW][i] - bestBase;
	bw_put(w, 2, 0);
	bw_put(w, 3, bestW);
	bw_put(w, 5, bestBase);
	bw_put(w, 2, bestLen - 3);
	bw_put(w, bestLen, t[0]);
	for (int i = 1; i < q; i++)
	{
		const int d = (t[i] - t[i - 1]) & ((1 << bestLen) - 1);
		int found = -1;
		for (int e = 0; e < sfAn[bestLen]; e++) if (sfA[bestLen][e].sym == d) { found = e; break; }
		if (found < 0) { fprintf(stderr, "scale factor delta %d not in table len %d\n", d, bestLen); exit(1); }
		bw_put(w, sfA[bestLen][found].len, sfA[bestLen][found].code);
	}
	*bits_out = bestBits;
}

static void write_coeffs(BW *w, Blk *b, int ch)
{
	for (int i = 0; i < b->qcnt; i++)
	{
		const int start = at9_q_unit_to_coeff_idx[i], cnt = at9_q_unit_to_coeff_cnt[i];
		const float scale = at9_scalefactor_c[b->sf[ch][i]];
		const int pc = b->pc[ch][i];
		const float stepc = at9_quant_step_coarse[pc];
		const int prec = pc + 1;

		if (prec <= 7)
		{
			const int cb = b->cbs[ch][i], cbi = at9_q_unit_to_codebookidx[i];
			CoefBook *bk = &cbook[cb][prec][cbi];
			const HuffmanCodebook *hf = &at9_huffman_coeffs[cb][prec][cbi];
			const int vc = hf->value_cnt;
			for (int g = 0; g < cnt / vc; g++)
			{
				float target[8];
				for (int k = 0; k < vc; k++) target[k] = b->coef[ch][start + g * vc + k] / (scale * stepc);
				int best = -1; float be = 1e30f;
				for (int e = 0; e < bk->n; e++)
				{
					float err = 0;
					for (int k = 0; k < vc; k++) { const float d = bk->vals[e][k] - target[k]; err += d * d; }
					if (err < be) { be = err; best = e; }
				}
				bw_put(w, bk->len[best], bk->code[best]);
			}
		}
		else
		{
			const int lo = -(1 << pc), hi = (1 << pc) - 1;
			for (int k = 0; k < cnt; k++)
			{
				int q = (int)lrintf(b->coef[ch][start + k] / (scale * stepc));
				if (q < lo) q = lo; if (q > hi) q = hi;
				bw_put(w, prec, q & ((1 << prec) - 1));
			}
		}
	}
}

/* coarse reconstruction needed by the fine pass; recomputed here to keep write_coeffs simple */
static void write_fine(BW *w, Blk *b, int ch)
{
	for (int i = 0; i < b->qcnt; i++)
	{
		const int pf = b->pf[ch][i];
		if (pf <= 0) continue;
		const int start = at9_q_unit_to_coeff_idx[i], end = at9_q_unit_to_coeff_idx[i + 1];
		const float scale = at9_scalefactor_c[b->sf[ch][i]];
		const int pc = b->pc[ch][i];
		const float stepc = at9_quant_step_coarse[pc], stepf = at9_quant_step_fine[pf];
		const int len = pf + 1;
		const int lo = -(1 << pf), hi = (1 << pf) - 1;
		for (int j = start; j < end; j++)
		{
			const float v = b->coef[ch][j] / scale;
			int qcv = (int)lrintf(v / stepc);
			const int clo = -(1 << pc), chi = (1 << pc) - 1;
			if (qcv < clo) qcv = clo; if (qcv > chi) qcv = chi;
			int qf = (int)lrintf((v - qcv * stepc) / stepf);
			if (qf < lo) qf = lo; if (qf > hi) qf = hi;
			bw_put(w, len, qf & ((1 << len) - 1));
		}
	}
}

/* ---------------- one block ---------------- */
static const int bc_to_q[19] = { 0, 4, 8, 10, 12, 13, 14, 15, 16, 18, 20, 21, 22, 23, 24, 25, 26, 28, 30 };

static int g_tilt = 4;
static int g_gmode = 1;
static int g_bound = 12;

static void make_block(Blk *b, int gval)
{
	b->gval = gval;
	b->gv0 = gval + g_tilt > 31 ? 31 : gval + g_tilt;
	const float grad_noise = ldexpf(1.0f, gval + 1 - 15);	/* roughly the noise floor in the normalised domain */
	int cutoff = 0;
	for (int ch = 0; ch < 2; ch++)
		for (int i = 0; i < MAXU; i++)
		{
			float pk = 0;
			for (int k = at9_q_unit_to_coeff_idx[i]; k < at9_q_unit_to_coeff_idx[i + 1]; k++)
				if (fabsf(b->coef[ch][k]) > pk) pk = fabsf(b->coef[ch][k]);
			int sf = 0;
			while (sf < 31 && ldexpf(1.0f, sf - 15) < pk) sf++;
			b->sf[ch][i] = sf;
			if (pk > grad_noise * 0.25f && i + 1 > cutoff) cutoff = i + 1;
		}
	int bc = 3;
	while (bc < 18 && bc_to_q[bc] < cutoff) bc++;
	b->band_count = bc;
	b->qcnt = bc_to_q[bc];
	b->gmode = g_gmode;
	b->gstart = b->qcnt > 4 ? b->qcnt - 3 : 1;
	b->gbound = g_bound < b->qcnt ? g_bound : b->qcnt;
	calc_gradient(b);
	for (int ch = 0; ch < 2; ch++)
	{
		for (int i = b->qcnt; i <= MAXU; i++) b->sf[ch][i] = b->qcnt ? b->sf[ch][b->qcnt - 1] : 0;
		calc_precision(b, ch);
		calc_codebook(b, ch);
	}
}

static int write_block(BW *w, Blk *b, int first)
{
	const int64_t start = w->pos;
	bw_put(w, 1, first ? 0 : 1);	/* 0 = first frame of the superframe (the others carry 1, like the Sony encoder) */
	bw_put(w, 1, 0);			/* no parameter reuse */
	bw_put(w, 4, b->band_count - 3);
	bw_put(w, 4, b->band_count - 3);	/* stereo band: no intensity stereo */
	bw_put(w, 1, 0);			/* no band extension */
	bw_put(w, 2, b->gmode);			/* gradient mode */
	if (b->gmode == 1)
	{
		bw_put(w, 5, b->gstart);
		bw_put(w, 5, b->gval);
	}
	else
	{
		bw_put(w, 6, 0);
		bw_put(w, 6, 30);
		bw_put(w, 5, b->gv0);
		bw_put(w, 5, b->gval);
	}
	bw_put(w, 4, b->gbound);		/* boundary: the lowest units get one more bit of precision */
	bw_put(w, 1, 0);			/* base channel 0 */
	bw_put(w, 1, 0);			/* no intensity signs */
	bw_put(w, 1, 0);			/* no band extension data */
	for (int ch = 0; ch < 2; ch++)
	{
		int bits;
		write_sf(w, b, ch, 0, &bits);
		write_coeffs(w, b, ch);
		write_fine(w, b, ch);
	}
	bw_align(w);
	return (int)(w->pos - start);
}


/* ---------------- MDCT ---------------- */
static float cosTab[2 * N][N / 1];	/* too big for the stack as a function local */
static float awin[2 * N];

static void init_mdct(void)
{
	for (int n = 0; n < N; n++)
	{
		const double s = sin(M_PI * (n + 0.5) / (2.0 * N));
		awin[n] = (float)(s * s);			/* rising half of the analysis window */
		awin[2 * N - 1 - n] = awin[n];
	}
	for (int n = 0; n < 2 * N; n++)
		for (int k = 0; k < N; k++)
			cosTab[n][k] = (float)cos(M_PI / N * (n + 0.5 + N / 2.0) * (k + 0.5));
}

static float g_mdctScale = -256.0f;

static void mdct(const float *x /* 2N */, float *X /* N */)
{
	float t[2 * N];
	for (int n = 0; n < 2 * N; n++) t[n] = x[n] * awin[n];
	for (int k = 0; k < N; k++)
	{
		double s = 0;
		for (int n = 0; n < 2 * N; n++) s += t[n] * cosTab[n][k];
		X[k] = (float)(s * g_mdctScale);
	}
}

/* ---------------- WAV I/O ---------------- */
static int16_t *read_wav(const char *path, int *nframes)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); exit(1); }
	uint8_t hdr[12];
	if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) { fprintf(stderr, "not a WAV file\n"); exit(1); }
	int16_t *pcm = NULL;
	int ch = 0, rate = 0, bits = 0;
	for (;;)
	{
		uint8_t ck[8];
		if (fread(ck, 1, 8, f) != 8) break;
		uint32_t sz = ck[4] | ck[5] << 8 | ck[6] << 16 | (uint32_t)ck[7] << 24;
		if (!memcmp(ck, "fmt ", 4))
		{
			uint8_t fm[40]; uint32_t rd = sz < 40 ? sz : 40;
			fread(fm, 1, rd, f);
			ch = fm[2] | fm[3] << 8; rate = fm[4] | fm[5] << 8 | fm[6] << 16; bits = fm[14] | fm[15] << 8;
			if (sz > rd) fseek(f, sz - rd, SEEK_CUR);
		}
		else if (!memcmp(ck, "data", 4))
		{
			if (ch != 2 || rate != 48000 || bits != 16) { fprintf(stderr, "need 48 kHz 16 bit stereo (got %d ch %d Hz %d bit)\n", ch, rate, bits); exit(1); }
			pcm = malloc(sz);
			if (fread(pcm, 1, sz, f) != sz) { fprintf(stderr, "short data chunk\n"); }
			*nframes = sz / 4;
			break;
		}
		else fseek(f, sz + (sz & 1), SEEK_CUR);
	}
	fclose(f);
	if (!pcm) { fprintf(stderr, "no data chunk\n"); exit(1); }
	return pcm;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: %s in.wav out.at9 [bytes per superframe, default 512]\n", argv[0]); return 1; }
	int sfBytes = argc > 3 ? atoi(argv[3]) : 0;
	if (getenv("AT9_TILT")) g_tilt = atoi(getenv("AT9_TILT"));
	if (getenv("AT9_GMODE")) g_gmode = atoi(getenv("AT9_GMODE"));
	if (getenv("AT9_BOUND")) g_bound = atoi(getenv("AT9_BOUND"));
	if (getenv("AT9_MDCT_SCALE")) g_mdctScale = (float)atof(getenv("AT9_MDCT_SCALE"));

	init_tables();
	init_mdct();

	int ns;
	int16_t *pcm = read_wav(argv[1], &ns);
	const int nframes = (ns + N - 1) / N + 1;		/* frame t covers samples (t-1)N .. (t+1)N */
	const int nsf = (nframes + FRAMES_PER_SF - 1) / FRAMES_PER_SF;

	/* the home screen takes at most 2 MiB of music and 192 kb/s: the biggest superframe that fits */
	if (sfBytes <= 0)
	{
		sfBytes = (2 * 1024 * 1024 - 256) / nsf;
		if (sfBytes > 512) sfBytes = 512;
	}
	sfBytes &= ~3;
	if (sfBytes < 96) { fprintf(stderr, "the audio is too long for 2 MiB (%.1f s); cut it\n", ns / 48000.0); return 1; }

	uint8_t *out = calloc((size_t)nsf, sfBytes);
	float *x[2];
	for (int c = 0; c < 2; c++) x[c] = calloc((size_t)(nsf * FRAMES_PER_SF + 2) * N, sizeof(float));
	for (int i = 0; i < ns; i++) { x[0][N + i] = pcm[2 * i] / 32768.0f; x[1][N + i] = pcm[2 * i + 1] / 32768.0f; }
	/* x has N zero samples in front: frame t uses x[t*N .. t*N+2N) */

	static Blk blk;
	uint8_t tmp[4096];
	long totalBits = 0;
	for (int sfi = 0; sfi < nsf; sfi++)
	{
		BW w = { out + (size_t)sfi * sfBytes, 0, (int64_t)sfBytes * 8 };
		for (int fi = 0; fi < FRAMES_PER_SF; fi++)
		{
			const int t = sfi * FRAMES_PER_SF + fi;
			for (int c = 0; c < 2; c++) mdct(&x[c][(size_t)t * N], blk.coef[c]);

			const int64_t remaining = (int64_t)sfBytes * 8 - w.pos;
			const int64_t budget = remaining / (FRAMES_PER_SF - fi);
			/* smallest gradient value whose block fits the budget */
			int lo = 0, hi = 31, bestG = 31;
			while (lo <= hi)
			{
				const int mid = (lo + hi) / 2;
				make_block(&blk, mid);
				BW tw = { tmp, 0, sizeof(tmp) * 8 };
				const int bits = write_block(&tw, &blk, fi == 0);
				if (bits <= budget) { bestG = mid; hi = mid - 1; } else lo = mid + 1;
			}
			make_block(&blk, bestG);
			write_block(&w, &blk, fi == 0);
			if (w.pos > (int64_t)sfBytes * 8) { fprintf(stderr, "superframe %d overflow\n", sfi); return 1; }
		}
		totalBits += w.pos;
	}

	/* RIFF container: the same layout the Sony tool writes */
	const uint32_t dataBytes = (uint32_t)nsf * sfBytes;
	const int bitrate = (int)((double)sfBytes * 8 * 48000 / 1024);
	uint8_t hdr[200];
	int p = 0;
	memcpy(hdr + p, "RIFF", 4); p += 4; put32(hdr + p, 0); p += 4; memcpy(hdr + p, "WAVE", 4); p += 4;
	memcpy(hdr + p, "fmt ", 4); p += 4; put32(hdr + p, 52); p += 4;
	hdr[p++] = 0xFE; hdr[p++] = 0xFF;				/* WAVE_FORMAT_EXTENSIBLE */
	hdr[p++] = 2; hdr[p++] = 0;					/* channels */
	put32(hdr + p, 48000); p += 4;
	put32(hdr + p, bitrate / 8); p += 4;			/* bytes per second */
	hdr[p++] = sfBytes & 255; hdr[p++] = sfBytes >> 8;	/* block align */
	hdr[p++] = 0; hdr[p++] = 0;					/* bits per sample */
	hdr[p++] = 34; hdr[p++] = 0;					/* cbSize */
	hdr[p++] = 0; hdr[p++] = 4;					/* samples per block 1024 */
	put32(hdr + p, 3); p += 4;					/* channel mask */
	static const uint8_t guid[16] = { 0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d, 0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c };
	memcpy(hdr + p, guid, 16); p += 16;
	put32(hdr + p, 1); p += 4;					/* ATRAC9 version */
	/* config word: 0xFE, sample rate index 7, block config 2 (stereo), 0, avg frame size - 1 (11 bits), superframe idx 2 */
	{
		uint32_t cfg = 0; int bp = 0;
		uint8_t cb[4] = { 0, 0, 0, 0 };
		#define PUT(nb, v) do { for (int _i = (nb) - 1; _i >= 0; _i--) { if (((v) >> _i) & 1) cb[bp >> 3] |= 0x80 >> (bp & 7); bp++; } } while (0)
		PUT(8, 0xFE); PUT(4, 7); PUT(3, 2); PUT(1, 0); PUT(11, sfBytes / 4 - 1); PUT(2, 2);
		(void)cfg;
		memcpy(hdr + p, cb, 4); p += 4;
	}
	put32(hdr + p, 0); p += 4;					/* reserved */
	memcpy(hdr + p, "fact", 4); p += 4; put32(hdr + p, 12); p += 4;
	put32(hdr + p, ns); p += 4; put32(hdr + p, 256); p += 4; put32(hdr + p, 256); p += 4;
	memcpy(hdr + p, "smpl", 4); p += 4; put32(hdr + p, 60); p += 4;
	put32(hdr + p, 0); p += 4; put32(hdr + p, 0); p += 4; put32(hdr + p, 20833); p += 4; put32(hdr + p, 60); p += 4;
	put32(hdr + p, 0); p += 4; put32(hdr + p, 0); p += 4; put32(hdr + p, 0); p += 4; put32(hdr + p, 1); p += 4; put32(hdr + p, 24); p += 4;
	put32(hdr + p, 0); p += 4; put32(hdr + p, 0); p += 4; put32(hdr + p, 256); p += 4;			/* loop: id, type, start */
	put32(hdr + p, ns + 255); p += 4; put32(hdr + p, 0); p += 4; put32(hdr + p, 0); p += 4;			/* end, fraction, play count */
	memcpy(hdr + p, "data", 4); p += 4; put32(hdr + p, dataBytes); p += 4;
	put32(hdr + 4, (uint32_t)p - 8 + dataBytes);

	FILE *fo = fopen(argv[2], "wb");
	if (!fo) { perror(argv[2]); return 1; }
	fwrite(hdr, 1, p, fo);
	fwrite(out, 1, dataBytes, fo);
	fclose(fo);
	fprintf(stderr, "%d samples, %d superframes of %d bytes (%d kb/s), %u bytes\n", ns, nsf, sfBytes, bitrate / 1000, (unsigned)(p + dataBytes));
	return 0;
}
