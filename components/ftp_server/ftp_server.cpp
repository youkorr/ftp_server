#include "ftp_server.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace esphome {
namespace ftp_server {

static const char *TAG = "ftp_server";

FTPServer::FTPServer() {}

void FTPServer::setup() {
  ESP_LOGI(TAG, "Setting up FTP server...");

  // Initialize SD card
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024
  };
  sdmmc_card_t *card;
  esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
    return;
  }
  ESP_LOGI(TAG, "SD card mounted successfully");

  // Initialize FTP server socket
  ftp_server_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (ftp_server_socket_ < 0) {
    ESP_LOGE(TAG, "Failed to create FTP server socket");
    return;
  }

  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port_);
  server_addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(ftp_server_socket_, (struct sockaddr *)&server_addr, sizeof(server_addr))) {
    ESP_LOGE(TAG, "Failed to bind FTP server socket");
    close(ftp_server_socket_);
    return;
  }

  if (listen(ftp_server_socket_, 5)) {
    ESP_LOGE(TAG, "Failed to listen on FTP server socket");
    close(ftp_server_socket_);
    return;
  }

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
  ESP_LOGCONFIG(TAG, "FTP Server:");
  ESP_LOGCONFIG(TAG, "  Port: %d", port_);
  ESP_LOGCONFIG(TAG, "  Root Path: %s", root_path_.c_str());
}

void FTPServer::handle_new_clients() {
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  int client_socket = accept(ftp_server_socket_, (struct sockaddr *)&client_addr, &client_len);
  if (client_socket >= 0) {
    ESP_LOGI(TAG, "New FTP client connected");
    client_sockets_.push_back(client_socket);
    client_states_.push_back(FTP_WAIT_LOGIN);
    client_usernames_.push_back("");
    client_current_paths_.push_back(root_path_);
    send_response(client_socket, 220, "Welcome to ESPHome FTP Server");
  }
}

void FTPServer::handle_ftp_client(int client_socket) {
  char buffer[128];
  int len = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
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
  }
}

void FTPServer::process_command(int client_socket, const std::string& command) {
  ESP_LOGI(TAG, "FTP command: %s", command.c_str());

  if (command.find("USER") == 0) {
    std::string username = command.substr(5);
    username.erase(username.find_last_not_of("\r\n") + 1);
    client_usernames_[std::distance(client_sockets_.begin(), std::find(client_sockets_.begin(), client_sockets_.end(), client_socket))] = username;
    send_response(client_socket, 331, "Password required for " + username);
  } else if (command.find("PASS") == 0) {
    std::string password = command.substr(5);
    password.erase(password.find_last_not_of("\r\n") + 1);
    if (authenticate(client_usernames_[std::distance(client_sockets_.begin(), std::find(client_sockets_.begin(), client_sockets_.end(), client_socket)], password)) {
      client_states_[std::distance(client_sockets_.begin(), std::find(client_sockets_.begin(), client_sockets_.end(), client_socket))] = FTP_LOGGED_IN;
      send_response(client_socket, 230, "Login successful");
    } else {
      send_response(client_socket, 530, "Login incorrect");
    }
  } else if (command.find("LIST") == 0) {
    std::string path = client_current_paths_[std::distance(client_sockets_.begin(), std::find(client_sockets_.begin(), client_sockets_.end(), client_socket))];
    list_directory(client_socket, path);
  } else if (command.find("QUIT") == 0) {
    send_response(client_socket, 221, "Goodbye");
    close(client_socket);
  } else if (command.find("PWD") == 0) {
    send_response(client_socket, 257, "\"" + current_path_ + "\" is the current directory");
  } else if (command.find("TYPE") == 0) {
    send_response(client_socket, 200, "Type set to " + command.substr(5));
  } else if (command.find("PASV") == 0) {
    send_response(client_socket, 502, "Passive mode not supported");
  } else if (command.find("STOR") == 0) {
    std::string filename = command.substr(5);
    filename.erase(filename.find_last_not_of("\r\n") + 1);
    std::string full_path = current_path_ + "/" + filename;
    start_file_upload(client_socket, full_path);
  } else if (command.find("RETR") == 0) {
    std::string filename = command.substr(5);
    filename.erase(filename.find_last_not_of("\r\n") + 1);
    std::string full_path = current_path_ + "/" + filename;
    start_file_download(client_socket, full_path);
  } else {
    send_response(client_socket, 502, "Command not implemented");
  }
}

void FTPServer::send_response(int client_socket, int code, const std::string& message) {
  std::string response = std::to_string(code) + " " + message + "\r\n";
  send(client_socket, response.c_str(), response.length(), 0);
}

bool FTPServer::authenticate(const std::string& username, const std::string& password) {
  return username == username_ && password == password_;
}

void FTPServer::list_directory(int client_socket, const std::string& path) {
  DIR *dir = opendir(path.c_str());
  if (dir == nullptr) {
    send_response(client_socket, 550, "Failed to open directory");
    return;
  }

  send_response(client_socket, 150, "Opening directory listing");

  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string entry_name = entry->d_name;
    std::string full_path = path + "/" + entry_name;
    struct stat entry_stat;
    if (stat(full_path.c_str(), &entry_stat) == 0) {
      std::string entry_info = entry_name + (S_ISDIR(entry_stat.st_mode) ? "/" : "");
      send_response(client_socket, 226, entry_info);
    }
  }

  closedir(dir);
  send_response(client_socket, 226, "Directory send OK");
}

void FTPServer::start_file_upload(int client_socket, const std::string& path) {
  int file_fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (file_fd < 0) {
    send_response(client_socket, 550, "Failed to open file for writing");
    return;
  }

  send_response(client_socket, 150, "Opening file for upload");

  char buffer[512];
  int len;
  while ((len = recv(client_socket, buffer, sizeof(buffer), 0)) > 0) {
    write(file_fd, buffer, len);
  }

  close(file_fd);
  send_response(client_socket, 226, "File upload complete");
}

void FTPServer::start_file_download(int client_socket, const std::string& path) {
  int file_fd = open(path.c_str(), O_RDONLY);
  if (file_fd < 0) {
    send_response(client_socket, 550, "Failed to open file for reading");
    return;
  }

  send_response(client_socket, 150, "Opening file for download");

  char buffer[512];
  int len;
  while ((len = read(file_fd, buffer, sizeof(buffer))) > 0) {
    send(client_socket, buffer, len, 0);
  }

  close(file_fd);
  send_response(client_socket, 226, "File download complete");
}

}  // namespace ftp_server
}  // namespace esphome
