#pragma once

#include <cstdint>
#include <ctime>

class DateTime {
  std::tm value_{};

public:
  explicit DateTime(uint32_t epoch) {
    const std::time_t raw = static_cast<std::time_t>(epoch);
    gmtime_r(&raw, &value_);
  }
  int hour() const { return value_.tm_hour; }
  int minute() const { return value_.tm_min; }
  int second() const { return value_.tm_sec; }
  int day() const { return value_.tm_mday; }
  int month() const { return value_.tm_mon + 1; }
  int year() const { return value_.tm_year + 1900; }
};

