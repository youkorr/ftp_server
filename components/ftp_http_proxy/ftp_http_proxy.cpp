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
  ESP_LOGI(TAG, "Initialisation du proxy FTP/HTTP");

  struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
  
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 60000,
    .idle_core_mask = 0,
    .trigger_panic = false
  };
  esp_err_t err = esp_task_wdt_init(&wdt_config);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Impossible de configurer le watchdog: %d", err);
  } else {
    ESP_LOGI(TAG, "Watchdog configuré avec timeout de 15s");
  }
  
  TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
  if (esp_task_wdt_add(current_task) == ESP_OK) {
    ESP_LOGI(TAG, "Tâche principale ajoutée au watchdog");
  }
  
  this->setup_http_server();
}

void FTPHTTPProxy::loop() {
  TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
  if (esp_task_wdt_status(current_task) == ESP_OK) {
    esp_task_wdt_reset();
  }
  
  static uint32_t last_heap_check = 0;
  uint32_t now = millis();
  if (now - last_heap_check > 30000) {
    last_heap_check = now;
    ESP_LOGI(TAG, "Heap: %d, min: %d, largest block: %d", 
             heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
             heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
  }
}

bool FTPHTTPProxy::connect_to_ftp() {
  struct hostent *ftp_host = gethostbyname(ftp_server_.c_str());
  if (!ftp_host) {
    ESP_LOGE(TAG, "Échec de la résolution DNS");
    return false;
  }

  sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock_ < 0) {
    ESP_LOGE(TAG, "Échec de création du socket : %d", errno);
    return false;
  }

  int flag = 1;
  setsockopt(sock_, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
  
  int rcvbuf = 16384;
  setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  
  struct timeval tv;
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(21);
  server_addr.sin_addr.s_addr = *((unsigned long *)ftp_host->h_addr);

  if (::connect(sock_, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
    ESP_LOGE(TAG, "Échec de connexion FTP : %d", errno);
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  char buffer[256];
  int bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "220 ")) {
    ESP_LOGE(TAG, "Message de bienvenue FTP non reçu");
    ::close(sock_);
    sock_ = -1;
    return false;
  }
  buffer[bytes_received] = '\0';

  snprintf(buffer, sizeof(buffer), "USER %s\r\n", username_.c_str());
  send(sock_, buffer, strlen(buffer), 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';

  snprintf(buffer, sizeof(buffer), "PASS %s\r\n", password_.c_str());
  send(sock_, buffer, strlen(buffer), 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';

  send(sock_, "TYPE I\r\n", 8, 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';

  return true;
}

bool FTPHTTPProxy::download_file(const std::string &remote_path, httpd_req_t *req) {
  // Déclarations déplacées en haut de la fonction
  bool is_media_file = false;
  size_t bytes_since_reset = 0;
  size_t total_bytes_transferred = 0;
  int chunk_count = 0;
  int data_sock = -1;
  bool success = false;
  char *pasv_start = nullptr;
  int data_port = 0;
  int ip[4], port[2]; 
  int bytes_received = 0;
  int flag = 1;
  int rcvbuf = 16384;
  char* buffer = nullptr;
  TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
  bool wdt_initialized = false;
  struct timeval tv;
  struct sockaddr_in data_addr;
  esp_err_t err = ESP_OK;

  size_t dot_pos = remote_path.find_last_of('.');
  if (dot_pos != std::string::npos) {
    std::string ext = remote_path.substr(dot_pos);
    is_media_file = (ext == ".mp3" || ext == ".wav" || ext == ".ogg" || ext == ".mp4");
  }

  if (esp_task_wdt_status(current_task) != ESP_OK) {
    if (esp_task_wdt_add(current_task) == ESP_OK) {
      wdt_initialized = true;
      ESP_LOGI(TAG, "Tâche ajoutée au watchdog");
    } else {
      ESP_LOGW(TAG, "Impossible d'ajouter la tâche au watchdog");
    }
  } else {
    wdt_initialized = true;
    ESP_LOGI(TAG, "Tâche déjà dans le watchdog");
  }
  
  int buffer_size = is_media_file ? 2048 : 8192;
  buffer = (char*)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
  if (!buffer) {
    ESP_LOGE(TAG, "Échec d'allocation SPIRAM pour le buffer");
    if (wdt_initialized) esp_task_wdt_delete(current_task);
    return false;
  }

  if (wdt_initialized) esp_task_wdt_reset();

  ESP_LOGI(TAG, "Heap avant connexion: %d, largest block: %d", 
          heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
          heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "Échec de connexion FTP");
    heap_caps_free(buffer);
    if (wdt_initialized) esp_task_wdt_delete(current_task);
    return false;
  }

  send(sock_, "PASV\r\n", 6, 0);
  bytes_received = recv(sock_, buffer, buffer_size - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "227 ")) {
    ESP_LOGE(TAG, "Erreur en mode passif");
    goto error;
  }
  buffer[bytes_received] = '\0';
  ESP_LOGD(TAG, "Réponse PASV: %s", buffer);

  pasv_start = strchr(buffer, '(');
  if (!pasv_start) {
    ESP_LOGE(TAG, "Format PASV incorrect");
    goto error;
  }

  sscanf(pasv_start, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]);
  data_port = port[0] * 256 + port[1];
  ESP_LOGD(TAG, "Port de données: %d", data_port);

  data_sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Échec de création du socket de données");
    goto error;
  }

  setsockopt(data_sock, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
  setsockopt(data_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  setsockopt(data_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  data_addr.sin_addr.s_addr = htonl(
      (ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]
  );

  if (::connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Échec de connexion au port de données");
    goto error;
  }

  snprintf(buffer, buffer_size, "RETR %s\r\n", remote_path.c_str());
  ESP_LOGD(TAG, "Envoi de la commande: %s", buffer);
  send(sock_, buffer, strlen(buffer), 0);

  bytes_received = recv(sock_, buffer, buffer_size - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "150 ")) {
    ESP_LOGE(TAG, "Fichier non trouvé ou inaccessible");
    goto error;
  }
  buffer[bytes_received] = '\0';
  ESP_LOGD(TAG, "Réponse RETR: %s", buffer);

  if (wdt_initialized) {
    esp_task_wdt_reset();
    ESP_LOGI(TAG, "WDT reset avant le transfert");
  }

  while ((bytes_received = recv(data_sock, buffer, buffer_size, 0)) > 0) {
    total_bytes_transferred += bytes_received;
    bytes_since_reset += bytes_received;
    chunk_count++;
  
    if (httpd_resp_send_chunk(req, buffer, bytes_received) != ESP_OK) {
      ESP_LOGE(TAG, "Erreur lors de l'envoi du chunk HTTP");
      goto error;
    }
  
    if (bytes_since_reset >= 32 * 1024) {
      esp_task_wdt_reset();
      ESP_LOGD(TAG, "WDT reset après ~32 Ko, total transféré: %d Ko", total_bytes_transferred / 1024);
      bytes_since_reset = 0;
    }
  }

  err = httpd_resp_send_chunk(req, buffer, bytes_received);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Échec d'envoi au client: %d", err);
    goto error;
  }
    
  chunk_count++;
  if (is_media_file && (chunk_count % 50 == 0)) {
    ESP_LOGD(TAG, "Streaming média: %d chunks envoyés, %zu Ko", chunk_count, total_bytes_transferred / 1024);
  }
    
  if (is_media_file && (bytes_received % 1024 == 0)) {
    vTaskDelay(pdMS_TO_TICKS(1));
      
    if (bytes_since_reset >= 20480 && wdt_initialized) {
      esp_task_wdt_reset();
      bytes_since_reset = 0;
    }
  } else if (!is_media_file && (bytes_received % 4096 == 0)) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  ::close(data_sock);
  data_sock = -1;

  bytes_received = recv(sock_, buffer, buffer_size - 1, 0);
  if (bytes_received > 0 && strstr(buffer, "226 ")) {
    success = true;
    buffer[bytes_received] = '\0';
    ESP_LOGD(TAG, "Transfert terminé: %s", buffer);
  }
  
  heap_caps_free(buffer);
  buffer = nullptr;
  
  httpd_resp_send_chunk(req, NULL, 0);
  
  ESP_LOGI(TAG, "Fichier transféré avec succès: %zu Ko, %d chunks", total_bytes_transferred / 1024, chunk_count);
  
  if (wdt_initialized) {
    esp_task_wdt_delete(current_task);
  }
  
  send(sock_, "QUIT\r\n", 6, 0);
  ::close(sock_);
  sock_ = -1;

  return success;

error:
  if (buffer) {
    heap_caps_free(buffer);
    buffer = nullptr;
  }
  if (data_sock != -1) {
    ::close(data_sock);
    data_sock = -1;
  }
  if (sock_ != -1) {
    send(sock_, "QUIT\r\n", 6, 0);
    ::close(sock_);
    sock_ = -1;
  }
  if (wdt_initialized) {
    esp_task_wdt_delete(current_task);
  }
  
  httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Échec du téléchargement");
  return false;
}

esp_err_t FTPHTTPProxy::internal_http_req_handler(httpd_req_t *req) {
  FTPHTTPProxy *instance = static_cast<FTPHTTPProxy *>(req->user_ctx);
  if (!instance) {
    ESP_LOGE(TAG, "No instance pointer in request");
    return ESP_FAIL;
  }

  std::string requested_path = req->uri;

  if (!requested_path.empty() && requested_path[0] == '/') {
    requested_path.erase(0, 1);
  }

  ESP_LOGI(TAG, "Requête reçue: %s", requested_path.c_str());

  std::string extension = "";
  size_t dot_pos = requested_path.find_last_of('.');
  if (dot_pos != std::string::npos) {
    extension = requested_path.substr(dot_pos);
    ESP_LOGD(TAG, "Extension détectée: %s", extension.c_str());
  }

  std::string filename = requested_path;
  size_t slash_pos = requested_path.find_last_of('/');
  if (slash_pos != std::string::npos) {
    filename = requested_path.substr(slash_pos + 1);
  }

  if (extension == ".mp3") {
    httpd_resp_set_type(req, "application/octet-stream");
    std::string header = "attachment; filename=\"" + filename + "\"";
    httpd_resp_set_hdr(req, "Content-Disposition", header.c_str());
    ESP_LOGD(TAG, "Configuré pour téléchargement MP3");
  } else if (extension == ".wav") {
    httpd_resp_set_type(req, "application/octet-stream");
    std::string header = "attachment; filename=\"" + filename + "\"";
    httpd_resp_set_hdr(req, "Content-Disposition", header.c_str());
    ESP_LOGD(TAG, "Configuré pour téléchargement WAV");
  } else if (extension == ".ogg") {
    httpd_resp_set_type(req, "application/octet-stream");
    std::string header = "attachment; filename=\"" + filename + "\"";
    httpd_resp_set_hdr(req, "Content-Disposition", header.c_str());
    ESP_LOGD(TAG, "Configuré pour téléchargement OGG");
  } else if (extension == ".pdf") {
    httpd_resp_set_type(req, "application/pdf");
  } else if (extension == ".jpg" || extension == ".jpeg") {
    httpd_resp_set_type(req, "image/jpeg");
  } else if (extension == ".png") {
    httpd_resp_set_type(req, "image/png");
  } else {
    httpd_resp_set_type(req, "application/octet-stream");
    std::string header = "attachment; filename=\"" + filename + "\"";
    httpd_resp_set_hdr(req, "Content-Disposition", header.c_str());
    ESP_LOGD(TAG, "Configuré pour téléchargement générique");
  }

  httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
  
  TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
  if (esp_task_wdt_status(current_task) == ESP_OK) {
    esp_task_wdt_reset();
  }
  
  for (const auto &configured_path : instance->remote_paths_) {
    if (requested_path == configured_path) {
      ESP_LOGI(TAG, "Téléchargement du fichier: %s", requested_path.c_str());
      if (instance->download_file(configured_path, req)) {
        ESP_LOGI(TAG, "Téléchargement réussi");
        return ESP_OK;
      } else {
        ESP_LOGE(TAG, "Échec du téléchargement");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Échec du téléchargement");
        return ESP_FAIL;
      }
    }
  }

  ESP_LOGW(TAG, "Fichier non trouvé: %s", requested_path.c_str());
  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Fichier non trouvé");
  return ESP_FAIL;
}

void FTPHTTPProxy::setup_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = local_port_;
  config.uri_match_fn = httpd_uri_match_wildcard;
  
  config.recv_wait_timeout = 20;
  config.send_wait_timeout = 20;
  config.max_uri_handlers = 8;
  config.max_resp_headers = 20;
  config.stack_size = 12288;
  config.task_priority = tskIDLE_PRIORITY + 5;

  if (httpd_start(&server_, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Échec du démarrage du serveur HTTP");
    return;
  }

  httpd_uri_t uri_proxy = {
    .uri       = "/*",
    .method    = HTTP_GET,
    .handler   = static_http_req_handler,
    .user_ctx  = this
  };

  httpd_register_uri_handler(server_, &uri_proxy);
  ESP_LOGI(TAG, "Serveur HTTP démarré sur le port %d", local_port_);
}

}  // namespace ftp_http_proxy
}  // namespace esphome
