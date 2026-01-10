#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stddef.h>

constexpr size_t k_max_msg = 4096;
constexpr size_t k_header_size = 4;

void die(const char* msg);
void msg(const char* msg);
int32_t read_all(int fd, char* buf, size_t n);
int32_t write_all(int fd, const char* buf, size_t n);

#endif
