#include "ftp_http_proxy.h"
#include "esp_log.h"
#include <lwip/sockets.h>
#include <netdb.h>
#include <cstring>
#include <arpa/inet.h>
#include "circular_buffer.h"
#include "esp_task_wdt.h" // Ajouter pour la surveillance du watchdog

static const char *TAG = "ftp_proxy";

namespace esphome {
namespace ftp_http_proxy {

void FTPHTTPProxy::setup() {
  ESP_LOGI(TAG, "Initialisation du proxy FTP/HTTP");
  this->setup_http_server();
}

void FTPHTTPProxy::loop() {}

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
  
  // Définir timeout pour les opérations socket
  struct timeval timeout;
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  
  // Augmenter la taille du buffer de réception
  int rcvbuf = 16384;
  setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

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
  if (bytes_received <= 0) {
    ESP_LOGE(TAG, "Pas de réponse à la commande USER");
    ::close(sock_);
    sock_ = -1;
    return false;
  }
  buffer[bytes_received] = '\0';

  snprintf(buffer, sizeof(buffer), "PASS %s\r\n", password_.c_str());
  send(sock_, buffer, strlen(buffer), 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0) {
    ESP_LOGE(TAG, "Pas de réponse à la commande PASS");
    ::close(sock_);
    sock_ = -1;
    return false;
  }
  buffer[bytes_received] = '\0';

  // Mode binaire
  send(sock_, "TYPE I\r\n", 8, 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0) {
    ESP_LOGE(TAG, "Pas de réponse à la commande TYPE I");
    ::close(sock_);
    sock_ = -1;
    return false;
  }
  buffer[bytes_received] = '\0';

  return true;
}

// Structure pour passer des données à la tâche de lecture FTP
struct FTPReaderTaskData {
  int sock;
  CircularBuffer* buffer;
  volatile bool done;
  volatile bool error;
  TaskHandle_t task_handle;
  
  FTPReaderTaskData(int s, CircularBuffer* b) : 
    sock(s), buffer(b), done(false), error(false), task_handle(nullptr) {}
};

// Fonction de tâche de lecture FTP
static void ftp_reader_task(void* arg) {
  auto* task_data = static_cast<FTPReaderTaskData*>(arg);
  
  // Enregistrer la tâche au watchdog
  esp_task_wdt_add(nullptr);

  char read_buffer[8192]; // Réduire la taille du buffer pour éviter les problèmes de pile
  int total_received = 0;
  
  while (!task_data->done) {
    // Réinitialiser le watchdog
    esp_task_wdt_reset();
    
    int bytes = recv(task_data->sock, read_buffer, sizeof(read_buffer), 0);
    if (bytes <= 0) {
      if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGE(TAG, "Erreur de lecture FTP: %d", errno);
        task_data->error = true;
      }
      break;
    }
    
    total_received += bytes;
    
    // Remplir le buffer circulaire avec une limite d'essais
    size_t written = 0;
    int retry_count = 0;
    const int max_retries = 50; // Limiter le nombre d'essais
    
    while (written < bytes && retry_count < max_retries) {
      // Si le buffer est plein, attendre un peu mais avec un maximum d'essais
      if (task_data->buffer->isFull()) {
        retry_count++;
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
      
      // Écrire autant qu'on peut dans le buffer
      size_t space = task_data->buffer->freeSpace();
      size_t to_write = std::min(space, bytes - written);
      size_t actual_written = task_data->buffer->write(read_buffer + written, to_write);
      
      written += actual_written;
      
      // Si on n'a pas pu écrire, attendre un peu
      if (actual_written == 0) {
        retry_count++;
        vTaskDelay(pdMS_TO_TICKS(10));
      } else {
        retry_count = 0; // Réinitialiser le compteur car on a réussi à écrire
      }
    }
    
    // Si on n'a pas réussi à écrire tout le buffer après max_retries tentatives,
    // signaler une erreur et arrêter
    if (written < bytes) {
      ESP_LOGE(TAG, "Impossible d'écrire dans le buffer circulaire après %d essais", max_retries);
      task_data->error = true;
      break;
    }
  }
  
  ESP_LOGI(TAG, "Tâche FTP reader terminée, %d octets reçus", total_received);
  
  // Se désinscrire du watchdog
  esp_task_wdt_delete(nullptr);
  
  task_data->done = true;
  vTaskDelete(NULL);
}

bool FTPHTTPProxy::download_file(const std::string &remote_path, httpd_req_t *req) {
  // Déclarations en haut pour éviter les goto cross-initialization
  int data_sock = -1;
  bool success = false;
  char *pasv_start = nullptr;
  int data_port = 0;
  int ip[4], port[2]; 
  char buffer[8192]; // Taille du buffer réduite pour éviter les problèmes de pile
  int bytes_received;
  int flag = 1;
  int rcvbuf = 32768;
  FTPReaderTaskData *task_data = nullptr;

  // Configurer timeout pour éviter les blocages
  struct timeval timeout;
  timeout.tv_sec = 10;
  timeout.tv_usec = 0;

  // Vérifier si c'est une requête de streaming audio
  bool is_streaming = false;
  std::string extension = "";
  size_t dot_pos = remote_path.find_last_of('.');
  if (dot_pos != std::string::npos) {
    extension = remote_path.substr(dot_pos);
    // Si c'est un fichier audio, considérer qu'il s'agit de streaming
    if (extension == ".mp3" || extension == ".wav" || extension == ".ogg") {
      is_streaming = true;
      ESP_LOGI(TAG, "Mode streaming activé pour %s", extension.c_str());
    }
  }

  // Connexion au serveur FTP
  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "Échec de connexion FTP");
    goto error;
  }

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
  setsockopt(data_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(data_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  
  // Augmenter la taille du buffer de réception pour le socket de données
  setsockopt(data_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  struct sockaddr_in data_addr;
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

  if (is_streaming) {
    // Pour le streaming, utiliser un buffer circulaire plus petit pour éviter la fragmentation
    // de la mémoire, mais suffisant pour un streaming fluide
    CircularBuffer stream_buffer(131072); // Buffer circulaire de 128 Ko
    
    // Création des données pour la tâche
    task_data = new FTPReaderTaskData(data_sock, &stream_buffer);
    
    // Démarrer une tâche de lecture FTP en arrière-plan avec une pile plus importante
    xTaskCreate(
      ftp_reader_task,
      "ftp_reader",
      8192, // Augmenter à 8Ko pour éviter les débordements de pile
      task_data,
      5,
      &task_data->task_handle
    );
    
    if (task_data->task_handle == nullptr) {
      ESP_LOGE(TAG, "Échec de création de la tâche FTP reader");
      delete task_data;
      task_data = nullptr;
      goto error;
    }
    
    // Envoyer les données au client HTTP avec un débit contrôlé
    const size_t chunk_size = 4096; // 4 Ko par chunk pour réduire l'utilisation de la pile
    char send_buffer[chunk_size];
    
    // Attendre que le buffer se remplisse suffisamment avant de commencer l'envoi
    // mais avec un timeout pour éviter les blocages
    int wait_count = 0;
    while (stream_buffer.available() < chunk_size * 2 && stream_buffer.available() < 32768) {
      vTaskDelay(pdMS_TO_TICKS(50));
      wait_count++;
      
      // Timeout après 5 secondes d'attente (100 * 50ms)
      if (wait_count > 100 || task_data->error || task_data->done) {
        if (task_data->error) {
          ESP_LOGE(TAG, "Erreur détectée lors du remplissage initial du buffer");
        } else if (wait_count > 100) {
          ESP_LOGW(TAG, "Timeout pendant le remplissage initial du buffer");
        }
        break;
      }
    }
    
    int empty_count = 0;
    while (!task_data->error) {
      size_t available = stream_buffer.available();
      
      // Si aucune donnée n'est disponible, vérifier si c'est terminé
      if (available == 0) {
        empty_count++;
        
        // Si le buffer est vide pendant un certain temps ou si la tâche a signalé la fin,
        // on considère que c'est terminé
        if (empty_count > 20 || task_data->done) {
          ESP_LOGI(TAG, "Fin du streaming (buffer vide ou tâche terminée)");
          break;
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
      } else {
        empty_count = 0; // Réinitialiser le compteur car on a des données
      }
      
      size_t to_read = std::min(available, chunk_size);
      size_t bytes_read = stream_buffer.read(send_buffer, to_read);
      
      if (bytes_read > 0) {
        esp_err_t err = httpd_resp_send_chunk(req, send_buffer, bytes_read);
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "Échec d'envoi au client: %d", err);
          break;
        }
      }
      
      // Contrôler le débit pour le streaming, délai variable selon le niveau de remplissage
      if (available > chunk_size * 4) {
        // Si le buffer est bien rempli, on peut aller plus vite
        vTaskDelay(pdMS_TO_TICKS(1));
      } else {
        // Si le buffer est peu rempli, on ralentit pour laisser le temps de le remplir
        vTaskDelay(pdMS_TO_TICKS(5));
      }
    }
    
    // Arrêter la tâche proprement
    task_data->done = true;
    
    // Attendre que la tâche se termine mais avec un timeout
    int timeout_counter = 0;
    while (!task_data->done && timeout_counter < 50) {
      vTaskDelay(pdMS_TO_TICKS(20));
      timeout_counter++;
    }
    
    // Supprimer la tâche si elle n'est pas terminée
    if (task_data->task_handle != nullptr) {
      vTaskDelete(task_data->task_handle);
      task_data->task_handle = nullptr;
    }
    
    delete task_data;
    task_data = nullptr;
    
  } else {
    // Transfert classique pour les fichiers non-streaming
    while (true) {
      bytes_received = recv(data_sock, buffer, sizeof(buffer), 0);
      if (bytes_received <= 0) {
        if (bytes_received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
          ESP_LOGE(TAG, "Erreur de réception des données: %d", errno);
        }
        break;
      }

      esp_err_t err = httpd_resp_send_chunk(req, buffer, bytes_received);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Échec d'envoi au client: %d", err);
        goto error;
      }
      
      // Petit délai pour permettre aux autres tâches de s'exécuter
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }

  // Fermeture du socket de données
  ::close(data_sock);
  data_sock = -1;

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
  return success;

error:
  // S'assurer que la tâche est arrêtée proprement
  if (task_data != nullptr) {
    task_data->done = true;
    
    // Attendre que la tâche se termine mais avec un timeout
    int timeout_counter = 0;
    while (!task_data->done && timeout_counter < 50) {
      vTaskDelay(pdMS_TO_TICKS(20));
      timeout_counter++;
    }
    
    // Supprimer la tâche si elle n'est pas terminée
    if (task_data->task_handle != nullptr) {
      vTaskDelete(task_data->task_handle);
    }
    
    delete task_data;
  }
  
  if (data_sock != -1) ::close(data_sock);
  if (sock_ != -1) {
    send(sock_, "QUIT\r\n", 6, 0);
    ::close(sock_);
    sock_ = -1;
  }
  
  // S'assurer d'envoyer une réponse d'erreur au client HTTP
  httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Échec du téléchargement");
  return false;
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
    httpd_resp_set_type(req, "audio/mpeg");
    // Pour le streaming, ne pas utiliser Content-Disposition: attachment
    // httpd_resp_set_hdr(req, "Content-Disposition", header.c_str());
    ESP_LOGD(TAG, "Configuré pour streaming MP3");
  } else if (extension == ".wav") {
    httpd_resp_set_type(req, "audio/wav");
    ESP_LOGD(TAG, "Configuré pour streaming WAV");
  } else if (extension == ".ogg") {
    httpd_resp_set_type(req, "audio/ogg");
    ESP_LOGD(TAG, "Configuré pour streaming OGG");
  } else if (extension == ".pdf") {
    httpd_resp_set_type(req, "application/pdf");
    std::string header = "attachment; filename=\"" + filename + "\"";
    httpd_resp_set_hdr(req, "Content-Disposition", header.c_str());
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
  
  for (const auto &configured_path : proxy->remote_paths_) {
    if (requested_path == configured_path) {
      ESP_LOGI(TAG, "Téléchargement du fichier: %s", requested_path.c_str());
      if (proxy->download_file(configured_path, req)) {
        ESP_LOGI(TAG, "Téléchargement réussi");
        return ESP_OK;
      } else {
        ESP_LOGE(TAG, "Échec du téléchargement");
        // La réponse d'erreur est déjà envoyée dans download_file
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
  config.stack_size = 16384; // Augmenter à 16 Ko pour éviter les débordements de pile

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
