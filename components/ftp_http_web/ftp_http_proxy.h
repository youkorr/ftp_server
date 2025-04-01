#pragma once

#include "esphome.h"
#include <vector>
#include <string>
#include <map>
#include <esp_http_server.h>
#include <lwip/sockets.h>

namespace esphome {
namespace ftp_http_proxy {

struct FTPFileInfo {
  std::string name;
  size_t size;
  bool is_directory;
  std::string modified_date;
};

class FTPHTTPProxy : public Component {
 public:
  void set_ftp_server(const std::string &server) { ftp_server_ = server; }
  void set_username(const std::string &username) { username_ = username; }
  void set_password(const std::string &password) { password_ = password; }
  void add_remote_path(const std::string &path) { remote_paths_.push_back(path); }
  void set_local_port(uint16_t port) { local_port_ = port; }
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return esphome::setup_priority::AFTER_WIFI; }
  
  // Point d'entrée public pour démarrer un téléchargement
  bool download_file(const std::string &remote_path, httpd_req_t *req);
  
  // Nouvelles méthodes pour l'interface web
  std::vector<FTPFileInfo> list_directory(const std::string &path);
  bool upload_file(const std::string &path, const uint8_t *data, size_t size);
  bool delete_file(const std::string &path);
  bool rename_file(const std::string &old_path, const std::string &new_path);
  bool create_directory(const std::string &path);

 protected:
  std::string ftp_server_;
  std::string username_;
  std::string password_;
  std::vector<std::string> remote_paths_;
  uint16_t local_port_{8000};
  httpd_handle_t server_{nullptr};
  int sock_{-1};
  int data_sock_{-1};
  int ftp_port_ = 21;
  
  // Méthodes FTP
  bool send_ftp_command(const std::string &cmd, std::string &response);
  bool connect_to_ftp();
  bool enter_passive_mode(int &data_port, std::string &data_ip);
  
  // Méthodes HTTP
  void setup_http_server();
  void embed_html_resources();
  
  // Gestionnaires de requêtes HTTP
  static esp_err_t http_req_handler(httpd_req_t *req);
  static esp_err_t http_api_handler(httpd_req_t *req);
  static esp_err_t http_ui_handler(httpd_req_t *req);
  static esp_err_t http_upload_handler(httpd_req_t *req);
  
  // Utilitaires
  std::string get_mime_type(const std::string &filename);
  std::string url_decode(const std::string &encoded);
  
  // Ressources embarquées pour l'UI
  static const char* HTML_INDEX;
  static const char* CSS_STYLES;
  static const char* JS_SCRIPTS;
  
  // Gestionnaire de sessions pour les téléchargements en plusieurs parties
  struct UploadSession {
    std::string filename;
    std::string temp_path;
    size_t size;
    size_t received;
  };
  std::map<std::string, UploadSession> upload_sessions_;
};

}  // namespace ftp_http_proxy
}  // namespace esphome
