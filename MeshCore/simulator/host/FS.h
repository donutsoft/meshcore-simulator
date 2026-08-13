#pragma once

#include "Stream.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs {

class FS;

class File : public Stream {
  struct State;
  std::shared_ptr<State> state_;

  explicit File(std::shared_ptr<State> state) : state_(std::move(state)) {}
  friend class FS;

public:
  File() = default;
  explicit operator bool() const;
  int available() override;
  int read() override;
  size_t read(uint8_t* destination, size_t length);
  size_t readBytes(uint8_t* destination, size_t length) override { return read(destination, length); }
  size_t write(uint8_t byte) override;
  size_t write(const uint8_t* source, size_t length) override;
  bool seek(size_t position);
  size_t size() const;
  void close();
  bool isDirectory() const;
  const char* name() const;
  File openNextFile();
};

class FS {
  friend class File;
  std::unordered_map<std::string, std::shared_ptr<std::vector<uint8_t>>> files_;
  std::unordered_set<std::string> directories_{"/"};
  std::string image_path_;

  static std::string normalise(const char* path);
  bool loadImage();
  bool persistImage() const;

public:
  virtual ~FS() = default;
  bool attachImage(const std::string& path);
  bool begin(bool = true) { return true; }
  File open(const char* path, const char* mode = "r", bool create = false);
  bool exists(const char* path) const;
  bool remove(const char* path);
  bool mkdir(const char* path);
  bool format();
  size_t usedBytes() const;
  size_t totalBytes() const { return 16 * 1024 * 1024; }
};

class SPIFFSFS : public FS {};

}  // namespace fs

using File = fs::File;
