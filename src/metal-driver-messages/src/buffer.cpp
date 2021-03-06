#include <metal-driver-messages/buffer.hpp>

#include <stdio.h>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <cstdio>
#include <stdexcept>

namespace metal {

Buffer Buffer::createTempFileForSharedBuffer(bool writable) {
  char output_file_name[23] = "/tmp/metal-mmap-XXXXXX";
  int file = mkstemp(output_file_name);

  int res = ftruncate(
      file,
      2 * BufferSize);  // Use double-buffering: Allocate twice the BufferSize
  if (res != 0) {
    close(file);
    throw std::runtime_error("Failed to extend buffer file");
  }

  // Map it
  void *buffer = mmap(nullptr, 2 * BufferSize,
                      writable ? (PROT_READ | PROT_WRITE) : PROT_READ,
                      MAP_SHARED, file, 0);
  if (buffer == MAP_FAILED) {
    close(file);
    throw std::runtime_error("Failed to memory-map file");
  }

  return Buffer(std::string(output_file_name), file, buffer);
}

Buffer Buffer::mapSharedBuffer(std::string file_name, bool writable) {
  int file = open(file_name.c_str(), writable ? O_RDWR : O_RDONLY);
  if (file == -1) {
    throw std::runtime_error("Failed to open file");
  }

  void *buffer = mmap(nullptr, 2 * BufferSize,
                      writable ? PROT_WRITE : PROT_READ, MAP_SHARED, file, 0);
  if (buffer == MAP_FAILED) {
    close(file);
    throw std::runtime_error("Failed to memory-map file");
  }

  return Buffer(file_name, file, buffer);
}

Buffer::~Buffer() {
  if (_buffer) munmap(_buffer, size());

  if (_file) close(_file);
}

}  // namespace metal
