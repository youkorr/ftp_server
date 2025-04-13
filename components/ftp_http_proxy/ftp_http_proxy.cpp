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

// Nouvelle méthode statique qui servira de point d'entrée pour le gestionnaire HTTP
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
  
  // Configuration du watchdog avec un délai plus long
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 60000,  // Augmenté à 15 secondes (au lieu de 10s)
    .idle_core_mask = 0,  // Sans surveillance des cœurs inactifs
    .trigger_panic = false // Ne pas paniquer en cas de timeout - juste un log
  };
  esp_err_t err = esp_task_wdt_init(&wdt_config);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Impossible de configurer le watchdog: %d", err);
  } else {
    ESP_LOGI(TAG, "Watchdog configuré avec timeout de 15s");
  }
  
  // Ajouter la tâche principale au watchdog
  TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
  if (esp_task_wdt_add(current_task) == ESP_OK) {
    ESP_LOGI(TAG, "Tâche principale ajoutée au watchdog");
  }
  
  this->setup_http_server();
}

void FTPHTTPProxy::loop() {
  // Réinitialiser le watchdog dans la boucle principale
  TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
  if (esp_task_wdt_status(current_task) == ESP_OK) {
    esp_task_wdt_reset();
  }
  
  // Surveiller l'état de la mémoire périodiquement
  static uint32_t last_heap_check = 0;
  uint32_t now = millis();
  if (now - last_heap_check > 30000) { // Toutes les 30 secondes
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

  // Configuration du socket pour être plus robuste
  int flag = 1;
  setsockopt(sock_, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
  
  // Augmenter la taille du buffer de réception
  int rcvbuf = 16384;
  setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  
  // Ajouter un timeout pour éviter les blocages
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

  // Authentification
  snprintf(buffer, sizeof(buffer), "USER %s\r\n", username_.c_str());
  send(sock_, buffer, strlen(buffer), 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';

  snprintf(buffer, sizeof(buffer), "PASS %s\r\n", password_.c_str());
  send(sock_, buffer, strlen(buffer), 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';

  // Mode binaire
  send(sock_, "TYPE I\r\n", 8, 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';

  return true;
}

bool FTPHTTPProxy::download_file(const std::string &remote_path, httpd_req_t *req) {
  if (!this->connected_) {
    ESP_LOGE(TAG, "FTP client not connected");
    return false;
  }

  int res;
  char buffer[512];

  // 1. Passive mode
  res = this->send_command("PASV\r\n");
  if (res <= 0 || !this->read_response(buffer, sizeof(buffer), "227")) {
    ESP_LOGE(TAG, "Failed entering passive mode");
    return false;
  }

  // 2. Extract IP and port
  int ip1, ip2, ip3, ip4, p1, p2;
  if (sscanf(buffer, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)",
             &ip1, &ip2, &ip3, &ip4, &p1, &p2) != 6) {
    ESP_LOGE(TAG, "Failed to parse PASV response: %s", buffer);
    return false;
  }
  int data_port = (p1 << 8) | p2;

  // 3. Open data socket
  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  char ip_str[32];
  snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", ip1, ip2, ip3, ip4);
  inet_pton(AF_INET, ip_str, &data_addr.sin_addr);

  int data_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Failed to create data socket");
    return false;
  }

  struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
  setsockopt(data_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(data_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  if (connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to connect to data port");
    close(data_sock);
    return false;
  }

  // 4. Send RETR command
  std::string cmd = "RETR " + remote_path + "\r\n";
  if (this->send_command(cmd.c_str()) <= 0 || !this->read_response(buffer, sizeof(buffer), "150")) {
    ESP_LOGE(TAG, "RETR command failed");
    close(data_sock);
    return false;
  }

  // 5. Read from data socket and stream to HTTP
  while (true) {
    ssize_t len = recv(data_sock, buffer, sizeof(buffer), 0);
    if (len <= 0)
      break;
    httpd_resp_send_chunk(req, buffer, len);
    delay(0);  // Feed the watchdog
  }

  httpd_resp_send_chunk(req, nullptr, 0);  // End chunked transfer
  close(data_sock);

  // 6. Check final FTP response
  if (!this->read_response(buffer, sizeof(buffer), "226")) {
    ESP_LOGW(TAG, "FTP transfer did not end cleanly");
  }

  return true;
}

esp_err_t FTPHTTPProxy::internal_http_req_handler(httpd_req_t *req) {
  FTPHTTPProxy *proxy = static_cast<FTPHTTPProxy *>(req->user_ctx);
  for (const auto &configured_path : proxy->remote_paths_) {
    // ...
  }
  return ESP_OK;
}

  ESP_LOGI(TAG, "Requête reçue: %s", requested_path.c_str());

  // Obtenir l'extension du fichier pour déterminer le type MIME
  std::string extension = "";
  size_t dot_pos = requested_path.find_last_of('.');
  if (dot_pos != std::string::npos) {
    extension = requested_path.substr(dot_pos);
    ESP_LOGD(TAG, "Extension détectée: %s", extension.c_str());
  }

  // Extraire le nom du fichier de requested_path pour l'en-tête Content-Disposition
  std::string filename = requested_path;
  size_t slash_pos = requested_path.find_last_of('/');
  if (slash_pos != std::string::npos) {
    filename = requested_path.substr(slash_pos + 1);
  }

  // Définir les types MIME et headers selon le type de fichier
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
    // Type par défaut pour les fichiers inconnus
    httpd_resp_set_type(req, "application/octet-stream");
    std::string header = "attachment; filename=\"" + filename + "\"";
    httpd_resp_set_hdr(req, "Content-Disposition", header.c_str());
    ESP_LOGD(TAG, "Configuré pour téléchargement générique");
  }

  // Pour traiter les gros fichiers, on ajoute des en-têtes supplémentaires
  httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
  
  // Réinitialiser le watchdog avant de traiter le fichier
  TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
  if (esp_task_wdt_status(current_task) == ESP_OK) {
    esp_task_wdt_reset();
  }
  
  for (const auto &configured_path : remote_paths_) {
    if (requested_path == configured_path) {
      ESP_LOGI(TAG, "Téléchargement du fichier: %s", requested_path.c_str());
      if (download_file(configured_path, req)) {
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
  
  // Augmenter les limites pour gérer les grandes requêtes
  config.recv_wait_timeout = 20;
  config.send_wait_timeout = 20;
  config.max_uri_handlers = 8;
  config.max_resp_headers = 20;
  config.stack_size = 12288;
  config.task_priority = tskIDLE_PRIORITY + 5; // Priorité plus élevée pour la tâche HTTP

  if (httpd_start(&server_, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Échec du démarrage du serveur HTTP");
    return;
  }

  httpd_uri_t uri_proxy = {
    .uri       = "/*",
    .method    = HTTP_GET,
    .handler   = static_http_req_handler,  // Utiliser le gestionnaire statique
    .user_ctx  = this
  };

  httpd_register_uri_handler(server_, &uri_proxy);
  ESP_LOGI(TAG, "Serveur HTTP démarré sur le port %d", local_port_);
}

}  // namespace ftp_http_proxy
}  // namespace esphome
