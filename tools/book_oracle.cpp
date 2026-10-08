// Replays a real MBO day through the reference book and the fast book side by
// side and compares their state: the order each record names, the top ten at
// every event boundary, and all of both books periodically.
//
// It takes no --status. Whether the venue was trading says nothing about
// whether two books built from the same records agree.

#include "bookreplay/book_oracle.hpp"

#include "bookreplay/book.hpp"
#include "bookreplay/dbn.hpp"
#include "bookreplay/dbn_reader.hpp"
#include "bookreplay/fast_book.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "books.hpp"
#include "cli.hpp"

namespace {

using namespace bookreplay;
using namespace bookreplay::tools;

struct Options {
  std::string mbo_path;
  std::string definition_path;
  std::uint64_t period = 1'000'000;
  std::uint64_t limit = 0;
};

int usage() {
  std::fputs(
      "usage: book_oracle <mbo.dbn[.zst]> --definition FILE.dbn[.zst] [--period N] [--limit N]\n",
      stderr);
  return 2;
}

/// Empty when both books pass their own consistency checks, otherwise which
/// failed and why. A failure is reported beside the comparison rather than in
/// place of it, since the comparison usually says where the fault began.
std::string inconsistency(const Book& reference, const FastBook& fast) {
  try {
    reference.verify();
  } catch (const BookError& e) {
    return std::string{"the reference book: "} + e.what();
  }
  try {
    fast.verify();
  } catch (const BookError& e) {
    return std::string{"the fast book: "} + e.what();
  }
  return {};
}

void print_report(const OracleReport& report, const FastBook& fast, bool truncated, bool consistent,
                  bool ok) {
  std::string out;
  row(out, "records", report.records);
  row(out, "order_checks", report.order_checks);
  row(out, "fill_checks", report.fill_checks);
  row(out, "boundary_checks", report.boundary_checks);
  row(out, "audits", report.audits);
  row(out, "differences", report.difference_count);
  for (std::size_t i = 0; i < kOracleDifferenceKinds; ++i) {
    const auto kind = static_cast<OracleDifferenceKind>(i);
    out += "difference_";
    out += oracle_difference_name(kind);
    field(out, std::to_string(report.count(kind)));
    out += '\n';
  }
  const FastBook::Growth& growth = fast.growth();
  row(out, "growth_slab", growth.slab);
  row(out, "growth_id_table", growth.id_table);
  row(out, "growth_pages", growth.pages);
  row(out, "growth_page_directory", growth.page_directory);
  row(out, "growth_attribution", growth.attribution);
  row(out, "truncated", truncated ? 1 : 0);
  row(out, "consistent", consistent ? 1 : 0);
  row(out, "ok", ok ? 1 : 0);

  for (const OracleDifference& d : report.differences) {
    out += "difference";
    field(out, std::to_string(d.record_index));
    field(out, oracle_difference_name(d.kind));
    field(out, d.field);
    field(out, std::to_string(d.instrument_id));
    field(out, std::to_string(d.order_id));
    field(out, std::string{static_cast<char>(d.side)});
    field(out, value_text(d.price));
    field(out, std::to_string(d.position));
    field(out, value_text(d.reference));
    field(out, value_text(d.candidate));
    out += '\n';
  }

  const std::size_t depth = report.first_difference_context.size();
  const std::uint64_t last =
      report.differences.empty() ? 0 : report.differences.front().record_index;
  for (std::size_t i = 0; i < depth; ++i) {
    out += "context";
    field(out, std::to_string(last - (depth - 1 - i)));
    append_mbo(out, report.first_difference_context[i]);
    out += '\n';
  }
  std::fwrite(out.data(), 1, out.size(), stdout);
}

int run(const Options& opts) {
  DbnReader reader{std::filesystem::path{opts.mbo_path}};
  Book reference;
  FastBook fast;
  set_tick_sizes(fast, read_catalog(opts.definition_path, reader.metadata()));

  BookOracle<Book, FastBook>::Options oracle_opts;
  oracle_opts.audit_period = opts.period;
  BookOracle oracle{reference, fast, oracle_opts};

  bool at_limit = false;
  std::string broken;
  std::string stopped;
  while (const RecordHeader* hd = reader.next()) {
    const MboMsg* rec = record_cast<MboMsg>(*hd);
    if (rec == nullptr) {
      std::fprintf(stderr, "book_oracle: rtype 0x%02x is not an MBO record\n", hd->rtype);
      return 1;
    }
    // A throw from either book ends the run but not the report: the
    // differences before it say where the fault began, and the message names
    // neither the book nor the record.
    try {
      oracle.before(*rec);
      reference.apply(*rec);
      fast.apply(*rec);
      oracle.after(*rec);
    } catch (const BookreplayError& e) {
      stopped = "stopped at record " + std::to_string(reader.record_count() - 1) + ": " + e.what();
      break;
    }
    // The views skip what a stale index points at, so the books' own
    // consistency checks run on the audit's schedule.
    if (opts.period != 0 && oracle.report().records % opts.period == 0) {
      broken = inconsistency(reference, fast);
      if (!broken.empty()) {
        break;
      }
    }
    if (opts.limit != 0 && reader.record_count() >= opts.limit) {
      at_limit = true;
      break;
    }
  }
  // An early exit from the loop counts as truncated whether or not a record
  // followed the one it stopped on; a book that fails only the check below
  // has read them all.
  const bool truncated = at_limit || !stopped.empty() || !broken.empty();
  if (broken.empty()) {
    broken = inconsistency(reference, fast);
  }
  // A book that fails its own check is not audited: the check names the
  // fault, and a comparison of what the book holds would not. Nor is a run
  // that a throw ended: the record it threw on may have reached one book and
  // not the other, and the audit would report that as their difference.
  if (broken.empty() && stopped.empty()) {
    oracle.finish();
  }

  const bool ok = oracle.ok() && broken.empty() && stopped.empty();
  print_report(oracle.report(), fast, truncated, broken.empty(), ok);
  if (!stopped.empty()) {
    std::fprintf(stderr, "book_oracle: %s\n", stopped.c_str());
  }
  if (!broken.empty()) {
    std::fprintf(stderr, "book_oracle: %s\n", broken.c_str());
  }
  if (oracle.report().order_checks + oracle.report().boundary_checks == 0) {
    std::fputs("book_oracle: no record named an order or closed an event, so ok proves nothing\n",
               stderr);
  }
  if (at_limit) {
    std::fputs("book_oracle: the run stopped at --limit, so ok covers only the records read\n",
               stderr);
  }
  return ok ? 0 : 1;
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
    if (arg == "--definition" && has_value) {
      opts.definition_path = argv[++i];
    } else if (arg == "--period" && has_value) {
      if (!parse_u64(argv[++i], opts.period)) {
        return usage();
      }
    } else if (arg == "--limit" && has_value) {
      if (!parse_u64(argv[++i], opts.limit)) {
        return usage();
      }
    } else {
      return usage();
    }
  }

  if (opts.definition_path.empty()) {
    std::fprintf(stderr, "book_oracle: %s\n", kFastNeedsDefinition);
    return usage();
  }

  try {
    return run(opts);
  } catch (const BookreplayError& e) {
    std::fprintf(stderr, "book_oracle: %s\n", e.what());
    return 1;
  }
}
