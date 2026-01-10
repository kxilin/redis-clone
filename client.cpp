#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include "utils.h"
#include <errno.h>

static int32_t query(int fd, const char* text) {
  uint32_t len = (uint32_t)strlen(text);
  if (len > k_max_msg) {
    msg("too long");
    return -1;
  }

  // send request
  char wbuf[k_header_size + k_max_msg];
  memcpy(wbuf, &len, k_header_size);
  memcpy(wbuf + k_header_size, text, len);
  int32_t err = write_all(fd, wbuf, k_header_size + len);
  if (err) {
    return err;
  }

  // receive request
  char rbuf[k_header_size + k_max_msg];
  errno = 0;
  err = read_all(fd, rbuf, k_header_size);
  if (err) {
    msg(errno == 0 ? "EOF" : "read() error");
    return err;
  }
  memcpy(&len, rbuf, k_header_size); // assume client and server same endianness
  if (len > k_max_msg) {
    msg("too long");
    return -1;
  }
  
  errno = 0;
  err = read_all(fd, rbuf + k_header_size, len);
  if (err) {
    msg("read() error");
    return err;
  }

  // process request
  printf("server says: %.*s\n", len, rbuf + k_header_size);
  return 0;
}

int main() {
  int fd = socket(AF_INET, SOCK_STREAM, 0); // get socket fd
  if (fd < 0) die("socket()");

  struct sockaddr_in addr = {}; // address of server to connect to
  addr.sin_family = AF_INET;
  addr.sin_port = htons(1234);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (const struct sockaddr*)&addr, sizeof(addr)) < 0) { // connect to the ip:port address
    die("connect()");
  }

  int32_t err = query(fd, "hello1");
  if (err) {
    goto L_DONE;
  }
  
  err = query(fd, "hello2");
  if (err) {
    goto L_DONE;
  }

L_DONE:
  close(fd);
  return 0;
}
