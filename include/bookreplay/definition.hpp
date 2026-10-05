#ifndef BOOKREPLAY_DEFINITION_HPP
#define BOOKREPLAY_DEFINITION_HPP

#include "bookreplay/dbn.hpp"
#include "bookreplay/dbn_reader.hpp"
#include "bookreplay/order.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bookreplay {

class DefinitionError : public BookreplayError {
 public:
  using BookreplayError::BookreplayError;
};

struct InstrumentLeg {
  std::uint16_t index = 0;
  std::uint32_t instrument_id = 0;
  /// The side taken on this leg when the spread is bought, not an aggressor
  /// side. Buying an NQ calendar sells the nearby, so its nearby leg is kAsk.
  Side side = Side::kNone;
  std::string raw_symbol;
};

struct InstrumentDefinition {
  std::uint32_t instrument_id = 0;
  std::string raw_symbol;
  std::string asset;
  std::string exchange;
  InstrumentClass instrument_class{};
  char match_algorithm = '\0';
  /// Raw rather than a `TickScale`, which has no empty state: a definition
  /// without an increment must still load, and Databento's XNAS.ITCH test
  /// vectors carry none.
  std::int64_t min_price_increment = kUndefPrice;
  /// Sticky once a later definition of a held instrument changes the increment:
  /// only the last definition is kept, and prices from before the change sit on
  /// the old grid.
  bool increment_changed = false;
  std::uint64_t expiration = kUndefTimestamp;
  std::uint64_t activation = kUndefTimestamp;
  std::uint64_t ts_event = 0;
  std::uint64_t ts_recv = 0;
  std::uint16_t leg_count = 0;
  /// Ascending by index. Shorter than `leg_count` until the send's last leg
  /// arrives, and for good if it never does.
  std::vector<InstrumentLeg> legs;

  [[nodiscard]] bool is_spread() const noexcept { return bookreplay::is_spread(instrument_class); }

  [[nodiscard]] bool has_all_legs() const noexcept { return legs.size() == leg_count; }

  [[nodiscard]] bool has_tick_size() const noexcept {
    return !increment_changed && min_price_increment > 0 && min_price_increment != kUndefPrice;
  }

  /// Throws DefinitionError unless `has_tick_size()`.
  [[nodiscard]] TickScale tick_scale() const;
};

struct InstrumentCatalogStats {
  std::uint64_t records = 0;
  std::uint64_t adds = 0;
  std::uint64_t modifies = 0;
  std::uint64_t deletions = 0;
  std::uint64_t skipped_other_schema = 0;
  std::uint64_t leg_records = 0;
  std::uint64_t replacements = 0;
  std::uint64_t class_leg_disagreements = 0;

  [[nodiscard]] bool reconciles() const noexcept {
    return records == adds + modifies + deletions + skipped_other_schema;
  }
};

/// A multi-leg instrument arrives as one record per leg under the spread's id,
/// in no promised order, so each leg is placed by `leg_index`. A leg record
/// joins the instrument only while it belongs to the same send, sharing its
/// timestamps and leg count, and brings a leg not yet held. Any other record
/// for a held id begins a new definition of it.
///
/// Outright or spread is the venue's own statement in `instrument_class`, never
/// inferred from `leg_count`. Where the two disagree the record is counted
/// rather than refused, so a product nobody has run this against still loads.
class InstrumentCatalog {
 public:
  /// Throws DefinitionError, changing nothing, on a byte outside its documented
  /// set or a leg index past the last leg. A delete is checked for its action
  /// alone. A legless record's leg fields are padding the venue need not fill,
  /// so they are not checked.
  void apply(const InstrumentDefMsg& rec);

  /// A record of any other type is counted in `skipped_other_schema`, not
  /// refused. `hd` must be 8-aligned with the whole record present behind it.
  void apply(const RecordHeader& hd);

  /// The pointer is good until the next `apply()`, which may replace or erase what it points to.
  [[nodiscard]] const InstrumentDefinition* find(std::uint32_t instrument_id) const noexcept;

  /// Throws DefinitionError, not std::out_of_range.
  [[nodiscard]] const InstrumentDefinition& at(std::uint32_t instrument_id) const;

  [[nodiscard]] std::size_t instrument_count() const noexcept { return definitions_.size(); }

  [[nodiscard]] std::size_t outright_count() const noexcept;

  [[nodiscard]] std::size_t spread_count() const noexcept;

  /// Ascending, so a report over them is stable across runs.
  [[nodiscard]] std::vector<std::uint32_t> instruments() const;

  /// Each instrument with a usable increment, and that increment, ascending by
  /// id: what a book that addresses its levels by tick needs up front.
  [[nodiscard]] std::vector<std::pair<std::uint32_t, std::int64_t>> tick_sizes() const;

  [[nodiscard]] const InstrumentCatalogStats& stats() const noexcept { return stats_; }

 private:
  void upsert(const InstrumentDefMsg& rec);

  std::unordered_map<std::uint32_t, InstrumentDefinition> definitions_;
  InstrumentCatalogStats stats_{};
};

/// Refuses definitions that close later than the data as well as earlier: the
/// catalog keeps each instrument's last definition, so one listed after the
/// data ends could replace the one in force. Two windows with no recorded end
/// count as closing together.
void require_definitions_for(const DbnMetadata& definitions, const DbnMetadata& data);

/// Refuses a stream whose metadata names another schema before reading a
/// record, so a wrong file fails instead of loading empty. A mixed-schema
/// stream loads its definitions and counts the rest.
[[nodiscard]] InstrumentCatalog read_definitions(DbnReader& reader);

[[nodiscard]] InstrumentCatalog read_definitions(const std::filesystem::path& path);

[[nodiscard]] InstrumentCatalog read_definitions(const std::byte* data, std::size_t size);

}  // namespace bookreplay

#endif  // BOOKREPLAY_DEFINITION_HPP
