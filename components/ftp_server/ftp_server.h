#ifndef FTP_SERVER_H
#define FTP_SERVER_H

#include "esphome.h"

namespace esphome {
namespace ftp_server {

class FTPServer : public Component {
 public:
  void set_username(const std::string& username) { username_ = username; }
  void set_password(const std::string& password) { password_ = password; }
  void set_root_path(const std::string& root_path) { root_path_ = root_path; }
  void set_port(uint16_t port) { port_ = port; }

  void setup() override;
  void loop() override;

 protected:
  std::string username_;
  std::string password_;
  std::string root_path_;
  uint16_t port_;

  void process_command(int client_fd, const std::string& command);
  void list_directory(int client_fd, const std::string& path);
};

}  // namespace ftp_server
}  // namespace esphome

#endif




