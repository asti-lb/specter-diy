/*
 * Image filters for the second, sharpened and full-resolution QR scan attempts.
 *
 * qrf_close_tiles(): grey-level closing (removes thin dark grid lines)
 * followed by a min/max tile threshold. One streaming pass over the rows:
 * each row is read once into ring buffers in internal RAM, and the min/max
 * work is done 16 bytes at a time with the ESP32-P4 PIE instructions.
 *
 * qrf_local_mean(): Bradley threshold, a pixel is dark when it is more than
 * t percent below the mean of the (2r+1) x (2r+1) window around it. Running
 * column and row sums, so the cost does not depend on r.
 *
 * qrf_sharpen(): unsharp mask p + k% (p - blur), blur = (2r+1) box filter
 * applied twice, with running sums; for the fourth attempt on the
 * half-size plane, where a blurred code from a screen loses its edges.
 *
 * All give results bit-identical to the plain reference functions below,
 * which qrf_selftest() checks. Off the P4 (ESP_PLATFORM undefined) the PIE
 * instructions are emulated in C, so the same code can be checked on a PC.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define QRF_MIN_CONTRAST 24  /* flatter tile neighbourhoods count as paper */

/* Work memory for qrf_close_tiles() on a w x h image with tile size `tile`,
 * and for qrf_local_mean() with window radius r. w must be a multiple of 16. */
size_t qrf_close_work_size(int w, int h, int tile);
size_t qrf_local_work_size(int w, int r);

/* Closing over (2*radius+1)^2, radius 1 or 2, then binarisation to 0/255
 * with tile x tile tiles (tile 8 or 16): each tile's threshold is the
 * midpoint of the darkest and lightest pixel in its 5x5 tile neighbourhood.
 * In place; img and work 16-byte aligned, work preferably internal RAM. */
void qrf_close_tiles(uint8_t *img, int w, int h, int radius, int tile, uint8_t *work);

/* Bradley threshold to 0/255 in place: dark when
 * p * n * 100 < window_sum * (100 - t), n = pixels in the (clipped) window. */
void qrf_local_mean(uint8_t *img, int w, int h, int r, int t, uint8_t *work);

/* Unsharp mask in place: p + k * (p - blur) / 100, clamped to 0..255; blur is
 * a (2r+1) x (2r+1) box filter (edges repeated) applied twice, r 1..3.
 * work: qrf_sharpen_work_size(w, h) bytes, any alignment. */
size_t qrf_sharpen_work_size(int w, int h);
void qrf_sharpen(uint8_t *img, int w, int h, int r, int k, uint8_t *work);

/* Reference implementations (plain C, slow on the P4).
 * pad: w * (h + 2*radius) bytes, line: w + 2*radius bytes,
 * tiles: 3 * ceil(w/tile) * ceil(h/tile) bytes. */
void qrf_close_ref(uint8_t *img, int w, int h, int radius, uint8_t *pad, uint8_t *line);
void qrf_tiles_ref(uint8_t *img, int w, int h, int tile, uint8_t *tiles);
/* Integral image version; allocates 4 * (w+1) * (h+1) bytes. Returns -1
 * when out of memory. */
int qrf_local_mean_ref(uint8_t *img, int w, int h, int r, int t);
/* Allocates 2 * w * h bytes. Returns -1 when out of memory. */
int qrf_sharpen_ref(uint8_t *img, int w, int h, int r, int k);

/* Compares the streaming filters with the references on synthetic images.
 * Returns 0 when they agree; 101..104 when a single PIE instruction does not
 * behave as assumed; otherwise the number of the failing case. */
int qrf_selftest(void);
