#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#define DEC 10
#define HEX 16

class Print {
public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t) { return 1; }
  virtual size_t write(const uint8_t* data, size_t size) {
    size_t written = 0;
    for (size_t i = 0; i < size; ++i) written += write(data[i]);
    return written;
  }
  size_t write(const char* text) {
    return text ? write(reinterpret_cast<const uint8_t*>(text), std::strlen(text)) : 0;
  }
  size_t print(char value) { return write(static_cast<uint8_t>(value)); }
  size_t print(const char* value) { return write(value); }
  size_t print(const std::string& value) { return write(value.c_str()); }
  size_t print(int value, int base = DEC) { return printNumber(value, base); }
  size_t print(unsigned value, int base = DEC) { return printNumber(value, base); }
  size_t print(long value, int base = DEC) { return printNumber(value, base); }
  size_t print(unsigned long value, int base = DEC) { return printNumber(value, base); }
  size_t print(double value) {
    char buffer[48];
    const int length = std::snprintf(buffer, sizeof(buffer), "%g", value);
    return length > 0 ? write(reinterpret_cast<const uint8_t*>(buffer), static_cast<size_t>(length)) : 0;
  }
  size_t print(double value, int digits) {
    char buffer[64];
    const int length = std::snprintf(buffer, sizeof(buffer), "%.*f", digits, value);
    return length > 0 ? write(reinterpret_cast<const uint8_t*>(buffer), static_cast<size_t>(length)) : 0;
  }
  size_t println() { return write(static_cast<uint8_t>('\n')); }
  template <typename T> size_t println(const T& value) { return print(value) + println(); }
  size_t printf(const char* format, ...) __attribute__((format(printf, 2, 3)));

private:
  template <typename T> size_t printNumber(T value, int base) {
    char buffer[72];
    int length;
    if (base == HEX)
      length = std::snprintf(buffer, sizeof(buffer), "%llX", static_cast<unsigned long long>(value));
    else
      length = std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    return length > 0 ? write(reinterpret_cast<const uint8_t*>(buffer), static_cast<size_t>(length)) : 0;
  }
};

class Stream : public Print {
public:
  virtual int available() { return 0; }
  virtual int read() { return -1; }
  virtual int peek() { return -1; }
  virtual void flush() {}
  virtual size_t readBytes(uint8_t* buffer, size_t length) {
    size_t count = 0;
    while (count < length && available()) buffer[count++] = static_cast<uint8_t>(read());
    return count;
  }
};
