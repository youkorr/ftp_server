#pragma once

#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/sd_mmc_card/sd_mmc.h"
#include <string>
#include <vector>
#include "esphome/core/component.h"  // Ajout de Component


namespace esphome {
namespace box3web {

// Nouvelle classe pour gérer les réponses de fichiers en chunks
class FileResponse : public AsyncWebServerResponse {
public:
    FileResponse(const char* path, const char* contentType, bool download, sd_mmc_card::SdMmc* card)
        : _path(path), _sd_mmc_card(card) {
        _contentType = contentType;
        _download = download;
        _fileSize = card->get_file_size(path);
        _sendFilePos = 0;
    }
    
    ~FileResponse() {
        // Fermer les ressources si nécessaire
    }
    
    bool _sourceValid() const { return _sd_mmc_card->exists(_path); }
    void _respond(AsyncWebServerRequest *request) {
        _state = RESPONSE_HEADERS;
        String headers = _assembleHead(request->version());
        request->client()->write(headers.c_str(), headers.length());
        _state = RESPONSE_CONTENT;
        _fillBuffer();
        _flushBuffer();
    }
    
    size_t _fillBuffer() {
        size_t remaining = _fileSize - _sendFilePos;
        size_t bytesToRead = (ASYNC_RESPONSE_BUFFER_SIZE < remaining) ? ASYNC_RESPONSE_BUFFER_SIZE : remaining;
        
        if (bytesToRead > 0) {
            size_t bytesRead = _sd_mmc_card->read_file_chunk(_path, (uint8_t*)_content, bytesToRead, _sendFilePos);
            _sendFilePos += bytesRead;
            _contentLength = bytesRead;
            return bytesRead;
        }
        return 0;
    }
    
    size_t _readBufferIdx(uint8_t* data, size_t len) {
        return 0; // Non utilisé dans cette implémentation
    }
    
    size_t _readBuffer(uint8_t* data, size_t len) {
        size_t bytesToCopy = (len < _contentLength) ? len : _contentLength;
        memcpy(data, _content, bytesToCopy);
        _contentLength -= bytesToCopy;
        return bytesToCopy;
    }
    
private:
    std::string _path;
    sd_mmc_card::SdMmc* _sd_mmc_card;
    size_t _fileSize;
    size_t _sendFilePos;
    char _content[ASYNC_RESPONSE_BUFFER_SIZE];
};

// Cette classe est nécessaire pour ESP8266/Arduino
class ChunkedResponse : public AsyncWebServerResponse {
public:
    typedef std::function<size_t(uint8_t *buffer, size_t maxLen, size_t index)> ChunkCallback;
    
    ChunkedResponse(const String& contentType, ChunkCallback callback, size_t chunkSize = 4096)
        : _callback(callback), _chunkIndex(0), _chunkSize(chunkSize) {
        _contentType = contentType;
        _chunked = true;  // Activer le mode chunked
    }
    
    ~ChunkedResponse() {
        // Cleanup
    }
    
    void _respond(AsyncWebServerRequest *request) {
        _state = RESPONSE_HEADERS;
        String headers = _assembleHead(request->version());
        request->client()->write(headers.c_str(), headers.length());
        _state = RESPONSE_CONTENT;
        
        // Envoyer le premier chunk
        _sendChunk(request);
    }
    
    bool _sourceValid() const { return true; }
    
    void _sendChunk(AsyncWebServerRequest *request) {
        uint8_t *buffer = new uint8_t[_chunkSize];
        size_t bytesRead = _callback(buffer, _chunkSize, _chunkIndex);
        
        if (bytesRead > 0) {
            // Envoyer le chunk avec sa taille en hexadécimal
            char lenStr[10];
            sprintf(lenStr, "%x\r\n", bytesRead);
            request->client()->write(lenStr, strlen(lenStr));
            request->client()->write((char*)buffer, bytesRead);
            request->client()->write("\r\n", 2);
            
            _chunkIndex += bytesRead;
            
            // Programmer l'envoi du prochain chunk
            // Note: Ceci est une simplification, il faudrait idéalement utiliser un mécanisme asynchrone
            if (bytesRead == _chunkSize) {
                // Il y a probablement plus de données à envoyer
                _sendChunk(request);
            } else {
                // C'était le dernier chunk, envoyer un chunk vide pour terminer
                request->client()->write("0\r\n\r\n", 5);
                _state = RESPONSE_WAIT_ACK;
            }
        } else {
            // Aucune donnée lue, envoyer un chunk vide pour terminer
            request->client()->write("0\r\n\r\n", 5);
            _state = RESPONSE_WAIT_ACK;
        }
        
        delete[] buffer;
    }
    
private:
    ChunkCallback _callback;
    size_t _chunkIndex;
    size_t _chunkSize;
};

struct Path {
    static const char separator = '/';
    static std::string file_name(std::string const &path);
    static bool is_absolute(std::string const &path);
    static bool trailing_slash(std::string const &path);
    static std::string join(std::string const &first, std::string const &second);
    static std::string remove_root_path(std::string path, std::string const &root);
};

class Box3Web : public web_server_base::RequestHandler {
public:
    explicit Box3Web(web_server_base::WebServerBase *base);
    void setup();
    void dump_config();
    bool canHandle(AsyncWebServerRequest *request) override;
    void handleRequest(AsyncWebServerRequest *request) override;
    void handleUpload(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data,
                    size_t len, bool final) override;
    void set_url_prefix(std::string const &prefix);
    void set_root_path(std::string const &path);
    void set_sd_mmc_card(sd_mmc_card::SdMmc *card);
    void set_deletion_enabled(bool allow);
    void set_download_enabled(bool allow);
    void set_upload_enabled(bool allow);

protected:
    String get_content_type(const std::string &path) const;
    void handle_get(AsyncWebServerRequest *request) const;
    void handle_download(AsyncWebServerRequest *request, std::string const &path) const;
    void handle_delete(AsyncWebServerRequest *request);
    void handle_index(AsyncWebServerRequest *request, std::string const &path) const;
    void write_row(AsyncResponseStream *response, sd_mmc_card::FileInfo const &info) const;
    std::string build_prefix() const;
    std::string extract_path_from_url(std::string const &url) const;
    std::string build_absolute_path(std::string relative_path) const;

    web_server_base::WebServerBase *base_;
    std::string url_prefix_ = "";
    std::string root_path_ = "/";
    sd_mmc_card::SdMmc *sd_mmc_card_{nullptr};
    bool deletion_enabled_ = false;
    bool download_enabled_ = true;
    bool upload_enabled_ = false;
};

}  // namespace box3web
}  // namespace esphome
