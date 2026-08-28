#include "bookreplay/dbn.hpp"
#include "bookreplay/dbn_reader.hpp"
#include "bookreplay/naked_window.hpp"
#include "bookreplay/order.hpp"
#include "bookreplay/replay.hpp"
#include "bookreplay/trade_source.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

using namespace bookreplay;

namespace {

using SharedTicks = std::shared_ptr<const std::vector<Tick>>;

class SharedTickSource final : public TradeSource {
 public:
  explicit SharedTickSource(SharedTicks ticks) : ticks_(std::move(ticks)) {}

  [[nodiscard]] const Tick* next() override {
    if (pos_ >= ticks_->size()) {
      return nullptr;
    }
    const Tick* tick = &(*ticks_)[pos_++];
    ++stats_.records;
    ++stats_.ticks;
    if (tick->aggressor == Side::kNone) {
      ++stats_.no_aggressor;
    }
    return tick;
  }

  [[nodiscard]] const TradeSourceStats& stats() const noexcept override { return stats_; }

 private:
  SharedTicks ticks_;
  std::size_t pos_ = 0;
  TradeSourceStats stats_{};
};

struct TickBuffer {
  SharedTicks ticks;
  TradeSourceStats stats;
};

TickBuffer load_ticks(const std::string& path, std::uint32_t instrument_id) {
  TradeSourceOptions opts;
  opts.instrument_id = instrument_id;

  auto owned = std::make_shared<std::vector<Tick>>();
  TradeSourceStats stats;
  {
    py::gil_scoped_release unlock;
    DbnTradeSource source{std::filesystem::path{path}, opts};
    *owned = collect(source);
    stats = source.stats();
  }
  return TickBuffer{owned, stats};
}

TickBuffer buffer_from(const std::vector<Tick>& ticks) {
  TradeSourceStats stats;
  stats.records = ticks.size();
  stats.ticks = ticks.size();
  std::int64_t last_ts = kNotStarted;
  for (const Tick& tick : ticks) {
    if (tick.ts < 0 || tick.ts < last_ts) {
      throw ReplayError("ticks must be non-negative and monotone in ts; record order is the queue");
    }
    last_ts = tick.ts;
    if (tick.size == 0) {
      throw ReplayError("a tick with zero size is not a print");
    }
    if (is_undef_price(tick.price)) {
      throw ReplayError("a tick with an undefined price is not a print");
    }
    if (tick.aggressor == Side::kNone) {
      ++stats.no_aggressor;
    }
  }
  return TickBuffer{std::make_shared<const std::vector<Tick>>(ticks), stats};
}

class PyReplay {
 public:
  PyReplay(std::unique_ptr<TradeSource> source, ReplayConfig config)
      : replay_(std::move(source), config) {}

  void submit(const Order& order) {
    refuse_while_advancing();
    replay_.submit(order);
  }

  void cancel(OrderId id) {
    refuse_while_advancing();
    replay_.cancel(id);
  }

  void cancel_at(OrderId id, std::int64_t request_ts_ns) {
    refuse_while_advancing();
    replay_.cancel_at(id, request_ts_ns);
  }

  std::vector<Fill> advance_to(std::int64_t ts_ns) {
    refuse_while_advancing();
    busy_ = true;
    std::vector<Fill> out;
    try {
      py::gil_scoped_release unlock;
      const std::span<const Fill> fills = replay_.advance_to(ts_ns);
      out.assign(fills.begin(), fills.end());
    } catch (...) {
      busy_ = false;
      throw;
    }
    busy_ = false;
    return out;
  }

  [[nodiscard]] std::int64_t now_ns() const {
    refuse_while_advancing();
    return replay_.now_ns();
  }

  [[nodiscard]] bool exhausted() const {
    refuse_while_advancing();
    return replay_.exhausted();
  }

  [[nodiscard]] OrderView order(OrderId id) const {
    refuse_while_advancing();
    return replay_.order(id);
  }

  [[nodiscard]] const ReplayStats& stats() const {
    refuse_while_advancing();
    return replay_.stats();
  }

  [[nodiscard]] const TradeSourceStats& source_stats() const {
    refuse_while_advancing();
    return replay_.source_stats();
  }

 private:
  // busy_ is only ever read or written under the GIL, so a plain bool suffices.
  void refuse_while_advancing() const {
    if (busy_) {
      throw ReplayError("replay is advancing on another thread; it is not thread-safe");
    }
  }

  Replay replay_;
  bool busy_ = false;
};

std::vector<NakedWindowResult> naked_window_scan(const TickBuffer& buffer,
                                                 const std::vector<NakedWindowQuery>& queries,
                                                 TickScale scale, std::size_t max_concurrent) {
  py::gil_scoped_release unlock;
  SharedTickSource source{buffer.ticks};
  NakedWindowScan scan{scale, max_concurrent};
  return scan.run(source, queries);
}

}  // namespace

PYBIND11_MODULE(bookreplay, m) {
  m.doc() = "Trade-print replay: submit, cancel, advance_to, fills.";

  py::register_exception<DbnError>(m, "DbnError");
  py::register_exception<ReplayError>(m, "ReplayError");

  py::enum_<Side>(m, "Side")
      .value("BID", Side::kBid)
      .value("ASK", Side::kAsk)
      .value("NONE", Side::kNone);

  py::enum_<OrderType>(m, "OrderType")
      .value("MARKET", OrderType::kMarket)
      .value("LIMIT", OrderType::kLimit)
      .value("STOP", OrderType::kStop)
      .value("STOP_LIMIT", OrderType::kStopLimit);

  py::enum_<LatencyClass>(m, "LatencyClass")
      .value("ORDER_ENTRY", LatencyClass::kOrderEntry)
      .value("PROTECTION_ARM", LatencyClass::kProtectionArm);

  py::enum_<OrderStatus>(m, "OrderStatus")
      .value("PENDING", OrderStatus::kPending)
      .value("LIVE", OrderStatus::kLive)
      .value("ELECTED", OrderStatus::kElected)
      .value("FILLED", OrderStatus::kFilled)
      .value("CANCELLED", OrderStatus::kCancelled)
      .value("OCO_CANCELLED", OrderStatus::kOcoCancelled);

  py::enum_<FillReason>(m, "FillReason")
      .value("MARKET", FillReason::kMarket)
      .value("LIMIT_THROUGH", FillReason::kLimitThrough)
      .value("STOP_ELECTED", FillReason::kStopElected)
      .value("STOP_LIMIT_THROUGH", FillReason::kStopLimitThrough);

  py::enum_<NoAggressorPolicy>(m, "NoAggressorPolicy")
      .value("NEITHER_SIDE", NoAggressorPolicy::kNeitherSide)
      .value("BOTH_SIDES", NoAggressorPolicy::kBothSides);

  py::enum_<FillSizePolicy>(m, "FillSizePolicy")
      .value("PRINT_CAPPED", FillSizePolicy::kPrintCapped)
      .value("FULL_REMAINING", FillSizePolicy::kFullRemaining);

  py::class_<Latency>(m, "Latency")
      .def(py::init<std::int64_t, std::int64_t, std::int64_t>(), py::kw_only(),
           py::arg("order_entry_ns"), py::arg("protection_arm_ns"), py::arg("cancel_ns"))
      .def_property_readonly("order_entry_ns", &Latency::order_entry_ns)
      .def_property_readonly("protection_arm_ns", &Latency::protection_arm_ns)
      .def_property_readonly("cancel_ns", &Latency::cancel_ns);

  py::class_<TickScale>(m, "TickScale")
      .def(py::init<std::int64_t>(), py::arg("tick_size"))
      .def_property_readonly("tick_size", &TickScale::tick_size)
      .def("to_price", &TickScale::to_price, py::arg("ticks"))
      .def("to_ticks", &TickScale::to_ticks, py::arg("price"));

  py::class_<Order>(m, "Order")
      .def(py::init([](OrderId id, OrderType type, Side side, std::uint32_t qty,
                       std::int64_t live_from_ns, std::int64_t trigger_ticks,
                       std::int64_t limit_ticks, OcoGroup oco_group, LatencyClass latency) {
             Order o;
             o.id = id;
             o.type = type;
             o.side = side;
             o.qty = qty;
             o.live_from_ns = live_from_ns;
             o.trigger_ticks = trigger_ticks;
             o.limit_ticks = limit_ticks;
             o.oco_group = oco_group;
             o.latency = latency;
             return o;
           }),
           py::kw_only(), py::arg("id"), py::arg("type"), py::arg("side"), py::arg("qty"),
           py::arg("live_from_ns"), py::arg("trigger_ticks") = 0, py::arg("limit_ticks") = 0,
           py::arg("oco_group") = kNoOcoGroup, py::arg("latency") = LatencyClass::kOrderEntry)
      .def_readwrite("id", &Order::id)
      .def_readwrite("type", &Order::type)
      .def_readwrite("side", &Order::side)
      .def_readwrite("qty", &Order::qty)
      .def_readwrite("trigger_ticks", &Order::trigger_ticks)
      .def_readwrite("limit_ticks", &Order::limit_ticks)
      .def_readwrite("live_from_ns", &Order::live_from_ns)
      .def_readwrite("oco_group", &Order::oco_group)
      .def_readwrite("latency", &Order::latency);

  py::class_<Fill>(m, "Fill")
      .def_readonly("seq", &Fill::seq)
      .def_readonly("order_id", &Fill::order_id)
      .def_readonly("oco_group", &Fill::oco_group)
      .def_readonly("ts_ns", &Fill::ts_ns)
      .def_readonly("ts_event", &Fill::ts_event)
      .def_readonly("price", &Fill::price)
      .def_readonly("price_ticks", &Fill::price_ticks)
      .def_readonly("qty", &Fill::qty)
      .def_readonly("remaining", &Fill::remaining)
      .def_readonly("print_size", &Fill::print_size)
      .def_readonly("sequence", &Fill::sequence)
      .def_readonly("side", &Fill::side)
      .def_readonly("aggressor", &Fill::aggressor)
      .def_readonly("reason", &Fill::reason);

  py::class_<Tick>(m, "Tick")
      .def(py::init([](std::int64_t ts, std::int64_t price, std::uint32_t size, Side aggressor,
                       std::int64_t ts_event, std::uint32_t instrument_id, std::uint32_t sequence) {
             Tick t;
             t.ts = ts;
             t.ts_event = ts_event == 0 ? ts : ts_event;
             t.price = price;
             t.size = size;
             t.instrument_id = instrument_id;
             t.sequence = sequence;
             t.aggressor = aggressor;
             return t;
           }),
           py::kw_only(), py::arg("ts"), py::arg("price"), py::arg("size"), py::arg("aggressor"),
           py::arg("ts_event") = 0, py::arg("instrument_id") = 0, py::arg("sequence") = 0)
      .def_readonly("ts", &Tick::ts)
      .def_readonly("ts_event", &Tick::ts_event)
      .def_readonly("price", &Tick::price)
      .def_readonly("size", &Tick::size)
      .def_readonly("instrument_id", &Tick::instrument_id)
      .def_readonly("sequence", &Tick::sequence)
      .def_readonly("aggressor", &Tick::aggressor);

  py::class_<OrderView>(m, "OrderView")
      .def_readonly("status", &OrderView::status)
      .def_readonly("type", &OrderView::type)
      .def_readonly("side", &OrderView::side)
      .def_readonly("oco_group", &OrderView::oco_group)
      .def_readonly("qty", &OrderView::qty)
      .def_readonly("remaining", &OrderView::remaining)
      .def_readonly("filled", &OrderView::filled)
      .def_readonly("oco_reduced", &OrderView::oco_reduced)
      .def_readonly("effective_live_ns", &OrderView::effective_live_ns)
      .def_readonly("effective_cancel_ns", &OrderView::effective_cancel_ns)
      .def_readonly("elected_ns", &OrderView::elected_ns)
      .def_readonly("trigger_price", &OrderView::trigger_price)
      .def_readonly("limit_price", &OrderView::limit_price);

  py::class_<TradeSourceStats>(m, "TradeSourceStats")
      .def_readonly("records", &TradeSourceStats::records)
      .def_readonly("ticks", &TradeSourceStats::ticks)
      .def_readonly("skipped_non_trade", &TradeSourceStats::skipped_non_trade)
      .def_readonly("skipped_other_instrument", &TradeSourceStats::skipped_other_instrument)
      .def_readonly("undef_price", &TradeSourceStats::undef_price)
      .def_readonly("zero_size", &TradeSourceStats::zero_size)
      .def_readonly("bad_ts_recv", &TradeSourceStats::bad_ts_recv)
      .def_readonly("no_aggressor", &TradeSourceStats::no_aggressor)
      .def_property_readonly("reconciles", &TradeSourceStats::reconciles);

  py::class_<ReplayStats>(m, "ReplayStats")
      .def_readonly("ticks", &ReplayStats::ticks)
      .def_readonly("no_aggressor_ticks", &ReplayStats::no_aggressor_ticks)
      .def_readonly("no_aggressor_passive_skips", &ReplayStats::no_aggressor_passive_skips)
      .def_readonly("elections", &ReplayStats::elections)
      .def_readonly("fills", &ReplayStats::fills)
      .def_readonly("partial_fills", &ReplayStats::partial_fills)
      .def_readonly("oco_reductions", &ReplayStats::oco_reductions)
      .def_readonly("oco_cancels", &ReplayStats::oco_cancels)
      .def_readonly("cancels_applied", &ReplayStats::cancels_applied)
      .def_readonly("late_arm_orders", &ReplayStats::late_arm_orders)
      .def_readonly("late_arm_ns_total", &ReplayStats::late_arm_ns_total)
      .def_readonly("reallocations", &ReplayStats::reallocations);

  py::class_<ReplayConfig>(m, "ReplayConfig")
      .def(py::init([](Latency latency, TickScale scale, NoAggressorPolicy no_aggressor,
                       FillSizePolicy fill_size, std::size_t reserve_live_orders,
                       std::size_t reserve_fills_per_advance) {
             ReplayConfig c{.latency = latency, .scale = scale};
             c.no_aggressor = no_aggressor;
             c.fill_size = fill_size;
             c.reserve_live_orders = reserve_live_orders;
             c.reserve_fills_per_advance = reserve_fills_per_advance;
             return c;
           }),
           py::kw_only(), py::arg("latency"), py::arg("scale"),
           py::arg("no_aggressor") = NoAggressorPolicy::kNeitherSide,
           py::arg("fill_size") = FillSizePolicy::kPrintCapped, py::arg("reserve_live_orders") = 64,
           py::arg("reserve_fills_per_advance") = 256)
      .def_readonly("latency", &ReplayConfig::latency)
      .def_readonly("scale", &ReplayConfig::scale)
      .def_readwrite("no_aggressor", &ReplayConfig::no_aggressor)
      .def_readwrite("fill_size", &ReplayConfig::fill_size)
      .def_readwrite("reserve_live_orders", &ReplayConfig::reserve_live_orders)
      .def_readwrite("reserve_fills_per_advance", &ReplayConfig::reserve_fills_per_advance);

  py::class_<TickBuffer>(m, "TickBuffer")
      .def("__len__", [](const TickBuffer& b) { return b.ticks->size(); })
      .def("__getitem__",
           [](const TickBuffer& b, std::ptrdiff_t i) {
             const auto size = static_cast<std::ptrdiff_t>(b.ticks->size());
             if (i < 0) {
               i += size;
             }
             if (i < 0 || i >= size) {
               throw py::index_error();
             }
             return (*b.ticks)[static_cast<std::size_t>(i)];
           })
      .def_readonly("stats", &TickBuffer::stats);

  py::class_<NakedWindowQuery>(m, "NakedWindowQuery")
      .def(py::init([](std::int64_t entry_fill_ts, std::int64_t window_ns, std::int64_t entry_price,
                       std::int64_t stop_price, Side position_side) {
             NakedWindowQuery q;
             q.entry_fill_ts = entry_fill_ts;
             q.window_ns = window_ns;
             q.entry_price = entry_price;
             q.stop_price = stop_price;
             q.position_side = position_side;
             return q;
           }),
           py::kw_only(), py::arg("entry_fill_ts"), py::arg("window_ns"), py::arg("entry_price"),
           py::arg("stop_price"), py::arg("position_side"))
      .def_readwrite("entry_fill_ts", &NakedWindowQuery::entry_fill_ts)
      .def_readwrite("window_ns", &NakedWindowQuery::window_ns)
      .def_readwrite("entry_price", &NakedWindowQuery::entry_price)
      .def_readwrite("stop_price", &NakedWindowQuery::stop_price)
      .def_readwrite("position_side", &NakedWindowQuery::position_side);

  py::class_<NakedWindowResult>(m, "NakedWindowResult")
      .def_readonly("ticks", &NakedWindowResult::ticks)
      .def_readonly("no_aggressor_ticks", &NakedWindowResult::no_aggressor_ticks)
      .def_readonly("volume", &NakedWindowResult::volume)
      .def_readonly("stop_reached", &NakedWindowResult::stop_reached)
      .def_readonly("first_reach_ts", &NakedWindowResult::first_reach_ts)
      .def_readonly("worst_price", &NakedWindowResult::worst_price)
      .def_readonly("mae_ticks", &NakedWindowResult::mae_ticks)
      .def_readonly("window_end_ns", &NakedWindowResult::window_end_ns);

  py::class_<PyReplay>(m, "Replay")
      .def(py::init([](const TickBuffer& buffer, ReplayConfig config) {
             return std::make_unique<PyReplay>(std::make_unique<SharedTickSource>(buffer.ticks),
                                               config);
           }),
           py::arg("ticks"), py::arg("config"))
      .def_static(
          "open_dbn",
          [](const std::string& path, ReplayConfig config, std::uint32_t instrument_id) {
            TradeSourceOptions opts;
            opts.instrument_id = instrument_id;
            return std::make_unique<PyReplay>(
                std::make_unique<DbnTradeSource>(std::filesystem::path{path}, opts), config);
          },
          py::arg("path"), py::arg("config"), py::kw_only(), py::arg("instrument_id"))
      .def("submit", &PyReplay::submit, py::arg("order"))
      .def("cancel", &PyReplay::cancel, py::arg("id"))
      .def("cancel_at", &PyReplay::cancel_at, py::arg("id"), py::arg("request_ts_ns"))
      .def("advance_to", &PyReplay::advance_to, py::arg("ts_ns"))
      .def("order", &PyReplay::order, py::arg("id"))
      .def_property_readonly("now_ns", &PyReplay::now_ns)
      .def_property_readonly("exhausted", &PyReplay::exhausted)
      .def_property_readonly("stats", &PyReplay::stats)
      .def_property_readonly("source_stats", &PyReplay::source_stats);

  m.def("load_ticks", &load_ticks, py::arg("path"), py::kw_only(), py::arg("instrument_id"),
        "Decode a trades or mbo file once into a shareable tick buffer, filtered to one "
        "instrument_id.");

  m.def("ticks_from", &buffer_from, py::arg("ticks"),
        "Wrap a list of Ticks as a buffer, for tests and synthetic streams.");

  m.def("naked_window_scan", &naked_window_scan, py::arg("ticks"), py::arg("queries"),
        py::arg("scale"), py::kw_only(), py::arg("max_concurrent") = 64,
        "Resolve every fill-to-protection window in one pass.");

  m.attr("NO_OCO_GROUP") = kNoOcoGroup;
  m.attr("ANY_INSTRUMENT") = kAnyInstrument;
  m.attr("NEVER_NS") = kNever;
}
