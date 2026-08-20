#include "bookreplay/trade_source.hpp"

#include <utility>

namespace bookreplay {

namespace {

[[nodiscard]] bool wanted_instrument(std::uint32_t iid, const TradeSourceOptions& opts) noexcept {
  return opts.instrument_id == kAnyInstrument || opts.instrument_id == iid;
}

/// Databento delivers GLBX in ts_recv order, so a backward step means the
/// stream was reordered upstream.
void require_monotone(std::int64_t prev, std::int64_t ts) {
  if (prev != kNotStarted && ts < prev) {
    throw ReplayError("trade prints are not monotone in ts_recv; the stream was reordered");
  }
}

[[nodiscard]] bool usable_ts_recv(std::uint64_t ts_recv, std::uint8_t flags) noexcept {
  return (flags & kFlagBadTsRecv) == 0 && ts_recv != kUndefTimestamp &&
         ts_recv <= static_cast<std::uint64_t>(kNever);
}

/// DBN metadata schema ids.
constexpr std::uint16_t kSchemaMbo = 0;
constexpr std::uint16_t kSchemaTrades = 4;

/// A capture of mixed schemas carries no schema id in its metadata.
void require_trade_schema(const DbnMetadata& meta) {
  if (meta.schema && *meta.schema != kSchemaMbo && *meta.schema != kSchemaTrades) {
    throw ReplayError("file schema is neither trades nor mbo; the replay would be silently empty");
  }
}

template <typename Rec>
[[nodiscard]] bool normalize_impl(const Rec& rec, const TradeSourceOptions& opts, Tick& out,
                                  TradeSourceStats& stats) {
  ++stats.records;
  if (!is_known_action(rec.action)) {
    throw ReplayError("record carries an action byte outside the documented set");
  }
  if (static_cast<Action>(rec.action) != Action::kTrade) {
    ++stats.skipped_non_trade;
    return false;
  }
  if (!wanted_instrument(rec.hd.instrument_id, opts)) {
    ++stats.skipped_other_instrument;
    return false;
  }
  if (!is_known_side(rec.side)) {
    throw ReplayError("trade print carries a side byte outside the documented set");
  }
  if (is_undef_price(rec.price)) {
    ++stats.undef_price;
    return false;
  }
  if (rec.size == 0) {
    ++stats.zero_size;
    return false;
  }
  if (!usable_ts_recv(rec.ts_recv, rec.flags)) {
    ++stats.bad_ts_recv;
    return false;
  }

  out.ts = static_cast<std::int64_t>(rec.ts_recv);
  out.ts_event = static_cast<std::int64_t>(rec.hd.ts_event);
  out.price = rec.price;
  out.size = rec.size;
  out.instrument_id = rec.hd.instrument_id;
  out.sequence = rec.sequence;
  out.aggressor = static_cast<Side>(rec.side);
  out.flags = rec.flags;

  ++stats.ticks;
  if (out.aggressor == Side::kNone) {
    ++stats.no_aggressor;
  }
  return true;
}

}  // namespace

bool normalize(const TradeMsg& rec, const TradeSourceOptions& opts, Tick& out,
               TradeSourceStats& stats) {
  return normalize_impl(rec, opts, out, stats);
}

bool normalize(const MboMsg& rec, const TradeSourceOptions& opts, Tick& out,
               TradeSourceStats& stats) {
  return normalize_impl(rec, opts, out, stats);
}

TradeSource::~TradeSource() = default;

struct DbnTradeSource::Impl {
  explicit Impl(const std::filesystem::path& path, TradeSourceOptions o) : reader(path), opts(o) {
    require_trade_schema(reader.metadata());
  }

  Impl(const std::byte* data, std::size_t size, TradeSourceOptions o)
      : reader(data, size), opts(o) {
    require_trade_schema(reader.metadata());
  }

  DbnReader reader;
  TradeSourceOptions opts;
  TradeSourceStats stats{};
  Tick slot{};
  std::int64_t last_ts = kNotStarted;
};

DbnTradeSource::DbnTradeSource(const std::filesystem::path& path, TradeSourceOptions opts)
    : impl_(std::make_unique<Impl>(path, opts)) {}

DbnTradeSource::DbnTradeSource(const std::byte* data, std::size_t size, TradeSourceOptions opts)
    : impl_(std::make_unique<Impl>(data, size, opts)) {}

DbnTradeSource::~DbnTradeSource() = default;

const Tick* DbnTradeSource::next() {
  while (const RecordHeader* hd = impl_->reader.next()) {
    bool accepted = false;
    if (const TradeMsg* trade = record_cast<TradeMsg>(*hd)) {
      accepted = normalize(*trade, impl_->opts, impl_->slot, impl_->stats);
    } else if (const MboMsg* mbo = record_cast<MboMsg>(*hd)) {
      accepted = normalize(*mbo, impl_->opts, impl_->slot, impl_->stats);
    } else {
      ++impl_->stats.records;
      ++impl_->stats.skipped_non_trade;
    }
    if (!accepted) {
      continue;
    }
    require_monotone(impl_->last_ts, impl_->slot.ts);
    impl_->last_ts = impl_->slot.ts;
    return &impl_->slot;
  }
  return nullptr;
}

const TradeSourceStats& DbnTradeSource::stats() const noexcept {
  return impl_->stats;
}

const DbnMetadata& DbnTradeSource::metadata() const noexcept {
  return impl_->reader.metadata();
}

struct RecordTradeSource::Impl {
  std::vector<TradeMsg> trades;
  std::vector<MboMsg> mbo;
  TradeSourceOptions opts;
  TradeSourceStats stats{};
  Tick slot{};
  std::size_t pos = 0;
  std::int64_t last_ts = kNotStarted;
};

RecordTradeSource::RecordTradeSource(std::vector<TradeMsg> records, TradeSourceOptions opts)
    : impl_(std::make_unique<Impl>()) {
  impl_->trades = std::move(records);
  impl_->opts = opts;
}

RecordTradeSource::RecordTradeSource(std::vector<MboMsg> records, TradeSourceOptions opts)
    : impl_(std::make_unique<Impl>()) {
  impl_->mbo = std::move(records);
  impl_->opts = opts;
}

RecordTradeSource::~RecordTradeSource() = default;

const Tick* RecordTradeSource::next() {
  const std::size_t count = impl_->trades.empty() ? impl_->mbo.size() : impl_->trades.size();
  while (impl_->pos < count) {
    const std::size_t at = impl_->pos++;
    const bool accepted =
        impl_->trades.empty()
            ? normalize(impl_->mbo[at], impl_->opts, impl_->slot, impl_->stats)
            : normalize(impl_->trades[at], impl_->opts, impl_->slot, impl_->stats);
    if (!accepted) {
      continue;
    }
    require_monotone(impl_->last_ts, impl_->slot.ts);
    impl_->last_ts = impl_->slot.ts;
    return &impl_->slot;
  }
  return nullptr;
}

const TradeSourceStats& RecordTradeSource::stats() const noexcept {
  return impl_->stats;
}

TickSpanSource::TickSpanSource(std::span<const Tick> ticks) : ticks_(ticks) {}

TickSpanSource::~TickSpanSource() = default;

const Tick* TickSpanSource::next() {
  if (pos_ >= ticks_.size()) {
    return nullptr;
  }
  const Tick* tick = &ticks_[pos_++];
  require_monotone(last_ts_, tick->ts);
  last_ts_ = tick->ts;
  ++stats_.records;
  ++stats_.ticks;
  if (tick->aggressor == Side::kNone) {
    ++stats_.no_aggressor;
  }
  return tick;
}

const TradeSourceStats& TickSpanSource::stats() const noexcept {
  return stats_;
}

void TickSpanSource::rewind() noexcept {
  pos_ = 0;
  last_ts_ = kNotStarted;
  stats_ = TradeSourceStats{};
}

std::vector<Tick> collect(TradeSource& source) {
  std::vector<Tick> out;
  while (const Tick* tick = source.next()) {
    out.push_back(*tick);
  }
  return out;
}

}  // namespace bookreplay
