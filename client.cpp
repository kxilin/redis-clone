#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include "utils.h"

static int32_t send_req(int fd, std::vector<std::string>& cmd) {
  uint32_t len = 4;
  for (const std::string& s : cmd) {
    len += 4 + s.size();
  }

  if (len > k_max_msg) {
    msg("too long");
    return -1;
  }

  struct Buffer wbuf;
  buf_init(&wbuf, 16 * 1024);

  buf_append(&wbuf, (const uint8_t*)&len, k_header_size);

  uint32_t n = (uint32_t)cmd.size();
  buf_append(&wbuf, (const uint8_t*)&n, k_header_size);

  for (const std::string& s : cmd) {
    uint32_t p = (uint32_t)s.size();
    buf_append(&wbuf, (const uint8_t*)&p, k_header_size);
    buf_append(&wbuf, (const uint8_t*)s.data(), s.size());
  }

  // Accessing the pointer directly via data_begin
  int32_t err = write_all(fd, wbuf.data_begin, buf_size(&wbuf));

  buf_destroy(&wbuf);
  return err;
}

static int32_t read_res(int fd) {
  // Protocol message header
  uint8_t header[k_header_size];
  errno = 0;
  int32_t err = read_all(fd, header, k_header_size);
  if (err) {
    msg(errno == 0 ? "EOF" : "read() error");
    return err;
  }

  uint32_t len = 0;
  memcpy(&len, header, k_header_size);
  if (len > k_max_msg) {
    msg("too long");
    return -1;
  }

  // Protocol message body
  struct Buffer rbuf;
  buf_init(&rbuf, len);

  // Read the body into the buffer
  // Note: read_all expects a raw pointer, so we pass rbuf.data_begin
  err = read_all(fd, rbuf.data_begin, len);
  if (err) {
    msg("read() error");
    buf_destroy(&rbuf);
    return err;
  }

  // Update the buffer's end pointer since we filled it manually via read_all
  rbuf.data_end = rbuf.data_begin + len;

  // Print the result
  if (len < 4) {
    msg("bad response");
    buf_destroy(&rbuf);
    return -1;
  }

  uint32_t rescode = 0;
  memcpy(&rescode, rbuf.data_begin, 4);
  printf("server says: [%u] %.*s\n", rescode, (int)(len - 4),
         rbuf.data_begin + 4);

  buf_destroy(&rbuf);
  return 0;
}

int main() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) die("socket()");

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(1234);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (connect(fd, (const struct sockaddr*)&addr, sizeof(addr)) < 0) {
    die("connect()");
  }

  std::vector<std::vector<std::string>> pipeline = {
      {"set", "k1", "v1"}, {"get", "k1"}, {"set", "k2", "v2"},
      {"get", "k2"},       {"del", "k1"}, {"get", "k1"}};

  printf("Sending %zu pipelined requests...\n", pipeline.size());
  for (auto& cmd : pipeline) {
    if (send_req(fd, cmd) != 0) {
      msg("send_req error");
      goto L_DONE;
    }
  }

  printf("Reading responses back...\n");
  for (size_t i = 0; i < pipeline.size(); i++) {
    printf("Response %zu: ", i + 1);
    if (read_res(fd) != 0) {
      msg("read_res error");
      goto L_DONE;
    }
  }

L_DONE:
  close(fd);
  return 0;
}
