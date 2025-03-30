#include "sd_mmc_card.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/core/hal.h"
#include <cstring>
#include <algorithm>
#include <vector>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_err.h"
#include "ff.h"

namespace esphome {
namespace sd_mmc_card {

static const char *const TAG = "sd_mmc";

long double convertBytes(uint64_t value, MemoryUnits unit) {
  switch (unit) {
    case MemoryUnits::KILOBYTES: return value / 1024.0;
    case MemoryUnits::MEGABYTES: return value / (1024.0 * 1024.0);
    case MemoryUnits::GIGABYTES: return value / (1024.0 * 1024.0 * 1024.0);
    case MemoryUnits::TERABYTES: return value / (1024.0 * 1024.0 * 1024.0 * 1024.0);
    case MemoryUnits::PETABYTES: return value / (1024.0 * 1024.0 * 1024.0 * 1024.0 * 1024.0);
    default: return value;
  }
}

std::string SdMmc::error_code_to_string(ErrorCode code) {
  switch (code) {
    case ErrorCode::NONE: return "No error";
    case ErrorCode::ERR_PIN_SETUP: return "Pin setup error";
    case ErrorCode::ERR_MOUNT: return "Mount error";
    case ErrorCode::ERR_NO_CARD: return "No SD card detected";
    default: return "Unknown error";
  }
}

SdMmc::SdMmc() : mounted_(false), card_(nullptr) {}

void SdMmc::setup() {
  ESP_LOGCONFIG(TAG, "Setting up SD/MMC card...");
  
  // Configuration de l'alimentation si nécessaire
  if (this->power_ctrl_pin_ != nullptr) {
    this->power_ctrl_pin_->setup();
    this->power_ctrl_pin_->digital_write(true);
    delay(100);
  }

  // Configuration du montage
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024
  };
  
  // Configuration de l'hôte
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;
  
  // Configuration du slot
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  
  // Définir la largeur du bus comme dans l'ancien code
  if (this->mode_1bit_) {
    slot_config.width = 1;
  } else {
    slot_config.width = 4;
  }
  
  // Configuration des broches
#ifdef SOC_SDMMC_USE_GPIO_MATRIX
  slot_config.clk = static_cast<gpio_num_t>(this->clk_pin_);
  slot_config.cmd = static_cast<gpio_num_t>(this->cmd_pin_);
  slot_config.d0 = static_cast<gpio_num_t>(this->data0_pin_);
  if (!this->mode_1bit_) {
    slot_config.d1 = static_cast<gpio_num_t>(this->data1_pin_);
    slot_config.d2 = static_cast<gpio_num_t>(this->data2_pin_);
    slot_config.d3 = static_cast<gpio_num_t>(this->data3_pin_);
  } else {
    // En mode 1-bit, désactiver explicitement les autres broches de données
    slot_config.d1 = GPIO_NUM_NC;
    slot_config.d2 = GPIO_NUM_NC;
    slot_config.d3 = GPIO_NUM_NC;
  }
#endif

  // Tenter de monter la carte SD/MMC
  sdmmc_card_t *card;
  esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
  
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to mount SD/MMC card: %s", esp_err_to_name(ret));
    this->init_error_ = ErrorCode::ERR_MOUNT;
    
    // Libération des broches en cas d'échec
    if (this->power_ctrl_pin_ != nullptr) {
      this->power_ctrl_pin_->digital_write(false);
    }
    return;
  }

  this->card_ = card;
  this->mounted_ = true;
  sdmmc_card_print_info(stdout, card);
  ESP_LOGI(TAG, "SD/MMC card initialized successfully");
  this->update_sensors();
}

void SdMmc::unmount() {
  if (this->mounted_) {
    esp_vfs_fat_sdcard_unmount("/sdcard", static_cast<sdmmc_card_t *>(this->card_));
    this->mounted_ = false;
    this->card_ = nullptr;
    
    // Désactiver l'alimentation après le démontage
    if (this->power_ctrl_pin_ != nullptr) {
      this->power_ctrl_pin_->digital_write(false);
    }
  }
}

void SdMmc::loop() {
  static uint32_t last_update = millis();
  if (millis() - last_update > 60000) {
    this->update_sensors();
    last_update = millis();
  }
}

// File Operations Implementation
void SdMmc::write_file_chunked(const char *path, const uint8_t *buffer, size_t len, const char *mode) {
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return;
  FILE *file = fopen(path, mode);
  if (file == nullptr) {
    ESP_LOGE(TAG, "Failed to open file %s: %s", path, strerror(errno));
    return;
  }

  size_t remaining = len;
  const uint8_t *ptr = buffer;
  while (remaining > 0) {
    size_t to_write = std::min(remaining, static_cast<size_t>(16384)); // 16KB chunks
    size_t written = fwrite(ptr, 1, to_write, file);
    if (written != to_write) {
      ESP_LOGE(TAG, "Write failed: %s", strerror(errno));
      break;
    }
    ptr += written;
    remaining -= written;
  }

  fflush(file);
  fclose(file);
}

void SdMmc::write_file(const char *path, const uint8_t *buffer, size_t len) {
  this->write_file_chunked(path, buffer, len, "wb");
}

void SdMmc::write_file(const char *path, const uint8_t *buffer, size_t len, const char *mode) {
  this->write_file_chunked(path, buffer, len, mode);
}

void SdMmc::append_file(const char *path, const uint8_t *buffer, size_t len) {
  this->write_file_chunked(path, buffer, len, "ab");
}

std::vector<uint8_t> SdMmc::read_file(const char *path) {
  std::vector<uint8_t> data;
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return data;

  FILE *file = fopen(path, "rb");
  if (file == nullptr) {
    ESP_LOGE(TAG, "Failed to open file %s: %s", path, strerror(errno));
    return data;
  }

  fseek(file, 0, SEEK_END);
  size_t file_size = ftell(file);
  fseek(file, 0, SEEK_SET);

  data.resize(file_size);
  size_t read_size = fread(data.data(), 1, file_size, file);

  if (read_size != file_size) {
    ESP_LOGW(TAG, "Read size %zu does not match file size %zu", read_size, file_size);
    data.resize(read_size); // Adjust vector size if read was incomplete
  }

  fclose(file);
  return data;
}

void SdMmc::read_file_chunked(const char *path, std::function<bool(const uint8_t *, size_t)> callback) {
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return;

  FILE *file = fopen(path, "rb");
  if (file == nullptr) {
    ESP_LOGE(TAG, "Failed to open file %s: %s", path, strerror(errno));
    return;
  }

  uint8_t buffer[16384]; // 16KB buffer
  size_t bytes_read;

  while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    if (!callback(buffer, bytes_read)) {
      ESP_LOGD(TAG, "Callback returned false, stopping read");
      break;
    }
  }

  fclose(file);
}

bool SdMmc::process_file(const char *path, std::function<bool(const uint8_t *, size_t)> callback, size_t buffer_size) {
    if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return false;

    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "Failed to open file %s: %s", path, strerror(errno));
        return false;
    }

    std::vector<uint8_t> buffer(buffer_size);
    size_t bytes_read;

    while ((bytes_read = fread(buffer.data(), 1, buffer_size, file)) > 0) {
        if (!callback(buffer.data(), bytes_read)) {
            ESP_LOGD(TAG, "Callback returned false, stopping read");
            fclose(file);
            return false;
        }
    }

    fclose(file);
    return true;
}

bool SdMmc::copy_file(const char *source_path, const char *dest_path) {
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return false;

  FILE *source_file = fopen(source_path, "rb");
  if (source_file == nullptr) {
    ESP_LOGE(TAG, "Failed to open source file %s: %s", source_path, strerror(errno));
    return false;
  }

  FILE *dest_file = fopen(dest_path, "wb");
  if (dest_file == nullptr) {
    ESP_LOGE(TAG, "Failed to open destination file %s: %s", dest_path, strerror(errno));
    fclose(source_file);
    return false;
  }

  uint8_t buffer[16384]; // 16KB buffer
  size_t bytes_read;

  while ((bytes_read = fread(buffer, 1, sizeof(buffer), source_file)) > 0) {
    size_t bytes_written = fwrite(buffer, 1, bytes_read, dest_file);
    if (bytes_written != bytes_read) {
      ESP_LOGE(TAG, "Write failed: %s", strerror(errno));
      fclose(source_file);
      fclose(dest_file);
      return false;
    }
  }

  fclose(source_file);
  fclose(dest_file);
  return true;
}

bool SdMmc::delete_file(const char *path) {
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return false;

  int result = unlink(path);
  if (result != 0) {
    ESP_LOGE(TAG, "Failed to delete file %s: %s", path, strerror(errno));
    return false;
  }
  return true;
}

bool SdMmc::create_directory(const char *path) {
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return false;

  int result = mkdir(path, 0777);
  if (result != 0) {
    ESP_LOGE(TAG, "Failed to create directory %s: %s", path, strerror(errno));
    return false;
  }
  return true;
}

bool SdMmc::remove_directory(const char *path) {
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return false;

  int result = rmdir(path);
  if (result != 0) {
    ESP_LOGE(TAG, "Failed to remove directory %s: %s", path, strerror(errno));
    return false;
  }
  return true;
}

bool SdMmc::file_exists(const char *path) {
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return false;

  struct stat buffer;
  return (stat(path, &buffer) == 0);
}

size_t SdMmc::file_size(const char *path) {
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return 0;

  struct stat buffer;
  if (stat(path, &buffer) == 0) {
    return buffer.st_size;
  }
  return 0;
}

bool SdMmc::is_directory(const char *path) {
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return false;

  struct stat buffer;
  if (stat(path, &buffer) == 0 && (buffer.st_mode & S_IFDIR)) {
    return true;
  }
  return false;
}

std::vector<std::string> SdMmc::list_directory(const char *path, uint8_t depth) {
  std::vector<std::string> entries;
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return entries;

  DIR *dir = opendir(path);
  if (dir == nullptr) {
    ESP_LOGE(TAG, "Failed to open directory %s: %s", path, strerror(errno));
    return entries;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    entries.push_back(entry->d_name);
  }

  closedir(dir);
  return entries;
}

std::vector<FileInfo> SdMmc::list_directory_file_info(const char *path, uint8_t depth) {
    std::vector<FileInfo> file_info_list;
    list_directory_file_info_rec(path, depth, file_info_list);
    return file_info_list;
}

void SdMmc::list_directory_file_info_rec(const char *path, uint8_t depth, std::vector<FileInfo> &list) {
    if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return;

    DIR *dir = opendir(path);
    if (dir == nullptr) {
        ESP_LOGE(TAG, "Failed to open directory %s: %s", path, strerror(errno));
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        std::string full_path = std::string(path) + "/" + entry->d_name;
        struct stat buffer;
        if (stat(full_path.c_str(), &buffer) == 0) {
            bool is_dir = (buffer.st_mode & S_IFDIR) != 0;
            list.emplace_back(full_path, buffer.st_size, is_dir);

            if (is_dir && depth > 0) {
                list_directory_file_info_rec(full_path.c_str(), depth - 1, list);
            }
        } else {
            ESP_LOGW(TAG, "Failed to stat %s: %s", full_path.c_str(), strerror(errno));
        }
    }

    closedir(dir);
}

void SdMmc::update_sensors() {
#ifdef USE_SENSOR
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return;

  FATFS *fs;
  DWORD fre_clust, tot_clust;

  /* Get volume information like total and free space */
  FRESULT res = f_getfree("/sdcard", &fre_clust, &fs);
  if (res != FR_OK) {
    ESP_LOGE(TAG, "f_getfree failed: %d", res);
    return;
  }

  /* Get total sectors and free sectors */
  tot_clust = fs->n_fatent - 2;
  uint64_t total_space_bytes = (uint64_t) tot_clust * fs->csize * 512;
  uint64_t free_space_bytes = (uint64_t) fre_clust * fs->csize * 512;
  uint64_t used_space_bytes = total_space_bytes - free_space_bytes;

  if (this->total_space_sensor_ != nullptr) {
    this->total_space_sensor_->publish_state(convertBytes(total_space_bytes, this->memory_unit_));
  }
  if (this->free_space_sensor_ != nullptr) {
    this->free_space_sensor_->publish_state(convertBytes(free_space_bytes, this->memory_unit_));
  }
  if (this->used_space_sensor_ != nullptr) {
    this->used_space_sensor_->publish_state(convertBytes(used_space_bytes, this->memory_unit_));
  }

    // Update file size sensors
    for (auto &pair : file_size_sensors_) {
        sensor::Sensor *sensor = pair.first;
        const std::string &path = pair.second;
        size_t file_size_bytes = file_size(path.c_str());
        sensor->publish_state(convertBytes(file_size_bytes, this->memory_unit_));
    }
#endif

#ifdef USE_TEXT_SENSOR
  LOG_TEXT_SENSOR("  ", "SD Card Type", this->sd_card_type_text_sensor_);
#endif
}

void SdMmc::set_clk_pin(uint8_t pin) { this->clk_pin_ = pin; }
void SdMmc::set_cmd_pin(uint8_t pin) { this->cmd_pin_ = pin; }
void SdMmc::set_data0_pin(uint8_t pin) { this->data0_pin_ = pin; }
void SdMmc::set_data1_pin(uint8_t pin) { this->data1_pin_ = pin; }
void SdMmc::set_data2_pin(uint8_t pin) { this->data2_pin_ = pin; }
void SdMmc::set_data3_pin(uint8_t pin) { this->data3_pin_ = pin; }
void SdMmc::set_mode_1bit(bool mode) { this->mode_1bit_ = mode; }
void SdMmc::set_power_ctrl_pin(GPIOPin *pin) { this->power_ctrl_pin_ = pin; }

#ifdef USE_SENSOR
void SdMmc::add_file_size_sensor(sensor::Sensor *sensor, std::string const &path) {
    file_size_sensors_.push_back({sensor, path});
}
#endif

void SdMmc::dump_config() {
  ESP_LOGCONFIG(TAG, "SD MMC Component");
  ESP_LOGCONFIG(TAG, "  Mode 1 bit: %s", TRUEFALSE(this->mode_1bit_));
  ESP_LOGCONFIG(TAG, "  CLK Pin: %d", this->clk_pin_);
  ESP_LOGCONFIG(TAG, "  CMD Pin: %d", this->cmd_pin_);
  ESP_LOGCONFIG(TAG, "  DATA0 Pin: %d", this->data0_pin_);
  if (!this->mode_1bit_) {
    ESP_LOGCONFIG(TAG, "  DATA1 Pin: %d", this->data1_pin_);
    ESP_LOGCONFIG(TAG, "  DATA2 Pin: %d", this->data2_pin_);
    ESP_LOGCONFIG(TAG, "  DATA3 Pin: %d", this->data3_pin_);
  }

  if (this->power_ctrl_pin_ != nullptr) {
    LOG_PIN("  Power Ctrl Pin: ", this->power_ctrl_pin_);
  }

#ifdef USE_SENSOR
  ESP_LOGCONFIG(TAG, "  Total Space Sensor: %s", this->total_space_sensor_ != nullptr ? "Yes" : "No");
  ESP_LOGCONFIG(TAG, "  Free Space Sensor: %s", this->free_space_sensor_ != nullptr ? "Yes" : "No");
  ESP_LOGCONFIG(TAG, "  Used Space Sensor: %s", this->used_space_sensor_ != nullptr ? "Yes" : "No");
  ESP_LOGCONFIG(TAG, "  File Size Sensors: %zu", file_size_sensors_.size());
#endif

#ifdef USE_TEXT_SENSOR
  ESP_LOGCONFIG(TAG, "  SD Card Type Text Sensor: %s", this->sd_card_type_text_sensor_ != nullptr ? "Yes" : "No");
#endif

  ESP_LOGCONFIG(TAG, "  Error State: %s", error_code_to_string(this->init_error_).c_str());
}

}  // namespace sd_mmc_card
}  // namespace esphome



