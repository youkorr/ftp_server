#pragma once

#include "esphome/core/component.h"
#include "esp_http_server.h"
#include <string>
#include <vector>

namespace esphome {
namespace ftp_http_proxy {

class FTPHTTPProxy : public Component {
public:
  void setup() override;
  void loop() override;
  
  void set_ftp_server(const std::string &ftp_server) { ftp_server_ = ftp_server; }
  void set_username(const std::string &username) { username_ = username; }
  void set_password(const std::string &password) { password_ = password; }
  void set_local_port(uint16_t local_port) { local_port_ = local_port; }
  void add_remote_path(const std::string &path) { remote_paths_.push_back(path); }
  
  // Gestionnaire statique public pour les requêtes HTTP
  static esp_err_t static_http_req_handler(httpd_req_t *req);

protected:
  // Méthode interne de traitement des requêtes
  esp_err_t internal_http_req_handler(httpd_req_t *req);
  
  bool connect_to_ftp();
  bool download_file(const std::string &remote_path, httpd_req_t *req);
  void setup_http_server();
  
  std::string ftp_server_;
  std::string username_;
  std::string password_;
  uint16_t local_port_{80};
  std::vector<std::string> remote_paths_;
  
  int sock_{-1};
  httpd_handle_t server_{nullptr};
};

}  // namespace ftp_http_proxy
}  // namespace esphome
