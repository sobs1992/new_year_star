#include "TLSSocket.h"

#include <stdexcept>  // for runtime_error

#include "BellLogger.h"  // for AbstractLogger, BELL_LOG
#include "esp_crt_bundle.h"
#include "esp_tls.h"

/**
 * Platform TLSSocket implementation for esp-tls
 */
void bell::TLSSocket::open(const std::string& hostUrl, uint16_t port) {
  close();
  esp_tls_cfg_t cfg = {};
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 10000;

  tls = esp_tls_init();
  if (tls == nullptr) {
    throw std::runtime_error("esp_tls_init failed");
  }
  if (esp_tls_conn_new_sync(hostUrl.c_str(), hostUrl.size(), port, &cfg, tls) != 1) {
    BELL_LOG(error, "http_tls", "TLS connection to %s:%u failed", hostUrl.c_str(),
             port);
    close();
    throw std::runtime_error("TLS connection failed");
  }
}

size_t bell::TLSSocket::read(uint8_t* buf, size_t len) {
  return esp_tls_conn_read(tls, buf, len);
}

size_t bell::TLSSocket::write(uint8_t* buf, size_t len) {
  return esp_tls_conn_write(tls, buf, len);
}

size_t bell::TLSSocket::poll() {
  ssize_t avail = esp_tls_get_bytes_avail(tls);
  return avail > 0 ? avail : 0;
}

bool bell::TLSSocket::isOpen() {
  return tls != nullptr;
}

void bell::TLSSocket::close() {
  if (tls != nullptr) {
    esp_tls_conn_destroy(tls);
    tls = nullptr;
  }
}

int bell::TLSSocket::getFd() {
  int fd = -1;
  if (tls != nullptr) {
    esp_tls_get_conn_sockfd(tls, &fd);
  }
  return fd;
}
