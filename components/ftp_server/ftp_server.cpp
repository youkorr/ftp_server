#include "ftp_server.h"
#include <dirent.h>
#include <sys/stat.h>

namespace esphome {
namespace ftp_server {

void FTPServer::setup() {
  ESP_LOGI("ftp_server", "FTP Server starting on port %d", port_);
  // Initialize your FTP server here
}

void FTPServer::loop() {
  // Handle FTP server logic here
}

void FTPServer::process_command(int client_fd, const std::string& command) {
  // Process FTP commands
}

void FTPServer::list_directory(int client_fd, const std::string& path) {
  DIR* dir = opendir(path.c_str());
  if (!dir) {
    ESP_LOGE("ftp_server", "Failed to open directory: %s", path.c_str());
    return;
  }

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string entry_name = entry->d_name;
    std::string full_path = path + "/" + entry_name;

    struct stat st;
    if (stat(full_path.c_str(), &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        // Handle directory
      } else {
        // Handle file
      }
    }
  }

  closedir(dir);
}

}  // namespace ftp_server
}  // namespace esphome






