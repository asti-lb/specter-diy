/**
 * @file p4camera.h
 * @brief MIPI-CSI camera capture for the Waveshare ESP32-P4 4.3-C.
 */

#ifndef P4CAMERA_H
#define P4CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** Sensor detectado no barramento SCCB. */
typedef enum {
    P4CAM_SENSOR_NONE = 0,
    P4CAM_SENSOR_OV5647,
    P4CAM_SENSOR_SC2336,
} p4camera_sensor_t;

/**
 * @brief Inicializa a camera em tons de cinza.
 *
 * Reusa o barramento I2C do p4board -- nesta placa o SCCB da camera e o touch
 * compartilham GPIO 8/7, e abrir um segundo master bus nos mesmos pinos falha.
 * Exige que p4board_touch_init() tenha rodado antes.
 */
esp_err_t p4camera_init(void);
void p4camera_deinit(void);

p4camera_sensor_t p4camera_sensor(void);
const char *p4camera_sensor_name(void);
void p4camera_size(uint16_t *width, uint16_t *height);

/** Etapa em que a ultima inicializacao parou -- diagnostico via Python. */
const char *p4camera_init_stage(void);

/** FourCC do formato de pixel escolhido. */
uint32_t p4camera_format(void);

/**
 * @brief Captura um quadro.
 *
 * Devolve ponteiro para o buffer mapeado do driver, valido ate a proxima
 * chamada de p4camera_capture() ou p4camera_release(). Nao copia. Stops
 * the scan tasks first.
 */
esp_err_t p4camera_capture(uint8_t **data, size_t *length);

/** Devolve o buffer ao driver. Obrigatorio depois de cada capture(). */
esp_err_t p4camera_release(void);

/**
 * @brief Returns a QR code read since the last call, without waiting.
 *
 * The first call starts two tasks on core 0 that grab every frame, render
 * the preview and decode in the background; they stop by themselves when
 * scan() is not called for a second. Decoding uses a half-size plane: the
 * sensor delivers 1280x960, and 640x480 is enough for most codes.
 *
 * @param payload   buffer de saida
 * @param capacity  tamanho de payload
 * @param length    bytes escritos
 * @return ESP_OK with a code, ESP_ERR_NOT_FOUND if none was read since the
 *         last call.
 */
esp_err_t p4camera_scan(uint8_t *payload, size_t capacity, size_t *length);

/**
 * @brief Start a new scan session from a clean state.
 *
 * Drops the frames waiting in the driver queue, which were exposed before the
 * call, and blanks the greyscale and preview buffers. Call it whenever a new
 * scan starts, so that a code read in an earlier session can neither be
 * decoded again nor show up in the preview. p4camera_capture() also drops
 * queued frames by itself after a pause longer than 300 ms.
 */
void p4camera_flush(void);

/** Dimensoes do buffer em tons de cinza usado pelo decodificador. */
void p4camera_gray_size(uint16_t *width, uint16_t *height);

/**
 * Previa da camera para a tela. Com a previa ligada, cada p4camera_scan()
 * tambem grava em *buffer (through p4camera_preview_frames()) o centro quadrado do quadro, reduzido para
 * width x height em RGB565 -- o formato do painel e do LVGL.
 *
 * O buffer e alocado na primeira chamada e nunca liberado: a tela pode
 * continuar apontando para ele depois que a camera parar. Chamadas seguintes
 * reaproveitam o mesmo buffer, entao o tamanho maximo e o da primeira.
 */
esp_err_t p4camera_preview_enable(uint16_t width, uint16_t height, uint16_t **buffer);
/** Para a previa e zera os buffers de previa e de tons de cinza, que podem
 * mostrar uma seed. Chamado quando a tela de leitura fecha. */
void p4camera_preview_disable(void);
/** Copies the newest preview frame into the buffer, if there is one, and
 * returns how many frames were copied so far; changes with every new frame.
 * Call it from the task that draws the buffer. */
uint32_t p4camera_preview_frames(void);

#endif  // P4CAMERA_H
