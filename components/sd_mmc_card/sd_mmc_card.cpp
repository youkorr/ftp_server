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

SdMmc::SdMmc() : mode_1bit_(false), mounted_(false), card_(nullptr) {}

void SdMmc::setup() {
  ESP_LOGCONFIG(TAG, "Setting up SD/MMC card...");
  if (this->power_ctrl_pin_ != nullptr) {
    this->power_ctrl_pin_->setup();
    this->power_ctrl_pin_->digital_write(true);
    delay(100);
  }

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.clk = static_cast<gpio_num_t>(this->clk_pin_);
  slot_config.cmd = static_cast<gpio_num_t>(this->cmd_pin_);
  slot_config.d0 = static_cast<gpio_num_t>(this->data0_pin_);
  if (!this->mode_1bit_) {
    slot_config.d1 = static_cast<gpio_num_t>(this->data1_pin_);
    slot_config.d2 = static_cast<gpio_num_t>(this->data2_pin_);
    slot_config.d3 = static_cast<gpio_num_t>(this->data3_pin_);
  }

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024
  };

  sdmmc_card_t *card;
  esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to mount SD/MMC card: %s", esp_err_to_name(ret));
    this->init_error_ = ErrorCode::ERR_MOUNT;
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

void SdMmc::read_file_chunked(const char *path, std::function<bool(const uint8_t *, size_t)> callback) {
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return;
  FILE *file = fopen(path, "rb");
  if (file == nullptr) {
    ESP_LOGE(TAG, "Failed to open file %s: %s", path, strerror(errno));
    return;
  }

  uint8_t buffer[16384]; // 16KB chunks
  size_t bytes_read;
  while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    if (!callback(buffer, bytes_read)) {
      break;
    }
  }

  fclose(file);
}

std::vector<uint8_t> SdMmc::read_file(const char *path) {
  std::vector<uint8_t> content;
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return content;

  struct stat st;
  if (stat(path, &st) != 0) {
    ESP_LOGE(TAG, "Failed to stat file %s", path);
    return content;
  }

  content.reserve(st.st_size);
  this->read_file_chunked(path, [&content](const uint8_t *chunk, size_t chunk_size) {
    content.insert(content.end(), chunk, chunk + chunk_size);
    return true;
  });

  return content;
}

bool SdMmc::process_file(const char *path, std::function<bool(const uint8_t *, size_t)> callback, size_t buffer_size) {
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return false;

  FILE *file = fopen(path, "rb");
  if (file == nullptr) {
    ESP_LOGE(TAG, "Failed to open file %s", path);
    return false;
  }

  std::vector<uint8_t> buffer(buffer_size);
  bool result = true;

  while (true) {
    size_t bytes_read = fread(buffer.data(), 1, buffer.size(), file);
    if (bytes_read == 0) break;
    if (!callback(buffer.data(), bytes_read)) {
      result = false;
      break;
    }
  }

  fclose(file);
  return result;
}

bool SdMmc::copy_file(const char *source_path, const char *dest_path) {
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return false;

  FILE *src = fopen(source_path, "rb");
  if (src == nullptr) {
    ESP_LOGE(TAG, "Failed to open source file %s", source_path);
    return false;
  }

  FILE *dst = fopen(dest_path, "wb");
  if (dst == nullptr) {
    fclose(src);
    ESP_LOGE(TAG, "Failed to open destination file %s", dest_path);
    return false;
  }

  uint8_t buffer[16384]; // 16KB chunks
  size_t bytes_read;
  bool success = true;

  while ((bytes_read = fread(buffer, 1, sizeof(buffer), src)) > 0) {
    if (fwrite(buffer, 1, bytes_read, dst) != bytes_read) {
      success = false;
      break;
    }
  }

  fclose(src);
  fclose(dst);
  return success;
}

bool SdMmc::delete_file(const char *path) {
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return false;
  if (unlink(path) != 0) {
    ESP_LOGE(TAG, "Failed to delete file %s: %s", path, strerror(errno));
    return false;
  }
  return true;
}

bool SdMmc::create_directory(const char *path) {
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return false;
  if (mkdir(path, 0755) != 0) {
    ESP_LOGE(TAG, "Failed to create directory %s: %s", path, strerror(errno));
    return false;
  }
  return true;
}

bool SdMmc::remove_directory(const char *path) {
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return false;
  if (rmdir(path) != 0) {
    ESP_LOGE(TAG, "Failed to remove directory %s: %s", path, strerror(errno));
    return false;
  }
  return true;
}

bool SdMmc::file_exists(const char *path) {
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return false;
  struct stat st;
  return stat(path, &st) == 0;
}

size_t SdMmc::file_size(const char *path) {
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return 0;
  struct stat st;
  if (stat(path, &st) != 0) {
    return 0;
  }
  return st.st_size;
}

bool SdMmc::is_directory(const char *path) {
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return false;
  struct stat st;
  if (stat(path, &st) != 0) {
    return false;
  }
  return S_ISDIR(st.st_mode);
}

std::vector<std::string> SdMmc::list_directory(const char *path, uint8_t depth) {
  std::vector<std::string> result;
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return result;

  DIR *dir = opendir(path);
  if (dir == nullptr) {
    ESP_LOGE(TAG, "Failed to open directory %s", path);
    return result;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    std::string full_path = std::string(path) + "/" + entry->d_name;
    result.push_back(full_path);
    if (depth > 0 && entry->d_type == DT_DIR) {
      auto subdir = this->list_directory(full_path.c_str(), depth - 1);
      result.insert(result.end(), subdir.begin(), subdir.end());
    }
  }

  closedir(dir);
  return result;
}

std::vector<FileInfo> SdMmc::list_directory_file_info(const char *path, uint8_t depth) {
  std::vector<FileInfo> result;
  this->list_directory_file_info_rec(path, depth, result);
  return result;
}

void SdMmc::list_directory_file_info_rec(const char *path, uint8_t depth, std::vector<FileInfo> &list) {
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return;

  DIR *dir = opendir(path);
  if (dir == nullptr) return;

  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    std::string full_path = std::string(path) + "/" + entry->d_name;
    struct stat st;
    if (stat(full_path.c_str(), &st) != 0) continue;
    list.emplace_back(full_path, st.st_size, S_ISDIR(st.st_mode));
    if (depth > 0 && S_ISDIR(st.st_mode)) {
      this->list_directory_file_info_rec(full_path.c_str(), depth - 1, list);
    }
  }

  closedir(dir);
}

void SdMmc::update_sensors() {
#ifdef USE_SENSOR
  if (!this->mounted_ || this->init_error_ != ErrorCode::NONE) return;

  sdmmc_card_t *card = static_cast<sdmmc_card_t *>(this->card_);
  uint64_t capacity = (uint64_t)card->csd.capacity * card->csd.sector_size;
  FATFS *fs;
  DWORD fre_clust;
  if (f_getfree("0:", &fre_clust, &fs) != FR_OK) {
    ESP_LOGE(TAG, "Failed to get free space");
    return;
  }

  uint64_t free_size = (uint64_t)fre_clust * fs->csize * card->csd.sector_size;
  uint64_t used_size = capacity - free_size;

  if (this->used_space_sensor_ != nullptr) {
    this->used_space_sensor_->publish_state(convertBytes(used_size, this->memory_unit_));
  }
  if (this->total_space_sensor_ != nullptr) {
    this->total_space_sensor_->publish_state(convertBytes(capacity, this->memory_unit_));
  }
  if (this->free_space_sensor_ != nullptr) {
    this->free_space_sensor_->publish_state(convertBytes(free_size, this->memory_unit_));
  }

  for (auto &sensor : this->file_size_sensors_) {
    if (sensor.first != nullptr) {
      size_t size = this->file_size(sensor.second.c_str());
      sensor.first->publish_state(convertBytes(size, this->memory_unit_));
    }
  }
#endif

#ifdef USE_TEXT_SENSOR
  if (this->sd_card_type_text_sensor_ != nullptr) {
    sdmmc_card_t *card = static_cast<sdmmc_card_t*>(this->card_);
    const char *type = "UNKNOWN";
    
    if (card->is_mmc) {
      type = "MMC";
    } else if (card->ocr & SD_OCR_CARD_CAPACITY) {
      type = "SDHC/SDXC";
    } else {
      type = "SDSC";
    }
    
    this->sd_card_type_text_sensor_->publish_state(type);
  }
#endif
}

#ifdef USE_SENSOR
void SdMmc::add_file_size_sensor(sensor::Sensor *sensor, std::string const &path) {
  this->file_size_sensors_.emplace_back(sensor, path);
}
#endif

void SdMmc::dump_config() {
  ESP_LOGCONFIG(TAG, "SD/MMC Card:");
  ESP_LOGCONFIG(TAG, "  CLK Pin: %d", this->clk_pin_);
  ESP_LOGCONFIG(TAG, "  CMD Pin: %d", this->cmd_pin_);
  ESP_LOGCONFIG(TAG, "  Data Pins: %d,%d,%d,%d", 
               this->data0_pin_, this->data1_pin_, 
               this->data2_pin_, this->data3_pin_);
  ESP_LOGCONFIG(TAG, "  Mode: %s", this->mode_1bit_ ? "1-bit" : "4-bit");
  
  if (this->is_failed()) {
    ESP_LOGCONFIG(TAG, "  Status: %s", this->error_code_to_string(this->init_error_).c_str());
  } else {
    ESP_LOGCONFIG(TAG, "  Status: Initialized");
  }
}

}  // namespace sd_mmc_card
}  // namespace esphome

