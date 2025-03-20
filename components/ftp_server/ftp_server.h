#pragma once

#include <string>
#include <vector>
#include <map>
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

namespace esphome {
namespace ftp_server {

enum FTPClientState {
  FTP_WAIT_LOGIN,
  FTP_LOGGED_IN
};

class FTPServer {
 public:
  FTPServer();
  ~FTPServer();

  void setup();
  void loop();
  void dump_config();

  void set_port(uint16_t port) { port_ = port; }
  void set_username(const std::string &username) { username_ = username; }
  void set_password(const std::string &password) { password_ = password; }
  void set_root_path(const std::string &root_path) { root_path_ = root_path; }

 protected:
  void handle_new_clients();
  void handle_ftp_client(int client_socket);
  void process_command(int client_socket, const std::string& command);
  void send_response(int client_socket, int code, const std::string& message);
  bool authenticate(const std::string& username, const std::string& password);
  void list_directory(int data_socket, const std::string& path);
  bool change_directory(size_t client_index, const std::string& path);
  int accept_data_connection(int client_socket);
  void send_file(int client_socket, int data_socket, const std::string& filepath);
  void receive_file(int client_socket, int data_socket, const std::string& filepath);

  uint16_t port_{21};
  std::string username_{"admin"};
  std::string password_{"admin"};
  std::string root_path_{"/sdcard"};
  std::string current_path_;

  int ftp_server_socket_{-1};
  std::vector<int> client_sockets_;
  std::vector<FTPClientState> client_states_;
  std::vector<std::string> client_usernames_;
  std::vector<std::string> client_current_paths_;
  std::map<int, int> data_sockets_;

  TaskHandle_t ftp_task_handle_{nullptr};
  static void ftp_task(void* pvParameters);
};

}  // namespace ftp_server
}  // namespace esphome
