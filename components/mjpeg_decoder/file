#ifndef MJPEG_DECODER_H
#define MJPEG_DECODER_H

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "JPEGDEC.h"

#define READ_BUFFER_SIZE 1024
#define MAXOUTPUTSIZE (288 / 3 / 16)
#define NUMBER_OF_DECODE_BUFFER 3
#define NUMBER_OF_DRAW_BUFFER 9

static const char* TAG = "MJPEG_DECODER";

typedef struct {
    int32_t size;
    uint8_t *buf;
} mjpegBuf;

typedef struct {
    QueueHandle_t xqh;
    JPEG_DRAW_CALLBACK *drawFunc;
} paramDrawTask;

typedef struct {
    QueueHandle_t xqh;
    mjpegBuf *mBuf;
    JPEG_DRAW_CALLBACK *drawFunc;
} paramDecodeTask;

static JPEGDRAW jpegdraws[NUMBER_OF_DRAW_BUFFER];
static int _draw_queue_cnt = 0;
static JPEGDEC _jpegDec;
static QueueHandle_t _xqh;
static bool _useBigEndian;

static unsigned long total_read_video_ms = 0;
static unsigned long total_decode_video_ms = 0;
static unsigned long total_show_video_ms = 0;

// Équivalent de Stream pour ESP-IDF
typedef struct {
    size_t (*read)(void* buf, size_t size);
    size_t (*available)();
} Stream_t;

Stream_t *_input;

int32_t _mjpegBufSize;

uint8_t *_read_buf;
int32_t _mjpeg_buf_offset = 0;

TaskHandle_t _decodeTask;
TaskHandle_t _draw_task;
paramDecodeTask _pDecodeTask;
paramDrawTask _pDrawTask;
uint8_t *_mjpeg_buf;
uint8_t _mBufIdx = 0;

int32_t _inputindex = 0;
int32_t _buf_read;
int32_t _remain = 0;
mjpegBuf _mjpegBufs[NUMBER_OF_DECODE_BUFFER];

// Fonction utilitaire pour obtenir le temps en millisecondes
static unsigned long millis() {
    return esp_timer_get_time() / 1000;
}

static int queueDrawMCU(JPEGDRAW *pDraw) {
    int len = pDraw->iWidth * pDraw->iHeight * 2;
    JPEGDRAW *j = &jpegdraws[_draw_queue_cnt % NUMBER_OF_DRAW_BUFFER];
    j->x = pDraw->x;
    j->y = pDraw->y;
    j->iWidth = pDraw->iWidth;
    j->iHeight = pDraw->iHeight;
    memcpy(j->pPixels, pDraw->pPixels, len);

    // ESP_LOGD(TAG, "queueDrawMCU start.");
    ++_draw_queue_cnt;
    xQueueSend(_xqh, &j, portMAX_DELAY);
    // ESP_LOGD(TAG, "queueDrawMCU end.");

    return 1;
}

static void decode_task(void *arg) {
    paramDecodeTask *p = (paramDecodeTask *)arg;
    mjpegBuf *mBuf;
    ESP_LOGI(TAG, "decode_task start.");
    
    while (xQueueReceive(p->xqh, &mBuf, portMAX_DELAY)) {
        // ESP_LOGD(TAG, "mBuf->size: %d", mBuf->size);
        unsigned long s = millis();

        _jpegDec.openRAM(mBuf->buf, mBuf->size, p->drawFunc);

        if (_useBigEndian) {
            _jpegDec.setPixelType(RGB565_BIG_ENDIAN);
        }
        _jpegDec.setMaxOutputSize(MAXOUTPUTSIZE);
        _jpegDec.decode(0, 0, 0);
        _jpegDec.close();

        total_decode_video_ms += millis() - s;
    }
    
    vQueueDelete(p->xqh);
    ESP_LOGI(TAG, "decode_task end.");
    vTaskDelete(NULL);
}

static void draw_task(void *arg) {
    paramDrawTask *p = (paramDrawTask *)arg;
    JPEGDRAW *pDraw;
    ESP_LOGI(TAG, "draw_task start.");
    
    while (xQueueReceive(p->xqh, &pDraw, portMAX_DELAY)) {
        // ESP_LOGD(TAG, "draw_task work start: x: %d, y: %d, iWidth: %d, iHeight: %d.", 
        //          pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight);
        p->drawFunc(pDraw);
        // ESP_LOGD(TAG, "draw_task work end.");
    }
    
    vQueueDelete(p->xqh);
    ESP_LOGI(TAG, "draw_task end.");
    vTaskDelete(NULL);
}

bool mjpeg_setup(Stream_t *input, int32_t mjpegBufSize, JPEG_DRAW_CALLBACK *pfnDraw,
                bool useBigEndian, int decodeAssignCore, int drawAssignCore) {
    _input = input;
    _mjpegBufSize = mjpegBufSize;
    _useBigEndian = useBigEndian;

    for (int i = 0; i < NUMBER_OF_DECODE_BUFFER; ++i) {
        _mjpegBufs[i].buf = (uint8_t *)heap_caps_malloc(mjpegBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (_mjpegBufs[i].buf) {
            ESP_LOGI(TAG, "#%d decode buffer allocated.", i);
        } else {
            ESP_LOGE(TAG, "#%d decode buffer allocation failed.", i);
            return false;
        }
    }
    _mjpeg_buf = _mjpegBufs[_mBufIdx].buf;

    if (!_read_buf) {
        _read_buf = (uint8_t *)heap_caps_malloc(READ_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (_read_buf) {
        ESP_LOGI(TAG, "Read buffer allocated.");
    } else {
        ESP_LOGE(TAG, "Read buffer allocation failed.");
        return false;
    }

    _xqh = xQueueCreate(NUMBER_OF_DRAW_BUFFER, sizeof(JPEGDRAW *));
    _pDrawTask.xqh = _xqh;
    _pDrawTask.drawFunc = pfnDraw;
    _pDecodeTask.xqh = xQueueCreate(NUMBER_OF_DECODE_BUFFER, sizeof(mjpegBuf *));
    _pDecodeTask.drawFunc = queueDrawMCU;

    xTaskCreatePinnedToCore(
        decode_task,
        "MJPEG_decode",
        4096,  // Taille de pile augmentée pour ESP-IDF
        &_pDecodeTask,
        configMAX_PRIORITIES - 1,
        &_decodeTask,
        decodeAssignCore);
        
    xTaskCreatePinnedToCore(
        draw_task,
        "MJPEG_draw",
        4096,  // Taille de pile augmentée pour ESP-IDF
        &_pDrawTask,
        configMAX_PRIORITIES - 1,
        &_draw_task,
        drawAssignCore);

    for (int i = 0; i < NUMBER_OF_DRAW_BUFFER; i++) {
        if (!jpegdraws[i].pPixels) {
            jpegdraws[i].pPixels = (uint16_t *)heap_caps_malloc(MAXOUTPUTSIZE * 16 * 16 * 2, MALLOC_CAP_DMA);
        }
        if (jpegdraws[i].pPixels) {
            ESP_LOGI(TAG, "#%d draw buffer allocated.", i);
        } else {
            ESP_LOGE(TAG, "#%d draw buffer allocation failed.", i);
            return false;
        }
    }

    return true;
}

// Fonction pour lire des octets depuis le stream vers un buffer
size_t readBytes(Stream_t *stream, uint8_t *buffer, size_t length) {
    return stream->read(buffer, length);
}

bool mjpeg_read_frame() {
    if (_inputindex == 0) {
        _buf_read = readBytes(_input, _read_buf, READ_BUFFER_SIZE);
        _inputindex += _buf_read;
    }
    
    _mjpeg_buf_offset = 0;
    int i = 0;
    bool found_FFD8 = false;
    
    while ((_buf_read > 0) && (!found_FFD8)) {
        i = 0;
        while ((i < _buf_read - 1) && (!found_FFD8)) {
            if ((_read_buf[i] == 0xFF) && (_read_buf[i + 1] == 0xD8)) { // JPEG header
                // ESP_LOGD(TAG, "Found FFD8 at: %d.", i);
                found_FFD8 = true;
            }
            ++i;
        }
        if (found_FFD8) {
            --i;
        } else {
            _buf_read = readBytes(_input, _read_buf, READ_BUFFER_SIZE);
        }
    }
    
    uint8_t *_p = _read_buf + i;
    _buf_read -= i;
    bool found_FFD9 = false;
    
    if (_buf_read > 0) {
        i = 3;
        while ((_buf_read > 0) && (!found_FFD9)) {
            if ((_mjpeg_buf_offset > 0) && (_mjpeg_buf[_mjpeg_buf_offset - 1] == 0xFF) && (_p[0] == 0xD9)) { // JPEG trailer
                found_FFD9 = true;
            } else {
                while ((i < _buf_read - 1) && (!found_FFD9)) {
                    if ((_p[i] == 0xFF) && (_p[i + 1] == 0xD9)) { // JPEG trailer
                        found_FFD9 = true;
                        ++i;
                    }
                    ++i;
                }
            }

            // ESP_LOGD(TAG, "i: %d", i);
            memcpy(_mjpeg_buf + _mjpeg_buf_offset, _p, i);
            _mjpeg_buf_offset += i;
            int32_t o = _buf_read - i;
            
            if (o > 0) {
                // ESP_LOGD(TAG, "o: %d", o);
                memcpy(_read_buf, _p + i, o);
                _buf_read = readBytes(_input, _read_buf + o, READ_BUFFER_SIZE - o);
                _p = _read_buf;
                _inputindex += _buf_read;
                _buf_read += o;
                // ESP_LOGD(TAG, "_buf_read: %d", _buf_read);
            } else {
                _buf_read = readBytes(_input, _read_buf, READ_BUFFER_SIZE);
                _p = _read_buf;
                _inputindex += _buf_read;
            }
            i = 0;
        }
        
        if (found_FFD9) {
            // ESP_LOGD(TAG, "Found FFD9 at: %d.", _mjpeg_buf_offset);
            if (_mjpeg_buf_offset > _mjpegBufSize) {
                ESP_LOGE(TAG, "_mjpeg_buf_offset(%d) > _mjpegBufSize (%d)", _mjpeg_buf_offset, _mjpegBufSize);
            }
            return true;
        }
    }

    return false;
}

bool mjpeg_draw_frame() {
    mjpegBuf *mBuf = &_mjpegBufs[_mBufIdx];
    mBuf->size = _mjpeg_buf_offset;
    // ESP_LOGD(TAG, "_mjpegBufs[%d].size: %d.", _mBufIdx, _mjpegBufs[_mBufIdx].size);
    
    xQueueSend(_pDecodeTask.xqh, &mBuf, portMAX_DELAY);
    ++_mBufIdx;
    if (_mBufIdx >= NUMBER_OF_DECODE_BUFFER) {
        _mBufIdx = 0;
    }
    _mjpeg_buf = _mjpegBufs[_mBufIdx].buf;
    // ESP_LOGD(TAG, "queue decode_task end");

    return true;
}

// Fonction de nettoyage pour libérer les ressources
void mjpeg_cleanup() {
    // Libère les buffers de décodage
    for (int i = 0; i < NUMBER_OF_DECODE_BUFFER; ++i) {
        if (_mjpegBufs[i].buf) {
            free(_mjpegBufs[i].buf);
            _mjpegBufs[i].buf = NULL;
        }
    }
    
    // Libère le buffer de lecture
    if (_read_buf) {
        free(_read_buf);
        _read_buf = NULL;
    }
    
    // Libère les buffers de dessin
    for (int i = 0; i < NUMBER_OF_DRAW_BUFFER; i++) {
        if (jpegdraws[i].pPixels) {
            free(jpegdraws[i].pPixels);
            jpegdraws[i].pPixels = NULL;
        }
    }
}

#endif // MJPEG_DECODER_H
