#include "bookreplay/definition.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "hex_byte.hpp"

namespace bookreplay {
namespace {

void require_definition_schema(const DbnMetadata& meta) {
  if (meta.schema && *meta.schema != kSchemaDefinition) {
    throw DefinitionError("expected the definition schema (" + std::to_string(kSchemaDefinition) +
                          "), found schema " + std::to_string(*meta.schema));
  }
}

[[nodiscard]] std::string instrument_label(std::uint32_t instrument_id,
                                           std::string_view raw_symbol) {
  return "instrument " + std::to_string(instrument_id) + " (" + std::string{raw_symbol} + ")";
}

[[nodiscard]] std::string end_of(const DbnMetadata& meta) {
  return meta.end ? std::to_string(*meta.end) : "no recorded end";
}

[[noreturn]] void refuse(const InstrumentDefMsg& rec, const std::string& what) {
  throw DefinitionError(instrument_label(rec.hd.instrument_id, cstr_view(rec.raw_symbol)) + " " +
                        what);
}

[[noreturn]] void refuse_byte(const InstrumentDefMsg& rec, std::string_view field, char byte) {
  refuse(rec, "carries " + std::string{field} + " " + hex_byte(static_cast<std::uint8_t>(byte)) +
                  ", outside the documented set");
}

[[nodiscard]] InstrumentDefinition definition_of(const InstrumentDefMsg& rec) {
  InstrumentDefinition def;
  def.instrument_id = rec.hd.instrument_id;
  def.raw_symbol = cstr_view(rec.raw_symbol);
  def.asset = cstr_view(rec.asset);
  def.exchange = cstr_view(rec.exchange);
  def.instrument_class = instrument_class_of(rec);
  def.match_algorithm = rec.match_algorithm;
  def.min_price_increment = rec.min_price_increment;
  def.expiration = rec.expiration;
  def.activation = rec.activation;
  def.ts_event = rec.hd.ts_event;
  def.ts_recv = rec.ts_recv;
  def.leg_count = rec.leg_count;
  return def;
}

[[nodiscard]] bool continues_send(const InstrumentDefinition& held, const InstrumentDefMsg& rec) {
  return rec.leg_count > 0 && held.leg_count == rec.leg_count && held.ts_event == rec.hd.ts_event &&
         held.ts_recv == rec.ts_recv &&
         std::none_of(held.legs.begin(), held.legs.end(),
                      [&rec](const InstrumentLeg& leg) { return leg.index == rec.leg_index; });
}

void place_leg(InstrumentDefinition& def, const InstrumentDefMsg& rec) {
  InstrumentLeg leg{rec.leg_index, rec.leg_instrument_id, static_cast<Side>(rec.leg_side),
                    std::string{cstr_view(rec.leg_raw_symbol)}};
  const auto later = std::ranges::lower_bound(def.legs, leg.index, {}, &InstrumentLeg::index);
  def.legs.insert(later, std::move(leg));
}

}  // namespace

TickScale InstrumentDefinition::tick_scale() const {
  if (has_tick_size()) {
    return TickScale{min_price_increment};
  }
  std::string why;
  if (increment_changed) {
    why = "the stream changed its min_price_increment";
  } else if (min_price_increment == kUndefPrice) {
    why = "its min_price_increment is undefined";
  } else {
    why = "its min_price_increment is " + std::to_string(min_price_increment);
  }
  throw DefinitionError(instrument_label(instrument_id, raw_symbol) + " has no tick size: " + why);
}

void InstrumentCatalog::apply(const InstrumentDefMsg& rec) {
  switch (rec.security_update_action) {
    case kSecurityUpdateAdd:
      upsert(rec);
      ++stats_.adds;
      break;
    case kSecurityUpdateModify:
      upsert(rec);
      ++stats_.modifies;
      break;
    case kSecurityUpdateDelete:
      definitions_.erase(rec.hd.instrument_id);
      ++stats_.deletions;
      break;
    default:
      refuse_byte(rec, "security_update_action", rec.security_update_action);
  }
  ++stats_.records;
}

void InstrumentCatalog::apply(const RecordHeader& hd) {
  if (const InstrumentDefMsg* rec = record_cast<InstrumentDefMsg>(hd)) {
    apply(*rec);
    return;
  }
  ++stats_.skipped_other_schema;
  ++stats_.records;
}

void InstrumentCatalog::upsert(const InstrumentDefMsg& rec) {
  const bool has_legs = rec.leg_count > 0;
  if (!is_known_instrument_class(rec.instrument_class)) {
    refuse_byte(rec, "instrument_class", rec.instrument_class);
  }
  if (has_legs && rec.leg_index >= rec.leg_count) {
    refuse(rec, "names leg index " + std::to_string(rec.leg_index) + " of a " +
                    std::to_string(rec.leg_count) + "-leg instrument");
  }
  if (has_legs && !is_known_side(rec.leg_side)) {
    refuse_byte(rec, "leg_side", rec.leg_side);
  }

  const InstrumentDefinition* held = find(rec.hd.instrument_id);
  const bool joins = held != nullptr && continues_send(*held, rec);
  const bool replaces = held != nullptr && !joins;
  InstrumentDefinition next = definition_of(rec);
  const bool class_disagrees = next.is_spread() != has_legs;
  if (held != nullptr) {
    next.increment_changed =
        held->increment_changed || held->min_price_increment != rec.min_price_increment;
  }
  if (joins) {
    next.legs = held->legs;
  }
  if (has_legs) {
    place_leg(next, rec);
  }
  definitions_.insert_or_assign(rec.hd.instrument_id, std::move(next));

  if (replaces) {
    ++stats_.replacements;
  }
  if (has_legs) {
    ++stats_.leg_records;
  }
  if (class_disagrees) {
    ++stats_.class_leg_disagreements;
  }
}

const InstrumentDefinition* InstrumentCatalog::find(std::uint32_t instrument_id) const noexcept {
  const auto it = definitions_.find(instrument_id);
  return it == definitions_.end() ? nullptr : &it->second;
}

const InstrumentDefinition& InstrumentCatalog::at(std::uint32_t instrument_id) const {
  const InstrumentDefinition* def = find(instrument_id);
  if (def == nullptr) {
    throw DefinitionError("no definition for instrument " + std::to_string(instrument_id));
  }
  return *def;
}

std::size_t InstrumentCatalog::spread_count() const noexcept {
  return static_cast<std::size_t>(
      std::count_if(definitions_.begin(), definitions_.end(),
                    [](const auto& entry) { return entry.second.is_spread(); }));
}

std::size_t InstrumentCatalog::outright_count() const noexcept {
  return definitions_.size() - spread_count();
}

std::vector<std::uint32_t> InstrumentCatalog::instruments() const {
  std::vector<std::uint32_t> out;
  out.reserve(definitions_.size());
  for (const auto& entry : definitions_) {
    out.push_back(entry.first);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::pair<std::uint32_t, std::int64_t>> InstrumentCatalog::tick_sizes() const {
  std::vector<std::pair<std::uint32_t, std::int64_t>> out;
  for (const std::uint32_t instrument_id : instruments()) {
    const InstrumentDefinition& def = definitions_.at(instrument_id);
    if (def.has_tick_size()) {
      out.emplace_back(instrument_id, def.min_price_increment);
    }
  }
  return out;
}

void require_definitions_for(const DbnMetadata& definitions, const DbnMetadata& data) {
  require_definition_schema(definitions);
  if (definitions.dataset != data.dataset) {
    throw DefinitionError("the definitions are from dataset " + definitions.dataset + ", not " +
                          data.dataset);
  }
  if (definitions.limit) {
    throw DefinitionError(
        "the definitions were requested with a record limit, so they may be incomplete");
  }
  if (definitions.start > data.start) {
    throw DefinitionError("the definitions start later than the data, at " +
                          std::to_string(definitions.start) + " against " +
                          std::to_string(data.start) +
                          ", so they miss what was listed when the data began");
  }
  const std::uint64_t definitions_end = definitions.end.value_or(kUndefTimestamp);
  const std::uint64_t data_end = data.end.value_or(kUndefTimestamp);
  if (definitions_end < data_end) {
    throw DefinitionError("the definitions end earlier than the data, at " + end_of(definitions) +
                          " against " + end_of(data) +
                          ", so they miss anything listed after they end");
  }
  if (definitions_end > data_end) {
    throw DefinitionError("the definitions end later than the data, at " + end_of(definitions) +
                          " against " + end_of(data) +
                          ", so a later definition could replace the one in force");
  }
}

InstrumentCatalog read_definitions(DbnReader& reader) {
  require_definition_schema(reader.metadata());
  InstrumentCatalog catalog;
  while (const RecordHeader* hd = reader.next()) {
    catalog.apply(*hd);
  }
  return catalog;
}

InstrumentCatalog read_definitions(const std::filesystem::path& path) {
  DbnReader reader{path};
  return read_definitions(reader);
}

InstrumentCatalog read_definitions(const std::byte* data, std::size_t size) {
  DbnReader reader{data, size};
  return read_definitions(reader);
}

}  // namespace bookreplay
