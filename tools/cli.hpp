// Argument parsing and report formatting shared by the command-line tools.
// Not installed: nothing outside tools/ includes it.

#ifndef BOOKREPLAY_TOOLS_CLI_HPP
#define BOOKREPLAY_TOOLS_CLI_HPP

#include "bookreplay/dbn.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

namespace bookreplay::tools {

inline bool parse_u64(const char* text, std::uint64_t& out) {
  if (text[0] == '-') {  // strtoull wraps a negative rather than refusing it
    return false;
  }
  errno = 0;
  char* end = nullptr;
  out = std::strtoull(text, &end, 10);
  return end != text && *end == '\0' && errno == 0;
}

inline bool parse_u32(const char* text, std::uint32_t& out) {
  std::uint64_t value = 0;
  if (!parse_u64(text, value) || value > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  out = static_cast<std::uint32_t>(value);
  return true;
}

inline void row(std::string& out, const char* key, std::uint64_t value) {
  out += key;
  out += '\t';
  out += std::to_string(value);
  out += '\n';
}

/// `HH:MM:SS.nnnnnnnnn` UTC. Which day it is, is the file name's job.
inline std::string time_of_day(std::uint64_t ts_ns) {
  const std::uint64_t second = ts_ns / 1'000'000'000;
  const std::uint64_t in_day = second % 86'400;
  std::array<char, 32> buf{};
  std::snprintf(buf.data(), buf.size(), "%02llu:%02llu:%02llu.%09llu",
                static_cast<unsigned long long>(in_day / 3600),
                static_cast<unsigned long long>((in_day / 60) % 60),
                static_cast<unsigned long long>(in_day % 60),
                static_cast<unsigned long long>(ts_ns % 1'000'000'000));
  return std::string{buf.data()};
}

/// Padding reads as a dash rather than as the sentinel's 9.2 quintillion.
inline std::string value_text(std::int64_t value) {
  return is_undef_price(value) ? std::string{"-"} : std::to_string(value);
}

inline void field(std::string& out, const std::string& text) {
  out += '\t';
  out += text;
}

inline void append_mbo(std::string& out, const MboMsg& rec) {
  field(out, std::to_string(rec.hd.instrument_id));
  field(out, std::string{rec.action});
  field(out, std::string{rec.side});
  field(out, std::to_string(rec.order_id));
  field(out, value_text(rec.price));
  field(out, std::to_string(rec.size));
  field(out, std::to_string(rec.flags));
  field(out, std::to_string(rec.sequence));
  field(out, time_of_day(rec.ts_recv));
}

}  // namespace bookreplay::tools

#endif  // BOOKREPLAY_TOOLS_CLI_HPP
