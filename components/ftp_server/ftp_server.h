#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include <string>
#include <vector>
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"

namespace esphome {
namespace ftp_server {

class FTPServer : public Component {
 public:
  FTPServer();
  
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  
  void set_username(std::string username) { username_ = username; }
  void set_password(std::string password) { password_ = password; }
  void set_root_path(std::string root_path) { root_path_ = root_path; }
  void set_port(uint16_t port) { port_ = port; }
  
 protected:
  enum FtpState {
    FTP_IDLE,
    FTP_WAIT_LOGIN,
    FTP_WAIT_PASSWORD,
    FTP_LOGGED_IN,
    FTP_WAIT_COMMAND,
    FTP_TRANSFER
  };
  
  enum TransferMode {
    NONE,
    PASSIVE,
    ACTIVE
  };
  
  void handle_new_clients();
  void handle_ftp_client(int client_socket);
  void process_command(int client_socket, const std::string& command);
  void send_response(int client_socket, int code, const std::string& message);
  
  bool authenticate(const std::string& username, const std::string& password);
  void start_data_connection();
  void stop_data_connection();
  bool open_file(const std::string& path, const char* mode);
  void close_file();
  void list_directory(int data_socket, const std::string& path);
  void send_file_data(int data_socket);
  void receive_file_data(int data_socket);
  
  std::string username_;
  std::string password_;
  std::string root_path_;
  uint16_t port_;
  
  int ftp_server_socket_{-1};
  int data_server_socket_{-1};
  int data_client_socket_{-1};
  
  FtpState state_{FTP_IDLE};
  TransferMode transfer_mode_{NONE};
  
  uint16_t data_port_{0};
  std::string data_ip_;
  
  std::string current_path_;
  std::string rename_from_;
  
  FILE* current_file_{nullptr};
  
  std::vector<char> buffer_;
  
  std::vector<int> client_sockets_;
  std::vector<FtpState> client_states_;
  std::vector<std::string> client_usernames_;
  std::vector<std::string> client_current_paths_;
};

}  // namespace ftp_server
}  // namespace esphome
