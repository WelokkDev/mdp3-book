#include "bookreplay/naked_window.hpp"
#include "bookreplay/order.hpp"
#include "bookreplay/replay.hpp"
#include "bookreplay/trade_source.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace bookreplay;

struct Options {
  std::string path;
  std::string orders_path;
  std::int64_t tick_size = 0;
  std::int64_t entry_ns = -1;
  std::int64_t arm_ns = -1;
  std::int64_t cancel_ns = -1;
  std::int64_t stop_ticks = 0;
  std::uint32_t instrument_id = kAnyInstrument;
  std::size_t reserve_fills = 4096;
  std::vector<std::int64_t> sweep;
  bool naked_window = false;
  bool stats = false;
  bool full_remaining = false;
  bool both_sides = false;
};

int usage() {
  std::fprintf(
      stderr,
      "usage: replay_demo FILE.dbn[.zst] --tick-size N --entry-ns N --arm-ns N\n"
      "                   --cancel-ns N [--instrument N] [--orders FILE.tsv]\n"
      "                   [--reserve-fills N] [--full-remaining] [--both-sides] [--stats]\n"
      "                   [--sweep-entry-ns a,b,c | --naked-window --stop-ticks N]\n"
      "\n"
      "order script columns, whitespace separated, '#' comments:\n"
      "  live_from_ns id type side qty [trigger_ticks [limit_ticks [oco [latency]]]]\n"
      "  type    market|limit|stop|stop_limit\n"
      "  side    bid|ask\n"
      "  latency entry|arm, default entry\n");
  return 2;
}

bool parse_i64(const char* text, std::int64_t& out) {
  errno = 0;
  char* end = nullptr;
  out = std::strtoll(text, &end, 10);
  return end != text && *end == '\0' && errno == 0;
}

bool parse_u64(const char* text, std::uint64_t& out) {
  if (text[0] == '-') {  // strtoull wraps a negative rather than refusing it
    return false;
  }
  errno = 0;
  char* end = nullptr;
  out = std::strtoull(text, &end, 10);
  return end != text && *end == '\0' && errno == 0;
}

bool parse_u32(const char* text, std::uint32_t& out) {
  std::uint64_t value = 0;
  if (!parse_u64(text, value) || value > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  out = static_cast<std::uint32_t>(value);
  return true;
}

bool parse_list(const char* text, std::vector<std::int64_t>& out) {
  std::stringstream stream{text};
  std::string item;
  while (std::getline(stream, item, ',')) {
    std::int64_t value = 0;
    if (!parse_i64(item.c_str(), value)) {
      return false;
    }
    out.push_back(value);
  }
  return !out.empty();
}

bool parse_type(const std::string& text, OrderType& out) {
  if (text == "market") {
    out = OrderType::kMarket;
  } else if (text == "limit") {
    out = OrderType::kLimit;
  } else if (text == "stop") {
    out = OrderType::kStop;
  } else if (text == "stop_limit") {
    out = OrderType::kStopLimit;
  } else {
    return false;
  }
  return true;
}

bool parse_side(const std::string& text, Side& out) {
  if (text == "bid") {
    out = Side::kBid;
  } else if (text == "ask") {
    out = Side::kAsk;
  } else {
    return false;
  }
  return true;
}

bool parse_latency(const std::string& text, LatencyClass& out) {
  if (text == "entry") {
    out = LatencyClass::kOrderEntry;
  } else if (text == "arm") {
    out = LatencyClass::kProtectionArm;
  } else {
    return false;
  }
  return true;
}

[[noreturn]] void bad_line(std::size_t line_no, const std::string& what) {
  throw ReplayError("order script line " + std::to_string(line_no) + ": " + what);
}

std::vector<std::string> split_fields(const std::string& line) {
  std::istringstream stream{line};
  std::vector<std::string> fields;
  std::string field;
  while (stream >> field) {
    fields.push_back(field);
  }
  return fields;
}

std::vector<Order> read_orders(const std::string& path) {
  std::ifstream file{path};
  if (!file) {
    throw ReplayError("cannot open the order script");
  }

  std::vector<Order> orders;
  std::string line;
  std::size_t line_no = 0;
  while (std::getline(file, line)) {
    ++line_no;
    const std::size_t hash = line.find('#');
    if (hash != std::string::npos) {
      line.resize(hash);
    }

    const std::vector<std::string> fields = split_fields(line);
    if (fields.empty()) {
      continue;
    }
    if (fields.size() < 5 || fields.size() > 9) {
      bad_line(line_no, "needs five to nine fields, found " + std::to_string(fields.size()));
    }

    Order o;
    if (!parse_i64(fields[0].c_str(), o.live_from_ns)) {
      bad_line(line_no, "live_from_ns is not an integer");
    }
    if (!parse_u64(fields[1].c_str(), o.id)) {
      bad_line(line_no, "id is not an unsigned integer");
    }
    if (!parse_type(fields[2], o.type)) {
      bad_line(line_no, "unknown order type '" + fields[2] + "'");
    }
    if (!parse_side(fields[3], o.side)) {
      bad_line(line_no, "unknown side '" + fields[3] + "'");
    }
    if (!parse_u32(fields[4].c_str(), o.qty)) {
      bad_line(line_no, "qty is not an unsigned 32-bit integer");
    }
    if (fields.size() > 5 && !parse_i64(fields[5].c_str(), o.trigger_ticks)) {
      bad_line(line_no, "trigger_ticks is not an integer");
    }
    if (fields.size() > 6 && !parse_i64(fields[6].c_str(), o.limit_ticks)) {
      bad_line(line_no, "limit_ticks is not an integer");
    }
    if (fields.size() > 7 && !parse_u32(fields[7].c_str(), o.oco_group)) {
      bad_line(line_no, "oco is not an unsigned 32-bit integer");
    }
    if (fields.size() > 8 && !parse_latency(fields[8], o.latency)) {
      bad_line(line_no, "latency is neither 'entry' nor 'arm'");
    }
    orders.push_back(o);
  }
  return orders;
}

std::unique_ptr<TradeSource> open_source(const Options& opts) {
  TradeSourceOptions source_opts;
  source_opts.instrument_id = opts.instrument_id;
  return std::make_unique<DbnTradeSource>(std::filesystem::path{opts.path}, source_opts);
}

ReplayConfig make_config(const Options& opts, std::int64_t entry_ns) {
  ReplayConfig config{.latency = Latency{entry_ns, opts.arm_ns, opts.cancel_ns},
                      .scale = TickScale{opts.tick_size}};
  config.reserve_fills_per_advance = opts.reserve_fills;
  if (opts.full_remaining) {
    config.fill_size = FillSizePolicy::kFullRemaining;
  }
  if (opts.both_sides) {
    config.no_aggressor = NoAggressorPolicy::kBothSides;
  }
  return config;
}

void print_fill_header() {
  std::printf(
      "seq\torder_id\toco\tts_ns\tts_event\tprice\tprice_ticks\tqty\tremaining\tprint_size\t"
      "sequence\tside\taggressor\treason\n");
}

void print_fill(const Fill& f) {
  std::printf("%llu\t%llu\t%u\t%lld\t%lld\t%lld\t%lld\t%u\t%u\t%u\t%u\t%s\t%s\t%s\n",
              static_cast<unsigned long long>(f.seq), static_cast<unsigned long long>(f.order_id),
              f.oco_group, static_cast<long long>(f.ts_ns), static_cast<long long>(f.ts_event),
              static_cast<long long>(f.price), static_cast<long long>(f.price_ticks), f.qty,
              f.remaining, f.print_size, f.sequence, side_name(f.side), side_name(f.aggressor),
              fill_reason_name(f.reason));
}

void stat_row(const char* label, const char* name, std::uint64_t value) {
  std::fprintf(stderr, "%s%s\t%llu\n", label, name, static_cast<unsigned long long>(value));
}

/// Counters go to stderr so stdout carries exactly one table, whichever the
/// report mode is.
void print_source_stats(const TradeSourceStats& s, const char* label) {
  const auto row = [label](const char* name, std::uint64_t value) { stat_row(label, name, value); };
  row("records", s.records);
  row("ticks", s.ticks);
  row("skipped_non_trade", s.skipped_non_trade);
  row("skipped_other_instrument", s.skipped_other_instrument);
  row("undef_price", s.undef_price);
  row("zero_size", s.zero_size);
  row("bad_ts_recv", s.bad_ts_recv);
  row("no_aggressor", s.no_aggressor);
  row("source_reconciles", s.reconciles());
}

void print_replay_stats(const ReplayStats& r, const char* label) {
  const auto row = [label](const char* name, std::uint64_t value) { stat_row(label, name, value); };
  row("elections", r.elections);
  row("stop_entry_rejects", r.stop_entry_rejects);
  row("fills", r.fills);
  row("partial_fills", r.partial_fills);
  row("oco_reductions", r.oco_reductions);
  row("oco_cancels", r.oco_cancels);
  row("cancels_applied", r.cancels_applied);
  row("no_aggressor_passive_skips", r.no_aggressor_passive_skips);
  row("tick_charged_qty", r.tick_charged_qty);
  row("late_arm_orders", r.late_arm_orders);
  row("reallocations", r.reallocations);
}

std::vector<Fill> run_once(const Options& opts, std::int64_t entry_ns,
                           std::unique_ptr<TradeSource> source, const std::vector<Order>& orders,
                           bool report_stats) {
  Replay replay{std::move(source), make_config(opts, entry_ns)};
  for (const Order& o : orders) {
    replay.submit(o);
  }
  const std::span<const Fill> fills = replay.advance_to(kNever);
  const std::vector<Fill> out{fills.begin(), fills.end()};
  if (report_stats) {
    print_source_stats(replay.source_stats(), "");
    print_replay_stats(replay.stats(), "");
  }
  return out;
}

int run_sweep(const Options& opts, const std::vector<Order>& orders) {
  std::unique_ptr<TradeSource> source = open_source(opts);
  const std::vector<Tick> day = collect(*source);
  std::fprintf(stderr, "decoded %zu prints once for %zu latency points\n", day.size(),
               opts.sweep.size());
  if (opts.stats) {
    print_source_stats(source->stats(), "");
  }

  std::printf("entry_ns\tfills\tfilled_qty\tfirst_fill_ts\tlast_fill_ts\n");
  for (const std::int64_t entry_ns : opts.sweep) {
    Replay replay{std::make_unique<TickSpanSource>(day), make_config(opts, entry_ns)};
    for (const Order& o : orders) {
      replay.submit(o);
    }
    const std::span<const Fill> fills = replay.advance_to(kNever);

    std::uint64_t qty = 0;
    for (const Fill& f : fills) {
      qty += f.qty;
    }
    std::printf("%lld\t%zu\t%llu\t%lld\t%lld\n", static_cast<long long>(entry_ns), fills.size(),
                static_cast<unsigned long long>(qty),
                static_cast<long long>(fills.empty() ? 0 : fills.front().ts_ns),
                static_cast<long long>(fills.empty() ? 0 : fills.back().ts_ns));
    if (opts.stats) {
      const std::string label = std::to_string(entry_ns) + "\t";
      print_replay_stats(replay.stats(), label.c_str());
    }
  }
  return 0;
}

int run_naked_window(const Options& opts, const std::vector<Fill>& fills) {
  std::vector<NakedWindowQuery> queries;
  queries.reserve(fills.size());
  for (const Fill& f : fills) {
    NakedWindowQuery q;
    q.entry_fill_ts = f.ts_ns;
    q.window_ns = opts.arm_ns;
    q.entry_price = f.price;
    q.position_side = f.side;
    q.stop_price = f.side == Side::kBid ? f.price - opts.stop_ticks * opts.tick_size
                                        : f.price + opts.stop_ticks * opts.tick_size;
    queries.push_back(q);
  }

  std::unique_ptr<TradeSource> source = open_source(opts);
  NakedWindowScan scan{TickScale{opts.tick_size}, queries.size() + 1};
  const std::vector<NakedWindowResult> results = scan.run(*source, queries);

  std::printf("entry_ts\twindow_end\tticks\tvolume\tstop_reached\tfirst_reach_ts\tmae_ticks\n");
  for (std::size_t i = 0; i < results.size(); ++i) {
    const NakedWindowResult& r = results[i];
    std::printf("%lld\t%lld\t%llu\t%llu\t%d\t%lld\t%lld\n",
                static_cast<long long>(queries[i].entry_fill_ts),
                static_cast<long long>(r.window_end_ns), static_cast<unsigned long long>(r.ticks),
                static_cast<unsigned long long>(r.volume), r.stop_reached ? 1 : 0,
                static_cast<long long>(r.stop_reached ? r.first_reach_ts : 0),
                static_cast<long long>(r.mae_ticks));
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return usage();
  }

  Options opts;
  opts.path = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--tick-size" && has_value) {
      if (!parse_i64(argv[++i], opts.tick_size)) {
        return usage();
      }
    } else if (arg == "--entry-ns" && has_value) {
      if (!parse_i64(argv[++i], opts.entry_ns)) {
        return usage();
      }
    } else if (arg == "--arm-ns" && has_value) {
      if (!parse_i64(argv[++i], opts.arm_ns)) {
        return usage();
      }
    } else if (arg == "--cancel-ns" && has_value) {
      if (!parse_i64(argv[++i], opts.cancel_ns)) {
        return usage();
      }
    } else if (arg == "--stop-ticks" && has_value) {
      if (!parse_i64(argv[++i], opts.stop_ticks)) {
        return usage();
      }
    } else if (arg == "--instrument" && has_value) {
      if (!parse_u32(argv[++i], opts.instrument_id)) {
        return usage();
      }
    } else if (arg == "--reserve-fills" && has_value) {
      std::int64_t value = 0;
      if (!parse_i64(argv[++i], value) || value < 1) {
        return usage();
      }
      opts.reserve_fills = static_cast<std::size_t>(value);
    } else if (arg == "--orders" && has_value) {
      opts.orders_path = argv[++i];
    } else if (arg == "--sweep-entry-ns" && has_value) {
      if (!parse_list(argv[++i], opts.sweep)) {
        return usage();
      }
    } else if (arg == "--naked-window") {
      opts.naked_window = true;
    } else if (arg == "--stats") {
      opts.stats = true;
    } else if (arg == "--full-remaining") {
      opts.full_remaining = true;
    } else if (arg == "--both-sides") {
      opts.both_sides = true;
    } else {
      return usage();
    }
  }

  if (opts.tick_size <= 0 || opts.entry_ns < 0 || opts.arm_ns < 0 || opts.cancel_ns < 0) {
    return usage();
  }
  if (!opts.sweep.empty() && opts.naked_window) {
    std::fprintf(stderr, "replay_demo: --sweep-entry-ns and --naked-window are separate reports\n");
    return usage();
  }
  if (opts.naked_window && opts.stop_ticks <= 0) {
    std::fprintf(stderr, "replay_demo: --naked-window needs --stop-ticks above zero\n");
    return usage();
  }
  if (!opts.naked_window && opts.stop_ticks != 0) {
    std::fprintf(stderr, "replay_demo: --stop-ticks only means something with --naked-window\n");
    return usage();
  }

  try {
    const std::vector<Order> orders =
        opts.orders_path.empty() ? std::vector<Order>{} : read_orders(opts.orders_path);

    if (!opts.sweep.empty()) {
      return run_sweep(opts, orders);
    }

    const std::vector<Fill> fills =
        run_once(opts, opts.entry_ns, open_source(opts), orders, opts.stats);

    if (opts.naked_window) {
      return run_naked_window(opts, fills);
    }
    if (!orders.empty()) {
      print_fill_header();
      for (const Fill& f : fills) {
        print_fill(f);
      }
    }
  } catch (const BookreplayError& e) {
    std::fprintf(stderr, "replay_demo: %s\n", e.what());
    return 1;
  }
  return 0;
}
