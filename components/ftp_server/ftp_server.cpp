#include "ftp_server.h"
#include "../sd_mmc_card/sd_mmc_card.h"  // Pour la gestion de la carte SD/MMC
#include "esp_log.h"                     // Pour les logs ESP-IDF
#include <fcntl.h>                       // Pour les opérations sur les fichiers
#include <dirent.h>                      // Pour la lecture des répertoires
#include <sys/stat.h>                    // Pour les informations sur les fichiers (stat)
#include <unistd.h>                      // Pour close() et autres fonctions POSIX
#include <chrono>                        // Pour les timestamps
#include <ctime>                         // Pour la manipulation des dates
#include "esp_netif.h"                   // Pour la gestion réseau ESP-IDF
#include "esp_err.h"                     // Pour les codes d'erreur ESP-IDF

namespace esphome {
namespace ftp_server {

// Définition du tag pour les logs
static const char* TAG = "FTP_SERVER";

FTPServer::FTPServer()
    : port_(21), username_("admin"), password_("admin"), root_path_("/sdcard"), current_path_("/") {}

void FTPServer::setup() {
  // Initialisation du serveur FTP
  ftp_server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
  if (ftp_server_socket_ < 0) {
    ESP_LOGE(TAG, "Failed to create FTP server socket.");
    return;
  }

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(port_);

  if (bind(ftp_server_socket_, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind FTP server socket.");
    close(ftp_server_socket_);
    ftp_server_socket_ = -1;
    return;
  }

  if (listen(ftp_server_socket_, 5) < 0) {
    ESP_LOGE(TAG, "Failed to listen on FTP server socket.");
    close(ftp_server_socket_);
    ftp_server_socket_ = -1;
    return;
  }

  ESP_LOGI(TAG, "FTP server started on port %d", port_);
}

void FTPServer::loop() {
  handle_new_clients();

  for (size_t i = 0; i < client_sockets_.size(); ++i) {
    int client_socket = client_sockets_[i];
    if (client_socket != -1) {
      handle_ftp_client(client_socket);
    }
  }
}

void FTPServer::dump_config() {
  ESP_LOGCONFIG(TAG, "FTP Server:");
  ESP_LOGCONFIG(TAG, "  Port: %d", port_);
  ESP_LOGCONFIG(TAG, "  Username: %s", username_.c_str());
  ESP_LOGCONFIG(TAG, "  Root Path: %s", root_path_.c_str());
}

bool FTPServer::is_running() const { return ftp_server_socket_ != -1; }

void FTPServer::handle_new_clients() {
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  int new_client = accept(ftp_server_socket_, (struct sockaddr *)&client_addr, &client_len);
  if (new_client >= 0) {
    ESP_LOGI(TAG, "New client connected from %s:%d", inet_ntoa(client_addr.sin_addr),
             ntohs(client_addr.sin_port));
    client_sockets_.push_back(new_client);
    client_states_.push_back(FTP_WAIT_LOGIN);
    client_usernames_.emplace_back();
    client_current_paths_.emplace_back("/");
    send_response(new_client, 220, "Welcome to ESPHome FTP Server");
  }
}

void FTPServer::handle_ftp_client(int client_socket) {
  char buffer[1024];
  ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0) {
    // Client disconnected
    ESP_LOGI(TAG, "Client disconnected.");
    close(client_socket);
    auto it = std::find(client_sockets_.begin(), client_sockets_.end(), client_socket);
    if (it != client_sockets_.end()) {
      size_t index = std::distance(client_sockets_.begin(), it);
      client_sockets_.erase(it);
      client_states_.erase(client_states_.begin() + index);
      client_usernames_.erase(client_usernames_.begin() + index);
      client_current_paths_.erase(client_current_paths_.begin() + index);
    }
    return;
  }

  buffer[bytes_received] = '\0';
  ESP_LOGD(TAG, "Received command: %s", buffer);
  process_command(client_socket, buffer);
}

void FTPServer::process_command(int client_socket, const std::string& command) {
  std::string cmd = command.substr(0, 4);
  std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);

  if (cmd == "USER") {
    std::string username = command.substr(5);
    auto it = std::find(client_sockets_.begin(), client_sockets_.end(), client_socket);
    if (it != client_sockets_.end()) {
      size_t index = std::distance(client_sockets_.begin(), it);
      client_usernames_[index] = username;
      send_response(client_socket, 331, "User name okay, need password.");
    }
  } else if (cmd == "PASS") {
    std::string password = command.substr(5);
    auto it = std::find(client_sockets_.begin(), client_sockets_.end(), client_socket);
    if (it != client_sockets_.end()) {
      size_t index = std::distance(client_sockets_.begin(), it);
      if (authenticate(client_usernames_[index], password)) {
        client_states_[index] = FTP_LOGGED_IN;
        send_response(client_socket, 230, "User logged in, proceed.");
      } else {
        send_response(client_socket, 530, "Login incorrect.");
      }
    }
  } else if (cmd == "LIST") {
    auto it = std::find(client_sockets_.begin(), client_sockets_.end(), client_socket);
    if (it != client_sockets_.end()) {
      size_t index = std::distance(client_sockets_.begin(), it);
      list_directory(client_socket, client_current_paths_[index]);
    }
  } else if (cmd == "RETR") {
    std::string path = command.substr(5);
    start_file_download(client_socket, path);
  } else if (cmd == "QUIT") {
    send_response(client_socket, 221, "Goodbye.");
    close(client_socket);
    auto it = std::find(client_sockets_.begin(), client_sockets_.end(), client_socket);
    if (it != client_sockets_.end()) {
      size_t index = std::distance(client_sockets_.begin(), it);
      client_sockets_.erase(it);
      client_states_.erase(client_states_.begin() + index);
      client_usernames_.erase(client_usernames_.begin() + index);
      client_current_paths_.erase(client_current_paths_.begin() + index);
    }
  } else {
    send_response(client_socket, 502, "Command not implemented.");
  }
}

void FTPServer::send_response(int client_socket, int code, const std::string& message) {
  std::string response = std::to_string(code) + " " + message + "\r\n";
  send(client_socket, response.c_str(), response.size(), 0);
}

bool FTPServer::authenticate(const std::string& username, const std::string& password) {
  return username == username_ && password == password_;
}

void FTPServer::list_directory(int client_socket, const std::string& path) {
  std::string full_path = root_path_ + "/" + path;
  DIR* dir = opendir(full_path.c_str());
  if (!dir) {
    send_response(client_socket, 550, "Failed to open directory.");
    return;
  }

  int data_socket = open_data_connection(client_socket);
  if (data_socket == -1) {
    closedir(dir);
    send_response(client_socket, 425, "Can't open data connection.");
    return;
  }

  send_response(client_socket, 150, "Here comes the directory listing.");

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string line = entry->d_name;
    line += "\r\n";
    send(data_socket, line.c_str(), line.size(), 0);
  }

  closedir(dir);
  close_data_connection(client_socket);
  send_response(client_socket, 226, "Directory send OK.");
}

void FTPServer::start_file_download(int client_socket, const std::string& path) {
  std::string full_path = root_path_ + "/" + path;
  FILE* file = fopen(full_path.c_str(), "rb");
  if (!file) {
    send_response(client_socket, 550, "File not found or access denied.");
    return;
  }

  int data_socket = open_data_connection(client_socket);
  if (data_socket == -1) {
    fclose(file);
    send_response(client_socket, 425, "Can't open data connection.");
    return;
  }

  send_response(client_socket, 150, "Opening data connection.");

  char buffer[1024];
  size_t bytes_read;
  while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    if (send(data_socket, buffer, bytes_read, 0) < 0) {
      break;
    }
  }

  fclose(file);
  close_data_connection(client_socket);
  send_response(client_socket, 226, "Transfer complete.");
}

bool FTPServer::start_passive_mode(int client_socket) {
  passive_data_socket_ = socket(AF_INET, SOCK_STREAM, 0);
  if (passive_data_socket_ < 0) {
    return false;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = 0;

  if (bind(passive_data_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(passive_data_socket_);
    passive_data_socket_ = -1;
    return false;
  }

  if (listen(passive_data_socket_, 1) < 0) {
    close(passive_data_socket_);
    passive_data_socket_ = -1;
    return false;
  }

  socklen_t len = sizeof(addr);
  if (getsockname(passive_data_socket_, (struct sockaddr*)&addr, &len) < 0) {
    close(passive_data_socket_);
    passive_data_socket_ = -1;
    return false;
  }

  passive_data_port_ = ntohs(addr.sin_port);
  return true;
}

int FTPServer::open_data_connection(int client_socket) {
  if (passive_mode_enabled_) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int data_socket = accept(passive_data_socket_, (struct sockaddr*)&client_addr, &client_len);
    if (data_socket < 0) {
      return -1;
    }
    return data_socket;
  } else {
    // Active mode not implemented
    return -1;
  }
}

void FTPServer::close_data_connection(int client_socket) {
  if (passive_mode_enabled_ && passive_data_socket_ != -1) {
    close(passive_data_socket_);
    passive_data_socket_ = -1;
  }
}

}  // namespace ftp_server
}  // namespace esphome










