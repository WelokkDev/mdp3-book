// Replays a real MBO day through Book under InvariantHarness, merging the
// status schema in so the crossed-book check runs only while the venue says
// the instrument is trading.

#include "bookreplay/book.hpp"
#include "bookreplay/dbn_reader.hpp"
#include "bookreplay/invariants.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace bookreplay;

/// `verify()` walks every resting order, so it is a periodic audit rather
/// than a per-record one.
constexpr std::uint64_t kVerifyPeriod = 1'000'000;

struct Options {
  std::string mbo_path;
  std::string status_path;
  std::uint32_t period = 1;
  std::uint64_t limit = 0;
  bool assume_trading = false;
};

int usage() {
  std::fputs(
      "usage: book_check <mbo.dbn[.zst]> [--status FILE.dbn[.zst]] [--assume-trading]\n"
      "                  [--period N] [--limit N]\n",
      stderr);
  return 2;
}

bool parse_u64(const char* text, std::uint64_t& out) {
  if (text[0] == '-') {  // strtoull wraps a negative rather than refusing it
    return false;
  }
  char* end = nullptr;
  out = std::strtoull(text, &end, 10);
  return end != text && *end == '\0';
}

/// Without this, the next day's status file merges without complaint: it opens
/// by carrying every instrument forward at the prior evening's open, two hours
/// inside this file, so the run checks that tail, exits 0, and never trips the
/// "no boundary was checked" warning below. The previous day's file already
/// fails loudly on its own.
void require_coverage(const DbnMetadata& status, const DbnMetadata& mbo) {
  constexpr std::uint64_t kOpenEnded = std::numeric_limits<std::uint64_t>::max();
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

void row(std::string& out, const char* key, std::uint64_t value) {
  out += key;
  out += '\t';
  out += std::to_string(value);
  out += '\n';
}

/// `HH:MM:SS.nnnnnnnnn` UTC. Which day it is, is the file name's job.
std::string time_of_day(std::uint64_t ts_ns) {
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

void print_report(const InvariantReport& report, const Book& book,
                  const std::vector<std::uint32_t>& without_status) {
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
  row(out, "reconciles", report.reconciles() ? 1 : 0);
  row(out, "ok", report.ok() ? 1 : 0);

  for (const std::uint32_t instrument_id : without_status) {
    out += "instrument_without_status\t";
    out += std::to_string(instrument_id);
    out += '\n';
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

int run(const Options& opts) {
  DbnReader reader{std::filesystem::path{opts.mbo_path}};
  const std::vector<StatusMsg> statuses =
      opts.status_path.empty() ? std::vector<StatusMsg>{}
                               : read_status(opts.status_path, reader.metadata());

  Book book;
  InvariantHarness<Book>::Options harness_opts;
  harness_opts.cross_check_period = opts.period;
  InvariantHarness<Book> harness(book, harness_opts);
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
  print_report(harness.report(), book, without_status);
  if (harness.report().cross_checks == 0) {
    std::fputs("book_check: no boundary was checked, so ok proves nothing about crossings\n",
               stderr);
  }
  return harness.ok() ? 0 : 1;
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
    } else if (arg == "--period" && has_value) {
      std::uint64_t value = 0;
      if (!parse_u64(argv[++i], value) || value > std::numeric_limits<std::uint32_t>::max()) {
        return usage();
      }
      opts.period = static_cast<std::uint32_t>(value);
    } else if (arg == "--limit" && has_value) {
      if (!parse_u64(argv[++i], opts.limit)) {
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

  try {
    return run(opts);
  } catch (const BookreplayError& e) {
    std::fprintf(stderr, "book_check: %s\n", e.what());
    return 1;
  }
}
