#include "ftp_server.h"
#include "../sd_mmc_card/sd_mmc_card.h"  // Inclure le composant SD MMC Card
#include "esp_log.h"
#include <string>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <ctime>
#include "esp_netif.h"
#include "esp_err.h"

namespace esphome {
namespace ftp_server {

static const char *TAG = "ftp_server";

FTPServer::FTPServer() {}

void FTPServer::setup() {
  ESP_LOGI(TAG, "Setting up FTP server...");

  // Initialize FTP server socket
  ftp_server_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (ftp_server_socket_ < 0) {
    ESP_LOGE(TAG, "Failed to create FTP server socket");
    return;
  }

  // Set socket option to reuse address
  int opt = 1;
  if (setsockopt(ftp_server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    ESP_LOGE(TAG, "Failed to set socket options");
    close(ftp_server_socket_);
    return;
  }

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port_);
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(ftp_server_socket_, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind FTP server socket");
    close(ftp_server_socket_);
    return;
  }

  if (listen(ftp_server_socket_, 5) < 0) {
    ESP_LOGE(TAG, "Failed to listen on FTP server socket");
    close(ftp_server_socket_);
    return;
  }

  // Set socket to non-blocking mode
  fcntl(ftp_server_socket_, F_SETFL, O_NONBLOCK);

  ESP_LOGI(TAG, "FTP server started on port %d", port_);
  ESP_LOGI(TAG, "Root directory: %s", root_path_.c_str());
  // Set the default current path
  current_path_ = root_path_;
}

void FTPServer::loop() {
  handle_new_clients();
  for (size_t i = 0; i < client_sockets_.size(); i++) {
    handle_ftp_client(client_sockets_[i]);
  }
}

void FTPServer::dump_config() {
  ESP_LOGI(TAG, "FTP Server:");
  ESP_LOGI(TAG, " Port: %d", port_);
  ESP_LOGI(TAG, " Root Path: %s", root_path_.c_str());
}

void FTPServer::handle_new_clients() {
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  int client_socket = accept(ftp_server_socket_, (struct sockaddr *)&client_addr, &client_len);
  if (client_socket >= 0) {
    // Set client socket to non-blocking mode
    fcntl(client_socket, F_SETFL, O_NONBLOCK);
    ESP_LOGI(TAG, "New FTP client connected");
    client_sockets_.push_back(client_socket);
    client_states_.push_back(FTP_WAIT_LOGIN);
    client_usernames_.push_back("");
    client_current_paths_.push_back(root_path_);
    send_response(client_socket, 220, "Welcome to ESPHome FTP Server (ESP-IDF)");
  }
}

void FTPServer::handle_ftp_client(int client_socket) {
  char buffer[512];
  int len = recv(client_socket, buffer, sizeof(buffer) - 1, MSG_DONTWAIT);
  if (len > 0) {
    buffer[len] = '\0';
    std::string command(buffer);
    process_command(client_socket, command);
  } else if (len == 0) {
    ESP_LOGI(TAG, "FTP client disconnected");
    close(client_socket);
    auto it = std::find(client_sockets_.begin(), client_sockets_.end(), client_socket);
    if (it != client_sockets_.end()) {
      size_t index = it - client_sockets_.begin();
      client_sockets_.erase(it);
      client_states_.erase(client_states_.begin() + index);
      client_usernames_.erase(client_usernames_.begin() + index);
      client_current_paths_.erase(client_current_paths_.begin() + index);
    }
  } else {
    // In non-blocking mode, EWOULDBLOCK or EAGAIN are expected errors
    if (errno != EWOULDBLOCK && errno != EAGAIN) {
      ESP_LOGW(TAG, "Socket error: %d", errno);
    }
  }
}

void FTPServer::process_command(int client_socket, const std::string& command) {
  ESP_LOGI(TAG, "FTP command: %s", command.c_str());
  std::string cmd_str = command;
  // Remove trailing CR LF
  size_t pos = cmd_str.find_first_of("\r\n");
  if (pos != std::string::npos) {
    cmd_str = cmd_str.substr(0, pos);
  }

  // Get client index
  auto it = std::find(client_sockets_.begin(), client_sockets_.end(), client_socket);
  if (it == client_sockets_.end()) {
    ESP_LOGE(TAG, "Client socket not found!");
    return;
  }

  size_t client_index = it - client_sockets_.begin();

  if (cmd_str.find("USER") == 0) {
    std::string username = cmd_str.substr(5);
    client_usernames_[client_index] = username;
    send_response(client_socket, 331, "Password required for " + username);
  } else if (cmd_str.find("PASS") == 0) {
    std::string password = cmd_str.substr(5);
    if (authenticate(client_usernames_[client_index], password)) {
      client_states_[client_index] = FTP_LOGGED_IN;
      send_response(client_socket, 230, "Login successful");
    } else {
      send_response(client_socket, 530, "Login incorrect");
    }
  } else if (client_states_[client_index] != FTP_LOGGED_IN) {
    send_response(client_socket, 530, "Not logged in");
  } else if (cmd_str.find("LIST") == 0) {
    std::string path = client_current_paths_[client_index];
    list_directory(client_socket, path);
  } else if (cmd_str.find("CWD") == 0) {
    std::string path = cmd_str.substr(4);
    if (path.empty()) {
      send_response(client_socket, 550, "Failed to change directory");
    } else {
      // Validate path is within root_path_
      std::string full_path;
      if (path[0] == '/') {
        full_path = root_path_ + path;
      } else {
        full_path = client_current_paths_[client_index] + "/" + path;
      }

      // Check if directory exists
      DIR *dir = opendir(full_path.c_str());
      if (dir != nullptr) {
        closedir(dir);
        client_current_paths_[client_index] = full_path;
        send_response(client_socket, 250, "Directory successfully changed");
      } else {
        send_response(client_socket, 550, "Failed to change directory");
      }
    }
  } else if (cmd_str.find("CDUP") == 0) {
    std::string current = client_current_paths_[client_index];
    size_t pos = current.find_last_of('/');
    if (pos != std::string::npos && current.length() > root_path_.length()) {
      client_current_paths_[client_index] = current.substr(0, pos);
      send_response(client_socket, 250, "Directory successfully changed");
    } else {
      send_response(client_socket, 550, "Failed to change directory");
    }
  } else if (cmd_str.find("QUIT") == 0) {
    send_response(client_socket, 221, "Goodbye");
    close(client_socket);
    client_sockets_.erase(client_sockets_.begin() + client_index);
    client_states_.erase(client_states_.begin() + client_index);
    client_usernames_.erase(client_usernames_.begin() + client_index);
    client_current_paths_.erase(client_current_paths_.begin() + client_index);
  } else if (cmd_str.find("PWD") == 0) {
    send_response(client_socket, 257, "\"" + client_current_paths_[client_index] + "\" is the current directory");
  } else if (cmd_str.find("TYPE") == 0) {
    send_response(client_socket, 200, "Type set to " + cmd_str.substr(5));
  } else if (cmd_str.find("PASV") == 0) {
    if (start_passive_mode(client_socket)) {
      passive_mode_enabled_ = true;
    } else {
      send_response(client_socket, 425, "Can't open passive connection.");
    }
  } else if (cmd_str.find("PORT") == 0) {
    send_response(client_socket, 502, "PORT command not supported yet");
  } else if (cmd_str.find("STOR") == 0) {
    std::string filename = cmd_str.substr(5);
    std::string full_path = client_current_paths_[client_index] + "/" + filename;
    start_file_upload(client_socket, full_path);
  } else if (cmd_str.find("RETR") == 0) {
    std::string filename = cmd_str.substr(5);
    std::string full_path = client_current_paths_[client_index] + "/" + filename;
    start_file_download(client_socket, full_path);
  } else if (cmd_str.find("SYST") == 0) {
    send_response(client_socket, 215, "UNIX Type: L8");
  } else if (cmd_str.find("FEAT") == 0) {
    send_response(client_socket, 211, "Extensions supported:");
    send_response(client_socket, 211, " SIZE");
    send_response(client_socket, 211, " MDTM");
    send_response(client_socket, 211, "End");
  } else if (cmd_str.find("SIZE") == 0) {
    std::string filename = cmd_str.substr(5);
    std::string full_path = client_current_paths_[client_index] + "/" + filename;
    struct stat file_stat;
    if (stat(full_path.c_str(), &file_stat) == 0) {
      send_response(client_socket, 213, std::to_string(file_stat.st_size));
    } else {
      send_response(client_socket, 550, "File not found");
    }
  } else if (cmd_str.find("DELE") == 0) {
      std::string filename = cmd_str.substr(5);
      std::string full_path = client_current_paths_[client_index] + "/" + filename;
      if (unlink(full_path.c_str()) == 0) {
        send_response(client_socket, 250, "File deleted");
      } else {
        send_response(client_socket, 550, "Delete operation failed");
      }
  } else {
    send_response(client_socket, 502, "Command not implemented");
  }
}

void FTPServer::send_response(int client_socket, int code, const std::string& message) {
  std::string response = std::to_string(code) + " " + message + "\r\n";
  send(client_socket, response.c_str(), response.length(), 0);
  ESP_LOGD(TAG, "Sent: %s", response.c_str());
}

bool FTPServer::authenticate(const std::string& username, const std::string& password) {
  return username == username_ && password == password_;
}

bool FTPServer::start_passive_mode(int client_socket) {
  // Créer un socket pour la connexion de données en mode passif
  passive_data_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (passive_data_socket_ < 0) {
    ESP_LOGE(TAG, "Failed to create passive data socket");
    return false;
  }

  // Configurer l'adresse du serveur pour le socket de données
  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  // Attribuer un port éphémère au socket de données
  data_addr.sin_port = htons(0);
  if (bind(passive_data_socket_, (struct sockaddr *)&data_addr, sizeof(data_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind passive data socket");
    close(passive_data_socket_);
    return false;
  }

  // Écouter sur le socket de données
  if (listen(passive_data_socket_, 1) < 0) {
    ESP_LOGE(TAG, "Failed to listen on passive data socket");
    close(passive_data_socket_);
    return false;
  }

  // Récupérer le numéro de port attribué
  struct sockaddr_in sin;
  socklen_t len = sizeof(sin);
  if (getsockname(passive_data_socket_, (struct sockaddr *)&sin, &len) == -1) {
    ESP_LOGE(TAG, "getsockname failed");
    close(passive_data_socket_);
    return false;
  }
  passive_data_port_ = ntohs(sin.sin_port);

  // Envoyer la réponse au client avec l'adresse IP et le port
  char ip_str[INET_ADDRSTRLEN];
  esp_netif_t *netif = esp_netif_get_default_netif();
  if (netif == nullptr) {
    ESP_LOGE(TAG, "No netif found");
    close(passive_data_socket_);
    return false;
  }
  esp_netif_ip_info_t ip_info;
  esp_err_t err = esp_netif_get_ip_info(netif, &ip_info);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get IP info: %s", esp_err_to_name(err));
    close(passive_data_socket_);
    return false;
  }
  inet_ntop(AF_INET, &ip_info.ip.addr, ip_str, INET_ADDRSTRLEN);

  // Convertir l'adresse IP en format numérique pour la réponse
  std::string ip_address_str = std::string(ip_str);
  std::replace(ip_address_str.begin(), ip_address_str.end(), '.', ',');

  // Construire la réponse PASV
  std::string pasv_response = "Entering Passive Mode (" + ip_address_str + "," +
                              std::to_string(passive_data_port_ / 256) + "," +
                              std::to_string(passive_data_port_ % 256) + ")";
  send_response(client_socket, 227, pasv_response);
  ESP_LOGI(TAG, "Passive mode started on port %d", passive_data_port_);
  return true;
}

int FTPServer::open_data_connection(int client_socket) {
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  int data_socket = accept(passive_data_socket_, (struct sockaddr *)&client_addr, &client_len);
  if (data_socket < 0) {
    ESP_LOGE(TAG, "Failed to accept passive data connection");
    return -1;
  }
  ESP_LOGI(TAG, "Passive data connection accepted");
  return data_socket;
}

void FTPServer::close_data_connection(int client_socket) {
  if (passive_data_socket_ != -1) {
    close(passive_data_socket_);
    passive_data_socket_ = -1;
    passive_data_port_ = -1;
    passive_mode_enabled_ = false;
    ESP_LOGI(TAG, "Passive data connection closed");
  }
}

void FTPServer::list_directory(int client_socket, const std::string& path) {
  DIR *dir = opendir(path.c_str());
  if (dir == nullptr) {
    send_response(client_socket, 550, "Failed to open directory");
    return;
  }

  int data_socket = open_data_connection(client_socket);
  if (data_socket < 0) {
    send_response(client_socket, 425, "Can't open data connection.");
    closedir(dir);
    return;
  }

  send_response(client_socket, 150, "Opening ASCII mode data connection for file list");

  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string entry_name = entry->d_name;
    if (entry_name == "." || entry_name == "..") {
      continue;  // Skip . and .. directories
    }

    std::string full_path = path + "/" + entry_name;
    struct stat entry_stat;
    if (stat(full_path.c_str(), &entry_stat) == 0) {
      char time_str[80];
      strftime(time_str, sizeof(time_str), "%b %d %H:%M", localtime(&entry_stat.st_mtime));
      char perm_str[11] = "----------";
      if (S_ISDIR(entry_stat.st_mode)) perm_str[0] = 'd';
      if (entry_stat.st_mode & S_IRUSR) perm_str[1] = 'r';
      if (entry_stat.st_mode & S_IWUSR) perm_str[2] = 'w';
      if (entry_stat.st_mode & S_IXUSR) perm_str[3] = 'x';
      if (entry_stat.st_mode & S_IRGRP) perm_str[4] = 'r';
      if (entry_stat.st_mode & S_IWGRP) perm_str[5] = 'w';
      if (entry_stat.st_mode & S_IXGRP) perm_str[6] = 'x';
      if (entry_stat.st_mode & S_IROTH) perm_str[7] = 'r';
      if (entry_stat.st_mode & S_IWOTH) perm_str[8] = 'w';
      if (entry_stat.st_mode & S_IXOTH) perm_str[9] = 'x';
      std::string user_name = "root";
      std::string group_name = "root";
      char list_item[512];
      snprintf(list_item, sizeof(list_item), "%s %3d %s %s %8ld %s %s\r\n",
               perm_str, 1, user_name.c_str(), group_name.c_str(),
               (long) entry_stat.st_size, time_str, entry_name.c_str());
      send(data_socket, list_item, strlen(list_item), 0);
    }
  }

  closedir(dir);
  close(data_socket);
  close_data_connection(client_socket);
  send_response(client_socket, 226, "Directory send OK");
}

void FTPServer::start_file_upload(int client_socket, const std::string& path) {
  int file_fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (file_fd < 0) {
    send_response(client_socket, 550, "Failed to open file for writing");
    return;
  }

  int data_socket = open_data_connection(client_socket);
  if (data_socket < 0) {
    send_response(client_socket, 425, "Can't open data connection.");
    close(file_fd);
    return;
  }

  send_response(client_socket, 150, "Opening connection for file upload");

  char buffer[2048];
  int len;
  while ((len = recv(data_socket, buffer, sizeof(buffer), 0)) > 0) {
    write(file_fd, buffer, len);
  }

  close(file_fd);
  close(data_socket);
  close_data_connection(client_socket);
  send_response(client_socket, 226, "File upload complete");
}

void FTPServer::start_file_download(int client_socket, const std::string& path) {
  int file_fd = open(path.c_str(), O_RDONLY);
  if (file_fd < 0) {
    send_response(client_socket, 550, "Failed to open file for reading");
    return;
  }

  int data_socket = open_data_connection(client_socket);
  if (data_socket < 0) {
    send_response(client_socket, 425, "Can't open data connection.");
    close(file_fd);
    return;
  }

  // Get file size
  struct stat file_stat;
  fstat(file_fd, &file_stat);
  std::string size_msg = "Opening connection for file download (" +
                          std::to_string(file_stat.st_size) + " bytes)";
  send_response(client_socket, 150, size_msg);

  char buffer[2048];
  int len;
  while ((len = read(file_fd, buffer, sizeof(buffer))) > 0) {
    send(data_socket, buffer, len, 0);
  }

  close(file_fd);
  close(data_socket);
  close_data_connection(client_socket);
  send_response(client_socket, 226, "File download complete");
}

bool FTPServer::is_running() const {
  return ftp_server_socket_ != -1;
}

}  // namespace ftp_server
}  // namespace esphome






