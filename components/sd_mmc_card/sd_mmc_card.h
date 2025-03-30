#pragma once

#include <vector>
#include <string>
#include <functional>

#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/core/defines.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

#ifdef USE_ESP_IDF
#include "sdmmc_cmd.h"
#endif

namespace esphome {
namespace sd_mmc_card {

static const char *const TAG = "sd_mmc";

// Unified enum for memory units
enum class MemoryUnits : uint8_t { BYTES = 0, KILOBYTES = 1, MEGABYTES = 2, GIGABYTES = 3, TERABYTES = 4, PETABYTES = 5 };

// File information structure
struct FileInfo {
  std::string path;
  size_t size;
  bool is_directory;

  FileInfo(std::string const &path, size_t size, bool is_directory)
    : path(path), size(size), is_directory(is_directory) {}
};

#ifdef USE_SENSOR
// File size sensor structure
struct FileSizeSensor {
  sensor::Sensor *sensor{nullptr};
  std::string path;

  FileSizeSensor() = default;
  FileSizeSensor(sensor::Sensor *sensor, std::string const &path)
    : sensor(sensor), path(path) {}
};
#endif

// Helper function to convert bytes to specified unit
long double convertBytes(uint64_t value, MemoryUnits unit);

class SdMmc : public Component {
 public:
  SdMmc() : mode_1bit_(false) {}

  void setup() override;
  void loop() override;
  void dump_config() override;

  // ===== FILE OPERATIONS =====

  // Standard file operations
  void write_file(const char *path, const uint8_t *buffer, size_t len);
  void write_file(const char *path, const uint8_t *buffer, size_t len, const char *mode);
  void append_file(const char *path, const uint8_t *buffer, size_t len);
  
  std::vector<uint8_t> read_file(const char *path);
  std::vector<uint8_t> read_file(std::string const &path) {
    return this->read_file(path.c_str());
  }
  
  bool delete_file(const char *path);
  bool delete_file(std::string const &path) {
    return this->delete_file(path.c_str());
  }

  // Chunked file operations
  void write_file_chunked(const char *path, const uint8_t *buffer, size_t len, const char *mode);
  
  // Process a file in chunks with a callback function
  bool process_file(const char *path, std::function<bool(const uint8_t*, size_t)> callback, size_t buffer_size = 4096);
  bool process_file(std::string const &path, std::function<bool(const uint8_t*, size_t)> callback, size_t buffer_size = 4096) {
    return this->process_file(path.c_str(), callback, buffer_size);
  }

  // File copy operation
  bool copy_file(const char *source_path, const char *dest_path);
  bool copy_file(std::string const &source_path, std::string const &dest_path) {
    return this->copy_file(source_path.c_str(), dest_path.c_str());
  }

  // Directory operations
  bool create_directory(const char *path);
  bool remove_directory(const char *path);
  
  // Directory listing
  std::vector<std::string> list_directory(const char *path, uint8_t depth = 0);
  std::vector<std::string> list_directory(std::string path, uint8_t depth = 0) {
    return this->list_directory(path.c_str(), depth);
  }
  
  std::vector<FileInfo> list_directory_file_info(const char *path, uint8_t depth = 0);
  std::vector<FileInfo> list_directory_file_info(std::string path, uint8_t depth = 0) {
    return this->list_directory_file_info(path.c_str(), depth);
  }

  // File/directory information
  size_t file_size(const char *path);
  size_t file_size(std::string const &path) {
    return this->file_size(path.c_str());
  }
  
  bool is_directory(const char *path);
  bool is_directory(std::string const &path) {
    return this->is_directory(path.c_str());
  }

  // ===== SENSOR CONFIGURATION =====
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

  // ===== PIN CONFIGURATION =====
  void set_clk_pin(uint8_t pin) { this->clk_pin_ = pin; }
  void set_cmd_pin(uint8_t pin) { this->cmd_pin_ = pin; }
  void set_data0_pin(uint8_t pin) { this->data0_pin_ = pin; }
  void set_data1_pin(uint8_t pin) { this->data1_pin_ = pin; }
  void set_data2_pin(uint8_t pin) { this->data2_pin_ = pin; }
  void set_data3_pin(uint8_t pin) { this->data3_pin_ = pin; }
  void set_mode_1bit(bool b) { this->mode_1bit_ = b; }
  void set_power_ctrl_pin(GPIOPin *pin) { this->power_ctrl_pin_ = pin; }

  // ===== STATUS AND ERROR HANDLING =====
  void update_sensors();
  
  enum class ErrorCode {
    NONE = 0,
    ERR_PIN_SETUP,
    ERR_MOUNT,
    ERR_NO_CARD,
  };
  
  bool is_failed() { return this->init_error_ != ErrorCode::NONE; }
  static std::string error_code_to_string(ErrorCode code);

 protected:
  // Internal methods
  void list_directory_file_info_rec(const char *path, uint8_t depth, std::vector<FileInfo> &list);
  
#ifdef USE_ESP32_FRAMEWORK_ARDUINO
  std::string sd_card_type_to_string(int type) const;
#endif
#ifdef USE_ESP_IDF
  std::string sd_card_type() const;
  sdmmc_card_t *card_{nullptr};
#endif

  // Member variables
  uint8_t clk_pin_;
  uint8_t cmd_pin_;
  uint8_t data0_pin_;
  uint8_t data1_pin_;
  uint8_t data2_pin_;
  uint8_t data3_pin_;
  bool mode_1bit_;
  GPIOPin *power_ctrl_pin_{nullptr};
  ErrorCode init_error_{ErrorCode::NONE};
  
#ifdef USE_SENSOR
  sensor::Sensor *used_space_sensor_{nullptr};
  sensor::Sensor *total_space_sensor_{nullptr};
  sensor::Sensor *free_space_sensor_{nullptr};
  std::vector<FileSizeSensor> file_size_sensors_;
  MemoryUnits memory_unit_{MemoryUnits::MEGABYTES};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *sd_card_type_text_sensor_{nullptr};
#endif
};

// ===== AUTOMATION ACTIONS =====

template<typename... Ts> class SdMmcWriteFileAction : public Action<Ts...> {
 public:
  SdMmcWriteFileAction(SdMmc *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(std::string, path)
  TEMPLATABLE_VALUE(std::vector<uint8_t>, data)

  void play(Ts... x) {
    auto path = this->path_.value(x...);
    auto buffer = this->data_.value(x...);
    this->parent_->write_file(path.c_str(), buffer.data(), buffer.size());
  }

 protected:
  SdMmc *parent_;
};

template<typename... Ts> class SdMmcAppendFileAction : public Action<Ts...> {
 public:
  SdMmcAppendFileAction(SdMmc *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(std::string, path)
  TEMPLATABLE_VALUE(std::vector<uint8_t>, data)

  void play(Ts... x) {
    auto path = this->path_.value(x...);
    auto buffer = this->data_.value(x...);
    this->parent_->append_file(path.c_str(), buffer.data(), buffer.size());
  }

 protected:
  SdMmc *parent_;
};

template<typename... Ts> class SdMmcCreateDirectoryAction : public Action<Ts...> {
 public:
  SdMmcCreateDirectoryAction(SdMmc *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(std::string, path)

  void play(Ts... x) {
    auto path = this->path_.value(x...);
    this->parent_->create_directory(path.c_str());
  }

 protected:
  SdMmc *parent_;
};

template<typename... Ts> class SdMmcRemoveDirectoryAction : public Action<Ts...> {
 public:
  SdMmcRemoveDirectoryAction(SdMmc *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(std::string, path)

  void play(Ts... x) {
    auto path = this->path_.value(x...);
    this->parent_->remove_directory(path.c_str());
  }

 protected:
  SdMmc *parent_;
};

template<typename... Ts> class SdMmcDeleteFileAction : public Action<Ts...> {
 public:
  SdMmcDeleteFileAction(SdMmc *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(std::string, path)

  void play(Ts... x) {
    auto path = this->path_.value(x...);
    this->parent_->delete_file(path.c_str());
  }

 protected:
  SdMmc *parent_;
};

}  // namespace sd_mmc_card
}  // namespace esphome
