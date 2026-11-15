/*
 * Grid-line removal and local threshold for the second QR scan attempt.
 *
 * qrf_close_binarize() is a grey-level closing over QRF_CLOSE x QRF_CLOSE
 * followed by a tile threshold, computed in one streaming pass over the
 * image: rows go through small buffers in internal RAM, and the min/max
 * work is done 16 bytes at a time with the ESP32-P4 PIE instructions. Its
 * result is bit-identical to the plain reference functions below, which
 * qrf_selftest() checks.
 *
 * Off the P4 (ESP_PLATFORM undefined) the PIE instructions are emulated in
 * C, so the same code can be checked on a PC.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define QRF_CLOSE        5   /* closing window, pixels */
#define QRF_TILE         16  /* threshold tile, pixels; equals the vector width */
#define QRF_MIN_CONTRAST 24  /* flatter tile neighbourhoods count as paper */

/* Bytes of work memory qrf_close_binarize() needs for an image w pixels
 * wide and h high. w must be a multiple of 16. */
size_t qrf_work_size(int w, int h);

/* Closing then binarisation to 0/255, in place. img and work must be
 * 16-byte aligned; work should be internal RAM. */
void qrf_close_binarize(uint8_t *img, int w, int h, uint8_t *work);

/* Reference implementations (plain C, slow on the P4).
 * pad: w * (h + QRF_CLOSE - 1) bytes, line: w + QRF_CLOSE - 1 bytes,
 * tiles: 3 * tiles_x * tiles_y bytes. */
void qrf_close_ref(uint8_t *img, int w, int h, uint8_t *pad, uint8_t *line);
void qrf_binarize_ref(uint8_t *img, int w, int h, uint8_t *tiles);

/* Compares qrf_close_binarize() with the reference on synthetic images.
 * Returns 0 when they agree, otherwise the number of the failing case. */
int qrf_selftest(void);
