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

namespace esphome {
namespace sd_mmc_card {

enum class ErrorCode : uint8_t {
  NONE,
  ERR_PIN_SETUP,
  ERR_MOUNT,
  ERR_NO_CARD
};

enum class MemoryUnits : uint8_t {
  BYTES,
  KILOBYTES,
  MEGABYTES,
  GIGABYTES,
  TERABYTES,
  PETABYTES
};

struct FileInfo {
  std::string path;
  size_t size;
  bool is_directory;

  FileInfo(std::string const &path, size_t size, bool is_directory)
      : path(path), size(size), is_directory(is_directory) {}
};

class SdMmc : public Component {
 public:
  SdMmc();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_clk_pin(int pin) { this->clk_pin_ = pin; }
  void set_cmd_pin(int pin) { this->cmd_pin_ = pin; }
  void set_data0_pin(int pin) { this->data0_pin_ = pin; }
  void set_data1_pin(int pin) { this->data1_pin_ = pin; }
  void set_data2_pin(int pin) { this->data2_pin_ = pin; }
  void set_data3_pin(int pin) { this->data3_pin_ = pin; }
  void set_power_ctrl_pin(GPIOPin *pin) { this->power_ctrl_pin_ = pin; }
  void set_mode_1bit(bool mode) { this->mode_1bit_ = mode; }

  void write_file_chunked(const char *path, const uint8_t *buffer, size_t len, const char *mode);
  void write_file(const char *path, const uint8_t *buffer, size_t len);
  void write_file(const char *path, const uint8_t *buffer, size_t len, const char *mode);
  void append_file(const char *path, const uint8_t *buffer, size_t len);
  std::vector<uint8_t> read_file(const char *path);
  void read_file_chunked(const char *path, std::function<bool(const uint8_t *, size_t)> callback);
  bool process_file(const char *path, std::function<bool(const uint8_t *, size_t)> callback, size_t buffer_size);
  bool copy_file(const char *source_path, const char *dest_path);
  bool delete_file(const char *path);
  bool create_directory(const char *path);
  bool remove_directory(const char *path);
  bool file_exists(const char *path);
  size_t file_size(const char *path);
  bool is_directory(const char *path);
  std::vector<std::string> list_directory(const char *path, uint8_t depth);
  std::vector<FileInfo> list_directory_file_info(const char *path, uint8_t depth);

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
  void unmount();
  void update_sensors();
  void list_directory_file_info_rec(const char *path, uint8_t depth, std::vector<FileInfo> &list);

  std::string error_code_to_string(ErrorCode code);

  bool mode_1bit_{false};
  bool mounted_{false};
  void *card_{nullptr};
  ErrorCode init_error_{ErrorCode::NONE};
  MemoryUnits memory_unit_{MemoryUnits::MEGABYTES};

  int clk_pin_{-1};
  int cmd_pin_{-1};
  int data0_pin_{-1};
  int data1_pin_{-1};
  int data2_pin_{-1};
  int data3_pin_{-1};
  GPIOPin *power_ctrl_pin_{nullptr};

#ifdef USE_SENSOR
  sensor::Sensor *used_space_sensor_{nullptr};
  sensor::Sensor *total_space_sensor_{nullptr};
  sensor::Sensor *free_space_sensor_{nullptr};
  std::vector<std::pair<sensor::Sensor *, std::string>> file_size_sensors_;
#endif

#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *sd_card_type_text_sensor_{nullptr};
#endif
};

}  // namespace sd_mmc_card
}  // namespace esphome


