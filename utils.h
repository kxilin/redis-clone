#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <cstdint>
#include <vector>

constexpr size_t k_max_msg = 32 << 20;
constexpr size_t k_header_size = 4;

struct Buffer {
  uint8_t* buffer_begin;
  uint8_t* buffer_end;
  uint8_t* data_begin;
  uint8_t* data_end;
};

void buf_init(struct Buffer* buf, size_t capacity);
void buf_destroy(struct Buffer* buf);
void buf_clear(struct Buffer* buf);
size_t buf_size(const struct Buffer* buf);
size_t buf_free_space(const struct Buffer* buf);

void buf_append(struct Buffer* buf, const uint8_t* data, size_t len);
void buf_consume(struct Buffer* buf, size_t len);

void die(const char* msg);
void msg(const char* msg);
void fd_set_nonblock(int fd);
int32_t read_all(int fd, uint8_t* buf, size_t n);
int32_t write_all(int fd, uint8_t* buf, size_t n);
