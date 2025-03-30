#pragma once

#include <vector>
#include <string>
#include <functional>
#include <cstdint>

#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/core/defines.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

namespace esphome {
namespace sd_mmc_card {

static const char *const TAG = "sd_mmc";

// Card types for ESP-IDF
enum CardType : uint8_t {
  CARD_NONE = 0,
  CARD_MMC = 1,
  CARD_SD = 2,
  CARD_SDHC = 3
};

// Memory units
enum class MemoryUnits : uint8_t { 
  BYTES = 0, 
  KILOBYTES = 1, 
  MEGABYTES = 2, 
  GIGABYTES = 3, 
  TERABYTES = 4, 
  PETABYTES = 5 
};

// File information
struct FileInfo {
  std::string path;
  size_t size;
  bool is_directory;

  FileInfo(std::string const &path, size_t size, bool is_directory)
    : path(path), size(size), is_directory(is_directory) {}
};

#ifdef USE_SENSOR
struct FileSizeSensor {
  sensor::Sensor *sensor{nullptr};
  std::string path;

  FileSizeSensor() = default;
  FileSizeSensor(sensor::Sensor *sensor, std::string const &path)
    : sensor(sensor), path(path) {}
};
#endif

class SdMmc : public Component {
 public:
  SdMmc() : mode_1bit_(false), mounted_(false), card_(nullptr) {}

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

#ifdef USE_SENSOR
  void update_sensors(); // Ajout de la déclaration manquante.
#endif

#ifdef USE_SENSOR
  void set_used_space_sensor(sensor::Sensor *sens) { this->used_space_sensor_ = sens; }
  void set_total_space_sensor(sensor::Sensor *sens) { this->total_space_sensor_ = sens; }
  void set_free_space_sensor(sensor::Sensor *sens) { this->free_space_sensor_ = sens; }
  void add_file_size_sensor(sensor::Sensor *sensor, std::string const &path);
  void set_memory_unit(MemoryUnits unit) { this->memory_unit_ = unit; }
#endif

#ifdef USE_TEXT_SENSOR
  void set_sd_card_type_text_sensor(text_sensor::TextSensor *sens) { this->sd_card_type_text_sensor_ = sens; }
#endif

 protected:
  void list_directory_file_info_rec(const char *path, uint8_t depth, std::vector<FileInfo> &list);
  
#ifdef USE_SENSOR
  sensor::Sensor *used_space_sensor_{nullptr};
  sensor::Sensor *total_space_sensor_{nullptr};
  sensor::Sensor *free_space_sensor_{nullptr};
  std::vector<FileSizeSensor> file_size_sensors_;
#endif
  
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *sd_card_type_text_sensor_{nullptr};
#endif
};

} // namespace sd_mmc_card
} // namespace esphome

