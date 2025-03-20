#include "ftp_server.h"
#include "esphome/core/log.h"
#include <algorithm>

namespace esphome {
namespace ftp_server {

static const char *TAG = "ftp_server";

FTPServer::FTPServer() {
  buffer_.resize(512);  // Buffer de taille fixe pour éviter la fragmentation
}

void FTPServer::setup() {
  // Initialiser le serveur FTP
  ftp_server_ = new WiFiServer(port_);
  ftp_server_->begin();
  
  // Initialiser la carte SD si nécessaire
  if (!SD.begin()) {
    ESP_LOGE(TAG, "Impossible d'initialiser la carte SD");
  }
  
  ESP_LOGI(TAG, "Serveur FTP démarré sur le port %d", port_);
  ESP_LOGI(TAG, "Répertoire racine: %s", root_path_.c_str());
  
  // Définir le chemin courant par défaut
  current_path_ = root_path_;
}

void FTPServer::loop() {
  // Gérer les nouvelles connexions
  handle_new_clients();
  
  // Gérer les clients existants
  for (size_t i = 0; i < clients_.size(); i++) {
    if (clients_[i].connected()) {
      handle_ftp_client(clients_[i]);
    } else {
      ESP_LOGD(TAG, "Client déconnecté");
      clients_.erase(clients_.begin() + i);
      client_states_.erase(client_states_.begin() + i);
      client_usernames_.erase(client_usernames_.begin() + i);
      client_current_paths_.erase(client_current_paths_.begin() + i);
      i--;  // Ajuster l'index car nous avons supprimé un élément
    }
  }
}

void FTPServer::dump_config() {
  ESP_LOGCONFIG(TAG, "FTP Server:");
  ESP_LOGCONFIG(TAG, "  Port: %d", port_);
  ESP_LOGCONFIG(TAG, "  Root Path: %s", root_path_.c_str());
}

void FTPServer::handle_new_clients() {
  if (ftp_server_->hasClient()) {
    WiFiClient client = ftp_server_->available();
    ESP_LOGI(TAG, "Nouveau client FTP connecté: %s", client.remoteIP().toString().c_str());
    
    // Ajouter le client à la liste
    clients_.push_back(client);
    client_states_.push_back(FTP_WAIT_LOGIN);
    client_usernames_.push_back("");
    client_current_paths_.push_back(root_path_);
    
    // Envoyer un message de bienvenue
    send_response(client, 220, "Bienvenue sur le serveur FTP ESPHome");
  }
}

void FTPServer::handle_ftp_client(WiFiClient& client) {
  int index = -1;
  for (size_t i = 0; i < clients_.size(); i++) {
    if (clients_[i] == client) {
      index = i;
      break;
    }
  }
  
  if (index == -1) {
    return;  // Client non trouvé
  }
  
  if (client.available()) {
    // Lire la commande du client
    String cmd = client.readStringUntil('\n');
    cmd.trim();
    
    // Traiter la commande FTP
    switch (client_states_[index]) {
      case FTP_WAIT_LOGIN:
        if (cmd.startsWith("USER ")) {
          client_usernames_[index] = cmd.substring(5).c_str();
          client_states_[index] = FTP_WAIT_PASSWORD;
          send_response(client, 331, "Mot de passe requis pour " + client_usernames_[index]);
        } else {
          send_response(client, 530, "Veuillez vous connecter avec USER");
        }
        break;
        
      case FTP_WAIT_PASSWORD:
        if (cmd.startsWith("PASS ")) {
          std::string pass = cmd.substring(5).c_str();
          if (authenticate(client_usernames_[index], pass)) {
            client_states_[index] = FTP_LOGGED_IN;
            send_response(client, 230, "Connexion réussie");
          } else {
            client_states_[index] = FTP_WAIT_LOGIN;
            send_response(client, 530, "Identifiants incorrects");
          }
        } else {
          send_response(client, 530, "Veuillez entrer votre mot de passe avec PASS");
        }
        break;
        
      case FTP_LOGGED_IN:
      case FTP_WAIT_COMMAND:
        process_command(client, cmd.c_str());
        break;
        
      default:
        ESP_LOGW(TAG, "État FTP non géré: %d", client_states_[index]);
        break;
    }
  }
}

void FTPServer::process_command(WiFiClient& client, const std::string& command) {
  ESP_LOGD(TAG, "Commande FTP reçue: %s", command.c_str());
  
  // Implémenter les commandes FTP de base
  if (command.substr(0, 4) == "QUIT") {
    send_response(client, 221, "Au revoir");
    client.stop();
  } 
  else if (command.substr(0, 3) == "PWD") {
    send_response(client, 257, "\"" + current_path_ + "\" est le répertoire courant");
  }
  else if (command.substr(0, 4) == "TYPE") {
    send_response(client, 200, "Type défini à " + command.substr(5));
  }
  else if (command.substr(0, 4) == "PASV") {
    // Mode passif - Non implémenté dans cette version simplifiée
    send_response(client, 502, "Mode passif non supporté");
  }
  else if (command.substr(0, 4) == "LIST") {
    // Listage de répertoire - Version simplifiée
    send_response(client, 150, "Ouverture de la connexion de données pour le listage de répertoire");
    
    // Code simplifié pour le listage (en pratique, il faudrait utiliser une connexion de données)
    File root = SD.open(current_path_.c_str());
    if (root && root.isDirectory()) {
      std::string listing;
      File file = root.openNextFile();
      while (file) {
        listing += file.isDirectory() ? "d" : "-";
        listing += "rw-rw-rw- 1 user group ";
        listing += std::to_string(file.size()) + " ";
        listing += "Jan 01 00:00 ";
        listing += file.name();
        listing += "\r\n";
        file = root.openNextFile();
      }
      send_response(client, 226, "Transfert terminé");
      client.print(listing.c_str());
    } else {
      send_response(client, 550, "Échec de l'ouverture du répertoire");
    }
  }
  else if (command.substr(0, 4) == "STOR") {
    // Upload de fichier - Version simplifiée (non opérationnelle)
    send_response(client, 502, "Fonction non implémentée");
  }
  else if (command.substr(0, 4) == "RETR") {
    // Téléchargement de fichier - Version simplifiée (non opérationnelle)
    send_response(client, 502, "Fonction non implémentée");
  }
  else {
    // Commande non reconnue
    send_response(client, 502, "Commande non implémentée");
  }
}

void FTPServer::send_response(WiFiClient& client, int code, const std::string& message) {
  std::string response = std::to_string(code) + " " + message + "\r\n";
  client.print(response.c_str());
  ESP_LOGD(TAG, "Réponse FTP: %s", response.c_str());
}

bool FTPServer::authenticate(const std::string& username, const std::string& password) {
  return (username == username_ && password == password_);
}

}  // namespace ftp_server
}  // namespace esphome
