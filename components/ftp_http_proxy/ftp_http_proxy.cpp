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
  esp_err_t err = ESP_OK;

  TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
  bool wdt_initialized = false;

  // Déterminer si c'est un fichier média
  size_t dot_pos = remote_path.find_last_of('.');
  if (dot_pos != std::string::npos) {
    std::string ext = remote_path.substr(dot_pos);
    is_media_file = (ext == ".mp3" || ext == ".wav" || ext == ".ogg" || ext == ".mp4");
  }

  // Ajout Watchdog
  if (esp_task_wdt_status(current_task) != ESP_OK) {
    if (esp_task_wdt_add(current_task) == ESP_OK) {
      wdt_initialized = true;
      ESP_LOGI(TAG, "Tâche ajoutée au watchdog");
    } else {
      ESP_LOGW(TAG, "Impossible d'ajouter la tâche au watchdog");
    }
  } else {
    wdt_initialized = true;
  }

  int buffer_size = is_media_file ? 2048 : 8192;
  buffer = (char*)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
  if (!buffer) {
    ESP_LOGE(TAG, "Échec allocation mémoire");
    goto error;
  }

  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "Échec de connexion FTP");
    goto error;
  }

  // Entrer en mode passif
  send(sock_, "PASV\r\n", 6, 0);
  bytes_received = recv(sock_, buffer, buffer_size - 1, 0);
  if (bytes_received <= 0) {
    ESP_LOGE(TAG, "Échec réception PASV");
    goto error;
  }
  buffer[bytes_received] = '\0';

  pasv_start = strchr(buffer, '(');
  if (!pasv_start) {
    ESP_LOGE(TAG, "Format PASV invalide");
    goto error;
  }
  sscanf(pasv_start + 1, "%d,%d,%d,%d,%d,%d", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]);
  data_port = (port[0] << 8) | port[1];

  // Connexion socket data
  data_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Socket data invalide");
    goto error;
  }
  setsockopt(data_sock, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
  setsockopt(data_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
  setsockopt(data_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(data_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct sockaddr_in data_addr{};
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  data_addr.sin_addr.s_addr = inet_addr(
    (std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." +
     std::to_string(ip[2]) + "." + std::to_string(ip[3])).c_str()
  );

  if (connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Échec de connexion socket data");
    goto error;
  }

  // Envoi de la commande RETR
  std::string cmd = "RETR " + remote_path + "\r\n";
  send(sock_, cmd.c_str(), cmd.length(), 0);

  // Lecture et envoi HTTP
  while ((bytes_received = recv(data_sock, buffer, buffer_size, 0)) > 0) {
    err = httpd_resp_send_chunk(req, buffer, bytes_received);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Erreur envoi HTTP: %d", err);
      goto error;
    }

    total_bytes_transferred += bytes_received;
    bytes_since_reset += bytes_received;
    chunk_count++;

    if (wdt_initialized && bytes_since_reset >= 32 * 1024) {
      esp_task_wdt_reset();
      bytes_since_reset = 0;
    }

    // Optionnel: log périodique
    if ((chunk_count % 32) == 0) {
      ESP_LOGI(TAG, "Transféré %d Ko", total_bytes_transferred / 1024);
    }
  }

  success = true;

error:
  if (data_sock >= 0)
    close(data_sock);
  if (buffer)
    heap_caps_free(buffer);
  if (wdt_initialized)
    esp_task_wdt_delete(current_task);

  // Terminer la réponse HTTP même en cas partiel
  httpd_resp_send_chunk(req, nullptr, 0);

  return success;
}

// Renommer http_req_handler en internal_http_req_handler
esp_err_t FTPHTTPProxy::internal_http_req_handler(httpd_req_t *req) {
  std::string requested_path = req->uri;

  // Suppression du premier slash
  if (!requested_path.empty() && requested_path[0] == '/') {
    requested_path.erase(0, 1);
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
