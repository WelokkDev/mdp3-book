#ifndef BOOKREPLAY_SRC_HEX_BYTE_HPP
#define BOOKREPLAY_SRC_HEX_BYTE_HPP

#include <cstdint>
#include <string>

namespace bookreplay {

[[nodiscard]] inline std::string hex_byte(std::uint8_t v) {
  static constexpr char kDigits[] = "0123456789abcdef";
  return std::string{'0', 'x', kDigits[v >> 4], kDigits[v & 0x0F]};
}

}  // namespace bookreplay

#endif  // BOOKREPLAY_SRC_HEX_BYTE_HPP
