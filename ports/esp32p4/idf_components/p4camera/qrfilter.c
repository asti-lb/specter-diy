/*
 * Image filters for the second and third QR scan attempts, see qrfilter.h.
 */
#include "qrfilter.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------
 * 16-byte vector operations.
 *
 * On the P4 these are single PIE instructions on the q0..q7 registers. The
 * compiler never allocates q registers, and FreeRTOS saves them on a task
 * switch, so values stay in them from one asm statement to the next.
 * ---------------------------------------------------------------------- */
#ifdef ESP_PLATFORM

#include "esp_heap_caps.h"

/* PIE instructions accept only x8-x15 and x24-x31 as integer operands, so
 * the address and the shift amount are pinned to a4 / a5. */
#define PIE_P(insn, q, p) do { register const void *_p __asm__("a4") = (p); \
    __asm__ volatile(insn " q" #q ", %0, 0" :: "r"(_p) : "memory"); } while (0)
#define VLD(q, p)       PIE_P("esp.vld.128.ip", q, p)
#define VST(q, p)       PIE_P("esp.vst.128.ip", q, p)
/* all 16 lanes = the byte at p */
#define VBC(q, p)       PIE_P("esp.vldbc.8.ip", q, p)
#define VMAX(d, a, b)   __asm__ volatile("esp.vmax.u8 q" #d ", q" #a ", q" #b)
#define VMIN(d, a, b)   __asm__ volatile("esp.vmin.u8 q" #d ", q" #a ", q" #b)
/* shift amount, in bytes, for VSRC */
#define VSAR(k)         do { register int _k __asm__("a5") = (k); \
    __asm__ volatile("esp.movx.w.sar.bytes %0" :: "r"(_k)); } while (0)
/* d = bytes sar .. sar+15 of the 32 bytes a (low) followed by b (high) */
#define VSRC(d, a, b)   __asm__ volatile("esp.src.q q" #d ", q" #a ", q" #b)
/* lanes where a < b become 0xff, the others 0 */
#define VCMPLT(d, a, b) __asm__ volatile("esp.vcmp.lt.u8 q" #d ", q" #a ", q" #b)
#define VNOT(d, a)      __asm__ volatile("esp.notq q" #d ", q" #a)

static void *aligned_buf(size_t n) {
    return heap_caps_aligned_alloc(16, n, MALLOC_CAP_8BIT);
}

#else /* emulation for checks on a PC */

static uint8_t Q[8][16];
static int SAR;

static void emu_minmax(uint8_t *d, const uint8_t *a, const uint8_t *b, int mx) {
    for (int i = 0; i < 16; ++i) d[i] = mx ? (a[i] > b[i] ? a[i] : b[i]) : (a[i] < b[i] ? a[i] : b[i]);
}
static void emu_src(uint8_t *d, const uint8_t *a, const uint8_t *b) {
    uint8_t cat[32];
    memcpy(cat, a, 16);
    memcpy(cat + 16, b, 16);
    memcpy(d, cat + SAR, 16);
}
static void emu_cmplt(uint8_t *d, const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 16; ++i) d[i] = a[i] < b[i] ? 0xff : 0;
}
static void emu_not(uint8_t *d, const uint8_t *a) {
    for (int i = 0; i < 16; ++i) d[i] = (uint8_t)~a[i];
}

#define VLD(q, p)       memcpy(Q[q], (p), 16)
#define VST(q, p)       memcpy((p), Q[q], 16)
#define VBC(q, p)       memset(Q[q], *(const uint8_t *)(p), 16)
#define VMAX(d, a, b)   emu_minmax(Q[d], Q[a], Q[b], 1)
#define VMIN(d, a, b)   emu_minmax(Q[d], Q[a], Q[b], 0)
#define VSAR(k)         (SAR = (k))
#define VSRC(d, a, b)   emu_src(Q[d], Q[a], Q[b])
#define VCMPLT(d, a, b) emu_cmplt(Q[d], Q[a], Q[b])
#define VNOT(d, a)      emu_not(Q[d], Q[a])

static void *aligned_buf(size_t n) {
    return aligned_alloc(16, (n + 15) & ~(size_t)15);
}

#endif

/* ------------------------------------------------------------------------
 * Closing + tile threshold, streaming
 * ---------------------------------------------------------------------- */

/* dst[x] = max (or min) of p[x-r .. x+r], r = 1 or 2. p has 16 neutral
 * bytes before and after its w bytes. */
static void hfilt(uint8_t *dst, const uint8_t *p, int w, int r, int dilate) {
    for (int x = 0; x < w; x += 16) {
        VLD(0, p + x - 16); VLD(1, p + x); VLD(2, p + x + 16);
        VSAR(15); VSRC(3, 0, 1);   /* p[x-1 ..] */
        VSAR(1);  VSRC(4, 1, 2);   /* p[x+1 ..] */
        if (r == 2) {
            VSAR(14); VSRC(5, 0, 1);   /* p[x-2 ..] */
            VSAR(2);  VSRC(6, 1, 2);   /* p[x+2 ..] */
            if (dilate) { VMAX(3, 3, 5); VMAX(4, 4, 6); } else { VMIN(3, 3, 5); VMIN(4, 4, 6); }
        }
        if (dilate) { VMAX(3, 3, 4); VMAX(3, 3, 1); } else { VMIN(3, 3, 4); VMIN(3, 3, 1); }
        VST(3, dst + x);
    }
}

/* dst = column-wise max (or min) of rows r[0 .. n-1], n = 3 or 5 */
static void vfilt(uint8_t *dst, const uint8_t *const r[5], int n, int w, int dilate) {
    for (int x = 0; x < w; x += 16) {
        VLD(0, r[0] + x); VLD(1, r[1] + x); VLD(2, r[2] + x);
        if (n == 5) {
            VLD(3, r[3] + x); VLD(4, r[4] + x);
            if (dilate) { VMAX(0, 0, 3); VMAX(1, 1, 4); } else { VMIN(0, 0, 3); VMIN(1, 1, 4); }
        }
        if (dilate) { VMAX(0, 0, 1); VMAX(0, 0, 2); } else { VMIN(0, 0, 1); VMIN(0, 0, 2); }
        VST(0, dst + x);
    }
}

static size_t round16(size_t n) {
    return (n + 15) & ~(size_t)15;
}

size_t qrf_close_work_size(int w, int h, int tile) {
    size_t bw = ((size_t)w + tile - 1) / tile, bh = ((size_t)h + tile - 1) / tile;
    return 2 * ((size_t)w + 32) + 16 * (size_t)w + round16(3 * bw * bh);
}

/* Midpoint of the darkest and lightest pixel in the 5x5 tile neighbourhood;
 * shared by both implementations. */
static void tile_thresholds(const uint8_t *mn, const uint8_t *mx, uint8_t *th, int bw, int bh) {
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            int lo = 255, hi = 0;
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    int yy = by + dy, xx = bx + dx;
                    if (yy < 0 || yy >= bh || xx < 0 || xx >= bw) continue;
                    if (mn[yy * bw + xx] < lo) lo = mn[yy * bw + xx];
                    if (mx[yy * bw + xx] > hi) hi = mx[yy * bw + xx];
                }
            }
            th[by * bw + bx] = hi - lo < QRF_MIN_CONTRAST ? (uint8_t)lo : (uint8_t)((lo + hi + 1) / 2);
        }
    }
}

/*
 * One pass over the rows, t = 0 .. h + 2r - 1 (r = radius):
 *   H(t)    = horizontal max of input row t                 (ring A)
 *   E(t-r)  = horizontal min of the vertical max of H(t-2r .. t)  (ring B)
 *   O(t-2r) = vertical min of E(t-3r .. t-r) -> written back to row t-2r
 * Row t is read before row t-2r is overwritten, so the image is filtered in
 * place. Outside the image the dilation sees 0 and the erosion 255, as in
 * the reference. Per-column minima and maxima of O are gathered on the way
 * and folded into tile minima and maxima at the end of each tile row.
 */
void qrf_close_tiles(uint8_t *img, int w, int h, int radius, int tile, uint8_t *work) {
    if (w <= 0 || h <= 0 || w % 16 || radius < 1 || radius > 2 || tile <= 0 || w % tile) return;
    const int r = radius, n = 2 * r + 1;
    const int bw = w / tile, bh = (h + tile - 1) / tile;
    uint8_t *pad_a = work, *pad_b = pad_a + w + 32;
    uint8_t *ring_a = pad_b + w + 32, *ring_b = ring_a + 5 * w;
    uint8_t *zero = ring_b + 5 * w, *ones = zero + w, *out = ones + w;
    uint8_t *acc_mn = out + w, *acc_mx = acc_mn + w, *thr = acc_mx + w;
    uint8_t *mn = thr + w, *mx = mn + bw * bh, *th = mx + bw * bh;
    const uint8_t *rows[5];

    memset(pad_a, 0, 16);
    memset(pad_a + 16 + w, 0, 16);
    memset(pad_b, 255, 16);
    memset(pad_b + 16 + w, 255, 16);
    memset(zero, 0, w);
    memset(ones, 255, w);

    for (int t = 0; t < h + 2 * r; ++t) {
        if (t < h) {
            memcpy(pad_a + 16, img + (size_t)t * w, w);
            hfilt(ring_a + (t % 5) * w, pad_a + 16, w, r, 1);
        }
        int e = t - r;
        if (e >= 0 && e < h) {
            for (int i = 0; i < n; ++i) {
                int s = e - r + i;
                rows[i] = (s < 0 || s >= h) ? zero : ring_a + (s % 5) * w;
            }
            vfilt(pad_b + 16, rows, n, w, 1);
            hfilt(ring_b + (e % 5) * w, pad_b + 16, w, r, 0);
        }
        int y = t - 2 * r;
        if (y < 0) continue;
        for (int i = 0; i < n; ++i) {
            int s = y - r + i;
            rows[i] = (s < 0 || s >= h) ? ones : ring_b + (s % 5) * w;
        }
        vfilt(out, rows, n, w, 0);

        if (y % tile == 0) {
            memset(acc_mn, 255, w);
            memset(acc_mx, 0, w);
        }
        for (int x = 0; x < w; x += 16) {
            VLD(0, out + x);
            VLD(1, acc_mn + x); VMIN(1, 1, 0); VST(1, acc_mn + x);
            VLD(2, acc_mx + x); VMAX(2, 2, 0); VST(2, acc_mx + x);
        }
        if (y % tile == tile - 1 || y == h - 1) {
            int ty = y / tile;
            for (int bx = 0; bx < bw; ++bx) {
                uint8_t lo = 255, hi = 0;
                for (int i = bx * tile; i < (bx + 1) * tile; ++i) {
                    lo = acc_mn[i] < lo ? acc_mn[i] : lo;
                    hi = acc_mx[i] > hi ? acc_mx[i] : hi;
                }
                mn[ty * bw + bx] = lo;
                mx[ty * bw + bx] = hi;
            }
        }
        memcpy(img + (size_t)y * w, out, w);
    }

    tile_thresholds(mn, mx, th, bw, bh);

    for (int y = 0; y < h; ++y) {
        if (y % tile == 0) {
            const uint8_t *bt = th + (y / tile) * bw;
            for (int bx = 0; bx < bw; ++bx) memset(thr + bx * tile, bt[bx], tile);
        }
        uint8_t *row = img + (size_t)y * w;
        for (int x = 0; x < w; x += 16) {
            VLD(0, row + x);
            VLD(1, thr + x);
            VCMPLT(2, 0, 1);
            VNOT(2, 2);
            VST(2, row + x);
        }
    }
}

/* ------------------------------------------------------------------------
 * Local mean threshold, streaming
 * ---------------------------------------------------------------------- */

size_t qrf_local_work_size(int w, int r) {
    return round16((size_t)(2 * r + 2) * w) + round16(sizeof(uint32_t) * (size_t)w);
}

/*
 * Column sums over the rows of the current window, updated by one row in and
 * one row out per output row; then a running sum along the row. The output
 * replaces row y while rows up to y + r are still needed as input, so every
 * row is copied into a ring of 2r + 2 rows when it enters the window and
 * taken from there when it leaves. The division by the window size is
 * avoided: p * n * 100 < sum * (100 - t) is the reference's own test.
 */
void qrf_local_mean(uint8_t *img, int w, int h, int r, int t, uint8_t *work) {
    if (w <= 0 || h <= 0 || r < 1) return;
    const int ring_n = 2 * r + 2;
    uint8_t *ring = work;
    uint32_t *col = (uint32_t *)(work + round16((size_t)ring_n * w));
    const uint32_t keep = (uint32_t)(100 - t);

    memset(col, 0, sizeof(uint32_t) * (size_t)w);
    /* rows 0 .. r-1 enter before the first output row */
    for (int q = 0; q < r && q < h; ++q) {
        uint8_t *dst = ring + (size_t)(q % ring_n) * w;
        memcpy(dst, img + (size_t)q * w, w);
        for (int x = 0; x < w; ++x) col[x] += dst[x];
    }
    for (int y = 0; y < h; ++y) {
        int in = y + r, out = y - r - 1;
        if (in < h) {
            uint8_t *dst = ring + (size_t)(in % ring_n) * w;
            memcpy(dst, img + (size_t)in * w, w);
            for (int x = 0; x < w; ++x) col[x] += dst[x];
        }
        if (out >= 0) {
            const uint8_t *src = ring + (size_t)(out % ring_n) * w;
            for (int x = 0; x < w; ++x) col[x] -= src[x];
        }
        int y0 = y - r < 0 ? 0 : y - r, y1 = y + r + 1 > h ? h : y + r + 1;
        uint32_t ny100 = (uint32_t)(y1 - y0) * 100;
        const uint8_t *src = ring + (size_t)(y % ring_n) * w;
        uint8_t *dst = img + (size_t)y * w;
        uint32_t sum = 0;
        for (int x = 0; x < r && x < w; ++x) sum += col[x];
        for (int x = 0; x < w; ++x) {
            if (x + r < w) sum += col[x + r];
            if (x - r - 1 >= 0) sum -= col[x - r - 1];
            int x0 = x - r < 0 ? 0 : x - r, x1 = x + r + 1 > w ? w : x + r + 1;
            uint32_t n100 = (uint32_t)(x1 - x0) * ny100;
            dst[x] = (uint32_t)src[x] * n100 < sum * keep ? 0 : 255;
        }
    }
}

/* ------------------------------------------------------------------------
 * Reference implementations (the code of qr-lab/qrlab.c and of the
 * previous firmware)
 * ---------------------------------------------------------------------- */

static void op_span(uint8_t *dst, const uint8_t *a, const uint8_t *b, size_t n, int dilate) {
    if (dilate) {
        for (size_t i = 0; i < n; ++i) dst[i] = a[i] > b[i] ? a[i] : b[i];
    } else {
        for (size_t i = 0; i < n; ++i) dst[i] = a[i] < b[i] ? a[i] : b[i];
    }
}

/* v[i] = op(v[i], v[i+step], ..., v[i+(k-1)*step]), by doubling */
static void reduce(uint8_t *v, size_t n, size_t step, int k, int dilate) {
    int p = 1;
    for (; p * 2 <= k; p *= 2) {
        op_span(v, v, v + (size_t)p * step, (n - p) * step, dilate);
    }
    if (p < k) {
        op_span(v, v, v + (size_t)(k - p) * step, (n - (k - p)) * step, dilate);
    }
}

static void filter_ref(uint8_t *img, int w, int h, int radius, uint8_t *pad, uint8_t *line, int dilate) {
    const int k = 2 * radius + 1, r0 = radius, padn = k - 1;
    const uint8_t neutral = dilate ? 0 : 255;
    for (int y = 0; y < h; ++y) {
        memset(line, neutral, (size_t)w + padn);
        memcpy(line + r0, img + (size_t)y * w, w);
        reduce(line, (size_t)w + padn, 1, k, dilate);
        memcpy(img + (size_t)y * w, line, w);
    }
    memset(pad, neutral, (size_t)w * (h + padn));
    memcpy(pad + (size_t)r0 * w, img, (size_t)w * h);
    reduce(pad, (size_t)h + padn, w, k, dilate);
    memcpy(img, pad, (size_t)w * h);
}

void qrf_close_ref(uint8_t *img, int w, int h, int radius, uint8_t *pad, uint8_t *line) {
    filter_ref(img, w, h, radius, pad, line, 1);
    filter_ref(img, w, h, radius, pad, line, 0);
}

void qrf_tiles_ref(uint8_t *img, int w, int h, int tile, uint8_t *tiles) {
    int bw = (w + tile - 1) / tile, bh = (h + tile - 1) / tile;
    uint8_t *mn = tiles, *mx = mn + bw * bh, *th = mx + bw * bh;
    memset(mn, 255, (size_t)bw * bh);
    memset(mx, 0, (size_t)bw * bh);
    for (int y = 0; y < h; ++y) {
        uint8_t *bmn = mn + (y / tile) * bw, *bmx = mx + (y / tile) * bw;
        const uint8_t *row = img + (size_t)y * w;
        for (int bx = 0, x0 = 0; bx < bw; ++bx, x0 += tile) {
            int x1 = x0 + tile < w ? x0 + tile : w;
            uint8_t lo = bmn[bx], hi = bmx[bx];
            for (int x = x0; x < x1; ++x) {
                uint8_t p = row[x];
                lo = p < lo ? p : lo;
                hi = p > hi ? p : hi;
            }
            bmn[bx] = lo;
            bmx[bx] = hi;
        }
    }
    tile_thresholds(mn, mx, th, bw, bh);
    for (int y = 0; y < h; ++y) {
        uint8_t *row = img + (size_t)y * w;
        const uint8_t *bt = th + (y / tile) * bw;
        for (int bx = 0, x0 = 0; bx < bw; ++bx, x0 += tile) {
            int x1 = x0 + tile < w ? x0 + tile : w;
            uint8_t t = bt[bx];
            for (int x = x0; x < x1; ++x) row[x] = row[x] < t ? 0 : 255;
        }
    }
}

int qrf_local_mean_ref(uint8_t *img, int w, int h, int r, int t) {
    uint32_t *ii = malloc(sizeof(uint32_t) * (size_t)(w + 1) * (h + 1));
    if (!ii) return -1;
    for (int x = 0; x <= w; x++) ii[x] = 0;
    for (int y = 1; y <= h; y++) {
        uint32_t row = 0;
        ii[(size_t)y * (w + 1)] = 0;
        for (int x = 1; x <= w; x++) {
            row += img[(y - 1) * w + x - 1];
            ii[(size_t)y * (w + 1) + x] = ii[(size_t)(y - 1) * (w + 1) + x] + row;
        }
    }
    for (int y = 0; y < h; y++) {
        int y0 = y - r < 0 ? 0 : y - r, y1 = y + r + 1 > h ? h : y + r + 1;
        for (int x = 0; x < w; x++) {
            int x0 = x - r < 0 ? 0 : x - r, x1 = x + r + 1 > w ? w : x + r + 1;
            uint32_t sum = ii[(size_t)y1 * (w + 1) + x1] - ii[(size_t)y0 * (w + 1) + x1]
                         - ii[(size_t)y1 * (w + 1) + x0] + ii[(size_t)y0 * (w + 1) + x0];
            uint32_t n = (uint32_t)(x1 - x0) * (y1 - y0);
            img[y * w + x] = (uint32_t)img[y * w + x] * n * 100 < sum * (uint32_t)(100 - t) ? 0 : 255;
        }
    }
    free(ii);
    return 0;
}

/* ------------------------------------------------------------------------
 * Self-test
 * ---------------------------------------------------------------------- */

static uint32_t lcg(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s >> 24;
}

/* Single instructions first, so a failure names the one that behaves
 * differently from the assumption (codes 101..104). */
static int test_ops(uint8_t *buf) {
    uint8_t *a = buf, *b = buf + 16, *d = buf + 32;
    for (int i = 0; i < 16; ++i) {
        a[i] = (uint8_t)(i * 17 + 3);
        b[i] = (uint8_t)(200 - i * 9);
    }
    VLD(0, a); VLD(1, b);
    for (int k = 1; k <= 15; ++k) {
        VSAR(k); VSRC(2, 0, 1); VST(2, d);
        for (int i = 0; i < 16; ++i) {
            if (d[i] != (i + k < 16 ? a[i + k] : b[i + k - 16])) return 101;
        }
    }
    VMAX(2, 0, 1); VST(2, d);
    for (int i = 0; i < 16; ++i) if (d[i] != (a[i] > b[i] ? a[i] : b[i])) return 102;
    VMIN(2, 0, 1); VST(2, d);
    for (int i = 0; i < 16; ++i) if (d[i] != (a[i] < b[i] ? a[i] : b[i])) return 102;
    VCMPLT(2, 0, 1); VNOT(2, 2); VST(2, d);
    for (int i = 0; i < 16; ++i) if (d[i] != (a[i] < b[i] ? 0 : 255)) return 103;
    VBC(2, b + 5); VST(2, d);
    for (int i = 0; i < 16; ++i) if (d[i] != b[5]) return 104;
    return 0;
}

/* kind 0: closing + tiles (radius, tile), kind 1: local mean (radius) */
static int test_image(int kind, int w, int h, int radius, int tile, uint32_t seed) {
    size_t n = (size_t)w * h;
    size_t ws = kind ? qrf_local_work_size(w, radius) : qrf_close_work_size(w, h, tile);
    int bw = (w + tile - 1) / tile, bh = (h + tile - 1) / tile;
    uint8_t *a = aligned_buf(n), *b = aligned_buf(n), *work = aligned_buf(ws);
    uint8_t *pad = malloc((size_t)w * (h + 2 * radius)), *line = malloc((size_t)w + 2 * radius);
    uint8_t *tiles = malloc(3 * (size_t)bw * bh);
    int bad = !a || !b || !work || !pad || !line || !tiles;
    if (!bad) {
        /* checkerboard of varying size with noise, plus a flat patch */
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int v = ((x / 3 + y / 4 + (x * y) / 97) & 1) ? 190 : 50;
                v += (int)(lcg(&seed) % 41) - 20;
                if (x > w / 2 && y < h / 3) v = 128;
                a[(size_t)y * w + x] = (uint8_t)v;
            }
        }
        memcpy(b, a, n);
        if (kind) {
            bad = qrf_local_mean_ref(a, w, h, radius, 15) != 0;
            qrf_local_mean(b, w, h, radius, 15, work);
        } else {
            qrf_close_ref(a, w, h, radius, pad, line);
            qrf_tiles_ref(a, w, h, tile, tiles);
            qrf_close_tiles(b, w, h, radius, tile, work);
        }
        bad = bad || memcmp(a, b, n) != 0;
    }
    free(a); free(b); free(work); free(pad); free(line); free(tiles);
    return bad;
}

int qrf_selftest(void) {
    uint8_t *buf = aligned_buf(48);
    if (!buf) return 100;
    int r = test_ops(buf);
    free(buf);
    if (r) return r;
    /* kind, w, h, radius, tile */
    static const int cases[][5] = {
        {0, 16, 1, 2, 16}, {0, 16, 3, 2, 16}, {0, 32, 5, 2, 16}, {0, 48, 37, 2, 16},
        {0, 64, 64, 2, 16}, {0, 96, 50, 2, 16},
        {0, 16, 1, 1, 8}, {0, 32, 7, 1, 8}, {0, 48, 37, 1, 8}, {0, 96, 50, 1, 8},
        {0, 64, 40, 2, 8}, {0, 64, 40, 1, 16},
        {1, 16, 1, 3, 16}, {1, 32, 9, 3, 16}, {1, 48, 37, 5, 16}, {1, 64, 64, 24, 16},
        {1, 96, 70, 24, 16},
    };
    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); ++i) {
        const int *c = cases[i];
        if (test_image(c[0], c[1], c[2], c[3], c[4], 12345u + i)) return i + 1;
    }
    return 0;
}
