/**
 * @file p4camera.c
 * @brief Captura MIPI-CSI em tons de cinza para leitura de QR.
 *
 * Modelado a partir de odudex/Kern, components/video/video.c (MIT), reduzido ao
 * que a leitura de QR precisa: um sensor, um formato, sem foco automatico, sem
 * pipeline de exibicao.
 *
 * Duas escolhas herdadas do Kern e que importam nesta placa:
 *
 *   - O SCCB da camera divide o barramento I2C com o touch (GPIO 8/7), entao
 *     init_sccb fica false e o handle vem do p4board. Abrir um segundo master
 *     bus nos mesmos pinos falha.
 *   - O sensor e sondado por endereco em vez de assumido: a Waveshare 4.3
 *     costuma vir com OV5647, mas placas irmas trazem SC2336.
 *
 * Formato: V4L2_PIX_FMT_GREY. O quirc trabalha em tons de cinza, entao pedir
 * GREY ao sensor evita converter a cada quadro.
 */

#include "p4camera.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "k_quirc.h"
#include "linux/videodev2.h"
#include "qrfilter.h"

#define OV5647_SCCB_ADDR 0x36
#define SC2336_SCCB_ADDR 0x30
/* Three, because while scanning the decoder holds one frame until it has
 * built its planes from it, the grab task holds the newest one for the
 * preview, and the driver needs one more to fill. */
#define BUFFER_COUNT     3
/* Igual ao Kern: sem timeout o VIDIOC_DQBUF espera para sempre quando nenhum
 * quadro chega. The grab task also checks between two frames whether it is
 * asked to stop. */
#define DQBUF_TIMEOUT_MS 100
/* A pause between two captures longer than this means the frames waiting in
 * the driver queue are from before the pause, see drop_queued_frames(). */
#define STALE_GAP_US     300000

static const char *TAG = "p4camera";

static int video_fd = -1;
static p4camera_sensor_t sensor = P4CAM_SENSOR_NONE;
static uint16_t frame_width;
static uint16_t frame_height;
static uint8_t *buffers[BUFFER_COUNT];
static size_t buffer_lengths[BUFFER_COUNT];
static struct v4l2_buffer current;
static uint32_t pixel_format;
/* Em que etapa a inicializacao parou. Os ESP_LOG nao chegam ao REPL cru do
 * MicroPython, entao a etapa precisa ser consultavel do lado Python. */
static const char *init_stage = "not started";

/* Buffer em tons de cinza para o decodificador de QR. Declarado aqui, junto
 * do restante do estado, porque p4camera_deinit() o libera e vem antes das
 * funcoes de QR no arquivo. */
static uint8_t *gray_buffer;
/* Full-resolution luminance plane for the third scan attempt, and the work
 * memory of the qrfilter functions (internal RAM if it fits). The
 * reference filter for the second attempt, used only if the PIE self-test
 * fails, needs a padded plane, a padded line and the tile minima, maxima
 * and thresholds. */
static uint8_t *full_buffer;
static size_t full_size;
static uint8_t *work_buffer;
static size_t work_size;
static uint8_t *pad_buffer;
static size_t pad_size;
static uint8_t *line_buffer;
static size_t line_size;
static uint8_t *tile_buffer;
static size_t tile_size;
/* qrf_selftest() result: -1 not run yet, 0 PIE filter in use, otherwise
 * the failing case and the reference filter in use. */
static int filter_selftest = -1;
static uint16_t gray_width;
static uint16_t gray_height;
static bool holding_frame;
static bool streaming;
/* esp_video_init() registra os dispositivos V4L2 uma unica vez; chama-lo de
 * novo depois de um deinit (ou de uma falha no meio do init) falha, e a camera
 * so voltaria reiniciando a placa. Fica inicializado ate o reset. */
static bool video_driver_ready;

/* Scanning runs in two tasks on the core MicroPython does not use, see
 * "Scan tasks" below. Declared here because deinit, capture and the
 * preview functions stop them or share their lock. */
static SemaphoreHandle_t scan_lock;
static void join_scan_tasks(void);

/* Declarado em p4board.h; evitamos incluir o header do board aqui para manter
 * este componente independente da BSP. */
extern void *p4board_i2c_bus(void);

static bool probe_sensor(i2c_master_bus_handle_t bus) {
    if (i2c_master_probe(bus, OV5647_SCCB_ADDR, 100) == ESP_OK) {
        sensor = P4CAM_SENSOR_OV5647;
        return true;
    }
    if (i2c_master_probe(bus, SC2336_SCCB_ADDR, 100) == ESP_OK) {
        sensor = P4CAM_SENSOR_SC2336;
        return true;
    }
    sensor = P4CAM_SENSOR_NONE;
    return false;
}

static esp_err_t start_streaming(void) {
    struct v4l2_requestbuffers req = {
        .count = BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(video_fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "REQBUFS failed: %s", strerror(errno));
        return ESP_FAIL;
    }

    for (int i = 0; i < BUFFER_COUNT; ++i) {
        struct v4l2_buffer buf = {
            .index = i,
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (ioctl(video_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QUERYBUF %d failed: %s", i, strerror(errno));
            return ESP_FAIL;
        }
        buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
            video_fd, buf.m.offset);
        if (buffers[i] == MAP_FAILED) {
            buffers[i] = NULL;
            ESP_LOGE(TAG, "mmap %d failed: %s", i, strerror(errno));
            return ESP_FAIL;
        }
        buffer_lengths[i] = buf.length;
        if (ioctl(video_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QBUF %d failed: %s", i, strerror(errno));
            return ESP_FAIL;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(video_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "STREAMON failed: %s", strerror(errno));
        return ESP_FAIL;
    }
    streaming = true;
    return ESP_OK;
}

esp_err_t p4camera_init(void) {
    if (video_fd >= 0) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)p4board_i2c_bus();
    if (bus == NULL) {
        ESP_LOGE(TAG, "I2C bus not up; call p4board touch init first");
        return ESP_ERR_INVALID_STATE;
    }
    if (!probe_sensor(bus)) {
        ESP_LOGE(TAG, "no camera sensor at 0x%02x or 0x%02x -- module attached?",
            OV5647_SCCB_ADDR, SC2336_SCCB_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "sensor: %s", p4camera_sensor_name());

    esp_video_init_csi_config_t csi_config = {
        .sccb_config = {
            .init_sccb = false,   /* barramento compartilhado com o touch */
            .i2c_handle = bus,
            .freq = 100000,
        },
        .reset_pin = -1,
        .pwdn_pin = -1,
    };
    esp_video_init_config_t config = { .csi = &csi_config };
    if (!video_driver_ready) {
        esp_err_t err = esp_video_init(&config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(err));
            init_stage = "esp_video_init";
            return err;
        }
        video_driver_ready = true;
    }

    video_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
    if (video_fd < 0) {
        ESP_LOGE(TAG, "open %s failed: %s", ESP_VIDEO_MIPI_CSI_DEVICE_NAME,
            strerror(errno));
        init_stage = "open";
        return ESP_FAIL;
    }

    struct v4l2_format format = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    if (ioctl(video_fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "G_FMT failed: %s", strerror(errno));
        init_stage = "G_FMT";
        goto fail;
    }
    frame_width = format.fmt.pix.width;
    frame_height = format.fmt.pix.height;

    /* Escolher o formato em vez de exigir um.
     *
     * O quirc trabalha em luminancia, entao GREY seria ideal -- um plano, sem
     * conversao. Mas nem todo sensor entrega GREY pelo pipeline CSI, e um
     * S_FMT recusado derruba a inicializacao inteira. Enumeramos o que o
     * driver oferece e ficamos com o melhor disponivel, guardando a escolha
     * para quem for decodificar saber o que recebeu. */
    static const uint32_t preferred[] = {
        V4L2_PIX_FMT_GREY,     /* melhor: ja e luminancia */
        V4L2_PIX_FMT_RGB565,   /* aceitavel: converter por quadro */
        V4L2_PIX_FMT_SBGGR8,   /* Bayer cru: o canal verde serve de aproximacao */
    };
    uint32_t chosen = format.fmt.pix.pixelformat;
    bool picked = false;
    for (size_t rank = 0; rank < sizeof(preferred) / sizeof(preferred[0])
        && !picked; ++rank) {
        struct v4l2_fmtdesc desc = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
        for (desc.index = 0; ioctl(video_fd, VIDIOC_ENUM_FMT, &desc) == 0;
            ++desc.index) {
            if (desc.pixelformat == preferred[rank]) {
                chosen = preferred[rank];
                picked = true;
                break;
            }
        }
    }
    if (chosen != format.fmt.pix.pixelformat) {
        struct v4l2_format want = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .fmt.pix.width = frame_width,
            .fmt.pix.height = frame_height,
            .fmt.pix.pixelformat = chosen,
        };
        if (ioctl(video_fd, VIDIOC_S_FMT, &want) != 0) {
            ESP_LOGE(TAG, "S_FMT %c%c%c%c failed: %s",
                (char)(chosen), (char)(chosen >> 8), (char)(chosen >> 16),
                (char)(chosen >> 24), strerror(errno));
            init_stage = "S_FMT";
            goto fail;
        }
    }
    pixel_format = chosen;

    struct timeval timeout = {
        .tv_sec = DQBUF_TIMEOUT_MS / 1000,
        .tv_usec = (DQBUF_TIMEOUT_MS % 1000) * 1000,
    };
    if (ioctl(video_fd, VIDIOC_S_DQBUF_TIMEOUT, &timeout) != 0) {
        ESP_LOGE(TAG, "S_DQBUF_TIMEOUT failed: %s", strerror(errno));
        init_stage = "DQBUF timeout";
        goto fail;
    }

    if (start_streaming() != ESP_OK) {
        init_stage = "streaming";
        goto fail;
    }
    init_stage = "ok";
    ESP_LOGI(TAG, "streaming %ux%u fmt=%c%c%c%c", frame_width, frame_height,
        (char)(pixel_format), (char)(pixel_format >> 8),
        (char)(pixel_format >> 16), (char)(pixel_format >> 24));
    return ESP_OK;

fail:
    p4camera_deinit();
    return ESP_FAIL;
}

static void blank_scan_buffers(void);
static void free_scan_buffers(void);

void p4camera_deinit(void) {
    join_scan_tasks();
    if (video_fd >= 0) {
        if (streaming) {
            int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(video_fd, VIDIOC_STREAMOFF, &type);
            streaming = false;
        }
        for (int i = 0; i < BUFFER_COUNT; ++i) {
            if (buffers[i]) {
                /* The last frames may show a seed; the memory goes back to
                 * the heap, so blank it first. Streaming is off, no DMA. */
                memset(buffers[i], 0, buffer_lengths[i]);
                munmap(buffers[i], buffer_lengths[i]);
                buffers[i] = NULL;
                buffer_lengths[i] = 0;
            }
        }
        close(video_fd);
        video_fd = -1;
    }
    holding_frame = false;
    frame_width = frame_height = 0;
    sensor = P4CAM_SENSOR_NONE;
    free_scan_buffers();
}

const char *p4camera_init_stage(void) {
    return init_stage;
}

uint32_t p4camera_format(void) {
    return pixel_format;
}

p4camera_sensor_t p4camera_sensor(void) {
    return sensor;
}

const char *p4camera_sensor_name(void) {
    switch (sensor) {
        case P4CAM_SENSOR_OV5647: return "OV5647";
        case P4CAM_SENSOR_SC2336: return "SC2336";
        default: return "none";
    }
}

void p4camera_size(uint16_t *width, uint16_t *height) {
    if (width) *width = frame_width;
    if (height) *height = frame_height;
}

/* Time of the last capture, for the stale-frame check in p4camera_capture(). */
static int64_t last_capture_us;

/* Dequeue and requeue every buffer once, so the next capture waits for a
 * frame exposed from now on.
 *
 * The sensor keeps streaming between scans. Nobody dequeues meanwhile, so
 * the queue fills with frames from the end of the previous scan and the next
 * capture returns one of them: after scanning one QR code, cancelling and
 * putting a different code under the camera, the old code was decoded again
 * and shown in the preview. For a seed import that means importing a seed
 * that is no longer in front of the camera. */
static void drop_queued_frames(void) {
    for (int i = 0; i < BUFFER_COUNT; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(video_fd, VIDIOC_DQBUF, &buf) != 0) {
            break;
        }
        ioctl(video_fd, VIDIOC_QBUF, &buf);
    }
}

esp_err_t p4camera_capture(uint8_t **data, size_t *length) {
    if (video_fd < 0 || !streaming) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Direct capture (test and measuring scripts) and the scan tasks would
     * dequeue from the same driver queue. */
    join_scan_tasks();
    if (holding_frame) {
        /* Segurar dois quadros esgota a fila de dois buffers e trava o
         * streaming. Devolver antes de pegar o proximo. */
        p4camera_release();
    }
    int64_t now = esp_timer_get_time();
    if (now - last_capture_us > STALE_GAP_US) {
        drop_queued_frames();
    }
    memset(&current, 0, sizeof(current));
    current.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    current.memory = V4L2_MEMORY_MMAP;
    if (ioctl(video_fd, VIDIOC_DQBUF, &current) != 0) {
        /* Sem quadro dentro de DQBUF_TIMEOUT_MS: o chamador tenta de novo. */
        return (errno == ETIMEDOUT || errno == EPERM) ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }
    holding_frame = true;
    last_capture_us = esp_timer_get_time();
    if (data) *data = buffers[current.index];
    if (length) *length = current.bytesused;
    return ESP_OK;
}

/* ------------------------------- Previa --------------------------------- */

/* preview_buffer is what the screen shows. The grab task renders into
 * preview_stage instead, on the other core; p4camera_preview_frames(),
 * called from the screen, copies the newest stage frame over under
 * scan_lock. So the task never writes the buffer LVGL is reading, and the
 * preview cannot tear. */
static uint16_t *preview_buffer;
static uint16_t *preview_stage;
static size_t preview_capacity;
static uint16_t preview_width;
static uint16_t preview_height;
static uint32_t preview_frames;
static uint32_t stage_frames;

static void blank_preview(void) {
    if (preview_buffer) {
        memset(preview_buffer, 0, preview_capacity * sizeof(uint16_t));
    }
    if (preview_stage) {
        memset(preview_stage, 0, preview_capacity * sizeof(uint16_t));
    }
}

esp_err_t p4camera_preview_enable(uint16_t width, uint16_t height, uint16_t **buffer) {
    if (width == 0 || height == 0 || !buffer) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t needed = (size_t)width * height;
    if (!preview_buffer) {
        preview_buffer = heap_caps_calloc(needed, sizeof(uint16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        preview_stage = heap_caps_calloc(needed, sizeof(uint16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!preview_buffer || !preview_stage) {
            free(preview_buffer);
            free(preview_stage);
            preview_buffer = preview_stage = NULL;
            return ESP_ERR_NO_MEM;
        }
        preview_capacity = needed;
    } else if (needed > preview_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* The grab task reads the size while rendering. */
    join_scan_tasks();
    preview_width = width;
    preview_height = height;
    *buffer = preview_buffer;
    return ESP_OK;
}

void p4camera_preview_disable(void) {
    /* The scan screen is closing. Stop scanning first, so nothing writes
     * the buffers while they are blanked. The preview buffers outlive the
     * screen, and the scan buffers still hold the last frame handed to the
     * decoder: all may show a seed, so blank them now instead of at the
     * next scan. */
    join_scan_tasks();
    preview_width = preview_height = 0;
    blank_preview();
    blank_scan_buffers();
}

uint32_t p4camera_preview_frames(void) {
    if (preview_buffer && scan_lock) {
        xSemaphoreTake(scan_lock, portMAX_DELAY);
        if (stage_frames != preview_frames) {
            memcpy(preview_buffer, preview_stage,
                (size_t)preview_width * preview_height * sizeof(uint16_t));
            preview_frames = stage_frames;
        }
        xSemaphoreGive(scan_lock);
    }
    return preview_frames;
}

/* Recorta o centro quadrado do quadro RGB565 e reduz por vizinho mais
 * proximo. Quadrado porque o QR e quadrado; o centro porque e para onde a
 * pessoa aponta. Passo em ponto fixo 16.16 para nao dividir por pixel.
 * Runs in the grab task, with scan_lock held. */
static void render_preview(const uint8_t *src) {
    const uint16_t *pixels = (const uint16_t *)src;
    uint16_t side = frame_width < frame_height ? frame_width : frame_height;
    uint16_t x0 = (frame_width - side) / 2;
    uint16_t y0 = (frame_height - side) / 2;
    uint32_t step_x = ((uint32_t)side << 16) / preview_width;
    uint32_t step_y = ((uint32_t)side << 16) / preview_height;
    uint16_t *out = preview_stage;
    uint32_t fy = 0;
    for (uint16_t y = 0; y < preview_height; ++y, fy += step_y) {
        const uint16_t *row = pixels + (size_t)(y0 + (fy >> 16)) * frame_width + x0;
        uint32_t fx = 0;
        for (uint16_t x = 0; x < preview_width; ++x, fx += step_x) {
            *out++ = row[fx >> 16];
        }
    }
    stage_frames++;
}

/* --------------------------------- QR ----------------------------------- */

/* Reducao por 2 em cada eixo: 1280x960 -> 640x480. Um QR a 640x480 tem modulos
 * com folga de sobra para o quirc, e decodificar 1,2 milhao de pixels custaria
 * varias vezes mais por quadro sem melhorar a taxa de acerto. */
#define GRAY_DOWNSCALE 2

void p4camera_gray_size(uint16_t *width, uint16_t *height) {
    if (width) *width = gray_width;
    if (height) *height = gray_height;
}

/* Second attempt, on the half-size plane: closing 3 x 3 (radius 1) and
 * min/max tiles of 8 px. Last attempt, full resolution: mean threshold over
 * 49 x 49 px (radius 24), dark when 15 % below the mean. Chosen on camera
 * images of SeedSigner and T templates at 10-25 cm, by day and in the
 * evening, and on simulated templates (journal 2.42). */
#define HALF_CLOSE_RADIUS 1
#define HALF_TILE         8
#define FULL_MEAN_RADIUS  24
#define FULL_MEAN_PERCENT 15

/* Sharpened attempt, between the second and the third: unsharp mask on a
 * fresh half-size plane, 5 x 5 box blur twice, strength 300 %. A code on a
 * screen seen from close up is out of focus for the fixed-focus lens; the
 * mask restores its edges. Chosen on camera captures of static PSBTs from
 * Specter Desktop and on simulated templates (journal 3.22). */
#define SHARPEN_RADIUS  2
#define SHARPEN_PERCENT 300

/* Run the full-resolution attempt on every n-th frame in which the others found
 * nothing. */
#define FULL_ATTEMPT_EVERY 3
static uint8_t full_attempt_turn;

/* Every scan buffer may hold a picture of a seed, or a filtered copy of it.
 * Blank them all in one place, so a buffer added later cannot be missed by
 * one of the callers: flush, closing the scan screen, and freeing. */
static void blank_scan_buffers(void) {
    if (gray_buffer) {
        memset(gray_buffer, 0, (size_t)gray_width * gray_height);
    }
    if (full_buffer) {
        memset(full_buffer, 0, full_size);
    }
    if (work_buffer) {
        memset(work_buffer, 0, work_size);
    }
    if (pad_buffer) {
        memset(pad_buffer, 0, pad_size);
    }
    if (line_buffer) {
        memset(line_buffer, 0, line_size);
    }
    if (tile_buffer) {
        memset(tile_buffer, 0, tile_size);
    }
}

static void free_scan_buffers(void) {
    blank_scan_buffers();
    free(gray_buffer);
    free(full_buffer);
    free(work_buffer);
    free(pad_buffer);
    free(line_buffer);
    free(tile_buffer);
    gray_buffer = full_buffer = work_buffer = pad_buffer = line_buffer = tile_buffer = NULL;
    full_size = work_size = pad_size = line_size = tile_size = 0;
    gray_width = gray_height = 0;
}

static esp_err_t ensure_gray_buffer(void) {
    uint16_t width = frame_width / GRAY_DOWNSCALE;
    uint16_t height = frame_height / GRAY_DOWNSCALE;
    if (gray_buffer && gray_width == width && gray_height == height) {
        return ESP_OK;
    }
    free_scan_buffers();
    size_t fw = frame_width, fh = frame_height;
    if (fw % 16) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (filter_selftest < 0) {
        filter_selftest = qrf_selftest();
        if (filter_selftest) {
            ESP_LOGW(TAG, "PIE filter self-test failed (%d), using the reference filter",
                     filter_selftest);
        }
    }
    gray_buffer = heap_caps_aligned_alloc(16, (size_t)width * height, MALLOC_CAP_8BIT);
    full_size = fw * fh;
    /* full_buffer doubles as the work memory of the sharpened attempt,
     * which runs before the full-resolution plane is built. */
    if (qrf_sharpen_work_size(width, height) > full_size) {
        return ESP_ERR_NO_MEM;
    }
    full_buffer = heap_caps_aligned_alloc(16, full_size, MALLOC_CAP_8BIT);
    work_size = qrf_close_work_size(width, height, HALF_TILE);
    if (qrf_local_work_size(fw, FULL_MEAN_RADIUS) > work_size) {
        work_size = qrf_local_work_size(fw, FULL_MEAN_RADIUS);
    }
    work_buffer = heap_caps_aligned_alloc(16, work_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!work_buffer) {
        work_buffer = heap_caps_aligned_alloc(16, work_size, MALLOC_CAP_8BIT);
    }
    bool ok = gray_buffer && full_buffer && work_buffer;
    if (ok && filter_selftest) {
        size_t tiles = (((size_t)width + HALF_TILE - 1) / HALF_TILE) *
                       (((size_t)height + HALF_TILE - 1) / HALF_TILE);
        pad_size = (size_t)width * (height + 2 * HALF_CLOSE_RADIUS);
        line_size = (size_t)width + 2 * HALF_CLOSE_RADIUS;
        tile_size = 3 * tiles;
        pad_buffer = malloc(pad_size);
        line_buffer = malloc(line_size);
        tile_buffer = malloc(tile_size);
        ok = pad_buffer && line_buffer && tile_buffer;
    }
    if (!ok) {
        free_scan_buffers();
        return ESP_ERR_NO_MEM;
    }
    gray_width = width;
    gray_height = height;
    return ESP_OK;
}

/* RGB565 -> luminancia.
 *
 * Usamos a aproximacao inteira (2R + 5G + B) / 8 em vez dos coeficientes ITU-R
 * exatos: e uma soma e um shift por pixel, e o quirc so precisa de contraste
 * entre modulo claro e escuro, nao de fidelidade colorimetrica. */
static inline uint16_t rgb565_luma(uint16_t pixel) {
    uint16_t r = (pixel >> 11) & 0x1f;
    uint16_t g = (pixel >> 5) & 0x3f;
    uint16_t b = pixel & 0x1f;
    /* Escalados para 8 bits: r,b sao 5 bits e g e 6. */
    return (uint16_t)(((r << 3) * 2 + (g << 2) * 5 + (b << 3)) / 8);
}

/* 2x2 mean of the luminance into gray_buffer, straight from the frame.
 * Averaging rather than sampling every second pixel halves the sensor noise
 * and keeps thin features such as the grid lines of a hand-filled SeedQR
 * template from aliasing into ragged dark specks.
 *
 * Each pixel is truncated to 8 bits before the sum, exactly as when the
 * full-resolution plane was built first, so gray_buffer is bit for bit the
 * same - it just no longer needs a pass over full_buffer on every frame. */
static void rgb565_to_gray(const uint8_t *src) {
    const uint16_t *pixels = (const uint16_t *)src;
    for (uint16_t y = 0; y < gray_height; ++y) {
        const uint16_t *row0 = pixels + (size_t)(y * GRAY_DOWNSCALE) * frame_width;
        const uint16_t *row1 = row0 + frame_width;
        uint8_t *out = gray_buffer + (size_t)y * gray_width;
        for (uint16_t x = 0; x < gray_width; ++x) {
            uint16_t x0 = x * GRAY_DOWNSCALE;
            uint16_t sum = (uint8_t)rgb565_luma(row0[x0]) + (uint8_t)rgb565_luma(row0[x0 + 1]) +
                           (uint8_t)rgb565_luma(row1[x0]) + (uint8_t)rgb565_luma(row1[x0 + 1]);
            out[x] = (uint8_t)((sum + 2) >> 2);
        }
    }
}

/* Full-resolution luminance into full_buffer, only for the last attempt. */
static void rgb565_to_full(const uint8_t *src) {
    const uint16_t *pixels = (const uint16_t *)src;
    size_t count = (size_t)frame_width * frame_height;
    for (size_t i = 0; i < count; ++i) {
        full_buffer[i] = (uint8_t)rgb565_luma(pixels[i]);
    }
}

/* Second attempt, in place on gray_buffer.
 *
 * Closing (max, then min): dark features narrower than the window disappear,
 * wider ones keep their size. SeedQR templates filled in by hand (SeedSigner)
 * print a grid across the whole symbol, including the finder patterns. A
 * grid line through the light ring of a finder joins its dark core to its
 * dark frame, and quirc no longer sees the ring structure it looks for. The
 * closing erases the lines while the pen dots and printed finders survive.
 *
 * Tile threshold: quirc's own comes from four image quadrants; a bright card
 * on a dark desk, or a keyboard in the frame, shifts it until the thin light
 * rings of the finders vanish. Here every tile gets the midpoint of the
 * darkest and lightest pixel in its 5 x 5 tile neighbourhood. On the
 * half-size plane the 2x2 mean has already halved the sensor noise, which
 * is what makes this work in dim light. */
static void filter_half(void) {
    if (filter_selftest == 0) {
        qrf_close_tiles(gray_buffer, gray_width, gray_height, HALF_CLOSE_RADIUS, HALF_TILE,
                        work_buffer);
    } else {
        qrf_close_ref(gray_buffer, gray_width, gray_height, HALF_CLOSE_RADIUS, pad_buffer,
                      line_buffer);
        qrf_tiles_ref(gray_buffer, gray_width, gray_height, HALF_TILE, tile_buffer);
    }
}

/* Third attempt, in place on full_buffer: mean threshold at full resolution
 * for codes too small or too far away for the half-size plane. Plain C, no
 * PIE, so there is no reference fallback. */
static void filter_full(void) {
    qrf_local_mean(full_buffer, frame_width, frame_height, FULL_MEAN_RADIUS, FULL_MEAN_PERCENT,
                   work_buffer);
}

/* Decodes the first readable code in plane. *seen is set when quirc located
 * a code - three finder patterns forming a grid - even if it could not read
 * it, which is what a code too fine for the plane looks like. */
static int decode_plane(const uint8_t *plane, uint16_t w, uint16_t h,
                        k_quirc_result_t *result, bool *seen) {
    *seen = false;
    k_quirc_t *q = k_quirc_new();
    if (!q) {
        return 0;
    }
    int found = 0;
    if (k_quirc_resize(q, w, h) == 0) {
        uint8_t *buf = k_quirc_begin(q, NULL, NULL);
        if (buf) {
            memcpy(buf, plane, (size_t)w * h);
            k_quirc_end(q, false);
            int count = k_quirc_count(q);
            *seen = count > 0;
            for (int i = 0; i < count && !found; ++i) {
                found = k_quirc_decode(q, i, result) == K_QUIRC_SUCCESS &&
                        result->valid && result->data.payload_len > 0;
            }
        }
    }
    k_quirc_destroy(q);
    return found;
}

/* ----------------------------- Scan tasks ------------------------------- */

/* MicroPython, LVGL and touch run in one task on core 1. Decoding a frame
 * takes up to about half a second, and while camera.scan() did it in that
 * task the preview, touch and the rest of the device stood still, and
 * animated codes lost parts. Now two tasks on core 0 do the work:
 *
 *   grab   - dequeues every frame, renders the preview from it, and hands
 *            it to the decoder when the decoder is free. All driver calls
 *            while scanning come from this task.
 *   decode - builds the planes from the frame, returns the frame, runs the
 *            three attempts and leaves a result for p4camera_scan().
 *
 * The grab task has the higher priority: a frame comes every few tens of
 * ms, rendering it is short, and the decoder continues in between.
 *
 * camera.scan() only starts the tasks and picks up a result. When it has
 * not been called for IDLE_STOP_US (the scan ended or the app is busy
 * elsewhere), the tasks stop by themselves and blank what they held; the
 * next scan() starts them again. flush, closing the preview, capture and
 * deinit stop them and wait until they are gone (join_scan_tasks), so a
 * frame or result of a previous session cannot appear later. */
#define SCAN_CORE        0
#define GRAB_PRIORITY    3
#define DECODE_PRIORITY  2
#define GRAB_STACK       4096
/* quirc keeps a bitmap of up to 4 KB on the stack while decoding. */
#define DECODE_STACK     16384
#define IDLE_STOP_US     1000000

static TaskHandle_t grab_handle;
static TaskHandle_t decode_handle;
static SemaphoreHandle_t grab_exited;
static SemaphoreHandle_t decode_exited;
/* Tasks created and not yet joined. Only changed from the MicroPython
 * side. */
static bool tasks_alive;
/* Set by join_scan_tasks() or by the grab task when idle. */
static volatile bool stop_requested;
static volatile int64_t last_poll_us;

/* Frame handoff, under scan_lock. decode_buf is the frame lent to the
 * decoder (decode_lent); the decoder sets decode_returned when it no
 * longer reads the frame, and the grab task requeues it. decode_busy lasts
 * until the decoder has finished all attempts. */
static struct v4l2_buffer decode_buf;
static bool decode_lent;
static bool decode_returned;
static bool decode_busy;

/* Result for p4camera_scan(), under scan_lock. decode_result is the
 * decoder's own scratch; both may hold a seed and are blanked after use. */
static uint8_t result_payload[K_QUIRC_MAX_PAYLOAD];
static size_t result_length;
static k_quirc_result_t decode_result;

static void lock(void) {
    xSemaphoreTake(scan_lock, portMAX_DELAY);
}

static void unlock(void) {
    xSemaphoreGive(scan_lock);
}

static void blank_result(void) {
    memset(result_payload, 0, sizeof(result_payload));
    result_length = 0;
}

static void return_decoder_frame(void) {
    lock();
    decode_returned = true;
    unlock();
}

/* The three attempts on one frame. Returns the frame as soon as the
 * planes are built from it. */
static bool decode_one(const uint8_t *frame) {
    k_quirc_result_t *result = &decode_result;
    rgb565_to_gray(frame);
    /* Cheap attempt first: the half-size plane as it is, which reads clean
     * codes. Then the same plane with grid lines closed and a tile
     * threshold, for hand-filled templates and dim light. Then a fresh
     * half-size plane, sharpened, for codes on screens that the lens does
     * not focus. Last, the full-resolution plane with a mean threshold.
     *
     * The sharpened plane gets its own attempt rather than feeding the
     * second one: closing a sharpened SeedQR template no longer removed its
     * grid lines, and four templates that read before were lost.
     *
     * The frame is held until it is clear whether the last attempt runs,
     * because only then is the full-resolution plane built from it. */
    bool seen = false, seen2 = false;
    int found = decode_plane(gray_buffer, gray_width, gray_height, result, &seen);
    if (!found) {
        filter_half();
        found = decode_plane(gray_buffer, gray_width, gray_height, result, &seen2);
        seen = seen || seen2;
    }
    if (!found) {
        rgb565_to_gray(frame);
        qrf_sharpen(gray_buffer, gray_width, gray_height, SHARPEN_RADIUS, SHARPEN_PERCENT,
                    full_buffer);
        found = decode_plane(gray_buffer, gray_width, gray_height, result, &seen2);
        seen = seen || seen2;
    }
    /* The last attempt costs more than the others together. It runs
     * at once when the half-size plane shows a code it could not read - a
     * dense code, such as a static PSBT from a screen, needs the full
     * resolution. Otherwise only on every few frames, for codes too small
     * to be located at half size: running it on every empty frame kept the
     * decoder from the next frames and lost parts of animated QR codes,
     * where many frames catch a change between two parts. */
    bool full_attempt = !found && (seen || ++full_attempt_turn >= FULL_ATTEMPT_EVERY);
    if (full_attempt) {
        full_attempt_turn = 0;
        rgb565_to_full(frame);
    }
    return_decoder_frame();
    if (full_attempt) {
        filter_full();
        found = decode_plane(full_buffer, frame_width, frame_height, result, &seen);
    }
    return found;
}

static void decode_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (stop_requested) {
            break;
        }
        if (decode_one(buffers[decode_buf.index])) {
            size_t n = (size_t)decode_result.data.payload_len;
            lock();
            /* A result p4camera_scan() has not picked up yet is replaced
             * by the newer one. */
            if (n <= sizeof(result_payload)) {
                memcpy(result_payload, decode_result.data.payload, n);
                result_length = n;
            }
            unlock();
        }
        memset(&decode_result, 0, sizeof(decode_result));
        lock();
        decode_busy = false;
        unlock();
    }
    blank_scan_buffers();
    memset(&decode_result, 0, sizeof(decode_result));
    xSemaphoreGive(decode_exited);
    vTaskDelete(NULL);
}

static void requeue_returned_frame(void) {
    lock();
    bool requeue = decode_lent && decode_returned;
    if (requeue) {
        decode_lent = false;
    }
    unlock();
    if (requeue) {
        ioctl(video_fd, VIDIOC_QBUF, &decode_buf);
    }
}

static void grab_task(void *arg) {
    (void)arg;
    while (!stop_requested) {
        if (esp_timer_get_time() - last_poll_us > IDLE_STOP_US) {
            break;
        }
        requeue_returned_frame();
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(video_fd, VIDIOC_DQBUF, &buf) != 0) {
            /* No frame within DQBUF_TIMEOUT_MS; also lets the idle task
             * of this core run if the driver fails at once. */
            vTaskDelay(1);
            continue;
        }
        bool lend = false;
        lock();
        if (preview_width && preview_stage) {
            render_preview(buffers[buf.index]);
        }
        if (!decode_busy && !decode_lent) {
            decode_buf = buf;
            decode_lent = true;
            decode_returned = false;
            decode_busy = true;
            lend = true;
        }
        unlock();
        if (lend) {
            xTaskNotifyGive(decode_handle);
        } else {
            ioctl(video_fd, VIDIOC_QBUF, &buf);
        }
    }
    /* Stopping, asked for or idle. The decoder may be in the middle of a
     * frame; wait for it, then give its frame back to the driver. */
    stop_requested = true;
    xTaskNotifyGive(decode_handle);
    xSemaphoreTake(decode_exited, portMAX_DELAY);
    lock();
    if (decode_lent) {
        ioctl(video_fd, VIDIOC_QBUF, &decode_buf);
        decode_lent = false;
    }
    decode_busy = false;
    blank_result();
    if (preview_stage) {
        memset(preview_stage, 0, preview_capacity * sizeof(uint16_t));
    }
    unlock();
    xSemaphoreGive(grab_exited);
    vTaskDelete(NULL);
}

/* Stop the scan tasks, if any, and wait until both are gone. Blocks for at
 * most one decoding pass. MicroPython side only. */
static void join_scan_tasks(void) {
    if (!tasks_alive) {
        return;
    }
    stop_requested = true;
    xSemaphoreTake(grab_exited, portMAX_DELAY);
    tasks_alive = false;
    grab_handle = decode_handle = NULL;
}

static esp_err_t start_scan_tasks(void) {
    if (!scan_lock) {
        scan_lock = xSemaphoreCreateMutex();
        grab_exited = xSemaphoreCreateBinary();
        decode_exited = xSemaphoreCreateBinary();
        if (!scan_lock || !grab_exited || !decode_exited) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (holding_frame) {
        p4camera_release();
    }
    /* Frames queued while nobody scanned are from before this scan. */
    drop_queued_frames();
    decode_lent = decode_returned = decode_busy = false;
    stop_requested = false;
    last_poll_us = esp_timer_get_time();
    if (xTaskCreatePinnedToCore(decode_task, "qr_decode", DECODE_STACK, NULL,
            DECODE_PRIORITY, &decode_handle, SCAN_CORE) != pdPASS) {
        decode_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(grab_task, "qr_grab", GRAB_STACK, NULL,
            GRAB_PRIORITY, &grab_handle, SCAN_CORE) != pdPASS) {
        stop_requested = true;
        xTaskNotifyGive(decode_handle);
        xSemaphoreTake(decode_exited, portMAX_DELAY);
        grab_handle = decode_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    tasks_alive = true;
    return ESP_OK;
}

/* Never blocks for a frame or a decode: starts the scan tasks if needed
 * and returns the result they left since the last call, if any. */
esp_err_t p4camera_scan(uint8_t *payload, size_t capacity, size_t *length) {
    if (length) *length = 0;
    if (video_fd < 0 || !streaming) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixel_format != V4L2_PIX_FMT_RGB565) {
        /* Formatos crus de Bayer precisariam de demosaico antes; o codigo
         * cobre so o caso que este sensor entrega de fato. */
        return ESP_ERR_NOT_SUPPORTED;
    }
    last_poll_us = esp_timer_get_time();
    if (tasks_alive && stop_requested) {
        /* Stopped by themselves after a pause in scanning. */
        join_scan_tasks();
    }
    if (!tasks_alive) {
        esp_err_t err = ensure_gray_buffer();
        if (err != ESP_OK) {
            return err;
        }
        err = start_scan_tasks();
        if (err != ESP_OK) {
            return err;
        }
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    lock();
    if (result_length) {
        if (result_length > capacity) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            memcpy(payload, result_payload, result_length);
            if (length) *length = result_length;
            err = ESP_OK;
        }
        blank_result();
    }
    unlock();
    return err;
}

esp_err_t p4camera_release(void) {
    if (!holding_frame) {
        return ESP_OK;
    }
    holding_frame = false;
    return ioctl(video_fd, VIDIOC_QBUF, &current) == 0 ? ESP_OK : ESP_FAIL;
}

void p4camera_flush(void) {
    join_scan_tasks();
    if (holding_frame) {
        p4camera_release();
    }
    if (video_fd >= 0 && streaming) {
        drop_queued_frames();
        last_capture_us = esp_timer_get_time();
    }
    blank_scan_buffers();
    blank_result();
    full_attempt_turn = 0;
    blank_preview();
}
