#include "ftp_http_proxy.h"
#include "esp_log.h"
#include <lwip/sockets.h>
#include <netdb.h>
#include <cstring>
#include <arpa/inet.h>
#include <esp_spiffs.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "ftp_proxy";

namespace esphome {
namespace ftp_http_proxy {

// Ressources HTML embarquées pour l'interface utilisateur
const char* FTPHTTPProxy::HTML_INDEX = R"=====(
<!DOCTYPE html>
<html lang="fr">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Gestionnaire de fichiers FTP</title>
    <style>
        /* CSS styles intégrés ici */
        body {
            font-family: Arial, sans-serif;
            max-width: 1200px;
            margin: 0 auto;
            padding: 20px;
            background-color: #f5f5f5;
        }
        .header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 20px;
        }
        .path-nav {
            background-color: #fff;
            padding: 10px;
            border-radius: 4px;
            margin-bottom: 10px;
            box-shadow: 0 1px 3px rgba(0,0,0,0.1);
        }
        .file-grid {
            display: grid;
            grid-template-columns: repeat(auto-fill, minmax(180px, 1fr));
            gap: 15px;
            margin-bottom: 20px;
        }
        .file-item {
            background: white;
            border-radius: 4px;
            padding: 15px;
            text-align: center;
            box-shadow: 0 1px 3px rgba(0,0,0,0.1);
            transition: transform 0.2s, box-shadow 0.2s;
            position: relative;
            cursor: pointer;
        }
        .file-item:hover {
            transform: translateY(-2px);
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
        }
        .file-icon {
            font-size: 2.5rem;
            margin-bottom: 10px;
            color: #555;
        }
        .folder-icon { color: #FFD700; }
        .image-icon { color: #4CAF50; }
        .audio-icon { color: #2196F3; }
        .pdf-icon { color: #F44336; }
        .generic-icon { color: #9E9E9E; }
        .file-name {
            font-size: 0.9rem;
            overflow: hidden;
            text-overflow: ellipsis;
            white-space: nowrap;
        }
        .file-size {
            font-size: 0.8rem;
            color: #777;
            margin-top: 5px;
        }
        .file-actions {
            position: absolute;
            top: 5px;
            right: 5px;
            display: none;
        }
        .file-item:hover .file-actions {
            display: block;
        }
        .action-btn {
            background: none;
            border: none;
            font-size: 1rem;
            color: #555;
            cursor: pointer;
            margin-left: 5px;
        }
        .action-btn:hover {
            color: #000;
        }
        .btn {
            padding: 8px 16px;
            background-color: #4CAF50;
            color: white;
            border: none;
            border-radius: 4px;
            cursor: pointer;
            font-size: 1rem;
            transition: background-color 0.2s;
        }
        .btn:hover {
            background-color: #3e8e41;
        }
        .btn-group {
            display: flex;
            gap: 10px;
        }
        .upload-area {
            border: 2px dashed #ccc;
            padding: 20px;
            text-align: center;
            margin-bottom: 20px;
            border-radius: 4px;
            background-color: #f9f9f9;
        }
        .upload-area.highlight {
            border-color: #4CAF50;
            background-color: rgba(76, 175, 80, 0.1);
        }
        .modal {
            display: none;
            position: fixed;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            background-color: rgba(0,0,0,0.5);
            z-index: 100;
            justify-content: center;
            align-items: center;
        }
        .modal-content {
            background-color: white;
            padding: 20px;
            border-radius: 4px;
            max-width: 500px;
            width: 100%;
            box-shadow: 0 4px 8px rgba(0,0,0,0.2);
        }
        .progress-bar {
            width: 100%;
            height: 20px;
            background-color: #f0f0f0;
            border-radius: 10px;
            overflow: hidden;
            margin-top: 10px;
        }
        .progress {
            height: 100%;
            background-color: #4CAF50;
            width: 0;
            transition: width 0.3s;
        }
        #toast {
            position: fixed;
            bottom: 20px;
            left: 50%;
            transform: translateX(-50%);
            background-color: #333;
            color: white;
            padding: 12px 24px;
            border-radius: 4px;
            z-index: 1000;
            display: none;
        }
        @media (max-width: 768px) {
            .file-grid {
                grid-template-columns: repeat(auto-fill, minmax(130px, 1fr));
            }
            .header {
                flex-direction: column;
                align-items: flex-start;
            }
            .btn-group {
                margin-top: 10px;
            }
        }
    </style>
</head>
<body>
    <div class="header">
        <h1>Gestionnaire de fichiers FTP</h1>
        <div class="btn-group">
            <button id="new-folder-btn" class="btn">Nouveau dossier</button>
            <button id="upload-btn" class="btn">Importer</button>
            <input type="file" id="file-input" style="display:none" multiple>
        </div>
    </div>

    <div class="path-nav" id="path-breadcrumb">
        / <span id="current-path"></span>
    </div>

    <div id="upload-area" class="upload-area">
        <p>Glissez et déposez vos fichiers ici ou cliquez pour sélectionner</p>
        <div id="upload-progress" class="progress-bar" style="display:none">
            <div class="progress" id="progress-bar"></div>
        </div>
    </div>

    <div class="file-grid" id="file-list">
        <!-- Liste de fichiers générée dynamiquement -->
    </div>

    <!-- Modals -->
    <div id="rename-modal" class="modal">
        <div class="modal-content">
            <h2>Renommer</h2>
            <input type="text" id="rename-input" style="width:100%;padding:8px;margin:10px 0">
            <div style="display:flex;justify-content:flex-end;gap:10px">
                <button class="btn" style="background-color:#ccc" onclick="closeModal('rename-modal')">Annuler</button>
                <button class="btn" id="rename-confirm">Renommer</button>
            </div>
        </div>
    </div>

    <div id="new-folder-modal" class="modal">
        <div class="modal-content">
            <h2>Nouveau dossier</h2>
            <input type="text" id="folder-name-input" style="width:100%;padding:8px;margin:10px 0" placeholder="Nom du dossier">
            <div style="display:flex;justify-content:flex-end;gap:10px">
                <button class="btn" style="background-color:#ccc" onclick="closeModal('new-folder-modal')">Annuler</button>
                <button class="btn" id="create-folder-confirm">Créer</button>
            </div>
        </div>
    </div>

    <div id="toast"></div>

    <script>
        // JavaScript pour l'interface utilisateur
        let currentPath = '';
        let selectedFile = null;

        // Initialisation
        document.addEventListener('DOMContentLoaded', () => {
            loadFiles('/');
            setupEventListeners();
        });

        function setupEventListeners() {
            // Bouton d'upload
            document.getElementById('upload-btn').addEventListener('click', () => {
                document.getElementById('file-input').click();
            });

            // Input de fichier
            document.getElementById('file-input').addEventListener('change', handleFileSelect);

            // Glisser-déposer pour l'upload
            const uploadArea = document.getElementById('upload-area');
            uploadArea.addEventListener('dragover', (e) => {
                e.preventDefault();
                uploadArea.classList.add('highlight');
            });
            
            uploadArea.addEventListener('dragleave', () => {
                uploadArea.classList.remove('highlight');
            });
            
            uploadArea.addEventListener('drop', (e) => {
                e.preventDefault();
                uploadArea.classList.remove('highlight');
                const files = e.dataTransfer.files;
                uploadFiles(files);
            });
            
            uploadArea.addEventListener('click', () => {
                document.getElementById('file-input').click();
            });

            // Bouton de création de dossier
            document.getElementById('new-folder-btn').addEventListener('click', () => {
                openModal('new-folder-modal');
            });

            // Confirmation de création de dossier
            document.getElementById('create-folder-confirm').addEventListener('click', createFolder);

            // Confirmation de renommage
            document.getElementById('rename-confirm').addEventListener('click', renameFile);
        }

        // Charger les fichiers du dossier
        async function loadFiles(path) {
            try {
                currentPath = path;
                document.getElementById('current-path').textContent = currentPath;
                
                const response = await fetch(`/api/files?path=${encodeURIComponent(path)}`);
                if (!response.ok) throw new Error('Erreur lors du chargement des fichiers');
                
                const files = await response.json();
                renderFiles(files);
            } catch (error) {
                showToast('Erreur: ' + error.message);
            }
        }

        // Afficher les fichiers
        function renderFiles(files) {
            const fileList = document.getElementById('file-list');
            fileList.innerHTML = '';
            
            // Ajouter le dossier parent sauf si on est à la racine
            if (currentPath !== '/') {
                const parentPath = currentPath.split('/').slice(0, -1).join('/') || '/';
                const parentItem = createFileItem({
                    name: '..',
                    is_directory: true,
                    size: 0,
                    modified_date: ''
                }, parentPath);
                fileList.appendChild(parentItem);
            }
            
            // Ajouter les dossiers en premier
            files.filter(f => f.is_directory).forEach(file => {
                const item = createFileItem(file);
                fileList.appendChild(item);
            });
            
            // Puis les fichiers
            files.filter(f => !f.is_directory).forEach(file => {
                const item = createFileItem(file);
                fileList.appendChild(item);
            });
        }

        // Créer un élément de fichier
        function createFileItem(file, customPath = null) {
            const item = document.createElement('div');
            item.className = 'file-item';
            
            const icon = getFileIcon(file);
            const path = customPath || (currentPath === '/' ? currentPath + file.name : currentPath + '/' + file.name);
            
            item.innerHTML = `
                <div class="file-icon ${icon.class}">${icon.symbol}</div>
                <div class="file-name">${file.name}</div>
                ${!file.is_directory ? `<div class="file-size">${formatFileSize(file.size)}</div>` : ''}
                ${file.name !== '..' ? `
                <div class="file-actions">
                    ${file.is_directory ? '' : '<button class="action-btn download-btn" title="Télécharger">⬇️</button>'}
                    <button class="action-btn rename-btn" title="Renommer">✏️</button>
                    <button class="action-btn delete-btn" title="Supprimer">🗑️</button>
                </div>` : ''}
            `;
            
            // Gérer le clic sur le fichier/dossier
            item.addEventListener('click', (e) => {
                // Si on a cliqué sur un bouton d'action, ne pas naviguer
                if (e.target.closest('.file-actions')) return;
                
                if (file.is_directory) {
                    loadFiles(path);
                } else {
                    window.open(path, '_blank');
                }
            });
            
            // Configurer les boutons d'action si présents
            if (file.name !== '..') {
                // Bouton de téléchargement
                const downloadBtn = item.querySelector('.download-btn');
                if (downloadBtn) {
                    downloadBtn.addEventListener('click', (e) => {
                        e.stopPropagation();
                        window.open(path, '_blank');
                    });
                }
                
                // Bouton de renommage
                const renameBtn = item.querySelector('.rename-btn');
                if (renameBtn) {
                    renameBtn.addEventListener('click', (e) => {
                        e.stopPropagation();
                        selectedFile = {
                            name: file.name,
                            path: path,
                            is_directory: file.is_directory
                        };
                        document.getElementById('rename-input').value = file.name;
                        openModal('rename-modal');
                    });
                }
                
                // Bouton de suppression
                const deleteBtn = item.querySelector('.delete-btn');
                if (deleteBtn) {
                    deleteBtn.addEventListener('click', (e) => {
                        e.stopPropagation();
                        if (confirm(`Êtes-vous sûr de vouloir supprimer "${file.name}" ?`)) {
                            deleteFile(path);
                        }
                    });
                }
            }
            
            return item;
        }

        // Obtenir l'icône appropriée pour un fichier
        function getFileIcon(file) {
            if (file.is_directory) {
                return { symbol: '📁', class: 'folder-icon' };
            }
            
            const ext = file.name.split('.').pop().toLowerCase();
            
            if (['jpg', 'jpeg', 'png', 'gif', 'bmp', 'svg'].includes(ext)) {
                return { symbol: '🖼️', class: 'image-icon' };
            } else if (['mp3', 'wav', 'ogg', 'flac', 'aac'].includes(ext)) {
                return { symbol: '🎵', class: 'audio-icon' };
            } else if (ext === 'pdf') {
                return { symbol: '📄', class: 'pdf-icon' };
            }
            
            return { symbol: '📄', class: 'generic-icon' };
        }

        // Formater la taille du fichier
        function formatFileSize(size) {
            if (size < 1024) return size + ' o';
            if (size < 1024 * 1024) return (size / 1024).toFixed(1) + ' Ko';
            if (size < 1024 * 1024 * 1024) return (size / (1024 * 1024)).toFixed(1) + ' Mo';
            return (size / (1024 * 1024 * 1024)).toFixed(1) + ' Go';
        }

        // Gérer la sélection de fichiers pour l'upload
        function handleFileSelect(e) {
            const files = e.target.files;
            if (files.length > 0) {
                uploadFiles(files);
            }
        }

        // Uploader des fichiers
        async function uploadFiles(files) {
            const progressBar = document.getElementById('progress-bar');
            const progressContainer = document.getElementById('upload-progress');
            progressContainer.style.display = 'block';
            
            for (let i = 0; i < files.length; i++) {
                const file = files[i];
                const formData = new FormData();
                formData.append('file', file);
                formData.append('path', currentPath);
                
                try {
                    const xhr = new XMLHttpRequest();
                    xhr.open('POST', '/api/upload', true);
                    
                    xhr.upload.onprogress = (e) => {
                        if (e.lengthComputable) {
                            const percentComplete = (e.loaded / e.total) * 100;
                            progressBar.style.width = percentComplete + '%';
                        }
                    };
                    
                    xhr.onload = () => {
                        if (xhr.status === 200) {
                            if (i === files.length - 1) {
                                showToast('Upload terminé avec succès');
                                progressContainer.style.display = 'none';
                                loadFiles(currentPath);
                            }
                        } else {
                            showToast('Erreur lors de l\'upload');
                            progressContainer.style.display = 'none';
                        }
                    };
                    
                    xhr.onerror = () => {
                        showToast('Erreur lors de l\'upload');
                        progressContainer.style.display = 'none';
                    };
                    
                    xhr.send(formData);
                    
                } catch (error) {
                    showToast('Erreur: ' + error.message);
                    progressContainer.style.display = 'none';
                }
            }
            
            // Réinitialiser l'input file
            document.getElementById('file-input').value = '';
        }

        // Supprimer un fichier
        async function deleteFile(path) {
            try {
                const response = await fetch('/api/delete', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify({ path })
                });
                
                if (!response.ok) throw new Error('Erreur lors de la suppression');
                
                showToast('Fichier supprimé avec succès');
                loadFiles(currentPath);
            } catch (error) {
                showToast('Erreur: ' + error.message);
            }
        }

        // Renommer un fichier
        async function renameFile() {
            const newName = document.getElementById('rename-input').value.trim();
            if (!newName) return;
            
            try {
                const newPath = currentPath === '/' 
                    ? currentPath + newName 
                    : currentPath + '/' + newName;
                
                const response = await fetch('/api/rename', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify({ 
                        old_path: selectedFile.path,
                        new_path: newPath
                    })
                });
                
                if (!response.ok) throw new Error('Erreur lors du renommage');
                
                showToast('Fichier renommé avec succès');
                closeModal('rename-modal');
                loadFiles(currentPath);
            } catch (error) {
                showToast('Erreur: ' + error.message);
            }
        }

        // Créer un nouveau dossier
        async function createFolder() {
            const folderName = document.getElementById('folder-name-input').value.trim();
            if (!folderName) return;
            
            try {
                const path = currentPath === '/' 
                    ? currentPath + folderName 
                    : currentPath + '/' + folderName;
                
                const response = await fetch('/api/mkdir', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify({ path })
                });
                
                if (!response.ok) throw new Error('Erreur lors de la création du dossier');
                
                showToast('Dossier créé avec succès');
                closeModal('new-folder-modal');
                loadFiles(currentPath);
            } catch (error) {
                showToast('Erreur: ' + error.message);
            }
        }

        // Ouvrir une modal
        function openModal(id) {
            document.getElementById(id).style.display = 'flex';
        }

        // Fermer une modal
        function closeModal(id) {
            document.getElementById(id).style.display = 'none';
        }

        // Afficher un toast
        function showToast(message) {
            const toast = document.getElementById('toast');
            toast.textContent = message;
            toast.style.display = 'block';
            
            setTimeout(() => {
                toast.style.display = 'none';
            }, 3000);
        }
    </script>
</body>
</html>
)=====";

void FTPHTTPProxy::setup() {
  ESP_LOGI(TAG, "Initialisation du proxy FTP/HTTP avec interface web");
  
  // Initialiser le système de fichiers SPIFFS pour stocker temporairement les fichiers téléchargés
  esp_vfs_spiffs_conf_t conf = {
    .base_path = "/spiffs",
    .partition_label = NULL,
    .max_files = 5,
    .format_if_mount_failed = true
  };
  
  esp_err_t ret = esp_vfs_spiffs_register(&conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Échec de l'initialisation SPIFFS (%s)", esp_err_to_name(ret));
  } else {
    ESP_LOGI(TAG, "SPIFFS initialisé avec succès");
  }
  
  this->setup_http_server();
}

void FTPHTTPProxy::loop() {
  // Rien à faire dans la boucle
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
  
  if (bytes_received <= 0 || !strstr(buffer, "230 ")) {
    ESP_LOGE(TAG, "Échec d'authentification FTP");
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  // Mode binaire
  send(sock_, "TYPE I\r\n", 8, 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';

  return true;
}

bool FTPHTTPProxy::send_ftp_command(const std::string &cmd, std::string &response) {
  if (sock_ < 0) {
    if (!connect_to_ftp()) {
      return false;
    }
  }
