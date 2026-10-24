#ifndef BELL_TLS_SOCKET_H
#define BELL_TLS_SOCKET_H

#include <stdint.h>  // for uint8_t, uint16_t
#include <stdlib.h>  // for size_t
#include <string>    // for string

#include "BellSocket.h"  // for Socket

struct esp_tls;

namespace bell {
// ESP-IDF port: esp-tls, server certificates checked against the IDF bundle
class TLSSocket : public bell::Socket {
 private:
  struct esp_tls* tls = nullptr;

 public:
  TLSSocket() {}
  ~TLSSocket() { close(); };

  void open(const std::string& host, uint16_t port);

  size_t read(uint8_t* buf, size_t len);
  size_t write(uint8_t* buf, size_t len);
  size_t poll();
  bool isOpen();

  void close();
  int getFd();
};

}  // namespace bell

#endif
