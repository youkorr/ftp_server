#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "mjpeg_decoder.h" // Notre fichier d'en-tête créé précédemment

static const char* TAG = "MJPEG_EXAMPLE";

// Exemple d'implémentation de Stream pour fichier SPIFFS
typedef struct {
    FILE* file;
} SPIFFSStream;

// Fonction de lecture pour le stream SPIFFS
size_t spiffs_read(void* buf, size_t size) {
    SPIFFSStream* stream = (SPIFFSStream*)_input; // Supposant que _input est accessible
    return fread(buf, 1, size, stream->file);
}

// Fonction pour vérifier les données disponibles
size_t spiffs_available() {
    SPIFFSStream* stream = (SPIFFSStream*)_input;
    long current = ftell(stream->file);
    fseek(stream->file, 0, SEEK_END);
    long size = ftell(stream->file);
    fseek(stream->file, current, SEEK_SET);
    return size - current;
}

// Implémentation de la fonction de dessin pour le décodeur MJPEG
// À adapter selon votre affichage (LCD, TFT, etc.)
int drawMCU(JPEGDRAW *pDraw) {
    uint16_t *pPixel = pDraw->pPixels;
    
    // Exemple : Afficher sur un écran LCD/TFT
    // lcd_draw_bitmap(pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight, pPixel);
    
    ESP_LOGI(TAG, "Drawing MCU at x=%d, y=%d, width=%d, height=%d", 
             pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight);
    
    return 1; // Return 1 pour continuer, 0 pour arrêter
}

// Initialisation du système de fichiers SPIFFS
static void init_spiffs(void) {
    ESP_LOGI(TAG, "Initializing SPIFFS");
    
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }
    
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting MJPEG Decoder Example");
    
    // Initialiser SPIFFS
    init_spiffs();
    
    // Ouvrir un fichier vidéo MJPEG depuis SPIFFS
    FILE* file = fopen("/spiffs/video.mjpeg", "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open video file");
        return;
    }
    
    // Créer un Stream pour le fichier
    SPIFFSStream spiffsStream = {
        .file = file
    };
    
    // Créer l'interface Stream requise par le décodeur
    Stream_t stream = {
        .read = spiffs_read,
        .available = spiffs_available
    };
    
    // Initialiser le décodeur MJPEG
    int mjpegBufferSize = 32 * 1024; // 32KB de buffer pour le décodage
    bool useBigEndian = false;       // Selon votre écran
    
    if (!mjpeg_setup(&stream, mjpegBufferSize, drawMCU, useBigEndian, 0, 1)) {
        ESP_LOGE(TAG, "Failed to initialize MJPEG decoder");
        fclose(file);
        return;
    }
    
    ESP_LOGI(TAG, "MJPEG decoder initialized successfully");
    
    // Boucle de lecture et d'affichage de la vidéo
    int frameCount = 0;
    unsigned long startTime = millis();
    
    while (1) {
        // Lire une frame
        if (mjpeg_read_frame()) {
            // Afficher la frame
            mjpeg_draw_frame();
            frameCount++;
            
            // Afficher des statistiques toutes les 30 frames
            if (frameCount % 30 == 0) {
                unsigned long currentTime = millis();
                float fps = frameCount * 1000.0 / (currentTime - startTime);
                ESP_LOGI(TAG, "FPS: %.2f", fps);
            }
            
            // Pause entre les frames
            vTaskDelay(pdMS_TO_TICKS(33)); // ~30fps
        } else {
            // Fin du fichier, recommencer ou terminer
            ESP_LOGI(TAG, "End of video file");
            
            // Pour boucler la vidéo:
            fseek(file, 0, SEEK_SET);
            
            // Ou pour quitter:
            // break;
        }
    }
    
    // Nettoyage
    mjpeg_cleanup();
    fclose(file);
    ESP_LOGI(TAG, "MJPEG decoder demo completed");
}
