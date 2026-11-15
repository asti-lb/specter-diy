/*
 * Grid-line removal and local threshold for the second QR scan attempt,
 * see qrfilter.h.
 */
#include "qrfilter.h"

#include <stdlib.h>
#include <string.h>

#if QRF_CLOSE != 5 || QRF_TILE != 16
#error "the streaming filter is written for a 5-pixel window and 16-pixel tiles"
#endif

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
 * Streaming implementation
 * ---------------------------------------------------------------------- */

/* dst[x] = max (or min) of p[x-2 .. x+2]. p has 16 neutral bytes before
 * and after its w bytes. */
static void hfilt_max(uint8_t *dst, const uint8_t *p, int w) {
    for (int x = 0; x < w; x += 16) {
        VLD(0, p + x - 16); VLD(1, p + x); VLD(2, p + x + 16);
        VSAR(14); VSRC(3, 0, 1);
        VSAR(15); VSRC(4, 0, 1);
        VSAR(1);  VSRC(5, 1, 2);
        VSAR(2);  VSRC(6, 1, 2);
        VMAX(3, 3, 4); VMAX(5, 5, 6); VMAX(3, 3, 5); VMAX(3, 3, 1);
        VST(3, dst + x);
    }
}

static void hfilt_min(uint8_t *dst, const uint8_t *p, int w) {
    for (int x = 0; x < w; x += 16) {
        VLD(0, p + x - 16); VLD(1, p + x); VLD(2, p + x + 16);
        VSAR(14); VSRC(3, 0, 1);
        VSAR(15); VSRC(4, 0, 1);
        VSAR(1);  VSRC(5, 1, 2);
        VSAR(2);  VSRC(6, 1, 2);
        VMIN(3, 3, 4); VMIN(5, 5, 6); VMIN(3, 3, 5); VMIN(3, 3, 1);
        VST(3, dst + x);
    }
}

/* dst = column-wise max (or min) of five rows */
static void vfilt_max(uint8_t *dst, const uint8_t *const r[5], int w) {
    for (int x = 0; x < w; x += 16) {
        VLD(0, r[0] + x); VLD(1, r[1] + x); VLD(2, r[2] + x); VLD(3, r[3] + x); VLD(4, r[4] + x);
        VMAX(0, 0, 1); VMAX(2, 2, 3); VMAX(0, 0, 4); VMAX(0, 0, 2);
        VST(0, dst + x);
    }
}

static void vfilt_min(uint8_t *dst, const uint8_t *const r[5], int w) {
    for (int x = 0; x < w; x += 16) {
        VLD(0, r[0] + x); VLD(1, r[1] + x); VLD(2, r[2] + x); VLD(3, r[3] + x); VLD(4, r[4] + x);
        VMIN(0, 0, 1); VMIN(2, 2, 3); VMIN(0, 0, 4); VMIN(0, 0, 2);
        VST(0, dst + x);
    }
}

size_t qrf_work_size(int w, int h) {
    size_t bw = (size_t)w / QRF_TILE, bh = ((size_t)h + QRF_TILE - 1) / QRF_TILE;
    return 2 * ((size_t)w + 32) + 15 * (size_t)w + ((3 * bw * bh + 15) & ~(size_t)15);
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
 * One pass over the rows, t = 0 .. h+3:
 *   H(t)   = horizontal max of input row t            (ring A, 5 rows)
 *   E(t-2) = horizontal min of the vertical max of H(t-4 .. t)  (ring B)
 *   O(t-4) = vertical min of E(t-6 .. t-2)  -> written back to row t-4
 * Row t is read before row t-4 is overwritten, so the image can be filtered
 * in place. Outside the image the dilation sees 0 and the erosion 255, as
 * in the reference. Tile minima and maxima are gathered from O on the way.
 */
void qrf_close_binarize(uint8_t *img, int w, int h, uint8_t *work) {
    const int bw = w / QRF_TILE, bh = (h + QRF_TILE - 1) / QRF_TILE;
    uint8_t *pad_a = work, *pad_b = pad_a + w + 32;
    uint8_t *ring_a = pad_b + w + 32, *ring_b = ring_a + 5 * w;
    uint8_t *zero = ring_b + 5 * w, *ones = zero + w, *out = ones + w;
    uint8_t *acc_mn = out + w, *acc_mx = acc_mn + w;
    uint8_t *mn = acc_mx + w, *mx = mn + bw * bh, *th = mx + bw * bh;
    const uint8_t *rows[5];

    if (w <= 0 || h <= 0 || w % 16) return;
    memset(pad_a, 0, 16);
    memset(pad_a + 16 + w, 0, 16);
    memset(pad_b, 255, 16);
    memset(pad_b + 16 + w, 255, 16);
    memset(zero, 0, w);
    memset(ones, 255, w);

    for (int t = 0; t < h + 4; ++t) {
        if (t < h) {
            memcpy(pad_a + 16, img + (size_t)t * w, w);
            hfilt_max(ring_a + (t % 5) * w, pad_a + 16, w);
        }
        int r = t - 2;
        if (r >= 0 && r < h) {
            for (int i = 0; i < 5; ++i) {
                int s = r - 2 + i;
                rows[i] = (s < 0 || s >= h) ? zero : ring_a + (s % 5) * w;
            }
            vfilt_max(pad_b + 16, rows, w);
            hfilt_min(ring_b + (r % 5) * w, pad_b + 16, w);
        }
        int y = t - 4;
        if (y < 0) continue;
        for (int i = 0; i < 5; ++i) {
            int s = y - 2 + i;
            rows[i] = (s < 0 || s >= h) ? ones : ring_b + (s % 5) * w;
        }
        vfilt_min(out, rows, w);

        if (y % QRF_TILE == 0) {
            memset(acc_mn, 255, w);
            memset(acc_mx, 0, w);
        }
        for (int x = 0; x < w; x += 16) {
            VLD(0, out + x);
            VLD(1, acc_mn + x); VMIN(1, 1, 0); VST(1, acc_mn + x);
            VLD(2, acc_mx + x); VMAX(2, 2, 0); VST(2, acc_mx + x);
        }
        if (y % QRF_TILE == QRF_TILE - 1 || y == h - 1) {
            int ty = y / QRF_TILE;
            for (int bx = 0; bx < bw; ++bx) {
                uint8_t lo = 255, hi = 0;
                for (int i = 0; i < 16; ++i) {
                    uint8_t a = acc_mn[bx * 16 + i], b = acc_mx[bx * 16 + i];
                    lo = a < lo ? a : lo;
                    hi = b > hi ? b : hi;
                }
                mn[ty * bw + bx] = lo;
                mx[ty * bw + bx] = hi;
            }
        }
        memcpy(img + (size_t)y * w, out, w);
    }

    tile_thresholds(mn, mx, th, bw, bh);

    for (int y = 0; y < h; ++y) {
        uint8_t *row = img + (size_t)y * w;
        const uint8_t *bt = th + (y / QRF_TILE) * bw;
        for (int bx = 0; bx < bw; ++bx) {
            VBC(1, bt + bx);
            VLD(0, row + bx * 16);
            VCMPLT(2, 0, 1);
            VNOT(2, 2);
            VST(2, row + bx * 16);
        }
    }
}

/* ------------------------------------------------------------------------
 * Reference implementation (the code the firmware used before)
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

static void filter_ref(uint8_t *img, int w, int h, uint8_t *pad, uint8_t *line, int dilate) {
    const int k = QRF_CLOSE, r0 = (k - 1) / 2, padn = k - 1;
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

void qrf_close_ref(uint8_t *img, int w, int h, uint8_t *pad, uint8_t *line) {
    filter_ref(img, w, h, pad, line, 1);
    filter_ref(img, w, h, pad, line, 0);
}

void qrf_binarize_ref(uint8_t *img, int w, int h, uint8_t *tiles) {
    int bw = (w + QRF_TILE - 1) / QRF_TILE, bh = (h + QRF_TILE - 1) / QRF_TILE;
    uint8_t *mn = tiles, *mx = mn + bw * bh, *th = mx + bw * bh;
    memset(mn, 255, (size_t)bw * bh);
    memset(mx, 0, (size_t)bw * bh);
    for (int y = 0; y < h; ++y) {
        uint8_t *bmn = mn + (y / QRF_TILE) * bw, *bmx = mx + (y / QRF_TILE) * bw;
        const uint8_t *row = img + (size_t)y * w;
        for (int bx = 0, x0 = 0; bx < bw; ++bx, x0 += QRF_TILE) {
            int x1 = x0 + QRF_TILE < w ? x0 + QRF_TILE : w;
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
        const uint8_t *bt = th + (y / QRF_TILE) * bw;
        for (int bx = 0, x0 = 0; bx < bw; ++bx, x0 += QRF_TILE) {
            int x1 = x0 + QRF_TILE < w ? x0 + QRF_TILE : w;
            uint8_t t = bt[bx];
            for (int x = x0; x < x1; ++x) row[x] = row[x] < t ? 0 : 255;
        }
    }
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

static int test_image(int w, int h, uint32_t seed) {
    size_t n = (size_t)w * h;
    int bw = w / QRF_TILE, bh = (h + QRF_TILE - 1) / QRF_TILE;
    uint8_t *a = aligned_buf(n), *b = aligned_buf(n), *work = aligned_buf(qrf_work_size(w, h));
    uint8_t *pad = malloc((size_t)w * (h + QRF_CLOSE - 1)), *line = malloc((size_t)w + QRF_CLOSE);
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
        qrf_close_ref(a, w, h, pad, line);
        qrf_binarize_ref(a, w, h, tiles);
        qrf_close_binarize(b, w, h, work);
        bad = memcmp(a, b, n) != 0;
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
    static const int sizes[][2] = {{16, 1}, {16, 3}, {32, 5}, {48, 37}, {64, 64}, {96, 50}};
    for (int i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); ++i) {
        if (test_image(sizes[i][0], sizes[i][1], 12345u + i)) return i + 1;
    }
    return 0;
}
