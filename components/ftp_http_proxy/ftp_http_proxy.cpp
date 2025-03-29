#include "ftp_http_proxy.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include <lwip/sockets.h>
#include <netdb.h>
#include <cstring>
#include <arpa/inet.h>
#include <fcntl.h>

static const char *TAG = "ftp_proxy";

namespace esphome {
namespace ftp_http_proxy {

// Structure pour les paramètres de téléchargement
struct DownloadParams {
  FTPHTTPProxy* proxy;
  std::string remote_path;
  httpd_req_t* req;
  bool completed;
};

void FTPHTTPProxy::setup() {
  ESP_LOGI(TAG, "Initialisation du proxy FTP/HTTP");
  // Reconfigurer le watchdog pour avoir un timeout plus long
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 30000,        // 30 secondes timeout
    .idle_core_mask = (1 << 0)  // Watch CPU 0
  };
  esp_task_wdt_reconfigure(&wdt_config);
  this->setup_http_server();
}

void FTPHTTPProxy::loop() {
  // Rien à faire ici, le traitement est dans des tâches séparées
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
  int rcvbuf = 32768; // 32 Ko
  setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  // Ajouter un timeout plus long pour les connexions
  struct timeval timeout;
  timeout.tv_sec = 30;  // 30 secondes
  timeout.tv_usec = 0;
  setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
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

// Fonction de tâche FreeRTOS pour le téléchargement FTP
void download_task_func(void *pvParameters) {
  DownloadParams *params = (DownloadParams *)pvParameters;
  bool result = params->proxy->download_file_impl(params->remote_path, params->req);
  params->completed = true;
  // Libération de la mémoire et fin de la tâche
  vTaskDelete(NULL);
}

// Implémentation réelle du téléchargement
bool FTPHTTPProxy::download_file_impl(const std::string &remote_path, httpd_req_t *req) {
  // Déclarations en haut pour éviter les goto cross-initialization
  int data_sock = -1;
  bool success = false;
  char *pasv_start = nullptr;
  int data_port = 0;
  int ip[4], port[2]; 
  char buffer[4096]; // Tampon de 4Ko pour réception
  int bytes_received = 0;
  int flag = 1;
  int rcvbuf = 32768; // 32 Ko
  struct timeval timeout;
  timeout.tv_sec = 30;
  timeout.tv_usec = 0;
  size_t total_processed = 0;
  const size_t MAX_CHUNK = 4096;
  int socket_flags = 0;

  // Enregistrer cette tâche avec le watchdog
  esp_task_wdt_add(NULL);

  // Connexion au serveur FTP
  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "Échec de connexion FTP");
    goto error;
  }

  // Reset du watchdog pendant les opérations longues
  esp_task_wdt_reset();

  // Mode passif
  send(sock_, "PASV\r\n", 6, 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "227 ")) {
    ESP_LOGE(TAG, "Erreur en mode passif");
    goto error;
  }
  buffer[bytes_received] = '\0';
  ESP_LOGD(TAG, "Réponse PASV: %s", buffer);

  // Extraction des données de connexion
  pasv_start = strchr(buffer, '(');
  if (!pasv_start) {
    ESP_LOGE(TAG, "Format PASV incorrect");
    goto error;
  }
  sscanf(pasv_start, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]);
  data_port = port[0] * 256 + port[1];
  ESP_LOGD(TAG, "Port de données: %d", data_port);

  // Création du socket de données
  data_sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Échec de création du socket de données");
    goto error;
  }

  // Configuration du socket de données pour être plus robuste
  setsockopt(data_sock, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
  setsockopt(data_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  setsockopt(data_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(data_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  // Mettre le socket en mode non-bloquant
  socket_flags = fcntl(data_sock, F_GETFL, 0);
  fcntl(data_sock, F_SETFL, socket_flags | O_NONBLOCK);

  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  data_addr.sin_addr.s_addr = htonl(
      (ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]
  );

  if (::connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    // Vérifier si l'erreur est EINPROGRESS (normal en mode non-bloquant)
    if (errno != EINPROGRESS) {
      ESP_LOGE(TAG, "Échec de connexion au port de données");
      goto error;
    }
    // Attendre que la connexion soit établie avec select()
    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(data_sock, &write_set);
    struct timeval connect_timeout;
    connect_timeout.tv_sec = 10;
    connect_timeout.tv_usec = 0;
    if (::select(data_sock + 1, NULL, &write_set, NULL, &connect_timeout) <= 0) {
      ESP_LOGE(TAG, "Timeout de connexion au port de données");
      goto error;
    }
  }

  // Revenir en mode bloquant pour la suite
  fcntl(data_sock, F_SETFL, socket_flags);
  esp_task_wdt_reset();

  // Envoi de la commande RETR
  snprintf(buffer, sizeof(buffer), "RETR %s\r\n", remote_path.c_str());
  ESP_LOGD(TAG, "Envoi de la commande: %s", buffer);
  send(sock_, buffer, strlen(buffer), 0);

  // Vérification de la réponse 150
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "150 ")) {
    ESP_LOGE(TAG, "Fichier non trouvé ou inaccessible");
    goto error;
  }
  buffer[bytes_received] = '\0';
  ESP_LOGD(TAG, "Réponse RETR: %s", buffer);

  // Préparation pour la lecture non-bloquante
  fd_set read_set;
  struct timeval tv;

  // Transfert en streaming avec traitement par chunks
  while (true) {
    // Reset du watchdog à chaque itération
    esp_task_wdt_reset();

    // Utiliser select pour attendre les données avec un timeout court
    FD_ZERO(&read_set);
    FD_SET(data_sock, &read_set);
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms
    int select_result = ::select(data_sock + 1, &read_set, NULL, NULL, &tv);
    if (select_result > 0) {
      // Des données sont disponibles
      bytes_received = recv(data_sock, buffer, sizeof(buffer), 0);
      if (bytes_received <= 0) {
        if (bytes_received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
          ESP_LOGE(TAG, "Erreur de réception des données: %d", errno);
        }
        break;
      }

      // Traiter par petits chunks pour éviter de bloquer trop longtemps
      size_t sent = 0;
      while (sent < bytes_received) {
        esp_task_wdt_reset();
        size_t chunk = std::min(MAX_CHUNK, bytes_received - sent);
        esp_err_t err = httpd_resp_send_chunk(req, buffer + sent, chunk);
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "Échec d'envoi au client: %d", err);
          goto error;
        }
        sent += chunk;
        total_processed += chunk;

        // Petit délai entre les chunks
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    } else if (select_result == 0) {
      // Timeout, continuer la boucle
      vTaskDelay(pdMS_TO_TICKS(10));
    } else {
      // Erreur
      ESP_LOGE(TAG, "Erreur select: %d", errno);
      break;
    }
  }

  ESP_LOGI(TAG, "Transfert terminé, %zu octets traités", total_processed);

  // Fermeture du socket de données
  ::close(data_sock);
  data_sock = -1;

  // Reset du watchdog
  esp_task_wdt_reset();

  // Vérification de la réponse finale 226
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received > 0 && strstr(buffer, "226 ")) {
    success = true;
    buffer[bytes_received] = '\0';
    ESP_LOGD(TAG, "Transfert terminé: %s", buffer);
  }

  // Fermeture des sockets
  send(sock_, "QUIT\r\n", 6, 0);
  ::close(sock_);
  sock_ = -1;

  // Envoi du chunk final
  httpd_resp_send_chunk(req, NULL, 0);

  // Désinscrire du watchdog
  esp_task_wdt_delete(NULL);
  return success;

error:
  // Désinscrire du watchdog en cas d'erreur
  esp_task_wdt_delete(NULL);
  if (data_sock != -1) ::close(data_sock);
  if (sock_ != -1) {
    send(sock_, "QUIT\r\n", 6, 0);
    ::close(sock_);
    sock_ = -1;
  }
  return false;
}

// Point d'entrée pour le téléchargement, lance une tâche séparée
bool FTPHTTPProxy::download_file(const std::string &remote_path, httpd_req_t *req) {
  DownloadParams *params = new DownloadParams{
    .proxy = this,
    .remote_path = remote_path,
    .req = req,
    .completed = false
  };

  // Créer une tâche dédiée avec une pile suffisamment grande
  TaskHandle_t task_handle = NULL;
  BaseType_t result = xTaskCreate(
    download_task_func,
    "ftp_download",
    16384,      // Taille de pile augmentée
    params,     // Paramètres
    5,          // Priorité
    &task_handle
  );
  if (result != pdPASS) {
    ESP_LOGE(TAG, "Échec de création de la tâche de téléchargement");
    delete params;
    return false;
  }

  // Attendre que la tâche soit terminée avec un timeout
  TickType_t start_time = xTaskGetTickCount();
  const TickType_t max_wait = pdMS_TO_TICKS(300000); // 5 minutes max
  while (!params->completed) {
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_task_wdt_reset(); // Reset du watchdog dans la tâche principale

    // Vérifier si le timeout est atteint
    if ((xTaskGetTickCount() - start_time) > max_wait) {
      ESP_LOGE(TAG, "Timeout de la tâche de téléchargement");
      if (task_handle != NULL) {
        vTaskDelete(task_handle);
      }
      delete params;
      return false;
    }
  }

  // Nettoyage
  delete params;
  return true;
}

esp_err_t FTPHTTPProxy::http_req_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
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

  // Augmenter le timeout pour les connections HTTP
  httpd_resp_set_hdr(req, "Keep-Alive", "timeout=60, max=1000");

  for (const auto &configured_path : proxy->remote_paths_) {
    if (requested_path == configured_path) {
      ESP_LOGI(TAG, "Téléchargement du fichier: %s", requested_path.c_str());
      if (proxy->download_file(configured_path, req)) {
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
  config.recv_wait_timeout = 30;  // Augmenté à 30
  config.send_wait_timeout = 30;  // Augmenté à 30
  config.max_uri_handlers = 8;
  config.max_resp_headers = 16;
  config.stack_size = 16384;  // Augmenté à 16384
  config.lru_purge_enable = true;  // Activer le mécanisme de purge LRU
  config.task_priority = 6;        // Priorité plus haute

  if (httpd_start(&server_, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Échec du démarrage du serveur HTTP");
    return;
  }

  httpd_uri_t uri_proxy = {
    .uri       = "/*",
    .method    = HTTP_GET,
    .handler   = http_req_handler,
    .user_ctx  = this
  };
  httpd_register_uri_handler(server_, &uri_proxy);
  ESP_LOGI(TAG, "Serveur HTTP démarré sur le port %d", local_port_);
}

}  // namespace ftp_http_proxy
}  // namespace esphome
