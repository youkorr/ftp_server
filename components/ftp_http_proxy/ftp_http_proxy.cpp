#include "ftp_http_proxy.h"
#include "esp_log.h"
#include <lwip/sockets.h>
#include <netdb.h>
#include <cstring>
#include <arpa/inet.h>
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"

static const char *TAG = "ftp_proxy";

namespace esphome {
namespace ftp_http_proxy {

esp_err_t FTPHTTPProxy::static_http_req_handler(httpd_req_t *req) {
  auto *proxy = static_cast<FTPHTTPProxy *>(req->user_ctx);
  if (proxy == nullptr) {
    ESP_LOGE(TAG, "User context is null");
    return ESP_FAIL;
  }
  return proxy->internal_http_req_handler(req);
}

void FTPHTTPProxy::setup() {
  ESP_LOGI(TAG, "Initializing FTP/HTTP Proxy");

  // Enhanced watchdog configuration
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 30000,  // Increased timeout to 30s
    .idle_core_mask = (1 << 0),  // Only monitor core 0
    .trigger_panic = false
  };
  
  if (esp_task_wdt_init(&wdt_config) != ESP_OK) {
    ESP_LOGW(TAG, "Failed to configure watchdog");
  }

  this->setup_http_server();
}

bool FTPHTTPProxy::connect_to_ftp() {
  struct hostent *ftp_host = gethostbyname(ftp_server_.c_str());
  if (!ftp_host) {
    ESP_LOGE(TAG, "DNS resolution failed");
    return false;
  }

  sock_ = socket(AF_INET, SOCK_STREAM, 0);
  if (sock_ < 0) {
    ESP_LOGE(TAG, "Socket creation failed: %d", errno);
    return false;
  }

  // Enhanced socket configuration
  int flag = 1;
  setsockopt(sock_, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
  
  int rcvbuf = 16384;
  setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  
  struct timeval tv {
    .tv_sec = 10,  // Increased timeout to 10s
    .tv_usec = 0
  };
  setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(21);
  server_addr.sin_addr.s_addr = *((unsigned long *)ftp_host->h_addr);

  if (connect(sock_, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
    ESP_LOGE(TAG, "FTP connection failed: %d", errno);
    close(sock_);
    sock_ = -1;
    return false;
  }

  char buffer[256];
  int bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "220 ")) {
    ESP_LOGE(TAG, "FTP welcome message not received");
    close(sock_);
    sock_ = -1;
    return false;
  }
  buffer[bytes_received] = '\0';

  // Authentication
  snprintf(buffer, sizeof(buffer), "USER %s\r\n", username_.c_str());
  send(sock_, buffer, strlen(buffer), 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';

  snprintf(buffer, sizeof(buffer), "PASS %s\r\n", password_.c_str());
  send(sock_, buffer, strlen(buffer), 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';

  // Binary mode
  send(sock_, "TYPE I\r\n", 8, 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';

  return true;
}

bool FTPHTTPProxy::setup_passive_mode(int &data_sock) {
  char buffer[256];
  int bytes_received;
  int ip[4], port[2];

  send(sock_, "PASV\r\n", 6, 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "227 ")) {
    ESP_LOGE(TAG, "PASV mode error");
    return false;
  }
  buffer[bytes_received] = '\0';

  char *pasv_start = strchr(buffer, '(');
  if (!pasv_start) {
    ESP_LOGE(TAG, "Invalid PASV format");
    return false;
  }

  sscanf(pasv_start, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]);
  int data_port = port[0] * 256 + port[1];

  // Create data socket
  data_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Data socket creation failed");
    return false;
  }

  // Configure data socket
  int flag = 1;
  setsockopt(data_sock, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
  int rcvbuf = 16384;
  setsockopt(data_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  
  struct timeval tv {
    .tv_sec = 10,
    .tv_usec = 0
  };
  setsockopt(data_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  data_addr.sin_addr.s_addr = htonl((ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]);

  if (connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Data connection failed");
    close(data_sock);
    data_sock = -1;
    return false;
  }

  return true;
}

bool FTPHTTPProxy::send_retr_command(const std::string &remote_path) {
  char buffer[512];
  snprintf(buffer, sizeof(buffer), "RETR %s\r\n", remote_path.c_str());
  send(sock_, buffer, strlen(buffer), 0);

  int bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "150 ")) {
    ESP_LOGE(TAG, "File not found or inaccessible");
    return false;
  }
  buffer[bytes_received] = '\0';
  return true;
}

bool FTPHTTPProxy::download_file(const std::string &remote_path, httpd_req_t *req) {
  // Consolidated declarations
  bool is_media_file = false;
  size_t total_bytes = 0;
  int data_sock = -1;
  int bytes_received;
  char *buffer = nullptr;
  bool success = false;
  esp_err_t err = ESP_OK;
  TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
  
  // Optimized configuration
  const int buffer_size = 4096;  // Optimal for streaming
  const int wdt_reset_interval = 100 * 1024; // Reset WDT every 100KB

  // Detect file type
  is_media_file = (remote_path.find(".mp3") != std::string::npos || 
                  remote_path.find(".wav") != std::string::npos);

  // Allocate buffer in PSRAM
  buffer = (char*)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buffer) {
    ESP_LOGE(TAG, "Failed to allocate PSRAM buffer");
    return false;
  }

  // Connect to FTP
  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "FTP connection failed");
    goto cleanup;
  }

  // Setup passive mode
  if (!setup_passive_mode(data_sock)) {
    ESP_LOGE(TAG, "Passive mode setup failed");
    goto cleanup;
  }

  // Send RETR command
  if (!send_retr_command(remote_path)) {
    ESP_LOGE(TAG, "RETR command failed");
    goto cleanup;
  }

  // Main transfer loop
  while ((bytes_received = recv(data_sock, buffer, buffer_size, 0)) > 0) {
    total_bytes += bytes_received;
    
    // Send HTTP chunk
    if ((err = httpd_resp_send_chunk(req, buffer, bytes_received)) != ESP_OK) {
      ESP_LOGE(TAG, "HTTP send error: %d", err);
      break;
    }

    // Watchdog management for large files
    if (total_bytes % wdt_reset_interval < buffer_size) {
      esp_task_wdt_reset();
      if (is_media_file && total_bytes > 3*1024*1024) {
        vTaskDelay(pdMS_TO_TICKS(2)); // Extra delay for >3MB files
      }
    }
  }

  // Check for transfer errors
  if (bytes_received < 0) {
    ESP_LOGE(TAG, "Data receive error");
    goto cleanup;
  }

  // Finalize transfer
  httpd_resp_send_chunk(req, NULL, 0);
  ESP_LOGI(TAG, "Transfer successful: %zu bytes", total_bytes);
  success = true;

cleanup:
  // Resource cleanup
  if (buffer) heap_caps_free(buffer);
  if (data_sock != -1) close(data_sock);
  if (sock_ != -1) {
    send(sock_, "QUIT\r\n", 6, 0);
    close(sock_);
    sock_ = -1;
  }
  return success;
}

esp_err_t FTPHTTPProxy::internal_http_req_handler(httpd_req_t *req) {
  FTPHTTPProxy *instance = static_cast<FTPHTTPProxy *>(req->user_ctx);
  if (!instance) {
    ESP_LOGE(TAG, "No instance pointer");
    return ESP_FAIL;
  }

  std::string path = req->uri;
  if (!path.empty() && path[0] == '/') {
    path.erase(0, 1);
  }

  ESP_LOGI(TAG, "Request received: %s", path.c_str());

  // Set MIME type based on extension
  if (path.find(".mp3") != std::string::npos) {
    httpd_resp_set_type(req, "audio/mpeg");
  } else if (path.find(".wav") != std::string::npos) {
    httpd_resp_set_type(req, "audio/wav");
  } else {
    httpd_resp_set_type(req, "application/octet-stream");
  }

  // Check against configured paths
  for (const auto &remote_path : instance->remote_paths_) {
    if (path == remote_path) {
      if (instance->download_file(remote_path, req)) {
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
  config.stack_size = 12288;
  config.task_priority = tskIDLE_PRIORITY + 3; // Reduced priority
  config.max_uri_handlers = 8;
  config.recv_wait_timeout = 30;
  config.send_wait_timeout = 30;

  if (httpd_start(&server_, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return;
  }

  httpd_uri_t uri_proxy = {
    .uri = "/*",
    .method = HTTP_GET,
    .handler = static_http_req_handler,
    .user_ctx = this
  };

  httpd_register_uri_handler(server_, &uri_proxy);
  ESP_LOGI(TAG, "HTTP server started on port %d", local_port_);
}

}  // namespace ftp_http_proxy
}  // namespace esphome
