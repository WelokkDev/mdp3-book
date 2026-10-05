// The --book choice, and the definitions a tool reads for the fast book's
// ticks and for its own grid check.

#ifndef BOOKREPLAY_TOOLS_BOOKS_HPP
#define BOOKREPLAY_TOOLS_BOOKS_HPP

#include "bookreplay/dbn_reader.hpp"
#include "bookreplay/definition.hpp"
#include "bookreplay/fast_book.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

namespace bookreplay::tools {

enum class BookChoice : std::uint8_t { kReference, kFast };

inline bool parse_book(const char* text, BookChoice& out) {
  if (std::strcmp(text, "reference") == 0) {
    out = BookChoice::kReference;
    return true;
  }
  if (std::strcmp(text, "fast") == 0) {
    out = BookChoice::kFast;
    return true;
  }
  return false;
}

inline InstrumentCatalog read_catalog(const std::string& path, const DbnMetadata& data) {
  DbnReader reader{std::filesystem::path{path}};
  require_definitions_for(reader.metadata(), data);
  return read_definitions(reader);
}

inline constexpr const char* kFastNeedsDefinition =
    "the fast book places each price by its instrument's tick, which only --definition supplies";

/// An instrument listed without a usable increment is left out, so its first
/// order fails the replay instead of landing on a grid nobody published.
inline void set_tick_sizes(FastBook& book, const InstrumentCatalog& catalog) {
  for (const auto& [instrument_id, tick_size] : catalog.tick_sizes()) {
    book.set_tick_size(instrument_id, tick_size);
  }
}

}  // namespace bookreplay::tools

#endif  // BOOKREPLAY_TOOLS_BOOKS_HPP
