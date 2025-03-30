#pragma once

#include <vector>
#include <string>
#include <functional>
#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
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

  void set_clk_pin(int pin) { this->clk_pin_ = pin; }
  void set_cmd_pin(int pin) { this->cmd_pin_ = pin; }
  void set_data0_pin(int pin) { this->data0_pin_ = pin; }
  void set_data1_pin(int pin) { this->data1_pin_ = pin; }
  void set_data2_pin(int pin) { this->data2_pin_ = pin; }
  void set_data3_pin(int pin) { this->data3_pin_ = pin; }
  void set_power_ctrl_pin(GPIOPin *pin) { this->power_ctrl_pin_ = pin; }

  void set_memory_unit(MemoryUnits unit) { this->memory_unit_ = unit; }

 protected:
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
};

}  // namespace sd_mmc_card
}  // namespace esphome

