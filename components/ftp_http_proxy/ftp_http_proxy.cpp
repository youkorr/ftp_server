#include "ftp_http_proxy.h"
#include "esp_log.h"
#include <lwip/netdb.h>
#include <cstring>
#include <arpa/inet.h>
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"

static const char *TAG = "ftp_proxy";

namespace esphome {
namespace ftp_http_proxy {

void FTPHTTPProxy::setup() {
  ESP_LOGI(TAG, "Initializing FTP/HTTP Proxy");
  this->setup_http_server();
}

void FTPHTTPProxy::loop() {
  // Periodic tasks if needed
}

bool FTPHTTPProxy::connect_to_ftp() {
  struct hostent *ftp_host = gethostbyname(ftp_server_.c_str());
  if (!ftp_host) {
    ESP_LOGE(TAG, "DNS resolution failed");
    return false;
  }

  sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock_ < 0) {
    ESP_LOGE(TAG, "Socket creation failed: %d", errno);
    return false;
  }

  struct timeval tv {
    .tv_sec = 10,
    .tv_usec = 0
  };
  ::setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(ftp_port_);
  server_addr.sin_addr.s_addr = *((unsigned long *)ftp_host->h_addr);

  if (::connect(sock_, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
    ESP_LOGE(TAG, "FTP connection failed: %d", errno);
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  char buffer[256];
  int bytes_received = ::recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "220 ")) {
    ESP_LOGE(TAG, "FTP welcome message not received");
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  return true;
}

bool FTPHTTPProxy::setup_passive_mode(int &data_sock) {
  std::string response;
  if (!send_ftp_command("PASV\r\n", response) || response.find("227") == std::string::npos) {
    ESP_LOGE(TAG, "Failed to enter passive mode");
    return false;
  }

  size_t start = response.find('(');
  size_t end = response.find(')');
  if (start == std::string::npos || end == std::string::npos) {
    ESP_LOGE(TAG, "Invalid PASV response");
    return false;
  }

  std::string pasv_str = response.substr(start + 1, end - start - 1);
  int ip1, ip2, ip3, ip4, port1, port2;
  if (sscanf(pasv_str.c_str(), "%d,%d,%d,%d,%d,%d", &ip1, &ip2, &ip3, &ip4, &port1, &port2) != 6) {
    ESP_LOGE(TAG, "Failed to parse PASV response");
    return false;
  }

  data_sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Data socket creation failed");
    return false;
  }

  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(port1 * 256 + port2);
  data_addr.sin_addr.s_addr = htonl((ip1 << 24) | (ip2 << 16) | (ip3 << 8) | ip4);

  if (::connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Data connection failed");
    ::close(data_sock);
    return false;
  }

  return true;
}

bool FTPHTTPProxy::send_ftp_command(const std::string &cmd, std::string &response) {
  if (::send(sock_, cmd.c_str(), cmd.length(), 0) < 0) {
    ESP_LOGE(TAG, "Failed to send FTP command");
    return false;
  }

  char buffer[256];
  int bytes_received = ::recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0) {
    ESP_LOGE(TAG, "Failed to receive FTP response");
    return false;
  }

  buffer[bytes_received] = '\0';
  response = buffer;
  return true;
}

bool FTPHTTPProxy::send_retr_command(const std::string &remote_path) {
  std::string cmd = "RETR " + remote_path + "\r\n";
  std::string response;
  
  if (!send_ftp_command(cmd, response) || response.find("150") == std::string::npos) {
    ESP_LOGE(TAG, "Failed to initiate file transfer");
    return false;
  }
  return true;
}

bool FTPHTTPProxy::download_file_impl(const std::string &remote_path, httpd_req_t *req) {
  int data_sock = -1;
  char *buffer = (char*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buffer) {
    ESP_LOGE(TAG, "Failed to allocate buffer");
    return false;
  }

  bool success = false;
  do {
    if (!setup_passive_mode(data_sock)) break;
    if (!send_retr_command(remote_path)) break;

    int bytes_received;
    while ((bytes_received = ::recv(data_sock, buffer, 4096, 0)) > 0) {
      if (httpd_resp_send_chunk(req, buffer, bytes_received) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send HTTP chunk");
        break;
      }
    }

    success = (bytes_received >= 0);
  } while (false);

  if (data_sock != -1) ::close(data_sock);
  heap_caps_free(buffer);
  return success;
}

bool FTPHTTPProxy::download_file(const std::string &remote_path, httpd_req_t *req) {
  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "Failed to connect to FTP");
    return false;
  }

  bool result = download_file_impl(remote_path, req);
  
  ::send(sock_, "QUIT\r\n", 6, 0);
  ::close(sock_);
  sock_ = -1;

  httpd_resp_send_chunk(req, nullptr, 0);
  return result;
}

esp_err_t FTPHTTPProxy::static_http_req_handler(httpd_req_t *req) {
  auto *proxy = static_cast<FTPHTTPProxy *>(req->user_ctx);
  if (!proxy) {
    ESP_LOGE(TAG, "No proxy instance");
    return ESP_FAIL;
  }
  return proxy->internal_http_req_handler(req);
}

esp_err_t FTPHTTPProxy::internal_http_req_handler(httpd_req_t *req) {
  std::string uri = req->uri;
  if (uri.empty() || uri[0] != '/') {
    return ESP_FAIL;
  }
  uri.erase(0, 1);

  for (const auto &path : remote_paths_) {
    if (uri == path) {
      if (download_file(path, req)) {
        return ESP_OK;
      }
      break;
    }
  }

  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
  return ESP_FAIL;
}

void FTPHTTPProxy::setup_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = local_port_;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.stack_size = 8192;

  if (httpd_start(&server_, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return;
  }

  httpd_uri_t uri = {
    .uri = "/*",
    .method = HTTP_GET,
    .handler = static_http_req_handler,
    .user_ctx = this
  };

  httpd_register_uri_handler(server_, &uri);
  ESP_LOGI(TAG, "HTTP server started on port %d", local_port_);
}

}  // namespace ftp_http_proxy
}  // namespace esphome
