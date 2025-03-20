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
#include <lwip/sockets.h>
#include <errno.h>

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
  server_addr.sin_addr.s_addr = INADDR_ANY;

  // Add error handling for bind
  if (bind(ftp_server_socket_, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind FTP server socket. Error: %d", errno);
    close(ftp_server_socket_);
    return;
  }

  // Add error handling for listen
  if (listen(ftp_server_socket_, 5) < 0) {
    ESP_LOGE(TAG, "Failed to listen on FTP server socket. Error: %d", errno);
    close(ftp_server_socket_);
    return;
  }

  // Set socket to non-blocking mode
  int flags = fcntl(ftp_server_socket_, F_GETFL, 0);
  fcntl(ftp_server_socket_, F_SETFL, flags | O_NONBLOCK);

  ESP_LOGI(TAG, "FTP server started successfully on port %d", port_);
  ESP_LOGI(TAG, "Root directory: %s", root_path_.c_str());

  // Initialize current path
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
  ESP_LOGI(TAG, "  Port: %d", port_);
  ESP_LOGI(TAG, "  Root Path: %s", root_path_.c_str());
}

void FTPServer::handle_new_clients() {
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  int client_socket = accept(ftp_server_socket_, (struct sockaddr *)&client_addr, &client_len);
  
  if (client_socket >= 0) {
    // Set client socket to non-blocking mode
    int flags = fcntl(client_socket, F_GETFL, 0);
    fcntl(client_socket, F_SETFL, flags | O_NONBLOCK);
    
    ESP_LOGI(TAG, "New FTP client connected");
    client_sockets_.push_back(client_socket);
    client_states_.push_back(FTP_WAIT_LOGIN);
    client_usernames_.push_back("");
    client_current_paths_.push_back(root_path_);
    send_response(client_socket, 220, "Welcome to ESPHome FTP Server");
  } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
    ESP_LOGE(TAG, "Accept failed with error: %d", errno);
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
    if (errno != EWOULDBLOCK && errno != EAGAIN) {
      ESP_LOGW(TAG, "Socket error: %d", errno);
    }
  }
}

void FTPServer::process_command(int client_socket, const std::string& command) {
  ESP_LOGI(TAG, "Received FTP command: %s", command.c_str());
  
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

  // Extract command and argument
  std::string cmd;
  std::string arg;
  pos = cmd_str.find(' ');
  if (pos != std::string::npos) {
    cmd = cmd_str.substr(0, pos);
    arg = cmd_str.substr(pos + 1);
  } else {
    cmd = cmd_str;
  }

  // Convert command to uppercase for case-insensitive comparison
  for (char& c : cmd) {
    c = toupper(c);
  }

  if (cmd == "USER") {
    client_usernames_[client_index] = arg;
    send_response(client_socket, 331, "Password required for " + arg);
  } else if (cmd == "PASS") {
    if (authenticate(client_usernames_[client_index], arg)) {
      client_states_[client_index] = FTP_LOGGED_IN;
      send_response(client_socket, 230, "Login successful");
    } else {
      send_response(client_socket, 530, "Login incorrect");
    }
  } else if (client_states_[client_index] != FTP_LOGGED_IN) {
    send_response(client_socket, 530, "Not logged in");
  } else if (cmd == "SYST") {
    send_response(client_socket, 215, "UNIX Type: L8");
  } else if (cmd == "FEAT") {
    send_response(client_socket, 211, "Features:");
    send_response(client_socket, 211, " SIZE");
    send_response(client_socket, 211, " MDTM");
    send_response(client_socket, 211, "End");
  } else if (cmd == "PWD") {
    send_response(client_socket, 257, "\"" + client_current_paths_[client_index] + "\" is current directory");
  } else if (cmd == "TYPE") {
    send_response(client_socket, 200, "Type set to " + arg);
  } else if (cmd == "PASV") {
    // Create data socket for passive mode
    int data_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (data_socket < 0) {
      send_response(client_socket, 425, "Can't open data connection");
      return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;  // Let system choose port

    if (bind(data_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      close(data_socket);
      send_response(client_socket, 425, "Can't open data connection");
      return;
    }

    if (listen(data_socket, 1) < 0) {
      close(data_socket);
      send_response(client_socket, 425, "Can't open data connection");
      return;
    }

    // Get the port number assigned by the system
    socklen_t addr_len = sizeof(addr);
    if (getsockname(data_socket, (struct sockaddr *)&addr, &addr_len) < 0) {
      close(data_socket);
      send_response(client_socket, 425, "Can't open data connection");
      return;
    }

    uint16_t port = ntohs(addr.sin_port);
    
    // Get local IP address
    struct sockaddr_in local_addr;
    socklen_t local_addr_len = sizeof(local_addr);
    getsockname(client_socket, (struct sockaddr *)&local_addr, &local_addr_len);
    
    // Format response: (h1,h2,h3,h4,p1,p2)
    char response[64];
    uint8_t *ip = (uint8_t *)&local_addr.sin_addr.s_addr;
    snprintf(response, sizeof(response), "Entering Passive Mode (%d,%d,%d,%d,%d,%d)",
             ip[0], ip[1], ip[2], ip[3], port >> 8, port & 0xFF);
    
    send_response(client_socket, 227, response);
    
    // Store data socket for later use
    data_sockets_[client_socket] = data_socket;
  } else if (cmd == "LIST") {
    int data_socket = accept_data_connection(client_socket);
    if (data_socket < 0) {
      send_response(client_socket, 425, "Can't open data connection");
      return;
    }

    send_response(client_socket, 150, "Opening ASCII mode data connection for file list");
    list_directory(data_socket, client_current_paths_[client_index]);
    close(data_socket);
    send_response(client_socket, 226, "Directory send OK");
  } else if (cmd == "CWD") {
    if (change_directory(client_index, arg)) {
      send_response(client_socket, 250, "Directory successfully changed");
    } else {
      send_response(client_socket, 550, "Failed to change directory");
    }
  } else if (cmd == "CDUP") {
    if (change_directory(client_index, "..")) {
      send_response(client_socket, 250, "Directory successfully changed");
    } else {
      send_response(client_socket, 550, "Failed to change directory");
    }
  } else if (cmd == "RETR") {
    int data_socket = accept_data_connection(client_socket);
    if (data_socket < 0) {
      send_response(client_socket, 425, "Can't open data connection");
      return;
    }

    std::string filepath = client_current_paths_[client_index] + "/" + arg;
    send_file(client_socket, data_socket, filepath);
    close(data_socket);
  } else if (cmd == "STOR") {
    int data_socket = accept_data_connection(client_socket);
    if (data_socket < 0) {
      send_response(client_socket, 425, "Can't open data connection");
      return;
    }

    std::string filepath = client_current_paths_[client_index] + "/" + arg;
    receive_file(client_socket, data_socket, filepath);
    close(data_socket);
  } else if (cmd == "DELE") {
    std::string filepath = client_current_paths_[client_index] + "/" + arg;
    if (unlink(filepath.c_str()) == 0) {
      send_response(client_socket, 250, "File deleted successfully");
    } else {
      send_response(client_socket, 550, "Delete operation failed");
    }
  } else if (cmd == "RMD") {
    std::string dirpath = client_current_paths_[client_index] + "/" + arg;
    if (rmdir(dirpath.c_str()) == 0) {
      send_response(client_socket, 250, "Directory removed");
    } else {
      send_response(client_socket, 550, "Remove directory operation failed");
    }
  } else if (cmd == "MKD") {
    std::string dirpath = client_current_paths_[client_index] + "/" + arg;
    if (mkdir(dirpath.c_str(), 0755) == 0) {
      send_response(client_socket, 257, "\"" + dirpath + "\" directory created");
    } else {
      send_response(client_socket, 550, "Create directory operation failed");
    }
  } else if (cmd == "QUIT") {
    send_response(client_socket, 221, "Goodbye");
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
    send_response(client_socket, 502, "Command not implemented");
  }
}

void FTPServer::send_response(int client_socket, int code, const std::string& message) {
  std::string response = std::to_string(code) + " " + message + "\r\n";
  send(client_socket, response.c_str(), response.length(), 0);
  ESP_LOGD(TAG, "Sent response: %s", response.c_str());
}

bool FTPServer::authenticate(const std::string& username, const std::string& password) {
  return username == username_ && password == password_;
}

void FTPServer::list_directory(int data_socket, const std::string& path) {
  DIR *dir = opendir(path.c_str());
  if (dir == nullptr) {
    ESP_LOGE(TAG, "Failed to open directory: %s", path.c_str());
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string name = entry->d_name;
    if (name == "." || name == "..") continue;

    std::string fullpath = path + "/" + name;
    struct stat st;
    if (stat(fullpath.c_str(), &st) == 0) {
      char timebuf[80];
      strftime(timebuf, sizeof(timebuf), "%b %d %H:%M", localtime(&st.st_mtime));
      
      char line[512];
      snprintf(line, sizeof(line),
               "%c%s 1 root root %8ld %s %s\r\n",
               S_ISDIR(st.st_mode) ? 'd' : '-',
               "rwxr-xr-x",
               (long)st.st_size,
               timebuf,
               name.c_str());
      
      send(data_socket, line, strlen(line), 0);
    }
  }

  closedir(dir);
}

bool FTPServer::change_directory(size_t client_index, const std::string& path) {
  std::string new_path;
  
  if (path.empty()) {
    return false;
  } else if (path[0] == '/') {
    new_path = root_path_ + path;
  } else if (path == "..") {
    new_path = client_current_paths_[client_index];
    size_t pos = new_path.find_last_of('/');
    if (pos != std::string::npos && new_path.length() > root_path_.length()) {
      new_path = new_path.substr(0, pos);
    }
  } else {
    new_path = client_current_paths_[client_index] + "/" + path;
  }

  DIR *dir = opendir(new_path.c_str());
  if (dir != nullptr) {
    closedir(dir);
    client_current_paths_[client_index] = new_path;
    return true;
  }
  return false;
}

int FTPServer::accept_data_connection(int client_socket) {
  auto it = data_sockets_.find(client_socket);
  if (it == data_sockets_.end()) {
    return -1;
  }

  int listen_socket = it->second;
  data_sockets_.erase(it);

  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  int data_socket = accept(listen_socket, (struct sockaddr *)&client_addr, &client_len);
  
  close(listen_socket);
  return data_socket;
}

void FTPServer::send_file(int client_socket, int data_socket, const std::string& filepath) {
  int file = open(filepath.c_str(), O_RDONLY);
  if (file < 0) {
    send_response(client_socket, 550, "Failed to open file");
    return;
  }

  struct stat st;
  if (fstat(file, &st) == 0) {
    send_response(client_socket, 150, "Opening BINARY mode data connection for " + filepath + " (" + std::to_string(st.st_size) + " bytes)");
  } else {
    send_response(client_socket, 150, "Opening BINARY mode data connection for " + filepath);
  }

  char buffer[2048];
  ssize_t bytes_read;
  while ((bytes_read = read(file, buffer, sizeof(buffer))) > 0) {
    send(data_socket, buffer, bytes_read, 0);
  }

  close(file);
  send_response(client_socket, 226, "Transfer complete");
}

void FTPServer::receive_file(int client_socket, int data_socket, const std::string& filepath) {
  int file = open(filepath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (file < 0) {
    send_response(client_socket, 550, "Failed to create file");
    return;
  }

  send_response(client_socket, 150, "Ok to send data");

  char buffer[2048];
  ssize_t bytes_read;
  while ((bytes_read = recv(data_socket, buffer, sizeof(buffer), 0)) > 0) {
    write(file, buffer, bytes_read);
  }

  close(file);
  send_response(client_socket, 226, "Transfer complete");
}

}  // namespace ftp_server
}  // namespace esphome
