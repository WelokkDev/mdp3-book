// Replays a real MBO day through a book under InvariantHarness, merging the
// status schema in so the crossed-book check runs only while the venue says
// the instrument is trading. Given the definition schema, it also checks every
// price against its own instrument's tick.

#include "bookreplay/book.hpp"
#include "bookreplay/dbn_reader.hpp"
#include "bookreplay/definition.hpp"
#include "bookreplay/fast_book.hpp"
#include "bookreplay/invariants.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "books.hpp"
#include "cli.hpp"

namespace {

using namespace bookreplay;
using namespace bookreplay::tools;

/// `verify()` walks every resting order, so it is a periodic audit rather
/// than a per-record one.
constexpr std::uint64_t kVerifyPeriod = 1'000'000;

struct Options {
  std::string mbo_path;
  std::string status_path;
  std::string definition_path;
  std::uint32_t period = 1;
  std::uint64_t limit = 0;
  bool assume_trading = false;
  BookChoice book = BookChoice::kReference;
};

int usage() {
  std::fputs(
      "usage: book_check <mbo.dbn[.zst]> [--status FILE.dbn[.zst]] [--assume-trading]\n"
      "                  [--definition FILE.dbn[.zst]] [--period N] [--limit N]\n"
      "                  [--book reference|fast]\n",
      stderr);
  return 2;
}

/// Without this, the next day's status file merges without complaint: it opens
/// by carrying every instrument forward at the prior evening's open, two hours
/// inside this file, so the run checks that tail, exits 0, and never trips the
/// "no boundary was checked" warning below. The previous day's file already
/// fails loudly on its own.
void require_coverage(const DbnMetadata& status, const DbnMetadata& mbo) {
  constexpr std::uint64_t kOpenEnded = std::numeric_limits<std::uint64_t>::max();
  if (status.schema && *status.schema != kSchemaStatus) {
    throw BookreplayError("expected the status schema (" + std::to_string(kSchemaStatus) +
                          "), found schema " + std::to_string(*status.schema));
  }
  if (status.dataset != mbo.dataset) {
    throw BookreplayError("the status file is from dataset " + status.dataset + ", not " +
                          mbo.dataset);
  }
  if (status.start > mbo.start) {
    throw BookreplayError(
        "the status file starts later than the mbo file; the records before it would be gated "
        "out and still report ok");
  }
  if (status.end.value_or(kOpenEnded) < mbo.end.value_or(kOpenEnded)) {
    throw BookreplayError(
        "the status file ends earlier than the mbo file; the records after it would be gated "
        "out and still report ok");
  }
}

std::vector<StatusMsg> read_status(const std::string& path, const DbnMetadata& mbo_meta) {
  DbnReader reader{std::filesystem::path{path}};
  require_coverage(reader.metadata(), mbo_meta);
  std::vector<StatusMsg> out;
  while (const RecordHeader* hd = reader.next()) {
    if (const StatusMsg* status = record_cast<StatusMsg>(*hd)) {
      out.push_back(*status);
    }
  }
  // The venue changes state at ts_event, so that is the merge key. The file
  // arrives in ts_recv order, which trails it by 9-143 ms and agrees on
  // ordering today; sorting says which of the two this depends on.
  std::stable_sort(out.begin(), out.end(), [](const StatusMsg& a, const StatusMsg& b) {
    return a.hd.ts_event < b.hd.ts_event;
  });
  return out;
}

struct DefinitionReport {
  std::uint64_t catalog_instruments = 0;
  std::uint64_t catalog_outrights = 0;
  std::uint64_t catalog_spreads = 0;
  std::uint64_t catalog_class_leg_disagreeing_records = 0;
  std::uint64_t catalog_instruments_missing_legs = 0;
  bool catalog_reconciles = true;
  std::uint64_t outrights_seen = 0;
  std::uint64_t spreads_seen = 0;
  std::uint64_t aligned_prices = 0;
  std::uint64_t misaligned_prices = 0;
  std::uint64_t undef_prices = 0;
  std::uint64_t prices_without_definition = 0;
  std::uint64_t prices_without_tick_size = 0;
  std::vector<std::uint32_t> without_definition;
  std::vector<std::uint32_t> without_tick_size;
  std::uint64_t first_misaligned_index = 0;
  std::int64_t first_misaligned_increment = 0;
  MboMsg first_misaligned{};

  [[nodiscard]] bool reconciles(std::uint64_t records) const noexcept {
    const std::uint64_t priced =
        aligned_prices + misaligned_prices + prices_without_definition + prices_without_tick_size;
    return catalog_reconciles && priced + undef_prices == records;
  }
};

/// The remainder catches only an increment too coarse for the data. One that
/// divides the true increment leaves every price on grid, and no run of prices
/// proves otherwise, since a day's prices need not span the grid.
void check_price(const InstrumentCatalog& catalog, const MboMsg& rec, std::uint64_t record_index,
                 DefinitionReport& out) {
  if (is_undef_price(rec.price)) {
    ++out.undef_prices;
    return;
  }
  const InstrumentDefinition* def = catalog.find(rec.hd.instrument_id);
  if (def == nullptr) {
    ++out.prices_without_definition;
  } else if (!def->has_tick_size()) {
    ++out.prices_without_tick_size;
  } else if (rec.price % def->min_price_increment == 0) {
    ++out.aligned_prices;
  } else {
    if (out.misaligned_prices == 0) {
      out.first_misaligned_index = record_index;
      out.first_misaligned_increment = def->min_price_increment;
      out.first_misaligned = rec;
    }
    ++out.misaligned_prices;
  }
}

void count_instruments(const InstrumentCatalog& catalog, const std::set<std::uint32_t>& seen,
                       DefinitionReport& out) {
  out.catalog_instruments = catalog.instrument_count();
  out.catalog_outrights = catalog.outright_count();
  out.catalog_spreads = catalog.spread_count();
  out.catalog_class_leg_disagreeing_records = catalog.stats().class_leg_disagreements;
  out.catalog_reconciles = catalog.stats().reconciles();
  for (const std::uint32_t instrument_id : catalog.instruments()) {
    if (!catalog.at(instrument_id).has_all_legs()) {
      ++out.catalog_instruments_missing_legs;
    }
  }
  for (const std::uint32_t instrument_id : seen) {
    const InstrumentDefinition* def = catalog.find(instrument_id);
    if (def == nullptr) {
      out.without_definition.push_back(instrument_id);
      continue;
    }
    if (!def->has_tick_size()) {
      out.without_tick_size.push_back(instrument_id);
    }
    if (def->is_spread()) {
      ++out.spreads_seen;
    } else {
      ++out.outrights_seen;
    }
  }
}

[[nodiscard]] bool reconciled(const InvariantReport& report,
                              const std::optional<DefinitionReport>& definitions) {
  return report.reconciles() && (!definitions || definitions->reconciles(report.records));
}

/// A price off its instrument's grid fails the run, since either the catalog
/// or the data is wrong. So does one the catalog cannot place at all: the grid
/// check silently never ran on it, which no caller reading the exit code could
/// tell from a clean day. A spread holding fewer leg records than it declares
/// is that same silence, so it fails too. An instrument the venue lists
/// without a usable increment is only listed, the way one without a status
/// record is.
[[nodiscard]] bool passed(const InvariantReport& report,
                          const std::optional<DefinitionReport>& definitions) {
  return report.ok() && reconciled(report, definitions) &&
         (!definitions ||
          (definitions->misaligned_prices == 0 && definitions->prices_without_definition == 0 &&
           definitions->catalog_instruments_missing_legs == 0));
}

/// Only the first violation's neighbourhood is captured, so only those
/// records carry a timestamp a later violation can be dated by.
std::unordered_map<std::uint64_t, std::uint64_t> context_times(const InvariantReport& report) {
  std::unordered_map<std::uint64_t, std::uint64_t> out;
  const std::size_t depth = report.first_violation_context.size();
  if (report.violations.empty() || depth == 0) {
    return out;
  }
  const std::uint64_t newest = report.violations.front().record_index;
  for (std::size_t i = 0; i < depth; ++i) {
    out[newest - (depth - 1 - i)] = report.first_violation_context[i].hd.ts_event;
  }
  return out;
}

template <typename B>
void print_report(const InvariantReport& report, const B& book,
                  const std::vector<std::uint32_t>& without_status,
                  const std::optional<DefinitionReport>& definitions) {
  std::string out;
  row(out, "records", report.records);
  row(out, "status_records", report.status_records);
  row(out, "unmapped_status_records", report.unmapped_status_records);
  row(out, "mutating_records", report.mutating_records);
  row(out, "passive_records", report.passive_records);
  row(out, "observed_mutations", report.observed_mutations);
  row(out, "passive_mutations", report.passive_mutations);
  row(out, "mutation_miscounts", report.mutation_miscounts);
  row(out, "boundaries", report.boundaries);
  row(out, "boundaries_outside_trading", report.boundaries_outside_trading);
  row(out, "cross_checks", report.cross_checks);
  row(out, "cross_violations", report.cross_violations);
  row(out, "materializations", report.materializations);
  row(out, "dematerializations", report.dematerializations);
  row(out, "unknown_order_fills", report.unknown_order_fills);
  row(out, "malformed_records", report.malformed_records);
  row(out, "instruments", book.instrument_count());
  row(out, "unknown_modifies", book.unknown_modifies());
  row(out, "duplicate_adds", book.duplicate_adds());
  row(out, "instruments_without_status", without_status.size());
  if (definitions) {
    row(out, "catalog_instruments", definitions->catalog_instruments);
    row(out, "catalog_outrights", definitions->catalog_outrights);
    row(out, "catalog_spreads", definitions->catalog_spreads);
    row(out, "catalog_class_leg_disagreeing_records",
        definitions->catalog_class_leg_disagreeing_records);
    row(out, "catalog_instruments_missing_legs", definitions->catalog_instruments_missing_legs);
    row(out, "outrights_seen", definitions->outrights_seen);
    row(out, "spreads_seen", definitions->spreads_seen);
    row(out, "aligned_prices", definitions->aligned_prices);
    row(out, "misaligned_prices", definitions->misaligned_prices);
    row(out, "undef_prices", definitions->undef_prices);
    row(out, "prices_without_definition", definitions->prices_without_definition);
    row(out, "prices_without_tick_size", definitions->prices_without_tick_size);
    row(out, "instruments_without_definition", definitions->without_definition.size());
    row(out, "instruments_without_tick_size", definitions->without_tick_size.size());
  }
  row(out, "reconciles", reconciled(report, definitions) ? 1 : 0);
  row(out, "ok", passed(report, definitions) ? 1 : 0);

  for (const std::uint32_t instrument_id : without_status) {
    out += "instrument_without_status\t";
    out += std::to_string(instrument_id);
    out += '\n';
  }

  if (definitions) {
    for (const std::uint32_t instrument_id : definitions->without_definition) {
      out += "instrument_without_definition\t";
      out += std::to_string(instrument_id);
      out += '\n';
    }
    for (const std::uint32_t instrument_id : definitions->without_tick_size) {
      out += "instrument_without_tick_size\t";
      out += std::to_string(instrument_id);
      out += '\n';
    }
    if (definitions->misaligned_prices != 0) {
      const MboMsg& rec = definitions->first_misaligned;
      out += "misaligned_price\t";
      out += std::to_string(definitions->first_misaligned_index);
      out += '\t';
      out += std::to_string(rec.hd.instrument_id);
      out += '\t';
      out += rec.action;
      out += '\t';
      out += std::to_string(rec.price);
      out += '\t';
      out += std::to_string(definitions->first_misaligned_increment);
      out += '\t';
      out += time_of_day(rec.hd.ts_event);
      out += '\n';
    }
  }

  const std::unordered_map<std::uint64_t, std::uint64_t> times = context_times(report);
  for (const Violation& v : report.violations) {
    const auto at = times.find(v.record_index);
    out += "violation\t";
    out += std::to_string(v.record_index);
    out += '\t';
    out += std::to_string(v.instrument_id);
    out += '\t';
    out += v.action;
    out += '\t';
    out += std::to_string(v.order_id);
    out += '\t';
    out += std::to_string(v.best_bid);
    out += '\t';
    out += std::to_string(v.best_ask);
    out += '\t';
    out += at == times.end() ? "-" : time_of_day(at->second);
    out += '\t';
    out += v.what;
    out += '\n';
  }

  std::fwrite(out.data(), 1, out.size(), stdout);
}

template <typename B>
int replay(B& book, DbnReader& reader, const Options& opts, const std::vector<StatusMsg>& statuses,
           const InstrumentCatalog& catalog, std::optional<DefinitionReport> definitions) {
  typename InvariantHarness<B>::Options harness_opts;
  harness_opts.cross_check_period = opts.period;
  InvariantHarness<B> harness(book, harness_opts);
  if (opts.assume_trading) {
    harness.set_session_state(SessionState::kTrading);
  }

  std::set<std::uint32_t> seen;
  std::set<std::uint32_t> described;
  std::size_t next_status = 0;
  std::uint64_t since_verify = 0;

  while (const RecordHeader* hd = reader.next()) {
    const MboMsg* rec = record_cast<MboMsg>(*hd);
    if (rec == nullptr) {
      std::fprintf(stderr, "book_check: rtype 0x%02x is not an MBO record\n", hd->rtype);
      return 1;
    }

    // Status first at an equal ts_event: the opening uncross shares one with
    // the Trading record that admits it, and it is the boundary most worth
    // checking.
    while (next_status < statuses.size() && statuses[next_status].hd.ts_event <= rec->hd.ts_event) {
      harness.observe(statuses[next_status]);
      described.insert(statuses[next_status].hd.instrument_id);
      ++next_status;
    }

    seen.insert(rec->hd.instrument_id);
    harness.before(*rec);
    book.apply(*rec);
    harness.after(*rec);
    if (definitions) {
      check_price(catalog, *rec, harness.report().records - 1, *definitions);
    }

    if (++since_verify == kVerifyPeriod) {
      since_verify = 0;
      book.verify();
    }
    if (opts.limit != 0 && reader.record_count() >= opts.limit) {
      break;
    }
  }
  book.verify();

  std::vector<std::uint32_t> without_status;
  std::set_difference(seen.begin(), seen.end(), described.begin(), described.end(),
                      std::back_inserter(without_status));
  if (definitions) {
    count_instruments(catalog, seen, *definitions);
  }
  print_report(harness.report(), book, without_status, definitions);
  if (harness.report().cross_checks == 0) {
    std::fputs("book_check: no boundary was checked, so ok proves nothing about crossings\n",
               stderr);
  }
  if (definitions && definitions->aligned_prices + definitions->misaligned_prices == 0) {
    std::fputs(
        "book_check: no price was checked against a tick, so ok proves nothing about the grid\n",
        stderr);
  }
  return passed(harness.report(), definitions) ? 0 : 1;
}

int run(const Options& opts) {
  DbnReader reader{std::filesystem::path{opts.mbo_path}};
  const std::vector<StatusMsg> statuses = opts.status_path.empty()
                                              ? std::vector<StatusMsg>{}
                                              : read_status(opts.status_path, reader.metadata());
  InstrumentCatalog catalog;
  std::optional<DefinitionReport> definitions;
  if (!opts.definition_path.empty()) {
    catalog = read_catalog(opts.definition_path, reader.metadata());
    definitions.emplace();
  }

  if (opts.book == BookChoice::kFast) {
    FastBook book;
    set_tick_sizes(book, catalog);
    return replay(book, reader, opts, statuses, catalog, definitions);
  }
  Book book;
  return replay(book, reader, opts, statuses, catalog, definitions);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return usage();
  }

  Options opts;
  opts.mbo_path = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--status" && has_value) {
      opts.status_path = argv[++i];
    } else if (arg == "--definition" && has_value) {
      opts.definition_path = argv[++i];
    } else if (arg == "--period" && has_value) {
      if (!parse_u32(argv[++i], opts.period)) {
        return usage();
      }
    } else if (arg == "--limit" && has_value) {
      if (!parse_u64(argv[++i], opts.limit)) {
        return usage();
      }
    } else if (arg == "--book" && has_value) {
      if (!parse_book(argv[++i], opts.book)) {
        return usage();
      }
    } else if (arg == "--assume-trading") {
      opts.assume_trading = true;
    } else {
      return usage();
    }
  }

  if (!opts.status_path.empty() && opts.assume_trading) {
    std::fputs("book_check: --assume-trading is the ungated run; it does not take --status\n",
               stderr);
    return usage();
  }
  if (opts.book == BookChoice::kFast && opts.definition_path.empty()) {
    std::fprintf(stderr, "book_check: %s\n", kFastNeedsDefinition);
    return usage();
  }

  try {
    return run(opts);
  } catch (const BookreplayError& e) {
    std::fprintf(stderr, "book_check: %s\n", e.what());
    return 1;
  }
}
