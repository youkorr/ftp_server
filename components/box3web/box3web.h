#pragma once

#include <string>
#include "esphome/components/web_server_base/web_server_base.h"
#include "../sd_mmc_card/sd_mmc_card.h"
#include "esphome/core/component.h"  // Ajout de Component

namespace esphome {
namespace box3web {

class Path {
 public:
  static const char separator = '/';
  static std::string file_name(std::string const &path);
  static bool is_absolute(std::string const &path);
  static bool trailing_slash(std::string const &path);
  static std::string join(std::string const &first, std::string const &second);
  static std::string remove_root_path(std::string path, std::string const &root);
};

class Box3Web : public Component, public AsyncWebHandler {  // Héritage de Component
 public:
  Box3Web(web_server_base::WebServerBase *base);

  void setup() override;  // Méthode obligatoire
  void dump_config() override;  // Méthode obligatoire

  void set_url_prefix(std::string const &prefix);
  void set_root_path(std::string const &path);
  void set_sd_mmc_card(sd_mmc_card::SdMmc *card);

  void set_deletion_enabled(bool allow);
  void set_download_enabled(bool allow);
  void set_upload_enabled(bool allow);

  bool canHandle(AsyncWebServerRequest *request) override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleUpload(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data,
                    size_t len, bool final) override;
class StreamingFileResponse : public AsyncWebServerResponse {
private:
    sd_mmc_card::SdMmc* sd_card_;
    std::string file_path_;
    size_t file_size_;
    File file_;
    size_t sent_size_ = 0;
    const size_t CHUNK_SIZE = 4096; // Taille des morceaux à envoyer
    
public:
    StreamingFileResponse(sd_mmc_card::SdMmc* sd_card, const std::string& path, 
                          const String& contentType, size_t size) 
        : sd_card_(sd_card), file_path_(path), file_size_(size) {
        _contentType = contentType;
        _contentLength = size;
    }
    
    void stream_file(AsyncWebServerRequest *request) {
        request->send(this);
    }
    
    // Implémentation des méthodes virtuelles nécessaires
    virtual void _respond(AsyncWebServerRequest *request) {
        file_ = sd_card_->open_file(file_path_.c_str(), "r");
        _state = RESPONSE_HEADERS;
        _sendHeaders(request);
        _state = RESPONSE_CONTENT;
        request->client()->setRxTimeout(0);
        request->client()->setNoDelay(true);
        _ack(request, 0, 0);
    }
    
    virtual size_t _fillBuffer(uint8_t *buf, size_t maxLen) {
        size_t remaining = file_size_ - sent_size_;
        size_t to_send = std::min(maxLen, std::min(CHUNK_SIZE, remaining));
        
        if (to_send == 0) {
            return 0;
        }
        
        size_t read = sd_card_->read_file_data(file_, buf, to_send);
        sent_size_ += read;
        
        if (sent_size_ >= file_size_) {
            sd_card_->close_file(file_);
        }
        
        return read;
    }
    
    virtual bool _sourceValid() const {
        return true;
    }
};



class StreamingFileResponse {
 public:
  StreamingFileResponse(sd_mmc_card::SdMmc *sd_card, const std::string &path, const std::string &content_type, size_t file_size)
      : sd_card_(sd_card), path_(path), content_type_(content_type), file_size_(file_size) {}

  void stream_file(AsyncWebServerRequest *request) {
    auto *response = request->beginResponseStream(content_type_.c_str());

    response->addHeader("Content-Disposition", ("attachment; filename=\"" + path_ + "\"").c_str());
    response->addHeader("Accept-Ranges", "bytes");
    response->addHeader("Content-Length", std::to_string(file_size_).c_str());


    size_t index = 0;
    const size_t chunk_size = 8192;

    while (index < file_size_) {
        std::vector<uint8_t> chunk = this->sd_card_->read_file_chunked(path_, index, chunk_size);
        size_t read_size = chunk.size();

        if (read_size == 0) break;

        // Convertir les données en string et les envoyer
        response->print(std::string(chunk.begin(), chunk.end()));

        index += read_size;
        delay(10);
    }

    request->send(response);
}


 private:
  sd_mmc_card::SdMmc *sd_card_;
  std::string path_;
  std::string content_type_;
  size_t file_size_;
};

 private:
  web_server_base::WebServerBase *base_{nullptr};
  sd_mmc_card::SdMmc *sd_mmc_card_{nullptr};

  std::string url_prefix_{"box3web"};
  std::string root_path_{"/sdcard"};

  bool deletion_enabled_{true};
  bool download_enabled_{true};
  bool upload_enabled_{true};

  void handle_get(AsyncWebServerRequest *request) const;
  void handle_index(AsyncWebServerRequest *request, std::string const &path) const;
  void handle_download(AsyncWebServerRequest *request, std::string const &path) const;
  void handle_delete(AsyncWebServerRequest *request);

  void write_row(AsyncResponseStream *response, sd_mmc_card::FileInfo const &info) const;

  String get_content_type(const std::string &path) const;
  std::string build_prefix() const;
  std::string extract_path_from_url(std::string const &url) const;
  std::string build_absolute_path(std::string relative_path) const;

  const char *component_source_{nullptr};  // Variable pour set_component_source (facultatif)
};

}  // namespace box3web
}  // namespace esphome
