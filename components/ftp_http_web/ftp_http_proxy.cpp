#include "ftp_http_proxy.h"
#include "esp_log.h"
#include <lwip/sockets.h>
#include <netdb.h>
#include <cstring>
#include <arpa/inet.h>
#include <esp_vfs.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "ftp_proxy";

// HTML template pour l'interface web
static const char *HTML_TEMPLATE = R"(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>FTP File Manager</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 0; padding: 20px; max-width: 1000px; margin: 0 auto; }
    h1 { color: #333; }
    .file-list { width: 100%; border-collapse: collapse; margin-top: 20px; }
    .file-list th, .file-list td { padding: 8px; text-align: left; border-bottom: 1px solid #ddd; }
    .file-list tr:hover { background-color: #f5f5f5; }
    .actions { display: flex; gap: 5px; }
    .actions button { cursor: pointer; }
    .form-group { margin-bottom: 15px; }
    .form-group label { display: block; margin-bottom: 5px; }
    .form-group input { width: 100%; padding: 8px; box-sizing: border-box; }
    .btn { padding: 8px 16px; background: #4CAF50; color: white; border: none; cursor: pointer; }
    .btn:hover { background: #45a049; }
    .upload-section { margin: 20px 0; padding: 15px; border: 1px solid #ddd; border-radius: 5px; }
    .modal { display: none; position: fixed; z-index: 1; left: 0; top: 0; width: 100%; height: 100%; overflow: auto; background-color: rgba(0,0,0,0.4); }
    .modal-content { background-color: #fefefe; margin: 15% auto; padding: 20px; border: 1px solid #888; width: 80%; max-width: 500px; }
    .close { color: #aaa; float: right; font-size: 28px; font-weight: bold; cursor: pointer; }
    .close:hover { color: black; }
  </style>
</head>
<body>
  <h1>FTP File Manager</h1>
  
  <div class="upload-section">
    <h2>Upload File</h2>
    <form id="uploadForm" enctype="multipart/form-data">
      <div class="form-group">
        <label for="fileInput">Select File:</label>
        <input type="file" id="fileInput" name="file" required>
      </div>
      <div class="form-group">
        <label for="remotePath">Remote Path (optional):</label>
        <input type="text" id="remotePath" name="remotePath" placeholder="Enter remote path or leave empty for root">
      </div>
      <button type="submit" class="btn">Upload</button>
    </form>
  </div>

  <h2>File List</h2>
  <table class="file-list">
    <thead>
      <tr>
        <th>Name</th>
        <th>Size</th>
        <th>Last Modified</th>
        <th>Actions</th>
      </tr>
    </thead>
    <tbody id="fileListBody">
      <!-- Files will be listed here -->
      %FILE_LIST%
    </tbody>
  </table>

  <!-- Rename Modal -->
  <div id="renameModal" class="modal">
    <div class="modal-content">
      <span class="close">&times;</span>
      <h2>Rename File</h2>
      <form id="renameForm">
        <input type="hidden" id="oldFilename">
        <div class="form-group">
          <label for="newFilename">New Name:</label>
          <input type="text" id="newFilename" required>
        </div>
        <button type="submit" class="btn">Rename</button>
      </form>
    </div>
  </div>

  <script>
    // JavaScript for handling user interactions
    document.addEventListener('DOMContentLoaded', function() {
      // Upload form handling
      document.getElementById('uploadForm').addEventListener('submit', function(e) {
        e.preventDefault();
        const formData = new FormData(this);
        
        fetch('/api/upload', {
          method: 'POST',
          body: formData
        })
        .then(response => response.json())
        .then(data => {
          if(data.success) {
            alert('File uploaded successfully!');
            window.location.reload();
          } else {
            alert('Upload failed: ' + data.message);
          }
        })
        .catch(error => {
          alert('Upload error: ' + error);
        });
      });

      // Delete file handling
      document.querySelectorAll('.delete-btn').forEach(button => {
        button.addEventListener('click', function() {
          const filename = this.getAttribute('data-filename');
          if(confirm('Are you sure you want to delete ' + filename + '?')) {
            fetch('/api/delete', {
              method: 'POST',
              headers: {
                'Content-Type': 'application/json',
              },
              body: JSON.stringify({filename: filename})
            })
            .then(response => response.json())
            .then(data => {
              if(data.success) {
                alert('File deleted successfully!');
                window.location.reload();
              } else {
                alert('Delete failed: ' + data.message);
              }
            })
            .catch(error => {
              alert('Delete error: ' + error);
            });
          }
        });
      });

      // Rename modal handling
      const renameModal = document.getElementById('renameModal');
      const closeBtn = document.querySelector('.close');
      
      document.querySelectorAll('.rename-btn').forEach(button => {
        button.addEventListener('click', function() {
          const filename = this.getAttribute('data-filename');
          document.getElementById('oldFilename').value = filename;
          document.getElementById('newFilename').value = filename;
          renameModal.style.display = 'block';
        });
      });

      closeBtn.addEventListener('click', function() {
        renameModal.style.display = 'none';
      });

      window.addEventListener('click', function(event) {
        if (event.target == renameModal) {
          renameModal.style.display = 'none';
        }
      });

      // Rename form handling
      document.getElementById('renameForm').addEventListener('submit', function(e) {
        e.preventDefault();
        const oldFilename = document.getElementById('oldFilename').value;
        const newFilename = document.getElementById('newFilename').value;
        
        fetch('/api/rename', {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json',
          },
          body: JSON.stringify({
            oldFilename: oldFilename,
            newFilename: newFilename
          })
        })
        .then(response => response.json())
        .then(data => {
          if(data.success) {
            alert('File renamed successfully!');
            window.location.reload();
          } else {
            alert('Rename failed: ' + data.message);
          }
        })
        .catch(error => {
          alert('Rename error: ' + error);
        });
        
        renameModal.style.display = 'none';
      });
    });
  </script>
</body>
</html>
)";

namespace esphome {
namespace ftp_http_proxy {

// Structure pour les informations de fichier
struct FileInfo {
  std::string name;
  size_t size;
  time_t modified;
};

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

  char buffer[512];
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
  
  if (!strstr(buffer, "230 ")) {
    ESP_LOGE(TAG, "Échec d'authentification FTP: %s", buffer);
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  return true;
}

bool FTPHTTPProxy::send_ftp_command(const std::string &cmd, std::string &response) {
  char buffer[2048];
  int bytes_received;

  // Envoi de la commande
  send(sock_, cmd.c_str(), cmd.length(), 0);
  
  // Réception de la réponse
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0) {
    return false;
  }
  
  buffer[bytes_received] = '\0';
  response = buffer;
  return true;
}

  std::vector<std::string> FTPHTTPProxy::list_files() {
  std::vector<FileInfo> files;
  int data_sock = -1;
  char buffer[2048];
  std::string response;

  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "Échec de connexion FTP pour lister les fichiers");
    return files;
  }

  // Mode binaire
  send(sock_, "TYPE I\r\n", 8, 0);
  recv(sock_, buffer, sizeof(buffer) - 1, 0);

  // Mode passif
  send(sock_, "PASV\r\n", 6, 0);
  int bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "227 ")) {
    goto cleanup;
  }
  buffer[bytes_received] = '\0';

  // Extraction des données de connexion
  char *pasv_start = strchr(buffer, '(');
  if (!pasv_start) {
    goto cleanup;
  }

  int ip[4], port[2];
  sscanf(pasv_start, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]);
  int data_port = port[0] * 256 + port[1];

  // Création du socket de données
  data_sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    goto cleanup;
  }

  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  data_addr.sin_addr.s_addr = htonl(
      (ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]
  );

  if (::connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    goto cleanup;
  }

  // Envoi de la commande LIST
  send(sock_, "LIST\r\n", 6, 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';

  // Lecture des données
  std::string list_data;
  while (true) {
    bytes_received = recv(data_sock, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received <= 0) {
      break;
    }
    buffer[bytes_received] = '\0';
    list_data += buffer;
  }

  // Traitement des données
  std::istringstream iss(list_data);
  std::string line;
  while (std::getline(iss, line)) {
    // Exemple format: -rw-r--r--    1 user     group        1024 Jan 01  2023 filename.txt
    FileInfo file;
    size_t last_space = line.find_last_of(' ');
    if (last_space != std::string::npos) {
      file.name = line.substr(last_space + 1);
      
      // Recherche de la taille
      std::istringstream line_stream(line);
      std::string token;
      for (int i = 0; i < 5; i++) {
        line_stream >> token;
      }
      file.size = std::stoul(token);
      
      // La date est approximative ici
      file.modified = time(NULL);
      
      files.push_back(file);
    }
  }

cleanup:
  if (data_sock != -1) ::close(data_sock);
  send(sock_, "QUIT\r\n", 6, 0);
  ::close(sock_);
  sock_ = -1;

  return files;
}

bool FTPHTTPProxy::upload_file(const std::string &local_path, const std::string &remote_path) {
  if (local_path.empty() || remote_path.empty()) {
    ESP_LOGE(TAG, "Empty path specified");
    return false;
  }
  
  // Initialize variables before any potential jumps
  FILE *fp = nullptr;
  int data_sock = -1;
  bool success = false;
  char buffer[1024];
  
  // Connect to control socket
  if (!send_command("PASV", buffer, sizeof(buffer))) {
    ESP_LOGE(TAG, "Failed to enter passive mode");
    goto cleanup;
  }
  
  char *pasv_start = strchr(buffer, '(');
  if (pasv_start == nullptr) {
    ESP_LOGE(TAG, "Invalid PASV response");
    goto cleanup;
  }
  
  // Parse port details
  uint8_t port[2];
  if (sscanf(pasv_start, "(%*d,%*d,%*d,%*d,%hhu,%hhu)", &port[0], &port[1]) != 2) {
    ESP_LOGE(TAG, "Failed to parse PASV response");
    goto cleanup;
  }
  
  int data_port = port[0] * 256 + port[1];
  
  // Open data connection
  data_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Failed to create data socket");
    goto cleanup;
  }
  
  cleanup:
    // Cleanup
    if (fp != nullptr) {
      fclose(fp);
    }
    
    if (data_sock >= 0) {
      close(data_sock);
    }
    
    return success;
  }
  
  // Open local file
  fp = fopen(local_path.c_str(), "rb");
  if (fp == nullptr) {
    ESP_LOGE(TAG, "Failed to open local file");
    goto cleanup;
  }
  
  int data_sock = -1;
  bool success = false;
  char buffer[8192];
  int bytes_received;
  int bytes_read;
  std::string response;

  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "Échec de connexion FTP pour upload");
    return false;
  }

  // Mode binaire
  send(sock_, "TYPE I\r\n", 8, 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';

  // Mode passif
  send(sock_, "PASV\r\n", 6, 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "227 ")) {
    goto error;
  }
  buffer[bytes_received] = '\0';

  // Extraction des données de connexion
  char *pasv_start = strchr(buffer, '(');
  if (!pasv_start) {
    goto error;
  }

  int ip[4], port[2];
  sscanf(pasv_start, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]);
  int data_port = port[0] * 256 + port[1];

  // Création du socket de données
  data_sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    goto error;
  }

  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  data_addr.sin_addr.s_addr = htonl(
      (ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]
  );

  if (::connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    goto error;
  }

  // Envoi de la commande STOR
  snprintf(buffer, sizeof(buffer), "STOR %s\r\n", remote_path.c_str());
  send(sock_, buffer, strlen(buffer), 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "150 ")) {
    goto error;
  }

  // Ouverture du fichier local
  FILE *fp = fopen(local_path.c_str(), "rb");
  if (!fp) {
    goto error;
  }

  // Transfert des données
  while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
    if (send(data_sock, buffer, bytes_read, 0) != bytes_read) {
      fclose(fp);
      goto error;
    }
  }

  fclose(fp);
  ::close(data_sock);
  data_sock = -1;

  // Vérification de la réponse finale
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received > 0 && strstr(buffer, "226 ")) {
    success = true;
  }

error:
  if (data_sock != -1) ::close(data_sock);
  send(sock_, "QUIT\r\n", 6, 0);
  ::close(sock_);
  sock_ = -1;

  return success;
}

bool FTPHTTPProxy::delete_file(const std::string &remote_path) {
  std::string response;
  bool success = false;

  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "Échec de connexion FTP pour suppression");
    return false;
  }

  // Envoi de la commande DELE
  std::string cmd = "DELE " + remote_path + "\r\n";
  if (send_ftp_command(cmd, response)) {
    success = (response.find("250 ") != std::string::npos);
  }

  send(sock_, "QUIT\r\n", 6, 0);
  ::close(sock_);
  sock_ = -1;

  return success;
}

bool FTPHTTPProxy::rename_file(const std::string &old_path, const std::string &new_path) {
  std::string response;
  bool success = false;

  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "Échec de connexion FTP pour renommage");
    return false;
  }

  // Envoi de la commande RNFR (rename from)
  std::string cmd = "RNFR " + old_path + "\r\n";
  if (send_ftp_command(cmd, response) && response.find("350 ") != std::string::npos) {
    // Envoi de la commande RNTO (rename to)
    cmd = "RNTO " + new_path + "\r\n";
    if (send_ftp_command(cmd, response)) {
      success = (response.find("250 ") != std::string::npos);
    }
  }

  send(sock_, "QUIT\r\n", 6, 0);
  ::close(sock_);
  sock_ = -1;

  return success;
}

bool FTPHTTPProxy::download_file(const std::string &remote_path, httpd_req_t *req) {
  // Déclarations en haut pour éviter les goto cross-initialization
  int data_sock = -1;
  bool success = false;
  char *pasv_start = nullptr;
  int data_port = 0;
  int ip[4], port[2]; 
  char buffer[8192]; // Tampon pour réception
  int bytes_received;
  int flag = 1;
  int rcvbuf = 16384;

  // Connexion au serveur FTP
  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "Échec de connexion FTP");
    goto error;
  }

  // Mode binaire
  send(sock_, "TYPE I\r\n", 8, 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';

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

  // Transfert en streaming
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

// Gestionnaire pour l'API web
esp_err_t FTPHTTPProxy::api_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
  char buffer[512];
  
  // Déterminer l'endpoint
  std::string uri = req->uri;
  
  // Endpoint pour lister les fichiers
  if (uri == "/api/list") {
    std::vector<FileInfo> files = proxy->list_files();
    
    // Construire la réponse JSON
    std::string json = "{\"success\":true,\"files\":[";
    for (size_t i = 0; i < files.size(); i++) {
      if (i > 0) json += ",";
      json += "{\"name\":\"" + files[i].name + "\",\"size\":" + std::to_string(files[i].size) + 
              ",\"modified\":" + std::to_string(files[i].modified) + "}";
    }
    json += "]}";
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
  }
  
  // Endpoint pour supprimer un fichier
  if (uri == "/api/delete" && req->method == HTTP_POST) {
    int content_len = req->content_len;
    if (content_len > sizeof(buffer) - 1) {
      content_len = sizeof(buffer) - 1;
    }
    int ret = httpd_req_recv(req, buffer, content_len);
    if (ret <= 0) {
      return ESP_FAIL;
    }
    buffer[ret] = '\0';
    
    // Extraire le nom du fichier (analyse JSON simplifiée)
    char filename[256] = {0};
    if (sscanf(buffer, "{\"filename\":\"%255[^\"]\"}", filename) != 1) {
      httpd_resp_set_type(req, "application/json");
      httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Invalid JSON format\"}");
      return ESP_OK;
    }
    
    bool success = proxy->delete_file(filename);
    httpd_resp_set_type(req, "application/json");
    if (success) {
      httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"File deleted successfully\"}");
    } else {
      httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Failed to delete file\"}");
    }
    return ESP_OK;
  }
  
// Complétion de l'endpoint pour renommer un fichier
  if (uri == "/api/rename" && req->method == HTTP_POST) {
    int content_len = req->content_len;
    if (content_len > sizeof(buffer) - 1) {
      content_len = sizeof(buffer) - 1;
    }
    int ret = httpd_req_recv(req, buffer, content_len);
    if (ret <= 0) {
      return ESP_FAIL;
    }
    buffer[ret] = '\0';
    
    // Extraire les noms des fichiers (analyse JSON simplifiée)
    char old_name[256] = {0};
    char new_name[256] = {0};
    if (sscanf(buffer, "{\"old_name\":\"%255[^\"]\",\"new_name\":\"%255[^\"]\"}", old_name, new_name) != 2) {
      httpd_resp_set_type(req, "application/json");
      httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Invalid JSON format\"}");
      return ESP_OK;
    }
    
    bool success = proxy->rename_file(old_name, new_name);
    httpd_resp_set_type(req, "application/json");
    if (success) {
      httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"File renamed successfully\"}");
    } else {
      httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Failed to rename file\"}");
    }
    return ESP_OK;
  }
  
  // Endpoint pour télécharger un fichier
  if (uri.find("/api/download/") == 0) {
    std::string filename = uri.substr(14); // Extraire le nom après "/api/download/"
    
    // Définir les en-têtes de réponse
    httpd_resp_set_type(req, "application/octet-stream");
    std::string disposition = "attachment; filename=\"" + filename + "\"";
    httpd_resp_set_hdr(req, "Content-Disposition", disposition.c_str());
    
    // Télécharger et streamer le fichier
    bool success = proxy->download_file(filename, req);
    if (!success) {
      httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    }
    return ESP_OK;
  }
  
  // Endpoint pour uploader un fichier
  if (uri == "/api/upload" && req->method == HTTP_POST) {
    // Obtenir le boundary du multipart form-data
    char boundary[100];
    char *content_type = nullptr;
    if ((content_type = httpd_req_get_hdr_value_str(req, "Content-Type", boundary, sizeof(boundary) - 1)) == nullptr) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content-Type header not found");
      return ESP_FAIL;
    }
    
    // Extraire le boundary
    char *boundary_start = strstr(content_type, "boundary=");
    if (!boundary_start) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Boundary not found in Content-Type");
      return ESP_FAIL;
    }
    
    boundary_start += 9; // Skip "boundary="
    snprintf(boundary, sizeof(boundary), "--%s", boundary_start);
    
    // Créer un fichier temporaire
    char temp_path[64];
    snprintf(temp_path, sizeof(temp_path), "/tmp/upload_XXXXXX");
    int fd = mkstemp(temp_path);
    if (fd < 0) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create temp file");
      return ESP_FAIL;
    }
    
    FILE *fp = fdopen(fd, "wb");
    if (!fp) {
      close(fd);
      unlink(temp_path);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open temp file");
      return ESP_FAIL;
    }
    
    // Variables pour l'analyse multipart
    char part_buf[1024];
    bool is_file_data = false;
    std::string filename;
    
    // Lire les données par morceaux
    int remaining = req->content_len;
    
    while (remaining > 0) {
      int recv_len = httpd_req_recv(req, part_buf, std::min(remaining, (int)sizeof(part_buf)));
      if (recv_len <= 0) {
        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
          continue;
        }
        fclose(fp);
        unlink(temp_path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
        return ESP_FAIL;
      }
      
      // Analyse basique multipart/form-data
      if (!is_file_data) {
        // Chercher le nom du fichier
        char *filename_start = strstr(part_buf, "filename=\"");
        if (filename_start) {
          filename_start += 10; // Skip "filename=\""
          char *filename_end = strchr(filename_start, '"');
          if (filename_end) {
            filename.assign(filename_start, filename_end - filename_start);
          }
        }
        
        // Chercher le début des données binaires (après deux retours à la ligne)
        char *data_start = strstr(part_buf, "\r\n\r\n");
        if (data_start) {
          data_start += 4; // Skip "\r\n\r\n"
          is_file_data = true;
          
          // Écrire seulement à partir du début des données
          fwrite(data_start, 1, part_buf + recv_len - data_start, fp);
        }
      } else {
        // Vérifier si nous avons atteint la boundary de fin
        char *boundary_pos = std::search(part_buf, part_buf + recv_len, 
                                        boundary, boundary + strlen(boundary));
        
        if (boundary_pos != part_buf + recv_len) {
          // Écrire seulement jusqu'à la boundary
          fwrite(part_buf, 1, boundary_pos - part_buf - 2, fp); // -2 pour ignorer \r\n
          is_file_data = false;
        } else {
          // Écrire tout le morceau
          fwrite(part_buf, 1, recv_len, fp);
        }
      }
      
      remaining -= recv_len;
    }
    
    fclose(fp);
    
    // Uploader le fichier vers le serveur FTP
    bool success = false;
    if (!filename.empty()) {
      success = proxy->upload_file(temp_path, filename);
    }
    
    // Supprimer le fichier temporaire
    unlink(temp_path);
    
    // Envoyer la réponse
    httpd_resp_set_type(req, "application/json");
    if (success) {
      httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"File uploaded successfully\"}");
    } else {
      httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Failed to upload file\"}");
    }
    
    return ESP_OK;
  }
  
  // Endpoint non trouvé
  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Endpoint not found");
  return ESP_OK;
}

// Configuration du serveur HTTP
void FTPHTTPProxy::setup_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192; // Augmenter la taille de la pile
  config.max_uri_handlers = 10; // Augmenter le nombre max de gestionnaires
  
  ESP_LOGI(TAG, "Démarrage du serveur HTTP sur le port %d", config.server_port);
  
  if (httpd_start(&server_, &config) == ESP_OK) {
    // Gestionnaire d'API
    httpd_uri_t api_uri = {
      .uri = "/api/*",
      .method = HTTP_ANY,
      .handler = FTPHTTPProxy::api_handler_wrapper,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_uri);
    
    // Gestionnaire pour fichiers statiques (interface web)
    httpd_uri_t static_uri = {
      .uri = "/*",
      .method = HTTP_GET,
      .handler = FTPHTTPProxy::static_handler,
      .user_ctx = nullptr
    };
    httpd_register_uri_handler(server_, &static_uri);
    
    ESP_LOGI(TAG, "Serveur HTTP démarré avec succès");
  } else {
    ESP_LOGE(TAG, "Échec du démarrage du serveur HTTP");
  }
}

// Wrapper statique pour le gestionnaire d'API
esp_err_t FTPHTTPProxy::api_handler_wrapper(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
  return proxy->api_handler(req);
}

// Gestionnaire pour servir les fichiers statiques
esp_err_t FTPHTTPProxy::static_handler(httpd_req_t *req) {
  // Obtenir le chemin du fichier demandé
  std::string uri = req->uri;
  if (uri == "/") {
    uri = "/index.html"; // Page par défaut
  }
  
  // Déterminer le type MIME
  const char *mime_type = "text/plain";
  if (uri.ends_with(".html")) mime_type = "text/html";
  else if (uri.ends_with(".css")) mime_type = "text/css";
  else if (uri.ends_with(".js")) mime_type = "application/javascript";
  else if (uri.ends_with(".png")) mime_type = "image/png";
  else if (uri.ends_with(".jpg") || uri.ends_with(".jpeg")) mime_type = "image/jpeg";
  else if (uri.ends_with(".ico")) mime_type = "image/x-icon";
  
  // Chemin complet dans la mémoire flash
  std::string fs_path = "/www" + uri;
  
  // Ouvrir le fichier
  FILE *fp = fopen(fs_path.c_str(), "rb");
  if (!fp) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_OK;
  }
  
  // Définir le type MIME
  httpd_resp_set_type(req, mime_type);
  
  // Buffer pour la lecture
  char *chunk = (char *)malloc(STATIC_FILE_CHUNK_SIZE);
  if (!chunk) {
    fclose(fp);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_FAIL;
  }
  
  // Envoyer le fichier par morceaux
  size_t bytes_read;
  do {
    bytes_read = fread(chunk, 1, STATIC_FILE_CHUNK_SIZE, fp);
    if (bytes_read > 0) {
      if (httpd_resp_send_chunk(req, chunk, bytes_read) != ESP_OK) {
        fclose(fp);
        free(chunk);
        return ESP_FAIL;
      }
    }
  } while (bytes_read == STATIC_FILE_CHUNK_SIZE);
  
  // Libérer les ressources
  fclose(fp);
  free(chunk);
  
  // Envoyer le chunk final (vide)
  httpd_resp_send_chunk(req, NULL, 0);
  
  return ESP_OK;
}

} // namespace ftp_http_proxy
} // namespace esphome
