#include "ftp_http_proxy.h"
#include "esp_log.h"
#include <lwip/sockets.h>
#include <netdb.h>
#include <cstring>
#include <arpa/inet.h>
#include <vector>
#include <algorithm>

static const char *TAG = "ftp_proxy";

// HTML pour l'interface web
static const char *index_html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>FTP/HTTP Manager</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            margin: 0;
            padding: 20px;
            background-color: #f5f5f5;
        }
        .container {
            max-width: 800px;
            margin: 0 auto;
            background-color: white;
            padding: 20px;
            border-radius: 8px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }
        h1 {
            color: #333;
            text-align: center;
        }
        .file-list {
            margin-top: 20px;
            border: 1px solid #ddd;
            border-radius: 4px;
        }
        .file-item {
            padding: 10px;
            border-bottom: 1px solid #eee;
            display: flex;
            justify-content: space-between;
            align-items: center;
        }
        .file-item:last-child {
            border-bottom: none;
        }
        .file-name {
            flex-grow: 1;
            margin-right: 10px;
        }
        .actions {
            display: flex;
            gap: 5px;
        }
        .btn {
            padding: 5px 10px;
            border: none;
            border-radius: 4px;
            cursor: pointer;
            font-size: 14px;
        }
        .btn-download {
            background-color: #4CAF50;
            color: white;
        }
        .btn-delete {
            background-color: #f44336;
            color: white;
        }
        .btn-rename {
            background-color: #2196F3;
            color: white;
        }
        .btn-share {
            background-color: #9c27b0;
            color: white;
        }
        .upload-section {
            margin-top: 20px;
            padding: 15px;
            border: 1px dashed #ddd;
            border-radius: 4px;
            text-align: center;
        }
        .status {
            margin-top: 10px;
            padding: 10px;
            border-radius: 4px;
            display: none;
        }
        .success {
            background-color: #e8f5e9;
            color: #388e3c;
            display: block;
        }
        .error {
            background-color: #ffebee;
            color: #d32f2f;
            display: block;
        }
        #uploadForm {
            display: flex;
            flex-direction: column;
            gap: 10px;
        }
        #refresh {
            background-color: #607d8b;
            color: white;
            margin-bottom: 10px;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>FTP/HTTP File Manager</h1>
        <button id="refresh" class="btn">Actualiser la liste</button>
        <div id="status" class="status"></div>
        <div class="file-list" id="fileList">
            <div class="file-item">Chargement des fichiers...</div>
        </div>
        <div class="upload-section">
            <h3>Téléverser un fichier</h3>
            <form id="uploadForm" enctype="multipart/form-data">
                <input type="file" name="file" id="file" required>
                <button type="submit" class="btn btn-download">Téléverser</button>
            </form>
        </div>
    </div>

    <script>
        document.addEventListener('DOMContentLoaded', function() {
            fetchFiles();
            
            // Actualiser la liste
            document.getElementById('refresh').addEventListener('click', fetchFiles);
            
            // Gestion du formulaire d'upload
            document.getElementById('uploadForm').addEventListener('submit', function(e) {
                e.preventDefault();
                uploadFile();
            });
        });

        function fetchFiles() {
            fetch('/api/list')
                .then(response => response.json())
                .then(data => {
                    const fileList = document.getElementById('fileList');
                    fileList.innerHTML = '';
                    
                    if (data.files.length === 0) {
                        fileList.innerHTML = '<div class="file-item">Aucun fichier trouvé</div>';
                        return;
                    }
                    
                    data.files.forEach(file => {
                        const fileItem = document.createElement('div');
                        fileItem.className = 'file-item';
                        
                        const fileName = document.createElement('div');
                        fileName.className = 'file-name';
                        fileName.textContent = file;
                        
                        const actions = document.createElement('div');
                        actions.className = 'actions';
                        
                        const downloadBtn = document.createElement('button');
                        downloadBtn.className = 'btn btn-download';
                        downloadBtn.textContent = 'Télécharger';
                        downloadBtn.onclick = function() { downloadFile(file); };
                        
                        const deleteBtn = document.createElement('button');
                        deleteBtn.className = 'btn btn-delete';
                        deleteBtn.textContent = 'Supprimer';
                        deleteBtn.onclick = function() { deleteFile(file); };
                        
                        const renameBtn = document.createElement('button');
                        renameBtn.className = 'btn btn-rename';
                        renameBtn.textContent = 'Renommer';
                        renameBtn.onclick = function() { renameFile(file); };
                        
                        const shareBtn = document.createElement('button');
                        shareBtn.className = 'btn btn-share';
                        shareBtn.textContent = 'Partager';
                        shareBtn.onclick = function() { shareFile(file); };
                        
                        actions.appendChild(downloadBtn);
                        actions.appendChild(deleteBtn);
                        actions.appendChild(renameBtn);
                        actions.appendChild(shareBtn);
                        
                        fileItem.appendChild(fileName);
                        fileItem.appendChild(actions);
                        fileList.appendChild(fileItem);
                    });
                })
                .catch(error => {
                    showStatus('Erreur lors du chargement des fichiers: ' + error, false);
                });
        }

        function uploadFile() {
            const fileInput = document.getElementById('file');
            const file = fileInput.files[0];
            if (!file) {
                showStatus('Veuillez sélectionner un fichier', false);
                return;
            }
            
            const formData = new FormData();
            formData.append('file', file);
            
            showStatus('Téléversement en cours...', true);
            
            fetch('/api/upload', {
                method: 'POST',
                body: formData
            })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    showStatus('Fichier téléversé avec succès', true);
                    fetchFiles();
                    document.getElementById('uploadForm').reset();
                } else {
                    showStatus('Erreur: ' + data.message, false);
                }
            })
            .catch(error => {
                showStatus('Erreur lors du téléversement: ' + error, false);
            });
        }

        function downloadFile(fileName) {
            window.location.href = '/' + encodeURIComponent(fileName);
        }

        function deleteFile(fileName) {
            if (confirm('Êtes-vous sûr de vouloir supprimer ' + fileName + ' ?')) {
                fetch('/api/delete', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json',
                    },
                    body: JSON.stringify({
                        file: fileName
                    })
                })
                .then(response => response.json())
                .then(data => {
                    if (data.success) {
                        showStatus('Fichier supprimé avec succès', true);
                        fetchFiles();
                    } else {
                        showStatus('Erreur: ' + data.message, false);
                    }
                })
                .catch(error => {
                    showStatus('Erreur lors de la suppression: ' + error, false);
                });
            }
        }

        function renameFile(fileName) {
            const newName = prompt('Entrez le nouveau nom pour ' + fileName + ':', fileName);
            if (newName && newName !== fileName) {
                fetch('/api/rename', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json',
                    },
                    body: JSON.stringify({
                        oldName: fileName,
                        newName: newName
                    })
                })
                .then(response => response.json())
                .then(data => {
                    if (data.success) {
                        showStatus('Fichier renommé avec succès', true);
                        fetchFiles();
                    } else {
                        showStatus('Erreur: ' + data.message, false);
                    }
                })
                .catch(error => {
                    showStatus('Erreur lors du renommage: ' + error, false);
                });
            }
        }

        function shareFile(fileName) {
            const url = window.location.origin + '/' + encodeURIComponent(fileName);
            
            if (navigator.clipboard) {
                navigator.clipboard.writeText(url)
                    .then(() => {
                        showStatus('Lien copié dans le presse-papiers: ' + url, true);
                    })
                    .catch(err => {
                        showManualCopy(url);
                    });
            } else {
                showManualCopy(url);
            }
        }

        function showManualCopy(url) {
            prompt('Copiez ce lien pour partager le fichier:', url);
        }

        function showStatus(message, isSuccess) {
            const statusElement = document.getElementById('status');
            statusElement.textContent = message;
            statusElement.className = 'status ' + (isSuccess ? 'success' : 'error');
            
            setTimeout(() => {
                statusElement.className = 'status';
            }, 5000);
        }
    </script>
</body>
</html>
)rawliteral";

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
  
  // Augmenter la taille du buffer de réception
  int rcvbuf = 16384;
  setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(ftp_port_);
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
  // Déclarations en haut pour éviter les goto cross-initialization
  int data_sock = -1;
  bool success = false;
  char *pasv_start = nullptr;
  int data_port = 0;
  int ip[4], port[2]; 
  char buffer[8192]; // Tampon de 8ko pour réception
  int bytes_received;
  int flag = 1;  // Déplacé avant les goto
  int rcvbuf = 16384; // Déplacé avant les goto

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

  // Transfert en streaming avec un buffer plus petit pour éviter les problèmes de mémoire
  while (true) {
    bytes_received = recv(data_sock, buffer, sizeof(buffer), 0);
    if (bytes_received <= 0) {
      if (bytes_received < 0) {
        ESP_LOGE(TAG, "Erreur de réception des données: %d", errno);
      }
      break;
    }

    esp_err_t err = httpd_resp_send_chunk(req, buffer, bytes_received);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Échec d'envoi au client: %d", err);
      goto error;
    }
    
    // Petit délai pour permettre au TCP/IP stack de respirer
    vTaskDelay(pdMS_TO_TICKS(1));
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
  if (data_sock != -1) ::close(data_sock);
  if (sock_ != -1) {
    send(sock_, "QUIT\r\n", 6, 0);
    ::close(sock_);
    sock_ = -1;
  }
  return false;
}

bool FTPHTTPProxy::upload_file(const std::string &remote_path, const uint8_t *data, size_t size) {
  int data_sock = -1;
  bool success = false;
  char *pasv_start = nullptr;
  int data_port = 0;
  int ip[4], port[2]; 
  char buffer[1024];
  int bytes_received;
  int flag = 1;
  int sndbuf = 16384;

  // Connexion au serveur FTP
  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "Échec de connexion FTP pour l'upload");
    return false;
  }

  // Mode passif
  send(sock_, "PASV\r\n", 6, 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "227 ")) {
    ESP_LOGE(TAG, "Erreur en mode passif pour l'upload");
    goto error;
  }
  buffer[bytes_received] = '\0';

  // Extraction des données de connexion
  pasv_start = strchr(buffer, '(');
  if (!pasv_start) {
    ESP_LOGE(TAG, "Format PASV incorrect pour l'upload");
    goto error;
  }

  sscanf(pasv_start, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]);
  data_port = port[0] * 256 + port[1];

  // Création du socket de données
  data_sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Échec de création du socket de données pour l'upload");
    goto error;
  }

  // Configuration du socket
  setsockopt(data_sock, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
  setsockopt(data_sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  data_addr.sin_addr.s_addr = htonl(
      (ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]
  );

  // Envoi de la commande STOR
  snprintf(buffer, sizeof(buffer), "STOR %s\r\n", remote_path.c_str());
  ESP_LOGD(TAG, "Envoi de la commande: %s", buffer);
  send(sock_, buffer, strlen(buffer), 0);

  // Connexion au socket de données
  if (::connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Échec de connexion au port de données pour l'upload");
    goto error;
  }

  // Vérification de la réponse 150
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "150 ")) {
    ESP_LOGE(TAG, "Impossible de commencer l'upload");
    goto error;
  }

  // Envoi des données
  size_t total_sent = 0;
  while (total_sent < size) {
    // Déterminer la taille du chunk à envoyer
    size_t chunk_size = std::min(size_t(4096), size - total_sent);
    
    // Envoyer le chunk
    int bytes_sent = send(data_sock, data + total_sent, chunk_size, 0);
    if (bytes_sent <= 0) {
      ESP_LOGE(TAG, "Erreur lors de l'envoi des données: %d", errno);
      goto error;
    }
    
    total_sent += bytes_sent;
    
    // Petit délai pour éviter de surcharger le stack TCP/IP
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  // Fermeture du socket de données
  ::close(data_sock);
  data_sock = -1;

  // Vérification de la réponse finale 226
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received > 0 && strstr(buffer, "226 ")) {
    success = true;
    ESP_LOGI(TAG, "Upload terminé avec succès");
  } else {
    ESP_LOGE(TAG, "Échec de confirmation de l'upload");
  }

  // Fermeture de la connexion
  send(sock_, "QUIT\r\n", 6, 0);
  ::close(sock_);
  sock_ = -1;

  return success;

error:
  if (data_sock != -1) ::close(data_sock);
  if (sock_ != -1) {
    send(sock_, "QUIT\r\n", 6, 0);
    ::close(sock_);
    sock_ = -1;
  }
  return false;
}

bool FTPHTTPProxy::delete_file(const std::string &remote_path) {
  char buffer[1024];
  int bytes_received;
  bool success = false;

  // Connexion au serveur FTP
  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "Échec de connexion FTP pour la suppression");
    return false;
  }

  // Envoi de la commande DELE
  snprintf(buffer, sizeof(buffer), "DELE %s\r\n", remote_path.c_str());
  ESP_LOGD(TAG, "Envoi de la commande: %s", buffer);
  send(sock_, buffer, strlen(buffer), 0);

  // Vérification de la réponse
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';

  if (bytes_received > 0 && strstr(buffer, "250 ")) {
    success = true;
    ESP_LOGI(TAG, "Fichier supprimé avec succès");
  } else {
    ESP_LOGE(TAG, "Échec de suppression: %s", buffer);
  }

  // Fermeture de la connexion
  send(sock_, "QUIT\r\n", 6, 0);
  ::close(sock_);
  sock_ = -1;

  return success;
}

bool FTPHTTPProxy::rename_file(const std::string &old_path, const std::string &new_path) {
  char buffer[1024];
  int bytes_received;
  bool success = false;

  // Connexion au serveur FTP
  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "Échec de connexion FTP pour le renommage");
    return false;
  }

  // Envoi de la commande RNFR (rename from)
  snprintf(buffer, sizeof(buffer), "RNFR %s\r\n", old_path.c_str());
  ESP_LOGD(TAG, "Envoi de la commande: %s", buffer);
  send(sock_, buffer, strlen(buffer), 0);

  // Vérification de la réponse
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';

  if (bytes_received <= 0 || !strstr(buffer, "350 ")) {
    ESP_LOGE(TAG, "Fichier source introuvable: %s", buffer);
    goto error;
  }

  // Envoi de la commande RNTO (rename to)
  snprintf(buffer, sizeof(buffer), "RNTO %s\r\n", new_path.c_str());
  ESP_LOGD(TAG, "Envoi de la commande: %s", buffer);
  send(sock_, buffer, strlen(buffer), 0);

  // Vérification de la réponse
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';

  if (bytes_received > 0 && strstr(buffer, "250 ")) {
    success = true;
    ESP_LOGI(TAG, "Fichier renommé avec succès");
  } else {
    ESP_LOGE(TAG, "Échec du renommage: %s", buffer);
  }

  // Fermeture de la connexion
  send(sock_, "QUIT\r\n", 6, 0);
  ::close(sock_);
  sock_ = -1;

  return success;

error:
  if (sock_ != -1) {
    send(sock_, "QUIT\r\n", 6, 0);
    ::close(sock_);
    sock_ = -1;
  }
  return false;
}

std::vector<std::string> FTPHTTPProxy::list_files() {
  std::vector<std::string> file_list;
  int data_sock = -1;
  char *pasv_start = nullptr;
  int data_port = 0;
  int ip[4], port[2]; 
  char buffer[4096]; // Buffer pour la réception de la liste
  int bytes_received;
  int flag = 1;
  int rcvbuf = 16384;

  // Connexion au serveur FTP
  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "Échec de connexion FTP pour lister les fichiers");
    return file_list;
  }

  // Mode passif
  send(sock_, "PASV\r\n", 6, 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "227 ")) {
    ESP_LOGE(TAG, "Erreur en mode passif pour le listing");
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

  // Transfert en streaming avec un buffer plus petit pour éviter les problèmes de mémoire
  while (true) {
    bytes_received = recv(data_sock, buffer, sizeof(buffer), 0);
    if (bytes_received <= 0) {
      if (bytes_received < 0) {
        ESP_LOGE(TAG, "Erreur de réception des données: %d", errno);
      }
      break;
    }

    esp_err_t err = httpd_resp_send_chunk(req, buffer, bytes_received);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Échec d'envoi au client: %d", err);
      goto error;
    }
    
    // Petit délai pour permettre au TCP/IP stack de respirer
    vTaskDelay(pdMS_TO_TICKS(1));
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
  if (data_sock != -1) ::close(data_sock);
  if (sock_ != -1) {
    send(sock_, "QUIT\r\n", 6, 0);
    ::close(sock_);
    sock_ = -1;
  }
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
  config.recv_wait_timeout = 20;
  config.send_wait_timeout = 20;
  config.max_uri_handlers = 8;
  config.max_resp_headers = 20;
  config.stack_size = 12288;

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
