#pragma once

#include "esp_http_server.h"
#include "../sd_mmc_card/sd_mmc_card.h"
#include <string>
#include <vector>
#include "esphome/core/component.h"  // Ajout de Component

namespace esphome {
namespace box3web {

// Définir une taille de buffer par défaut
#ifndef ASYNC_RESPONSE_BUFFER_SIZE
#define ASYNC_RESPONSE_BUFFER_SIZE 1024
#endif

// Nouvelle classe pour gérer les réponses de fichiers en chunks
class FileResponse {
public:
    FileResponse(const char* path, const char* content_type, bool download, sd_mmc_card::SdMmc* card)
        : _path(path), _content_type(content_type), _download(download), _sd_mmc_card(card) {
        _file_size = _sd_mmc_card->get_file_size(path); // Implémentez cette méthode dans SdMmc
        _send_file_pos = 0;
    }

    ~FileResponse() {
        // Fermer les ressources si nécessaire
    }

    bool source_valid() const {
        return _sd_mmc_card->exists(_path); // Implémentez cette méthode dans SdMmc
    }

    esp_err_t respond(httpd_req_t *req) {
        if (!source_valid()) {
            return httpd_resp_send_404(req);
        }

        // Envoyer les en-têtes HTTP
        httpd_resp_set_type(req, _content_type);
        if (_download) {
            httpd_resp_set_hdr(req, "Content-Disposition", "attachment");
        }

        // Envoyer le contenu du fichier en chunks
        size_t remaining = _file_size;
        while (remaining > 0) {
            size_t bytes_to_read = (ASYNC_RESPONSE_BUFFER_SIZE < remaining) ? ASYNC_RESPONSE_BUFFER_SIZE : remaining;
            uint8_t buffer[ASYNC_RESPONSE_BUFFER_SIZE];
            size_t bytes_read = _sd_mmc_card->read_file_chunk(_path, buffer, bytes_to_read, _send_file_pos);

            if (bytes_read == 0) {
                break; // Fin du fichier
            }

            esp_err_t err = httpd_resp_send_chunk(req, reinterpret_cast<const char*>(buffer), bytes_read);
            if (err != ESP_OK) {
                return err; // Erreur lors de l'envoi
            }

            _send_file_pos += bytes_read;
            remaining -= bytes_read;
        }

        // Terminer la réponse
        return httpd_resp_send_chunk(req, nullptr, 0);
    }

private:
    std::string _path;
    const char* _content_type;
    bool _download;
    sd_mmc_card::SdMmc* _sd_mmc_card;
    size_t _file_size;
    size_t _send_file_pos;
};

// Réponse chunked pour ESP-IDF
class ChunkedResponse {
public:
    typedef std::function<size_t(uint8_t *buffer, size_t max_len, size_t index)> ChunkCallback;

    ChunkedResponse(const char* content_type, ChunkCallback callback, size_t chunk_size = 4096)
        : _content_type(content_type), _callback(callback), _chunk_index(0), _chunk_size(chunk_size) {}

    esp_err_t respond(httpd_req_t *req) {
        // Envoyer les en-têtes HTTP
        httpd_resp_set_type(req, _content_type);

        // Envoyer les données en chunks
        while (true) {
            uint8_t buffer[_chunk_size];
            size_t bytes_read = _callback(buffer, _chunk_size, _chunk_index);

            if (bytes_read == 0) {
                break; // Fin des données
            }

            esp_err_t err = httpd_resp_send_chunk(req, reinterpret_cast<const char*>(buffer), bytes_read);
            if (err != ESP_OK) {
                return err; // Erreur lors de l'envoi
            }

            _chunk_index += bytes_read;
        }

        // Terminer la réponse
        return httpd_resp_send_chunk(req, nullptr, 0);
    }

private:
    const char* _content_type;
    ChunkCallback _callback;
    size_t _chunk_index;
    size_t _chunk_size;
};

// Classe principale Box3Web
class Box3Web {
public:
    explicit Box3Web() : sd_mmc_card_(nullptr) {}

    void setup(httpd_handle_t server) {
        this->server_ = server;
        register_handlers();
    }

    void set_url_prefix(const std::string &prefix) {
        url_prefix_ = prefix;
    }

    void set_root_path(const std::string &path) {
        root_path_ = path;
    }

    void set_sd_mmc_card(sd_mmc_card::SdMmc *card) {
        sd_mmc_card_ = card;
    }

    void set_deletion_enabled(bool allow) {
        deletion_enabled_ = allow;
    }

    void set_download_enabled(bool allow) {
        download_enabled_ = allow;
    }

    void set_upload_enabled(bool allow) {
        upload_enabled_ = allow;
    }

protected:
    void register_handlers() {
        // Enregistrez les gestionnaires de routes ici
    }

    std::string extract_path_from_url(const std::string &url) const {
        return url.substr(url_prefix_.size());
    }

    std::string build_absolute_path(const std::string &relative_path) const {
        return root_path_ + relative_path;
    }

    httpd_handle_t server_;
    std::string url_prefix_ = "";
    std::string root_path_ = "/";
    sd_mmc_card::SdMmc *sd_mmc_card_{nullptr};
    bool deletion_enabled_ = false;
    bool download_enabled_ = true;
    bool upload_enabled_ = false;
};

}  // namespace box3web
}  // namespace esphome
