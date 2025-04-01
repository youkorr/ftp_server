#include "ftp_http_proxy.h"
#include "esphome/core/log.h"
#include <lwip/sockets.h>
#include <netdb.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <sstream>
#include <iomanip>

namespace esphome {
namespace ftp_http_proxy {

static const char *TAG = "ftp_proxy";

void FTPHTTPProxy::setup() {
  ESP_LOGCONFIG(TAG, "Setting up FTP/HTTP Proxy");
  this->start_web_server();
}

void FTPHTTPProxy::loop() {
  // Nothing to do here for now
}

bool FTPHTTPProxy::connect_ftp() {
  struct hostent *server = gethostbyname(ftp_server_.c_str());
  if (!server) {
    ESP_LOGE(TAG, "DNS lookup failed");
    return false;
  }

  sock_ = socket(AF_INET, SOCK_STREAM, 0);
  if (sock_ < 0) {
    ESP_LOGE(TAG, "Socket creation error: %s", strerror(errno));
    return false;
  }

  struct sockaddr_in serv_addr;
  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(21);
  serv_addr.sin_addr.s_addr = *((unsigned long *)server->h_addr);

  if (connect(sock_, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
    ESP_LOGE(TAG, "Connection failed: %s", strerror(errno));
    close(sock_);
    sock_ = -1;
    return false;
  }

  char buffer[256];
  int len = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (len <= 0 || !strstr(buffer, "220")) {
    ESP_LOGE(TAG, "FTP welcome failed");
    close(sock_);
    sock_ = -1;
    return false;
  }

  snprintf(buffer, sizeof(buffer), "USER %s\r\n", username_.c_str());
  if (send(sock_, buffer, strlen(buffer), 0) < 0) {
    ESP_LOGE(TAG, "USER command failed");
    close(sock_);
    sock_ = -1;
    return false;
  }

  len = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  snprintf(buffer, sizeof(buffer), "PASS %s\r\n", password_.c_str());
  if (send(sock_, buffer, strlen(buffer), 0) < 0) {
    ESP_LOGE(TAG, "PASS command failed");
    close(sock_);
    sock_ = -1;
    return false;
  }

  len = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (len <= 0 || !strstr(buffer, "230")) {
    ESP_LOGE(TAG, "Login failed");
    close(sock_);
    sock_ = -1;
    return false;
  }

  return true;
}

bool FTPHTTPProxy::send_ftp_command(const std::string &cmd, std::string &response) {
  if (send(sock_, cmd.c_str(), cmd.length(), 0) < 0) {
    ESP_LOGE(TAG, "Command send failed");
    return false;
  }

  char buffer[1024];
  int len = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (len <= 0) {
    ESP_LOGE(TAG, "No response received");
    return false;
  }

  buffer[len] = '\0';
  response = buffer;
  return true;
}

std::vector<FileInfo> FTPHTTPProxy::list_files() {
  std::vector<FileInfo> files;
  if (!connect_ftp()) {
    return files;
  }

  // Set passive mode
  std::string response;
  if (!send_ftp_command("PASV\r\n", response) || response.find("227") == std::string::npos) {
    ESP_LOGE(TAG, "Failed to enter passive mode");
    close(sock_);
    sock_ = -1;
    return files;
  }

  // Parse passive port
  char *ip_start = strchr(response.c_str(), '(');
  if (!ip_start) {
    ESP_LOGE(TAG, "Invalid PASV response");
    close(sock_);
    sock_ = -1;
    return files;
  }

  int ip[4], port[2];
  sscanf(ip_start, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]);
  int data_port = port[0] * 256 + port[1];

  // Create data socket
  int data_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Data socket creation failed");
    close(sock_);
    sock_ = -1;
    return files;
  }

  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  data_addr.sin_addr.s_addr = htonl((ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]);

  if (connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) < 0) {
    ESP_LOGE(TAG, "Data connection failed");
    close(data_sock);
    close(sock_);
    sock_ = -1;
    return files;
  }

  // Send LIST command
  if (!send_ftp_command("LIST\r\n", response) || response.find("150") == std::string::npos) {
    ESP_LOGE(TAG, "LIST command failed");
    close(data_sock);
    close(sock_);
    sock_ = -1;
    return files;
  }

  // Read directory listing
  std::string listing;
  char buffer[4096];
  int len;
  while ((len = recv(data_sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
    buffer[len] = '\0';
    listing += buffer;
  }

  // Parse listing
  std::istringstream iss(listing);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.empty()) continue;

    std::istringstream line_stream(line);
    std::string perms, links, owner, group, size, month, day, time_year, name;
    line_stream >> perms >> links >> owner >> group >> size >> month >> day >> time_year;
    std::getline(line_stream, name);

    // Skip parent and current directory entries
    if (name.find(".") == 0) {
      continue;
    }

    FileInfo file;
    file.name = name.substr(name.find_first_not_of(" "));
    try {
      file.size = std::stoul(size);
    } catch (...) {
      file.size = 0;
    }

    // Simple time parsing (could be improved)
    struct tm tm = {0};
    strptime((month + " " + day + " " + time_year).c_str(), "%b %d %H:%M", &tm);
    file.modified = mktime(&tm);

    files.push_back(file);
  }

  close(data_sock);

  // Check transfer complete
  if (!send_ftp_command("QUIT\r\n", response)) {
    ESP_LOGW(TAG, "QUIT command failed");
  }

  close(sock_);
  sock_ = -1;

  return files;
}

bool FTPHTTPProxy::upload_file(const std::string &local_path, const std::string &remote_path) {
  if (!connect_ftp()) {
    return false;
  }

  // Set passive mode
  std::string response;
  if (!send_ftp_command("PASV\r\n", response) || response.find("227") == std::string::npos) {
    ESP_LOGE(TAG, "Failed to enter passive mode");
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Parse passive port
  char *ip_start = strchr(response.c_str(), '(');
  if (!ip_start) {
    ESP_LOGE(TAG, "Invalid PASV response");
    close(sock_);
    sock_ = -1;
    return false;
  }

  int ip[4], port[2];
  sscanf(ip_start, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]);
  int data_port = port[0] * 256 + port[1];

  // Create data socket
  int data_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Data socket creation failed");
    close(sock_);
    sock_ = -1;
    return false;
  }

  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  data_addr.sin_addr.s_addr = htonl((ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]);

  if (connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) < 0) {
    ESP_LOGE(TAG, "Data connection failed");
    close(data_sock);
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Send STOR command
  std::string stor_cmd = "STOR " + remote_path + "\r\n";
  if (!send_ftp_command(stor_cmd, response) || response.find("150") == std::string::npos) {
    ESP_LOGE(TAG, "STOR command failed");
    close(data_sock);
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Open local file
  FILE *file = fopen(local_path.c_str(), "rb");
  if (!file) {
    ESP_LOGE(TAG, "Failed to open local file");
    close(data_sock);
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Transfer file
  char buffer[4096];
  size_t bytes_read;
  bool success = true;
  while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    if (send(data_sock, buffer, bytes_read, 0) < 0) {
      ESP_LOGE(TAG, "File transfer failed");
      success = false;
      break;
    }
  }

  fclose(file);
  close(data_sock);

  // Check transfer complete
  if (!send_ftp_command("QUIT\r\n", response) || response.find("226") == std::string::npos) {
    ESP_LOGW(TAG, "Transfer verification failed");
    success = false;
  }

  close(sock_);
  sock_ = -1;

  return success;
}

bool FTPHTTPProxy::delete_file(const std::string &path) {
  if (!connect_ftp()) {
    return false;
  }

  std::string cmd = "DELE " + path + "\r\n";
  std::string response;
  bool success = send_ftp_command(cmd, response) && response.find("250") != std::string::npos;

  if (!send_ftp_command("QUIT\r\n", response)) {
    ESP_LOGW(TAG, "QUIT command failed");
  }

  close(sock_);
  sock_ = -1;

  return success;
}

bool FTPHTTPProxy::rename_file(const std::string &from, const std::string &to) {
  if (!connect_ftp()) {
    return false;
  }

  std::string cmd = "RNFR " + from + "\r\n";
  std::string response;
  if (!send_ftp_command(cmd, response) || response.find("350") == std::string::npos) {
    ESP_LOGE(TAG, "RNFR command failed");
    close(sock_);
    sock_ = -1;
    return false;
  }

  cmd = "RNTO " + to + "\r\n";
  bool success = send_ftp_command(cmd, response) && response.find("250") != std::string::npos;

  if (!send_ftp_command("QUIT\r\n", response)) {
    ESP_LOGW(TAG, "QUIT command failed");
  }

  close(sock_);
  sock_ = -1;

  return success;
}

bool FTPHTTPProxy::download_file(const std::string &remote_path, httpd_req_t *req) {
  if (!connect_ftp()) {
    return false;
  }

  // Set passive mode
  std::string response;
  if (!send_ftp_command("PASV\r\n", response) || response.find("227") == std::string::npos) {
    ESP_LOGE(TAG, "Failed to enter passive mode");
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Parse passive port
  char *ip_start = strchr(response.c_str(), '(');
  if (!ip_start) {
    ESP_LOGE(TAG, "Invalid PASV response");
    close(sock_);
    sock_ = -1;
    return false;
  }

  int ip[4], port[2];
  sscanf(ip_start, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]);
  int data_port = port[0] * 256 + port[1];

  // Create data socket
  int data_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Data socket creation failed");
    close(sock_);
    sock_ = -1;
    return false;
  }

  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  data_addr.sin_addr.s_addr = htonl((ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]);

  if (connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) < 0) {
    ESP_LOGE(TAG, "Data connection failed");
    close(data_sock);
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Send RETR command
  std::string retr_cmd = "RETR " + remote_path + "\r\n";
  if (!send_ftp_command(retr_cmd, response) || response.find("150") == std::string::npos) {
    ESP_LOGE(TAG, "RETR command failed");
    close(data_sock);
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Stream file to HTTP client
  char buffer[4096];
  int bytes_received;
  bool success = true;
  while ((bytes_received = recv(data_sock, buffer, sizeof(buffer), 0)) > 0) {
    if (httpd_resp_send_chunk(req, buffer, bytes_received) != ESP_OK) {
      ESP_LOGE(TAG, "HTTP send failed");
      success = false;
      break;
    }
  }

  close(data_sock);

  // Check transfer complete
  if (!send_ftp_command("QUIT\r\n", response) || response.find("226") == std::string::npos) {
    ESP_LOGW(TAG, "Transfer verification failed");
    success = false;
  }

  close(sock_);
  sock_ = -1;

  // Finalize HTTP response
  httpd_resp_send_chunk(req, NULL, 0);
  return success;
}

void FTPHTTPProxy::start_web_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 8080;
  config.ctrl_port = 8081;
  config.stack_size = 16384;

  if (httpd_start(&this->server_, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start web server");
    return;
  }

  httpd_uri_t api_uri = {
    .uri = "/api/*",
    .method = HTTP_GET,
    .handler = &FTPHTTPProxy::api_handler,
    .user_ctx = this
  };
  httpd_register_uri_handler(this->server_, &api_uri);

  httpd_uri_t web_uri = {
    .uri = "/*",
    .method = HTTP_GET,
    .handler = &FTPHTTPProxy::web_handler,
    .user_ctx = this
  };
  httpd_register_uri_handler(this->server_, &web_uri);

  ESP_LOGI(TAG, "Web server started on port 8080");
}

esp_err_t FTPHTTPProxy::handle_api(httpd_req_t *req) {
  std::string uri(req->uri);
  
  if (uri == "/api/list") {
    auto files = this->list_files();
    std::string json = "{\"files\":[";
    
    for (size_t i = 0; i < files.size(); i++) {
      if (i > 0) json += ",";
      json += "{\"name\":\"" + files[i].name + "\",\"size\":" + 
              std::to_string(files[i].size) + ",\"modified\":" + 
              std::to_string(files[i].modified) + "}";
    }
    json += "]}";
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.size());
    return ESP_OK;
  }
  else if (uri.find("/api/download/") == 0) {
    std::string filename = uri.substr(14);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", ("attachment; filename=" + filename).c_str());
    
    bool success = this->download_file(filename, req);
    if (!success) {
      httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    }
    return ESP_OK;
  }
  else if (uri == "/api/delete") {
    char content[256];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
      return ESP_OK;
    }
    content[ret] = '\0';
    
    std::string filename;
    if (sscanf(content, "filename=%s", filename.c_str()) != 1) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
      return ESP_OK;
    }
    
    bool success = this->delete_file(filename);
    httpd_resp_set_type(req, "application/json");
    if (success) {
      httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    } else {
      httpd_resp_send(req, "{\"success\":false}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
  }
  else if (uri == "/api/rename") {
    char content[256];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
      return ESP_OK;
    }
    content[ret] = '\0';
    
    std::string old_name, new_name;
    if (sscanf(content, "old_name=%s&new_name=%s", old_name.c_str(), new_name.c_str()) != 2) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid parameters");
      return ESP_OK;
    }
    
    bool success = this->rename_file(old_name, new_name);
    httpd_resp_set_type(req, "application/json");
    if (success) {
      httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    } else {
      httpd_resp_send(req, "{\"success\":false}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
  }
  else if (uri == "/api/upload") {
    // Handle file upload (multipart form-data)
    // Implementation would be similar to the download but in reverse
    httpd_resp_send_err(req, HTTPD_501_NOT_IMPLEMENTED, "Not implemented");
    return ESP_OK;
  }

  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
  return ESP_OK;
}

esp_err_t FTPHTTPProxy::handle_web(httpd_req_t *req) {
  std::string uri(req->uri);
  if (uri == "/") {
    uri = "/index.html";
  }

  if (uri == "/index.html") {
    const char* html = R"(<!DOCTYPE html>
<html>
<head>
  <title>ESPHome FTP Proxy</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 20px; }
    table { border-collapse: collapse; width: 100%; }
    th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }
    tr:nth-child(even) { background-color: #f2f2f2; }
    button { margin: 2px; padding: 4px 8px; cursor: pointer; }
    .modal { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background-color: rgba(0,0,0,0.5); }
    .modal-content { background-color: white; margin: 100px auto; padding: 20px; width: 300px; }
  </style>
</head>
<body>
  <h1>FTP File Browser</h1>
  
  <div>
    <h2>Upload File</h2>
    <form id="uploadForm" enctype="multipart/form-data">
      <input type="file" name="file" required>
      <input type="submit" value="Upload">
    </form>
  </div>
  
  <table id="fileTable">
    <thead>
      <tr>
        <th>Name</th>
        <th>Size</th>
        <th>Modified</th>
        <th>Actions</th>
      </tr>
    </thead>
    <tbody></tbody>
  </table>
  
  <div id="renameModal" class="modal">
    <div class="modal-content">
      <span class="close">&times;</span>
      <h2>Rename File</h2>
      <form id="renameForm">
        <input type="hidden" id="oldName">
        <input type="text" id="newName" required>
        <input type="submit" value="Rename">
      </form>
    </div>
  </div>

  <script>
    async function loadFiles() {
      try {
        const response = await fetch('/api/list');
        const data = await response.json();
        const table = document.querySelector('#fileTable tbody');
        table.innerHTML = '';
        
        data.files.forEach(file => {
          const row = document.createElement('tr');
          
          // Name cell
          const nameCell = document.createElement('td');
          nameCell.textContent = file.name;
          row.appendChild(nameCell);
          
          // Size cell
          const sizeCell = document.createElement('td');
          sizeCell.textContent = file.size + ' bytes';
          row.appendChild(sizeCell);
          
          // Modified cell
          const modCell = document.createElement('td');
          modCell.textContent = new Date(file.modified * 1000).toLocaleString();
          row.appendChild(modCell);
          
          // Actions cell
          const actionCell = document.createElement('td');
          
          // Download button
          const downloadBtn = document.createElement('button');
          downloadBtn.textContent = 'Download';
          downloadBtn.onclick = () => {
            window.location.href = '/api/download/' + encodeURIComponent(file.name);
          };
          actionCell.appendChild(downloadBtn);
          
          // Delete button
          const deleteBtn = document.createElement('button');
          deleteBtn.textContent = 'Delete';
          deleteBtn.onclick = async () => {
            if (confirm('Delete ' + file.name + '?')) {
              const response = await fetch('/api/delete', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: 'filename=' + encodeURIComponent(file.name)
              });
              const result = await response.json();
              if (result.success) {
                loadFiles();
              } else {
                alert('Delete failed');
              }
            }
          };
          actionCell.appendChild(deleteBtn);
          
          // Rename button
          const renameBtn = document.createElement('button');
          renameBtn.textContent = 'Rename';
          renameBtn.onclick = () => {
            document.getElementById('oldName').value = file.name;
            document.getElementById('newName').value = file.name;
            document.getElementById('renameModal').style.display = 'block';
          };
          actionCell.appendChild(renameBtn);
          
          row.appendChild(actionCell);
          table.appendChild(row);
        });
      } catch (error) {
        console.error('Error:', error);
      }
    }

    // Handle upload form
    document.getElementById('uploadForm').addEventListener('submit', async (e) => {
      e.preventDefault();
      const formData = new FormData(e.target);
      
      try {
        const response = await fetch('/api/upload', {
          method: 'POST',
          body: formData
        });
        
        if (response.ok) {
          loadFiles();
          e.target.reset();
        } else {
          alert('Upload failed');
        }
      } catch (error) {
        console.error('Upload error:', error);
        alert('Upload error');
      }
    });

    // Handle rename form
    document.getElementById('renameForm').addEventListener('submit', async (e) => {
      e.preventDefault();
      const oldName = document.getElementById('oldName').value;
      const newName = document.getElementById('newName').value;
      
      try {
        const response = await fetch('/api/rename', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'old_name=' + encodeURIComponent(oldName) + '&new_name=' + encodeURIComponent(newName)
        });
        
        const result = await response.json();
        if (result.success) {
          document.getElementById('renameModal').style.display = 'none';
          loadFiles();
        } else {
          alert('Rename failed');
        }
      } catch (error) {
        console.error('Rename error:', error);
        alert('Rename error');
      }
    });

    // Close modal
    document.querySelector('.close').onclick = () => {
      document.getElementById('renameModal').style.display = 'none';
    };

    // Initial load
    document.addEventListener('DOMContentLoaded', loadFiles);
  </script>
</body>
</html>)";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
  }

  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File Not Found");
  return ESP_OK;
}

esp_err_t FTPHTTPProxy::api_handler(httpd_req_t *req) {
  FTPHTTPProxy *proxy = (FTPHTTPProxy *)req->user_ctx;
  return proxy->handle_api(req);
}

esp_err_t FTPHTTPProxy::web_handler(httpd_req_t *req) {
  FTPHTTPProxy *proxy = (FTPHTTPProxy *)req->user_ctx;
  return proxy->handle_web(req);
}

}  // namespace ftp_http_proxy
}  // namespace esphome
