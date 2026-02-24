#include "utils.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>

void die(const char* msg) {
  int err = errno;
  fprintf(stderr, "[%d] %s\n", err, msg);
  abort();
}

void msg(const char* msg) { fprintf(stderr, "%s\n", msg); }

void fd_set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  flags |= O_NONBLOCK;
  fcntl(fd, F_SETFL, flags);

  if (errno) {
    die("fcntl() error");
  }
}

void buf_init(struct Buffer* buf, size_t capacity) {
  uint8_t* ptr = (uint8_t*)malloc(capacity);
  if (!ptr) die("malloc()");
  buf->buffer_begin = buf->data_begin = buf->data_end = ptr;
  buf->buffer_end = ptr + capacity;
}

void buf_destroy(struct Buffer* buf) {
  free(buf->buffer_begin);
  // Best practice: zero out pointers to prevent use-after-free
  memset(buf, 0, sizeof(*buf));
}

size_t buf_size(const struct Buffer* buf) {
  return buf->data_end - buf->data_begin;
}

size_t buf_free_space(const struct Buffer* buf) {
  return buf->buffer_end - buf->data_end;
}

void buf_append(struct Buffer* buf, const uint8_t* data, size_t len) {
  if (buf_free_space(buf) < len) {
    size_t data_size = buf_size(buf);
    size_t capacity = buf->buffer_end - buf->buffer_begin;

    if (capacity >= data_size + len) {
      // Case 1: Just slide data back to the start to make room
      memmove(buf->buffer_begin, buf->data_begin, data_size);
      buf->data_begin = buf->buffer_begin;
      buf->data_end = buf->buffer_begin + data_size;
    } else {
      // Case 2: Actually need more memory
      size_t new_capacity = capacity * 2 + len;
      uint8_t* new_ptr = (uint8_t*)realloc(buf->buffer_begin, new_capacity);
      if (!new_ptr) die("realloc()");

      // Re-adjust pointers relative to the new memory address
      buf->data_begin = new_ptr + (buf->data_begin - buf->buffer_begin);
      buf->data_end = new_ptr + (buf->data_end - buf->buffer_begin);
      buf->buffer_begin = new_ptr;
      buf->buffer_end = new_ptr + new_capacity;
    }
  }

  memcpy(buf->data_end, data, len);
  buf->data_end += len;
}

void buf_consume(struct Buffer* buf, size_t len) {
  buf->data_begin += len;
  // Optimization: if the buffer is empty, reset pointers to the start
  if (buf->data_begin == buf->data_end) {
    buf->data_begin = buf->data_end = buf->buffer_begin;
  }
}

int32_t read_all(int fd, uint8_t* buf, size_t n) {
  while (n > 0) {
    ssize_t bytes_read = read(fd, buf, n);
    if (bytes_read <= 0) {
      if (errno == EINTR) continue;  // interrupted by signal
      return -1;  // EOF (== 0) before n bytes sent is an error
    }
    n -= bytes_read;
    buf += bytes_read;
  }

  return 0;
}

int32_t write_all(int fd, uint8_t* buf, size_t n) {
  while (n > 0) {
    ssize_t bytes_written = write(fd, buf, n);
    if (bytes_written <= 0) {
      if (errno == EINTR) continue;
      return -1;
    };
    n -= bytes_written;
    buf += bytes_written;
  }

  return 0;
}
