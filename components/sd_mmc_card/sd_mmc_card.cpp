#include "sd_mmc_card.h"

#include <algorithm>
#include <cstring>

#include "math.h"
#include "esphome/core/log.h"

namespace esphome {
namespace sd_mmc_card {

static const char *TAG = "sd_mmc_card";
// Augmentation de la taille des chunks pour de meilleures performances
static const size_t CHUNK_SIZE = 16384;  // 16KB au lieu de 4KB
// Buffer statique pour éviter les allocations dynamiques répétées
static uint8_t transfer_buffer[CHUNK_SIZE];

#ifdef USE_SENSOR
FileSizeSensor::FileSizeSensor(sensor::Sensor *sensor, std::string const &path) : sensor(sensor), path(path) {}
#endif

// Méthode pour écrire un fichier en chunks - optimisée
void SdMmc::write_file_chunked(const char *path, const uint8_t *buffer, size_t len, const char *mode) {
  if (this->is_failed())
    return;

  FILE *f = fopen(path, mode);
  if (f == nullptr) {
    ESP_LOGE(TAG, "Failed to open file %s for writing: %s", path, strerror(errno));
    return;
  }

  // Configurer un buffer d'écriture plus grand mais statique pour éviter l'allocation dynamique
  // Utilisation du mode _IOFBF (fully buffered) pour maximiser les performances
  setvbuf(f, NULL, _IOFBF, CHUNK_SIZE);

  size_t remaining = len;
  size_t pos = 0;
  size_t flush_counter = 0;
  
  while (remaining > 0) {
    size_t chunk_size = std::min(remaining, CHUNK_SIZE);
    size_t written = fwrite(buffer + pos, 1, chunk_size, f);
    
    if (written != chunk_size) {
      ESP_LOGE(TAG, "Failed to write to file %s: %s", path, strerror(errno));
      fclose(f);
      return;
    }
    
    // Flush moins fréquemment (tous les ~64KB) pour augmenter les performances
    flush_counter += chunk_size;
    if (flush_counter >= CHUNK_SIZE * 4) {
      fflush(f);
      flush_counter = 0;
    }
    
    pos += chunk_size;
    remaining -= chunk_size;
    
    // Réduire la fréquence des logs pour améliorer les performances
    if (len > CHUNK_SIZE * 10 && (pos % (CHUNK_SIZE * 20) == 0)) {
      ESP_LOGV(TAG, "Writing progress: %.1f%%", (float)pos / len * 100);
    }
  }

  // Flush final pour s'assurer que toutes les données sont écrites
  fflush(f);
  fclose(f);
  ESP_LOGI(TAG, "Successfully wrote %u bytes to file %s", len, path);
  
  // Mettre à jour les capteurs après l'écriture du fichier
  this->update_sensors();
}

// Remplacer l'ancienne méthode pour utiliser la version chunked
void SdMmc::write_file(const char *path, const uint8_t *buffer, size_t len, const char *mode) {
  ESP_LOGV(TAG, "Writing to file: %s (mode: %s, size: %u bytes)", path, mode, len);
  this->write_file_chunked(path, buffer, len, mode);
}

void SdMmc::write_file(const char *path, const uint8_t *buffer, size_t len) {
  ESP_LOGV(TAG, "Writing to file: %s", path);
  this->write_file_chunked(path, buffer, len, "w");
}

void SdMmc::append_file(const char *path, const uint8_t *buffer, size_t len) {
  ESP_LOGV(TAG, "Appending to file: %s", path);
  this->write_file_chunked(path, buffer, len, "a");
}

// Méthode pour lire un fichier en chunks avec callback - optimisée
void SdMmc::read_file_chunked(const char *path, std::function<bool(const uint8_t*, size_t)> callback) {
  if (this->is_failed())
    return;

  FILE *f = fopen(path, "r");
  if (f == nullptr) {
    ESP_LOGE(TAG, "Failed to open file %s for reading: %s", path, strerror(errno));
    return;
  }

  // Configurer un buffer de lecture plus grand mais éviter l'allocation dynamique
  setvbuf(f, NULL, _IOFBF, CHUNK_SIZE);

  // Obtenir la taille du fichier
  fseek(f, 0, SEEK_END);
  size_t file_size = ftell(f);
  fseek(f, 0, SEEK_SET);
  
  ESP_LOGI(TAG, "Reading file %s, size: %u bytes", path, file_size);
  
  size_t total_read = 0;
  
  while (!feof(f)) {
    size_t read = fread(transfer_buffer, 1, CHUNK_SIZE, f);
    if (read > 0) {
      total_read += read;
      
      // Appeler le callback avec le chunk actuel
      bool continue_reading = callback(transfer_buffer, read);
      if (!continue_reading) {
        ESP_LOGW(TAG, "Reading stopped by callback at %u bytes", total_read);
        break;
      }
      
      // Réduire la fréquence des logs pour améliorer les performances
      if (file_size > CHUNK_SIZE * 10 && (total_read % (CHUNK_SIZE * 20) == 0)) {
        ESP_LOGV(TAG, "Reading progress: %.1f%%", (float)total_read / file_size * 100);
      }
    }
    
    if (ferror(f)) {
      ESP_LOGE(TAG, "Error reading file %s: %s", path, strerror(errno));
      break;
    }
  }
  
  fclose(f);
  ESP_LOGI(TAG, "Successfully read %u bytes from file %s", total_read, path);
}

// Méthode pour lire un fichier en entier avec optimisations pour la mémoire
std::vector<uint8_t> SdMmc::read_file(const char *path) {
  std::vector<uint8_t> content;
  
  if (this->is_failed())
    return content;
  
  // Obtenir la taille du fichier pour pré-allocation
  size_t size = this->file_size(path);
  
  // Si le fichier est trop grand pour tenir en mémoire, utiliser une approche par chunks
  if (size > 0) {
    if (size > 512 * 1024) {  // Si le fichier dépasse 512KB
      ESP_LOGW(TAG, "File %s is large (%u bytes). Consider using read_file_chunked for better memory usage", path, size);
    }
    
    try {
      // Pré-allouer pour éviter les redimensionnements
      content.reserve(size);
    } catch (std::bad_alloc &e) {
      ESP_LOGE(TAG, "Failed to allocate memory for file %s (%u bytes): %s", path, size, e.what());
      return content;
    }
  }
  
  // Utiliser read_file_chunked avec une méthode optimisée pour ajouter au vecteur
  this->read_file_chunked(path, [&content, size](const uint8_t* chunk, size_t chunk_size) -> bool {
    // Vérifier qu'on ne dépasse pas la capacité réservée
    if (content.size() + chunk_size > size) {
      // Ajuster la capacité si nécessaire
      try {
        content.reserve(content.size() + chunk_size);
      } catch (std::bad_alloc &e) {
        ESP_LOGE(TAG, "Memory allocation failed during read: %s", e.what());
        return false;  // Arrêter la lecture
      }
    }
    
    // Ajouter les données efficacement
    content.insert(content.end(), chunk, chunk + chunk_size);
    return true;
  });
  
  return content;
}

// Méthode pour copier des fichiers volumineux - optimisée
bool SdMmc::copy_file(const char *source_path, const char *dest_path) {
  ESP_LOGI(TAG, "Copying file from %s to %s", source_path, dest_path);
  
  if (this->is_failed())
    return false;
  
  FILE *src = fopen(source_path, "r");
  if (src == nullptr) {
    ESP_LOGE(TAG, "Failed to open source file %s: %s", source_path, strerror(errno));
    return false;
  }
  
  FILE *dst = fopen(dest_path, "w");
  if (dst == nullptr) {
    ESP_LOGE(TAG, "Failed to open destination file %s: %s", dest_path, strerror(errno));
    fclose(src);
    return false;
  }
  
  // Configurer de grands buffers pour la lecture et l'écriture
  setvbuf(src, NULL, _IOFBF, CHUNK_SIZE);
  setvbuf(dst, NULL, _IOFBF, CHUNK_SIZE);
  
  // Obtenir la taille du fichier source
  fseek(src, 0, SEEK_END);
  size_t file_size = ftell(src);
  fseek(src, 0, SEEK_SET);
  
  size_t total_copied = 0;
  bool success = true;
  size_t flush_counter = 0;
  
  while (!feof(src)) {
    size_t read_size = fread(transfer_buffer, 1, CHUNK_SIZE, src);
    if (read_size > 0) {
      size_t write_size = fwrite(transfer_buffer, 1, read_size, dst);
      if (write_size != read_size) {
        ESP_LOGE(TAG, "Failed to write to destination file: %s", strerror(errno));
        success = false;
        break;
      }
      
      total_copied += read_size;
      flush_counter += read_size;
      
      // Flush périodiquement mais moins fréquemment
      if (flush_counter >= CHUNK_SIZE * 4) {
        fflush(dst);
        flush_counter = 0;
      }
      
      // Réduire la fréquence des logs pour améliorer les performances
      if (file_size > CHUNK_SIZE * 10 && (total_copied % (CHUNK_SIZE * 20) == 0)) {
        ESP_LOGV(TAG, "Copy progress: %.1f%%", (float)total_copied / file_size * 100);
      }
    }
    
    if (ferror(src)) {
      ESP_LOGE(TAG, "Error reading source file: %s", strerror(errno));
      success = false;
      break;
    }
  }
  
  // Flush final pour s'assurer que toutes les données sont écrites
  fflush(dst);
  fclose(src);
  fclose(dst);
  
  if (success) {
    ESP_LOGI(TAG, "Successfully copied %u bytes", total_copied);
  }
  
  return success;
}

// Méthode pour supprimer un fichier
bool SdMmc::delete_file(const char *path) {
  ESP_LOGI(TAG, "Deleting file: %s", path);
  
  if (this->is_failed())
    return false;
  
  // Vérifier si le fichier existe avant de tenter de le supprimer
  if (!this->file_exists(path)) {
    ESP_LOGW(TAG, "File %s does not exist", path);
    return false;
  }
  
  // Utiliser la fonction remove() pour supprimer le fichier
  if (remove(path) != 0) {
    ESP_LOGE(TAG, "Failed to delete file %s: %s", path, strerror(errno));
    return false;
  }
  
  ESP_LOGI(TAG, "Successfully deleted file %s", path);
  
  // Mettre à jour les capteurs après la suppression du fichier
  this->update_sensors();
  
  return true;
}

// Méthode auxiliaire pour vérifier si un fichier existe
bool SdMmc::file_exists(const char *path) {
  if (this->is_failed())
    return false;
    
  FILE *f = fopen(path, "r");
  if (f == nullptr) {
    return false;
  }
  
  fclose(f);
  return true;
}

// Méthode pour initialiser la carte SD avec des paramètres optimisés pour la vitesse
void SdMmc::setup() {
  // Configurer SD_MMC pour la vitesse maximale supportée par le matériel
  // Mode 4-bit à haute vitesse (SDMMC_FREQ_HIGHSPEED)
  if (!SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_HIGHSPEED)) {
    ESP_LOGE(TAG, "SD card mount failed");
    this->mark_failed();
    return;
  }
  
  uint8_t card_type = SD_MMC.cardType();
  if (card_type == CARD_NONE) {
    ESP_LOGE(TAG, "No SD card attached");
    this->mark_failed();
    return;
  }
  
  ESP_LOGI(TAG, "SD card initialized successfully with optimized parameters");
  this->update_sensors();
}

// Méthode pour mettre à jour les capteurs
void SdMmc::update_sensors() {
#ifdef USE_SENSOR
  if (this->is_failed())
    return;
    
  SDFS_cardType_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    ESP_LOGW(TAG, "No SD card attached");
    return;
  }

  uint64_t cardSize = SD_MMC.cardSize();
  uint64_t usedBytes = SD_MMC.usedBytes();
  uint64_t freeBytes = cardSize - usedBytes;

  if (this->used_space_sensor_ != nullptr) {
    this->used_space_sensor_->publish_state(convertBytes(usedBytes, this->memory_unit_));
  }
  if (this->total_space_sensor_ != nullptr) {
    this->total_space_sensor_->publish_state(convertBytes(cardSize, this->memory_unit_));
  }
  if (this->free_space_sensor_ != nullptr) {
    this->free_space_sensor_->publish_state(convertBytes(freeBytes, this->memory_unit_));
  }

  // Mettre à jour les capteurs de taille de fichier
  for (auto &sensor : this->file_size_sensors_) {
    if (sensor.sensor != nullptr) {
      size_t size = this->file_size(sensor.path);
      sensor.sensor->publish_state(convertBytes(size, this->memory_unit_));
    }
  }
  
#ifdef USE_TEXT_SENSOR
  if (this->sd_card_type_text_sensor_ != nullptr) {
    std::string card_type_str;
    switch (cardType) {
      case CARD_MMC:
        card_type_str = "MMC";
        break;
      case CARD_SD:
        card_type_str = "SDSC";
        break;
      case CARD_SDHC:
        card_type_str = "SDHC";
        break;
      default:
        card_type_str = "UNKNOWN";
        break;
    }
    this->sd_card_type_text_sensor_->publish_state(card_type_str);
  }
#endif

#endif
}

void SdMmc::loop() {
  // Mettre à jour périodiquement les capteurs (peut être appelé moins fréquemment selon les besoins)
  static uint32_t last_update = 0;
  if (millis() - last_update > 60000) {  // Mettre à jour toutes les minutes
    this->update_sensors();
    last_update = millis();
  }
}


}  // namespace sd_mmc_card
}  // namespace esphome

