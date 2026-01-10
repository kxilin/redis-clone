#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include "utils.h"

// one read and write
static int32_t one_request(int connfd) {
  // get the message length
  char rbuf[k_header_size + k_max_msg];
  errno = 0;
  int32_t err = read_all(connfd, rbuf, k_header_size);
  if (err) {
    msg(errno == 0 ? "EOF" : "read() error");
    return err;
  }

  uint32_t len = 0;
  memcpy(&len, rbuf, k_header_size); // assumes client and server same endianness
  if (len > k_max_msg) {
    msg("too long");
    return -1;
  }

  // get actual request
  err = read_all(connfd, rbuf + k_header_size, len);
  if (err) {
    msg(errno == 0 ? "EOF" : "read() error");
    return err;
  }

  // respond to request
  printf("client says: %.*s\n", len, rbuf + k_header_size);
  const char reply[] = "world";
  char wbuf[k_header_size + sizeof(reply)]; // wastes the last byte but at least no VLA
  len = (uint32_t)strlen(reply);
  memcpy(wbuf, &len, k_header_size);
  memcpy(wbuf + k_header_size, reply, len);

  return write_all(connfd, wbuf, k_header_size + len);
}

int main() {
  int fd = socket(AF_INET, SOCK_STREAM, 0); // get socket fd
  if (fd < 0) die("socket()");
  int val = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) { // socket option to allow same ip:port after restart
    die("setsockopt()");
  }

  struct sockaddr_in addr = {}; // address to bind to the socket that the server will listen on
  addr.sin_family = AF_INET;
  addr.sin_port = htons(1234);
  addr.sin_addr.s_addr = htonl(0);
  if (bind(fd, (const struct sockaddr*)&addr, sizeof(addr)) < 0) { // // binds the socket to this address
    die("bind()");
  }

  if (listen(fd, SOMAXCONN) < 0) { // server turns on and starts listening for incoming connections
    die("listen()");
  }

  // main loop for accepting connections
  while (true) {
    struct sockaddr_in client_addr = {}; // structure for client address
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr*)&client_addr, &addrlen);
    if (connfd < 0) continue; // error with connection, ignore

    while (true) { // handle more than one request from a client
      int32_t err = one_request(connfd);
      if (err) break;
    }
    close(connfd);
  }

  return 0;
}
