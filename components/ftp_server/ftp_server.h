#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sd_mmc_card/sd_mmc_card.h"
#include <string>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>

namespace esphome {
namespace ftp_server {

enum FTPClientState {
  FTP_WAIT_LOGIN,
  FTP_LOGGED_IN
};

class FTPServer : public Component {
 public:
  FTPServer();

  void setup() override;
  void loop() override;
  void dump_config() override;
  
  float get_setup_priority() const override { 
    return setup_priority::LATE - 1; 
  }

  void set_port(uint16_t port) { port_ = port; }
  void set_username(const std::string &username) { username_ = username; }
  void set_password(const std::string &password) { password_ = password; }
  void set_root_path(const std::string &root_path) { root_path_ = root_path; }
  void set_sd_mmc_card(SDMMCCard* card) { sd_mmc_card_ = card; }
  
  bool is_running() const;

 protected:
  void handle_new_clients();
  void handle_ftp_client(int client_socket);
  void process_command(int client_socket, const std::string& command);
  void send_response(int client_socket, int code, const std::string& message);
  bool authenticate(const std::string& username, const std::string& password);
  void list_directory(int client_socket, const std::string& path);
  void start_file_upload(int client_socket, const std::string& path);
  void start_file_download(int client_socket, const std::string& path);

  // Pointeur vers la carte SD
  SDMMCCard* sd_mmc_card_ = nullptr;

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

 private:
  bool is_valid_sd_path(const std::string& path) const;
};

}  // namespace ftp_server
}  // namespace esphome



