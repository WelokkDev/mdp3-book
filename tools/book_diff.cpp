// Replays a real MBO day through a book and compares its top ten levels per
// side against the venue's own mbp-10 at every event boundary.
//
// Unlike book_check this takes no --status. The venue's mbp-10 carries the
// same crossed book outside trading hours that the replayed book holds, so
// gating the comparison on the session would only throw agreement away.

#include "bookreplay/book.hpp"
#include "bookreplay/dbn.hpp"
#include "bookreplay/dbn_reader.hpp"
#include "bookreplay/definition.hpp"
#include "bookreplay/fast_book.hpp"
#include "bookreplay/mbp10_diff.hpp"

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
  std::string mbp10_path;
  std::string definition_path;
  std::uint64_t limit = 0;
  BookChoice book = BookChoice::kReference;
};

int usage() {
  std::fputs(
      "usage: book_diff <mbo.dbn[.zst]> --mbp10 FILE.dbn[.zst] [--limit N]\n"
      "                 [--book reference|fast] [--definition FILE.dbn[.zst]]\n",
      stderr);
  return 2;
}

std::string window_end(const DbnMetadata& meta) {
  return meta.end ? std::to_string(*meta.end) : std::string{"an open end"};
}

/// Both files carry their exact interval, so a mispairing is cheap to refuse
/// here. Left to the replay it would fail at the first snapshot instead,
/// naming a divergence rather than the wrong file.
void require_same_window(const DbnMetadata& mbp10, const DbnMetadata& mbo) {
  if (mbp10.schema && *mbp10.schema != kSchemaMbp10) {
    throw BookreplayError("expected the mbp-10 schema (" + std::to_string(kSchemaMbp10) +
                          "), found schema " + std::to_string(*mbp10.schema));
  }
  if (mbp10.dataset != mbo.dataset) {
    throw BookreplayError("the mbp-10 file is from dataset " + mbp10.dataset + ", not " +
                          mbo.dataset);
  }
  if (mbp10.start != mbo.start) {
    throw BookreplayError("the mbp-10 file starts at " + std::to_string(mbp10.start) +
                          " and the mbo file at " + std::to_string(mbo.start));
  }
  if (mbp10.end != mbo.end) {
    throw BookreplayError("the mbp-10 file ends at " + window_end(mbp10) + " and the mbo file at " +
                          window_end(mbo));
  }
}

void append_level(std::string& out, const BidAskPair& level) {
  field(out, value_text(level.bid_px));
  field(out, std::to_string(level.bid_sz));
  field(out, std::to_string(level.bid_ct));
  field(out, value_text(level.ask_px));
  field(out, std::to_string(level.ask_sz));
  field(out, std::to_string(level.ask_ct));
}

void print_context(std::string& out, const DivergenceContext& ctx) {
  const std::size_t depth = ctx.records.size();
  for (std::size_t i = 0; i < depth; ++i) {
    out += "context";
    field(out, std::to_string(ctx.last_record_index - (depth - 1 - i)));
    append_mbo(out, ctx.records[i]);
    out += '\n';
  }

  if (ctx.venue_record) {
    const Mbp10Msg& rec = *ctx.venue_record;
    out += "mbp10_record";
    field(out, std::to_string(ctx.venue_record_index));
    field(out, std::to_string(rec.hd.instrument_id));
    field(out, std::string{rec.action});
    field(out, std::string{rec.side});
    field(out, value_text(rec.price));
    field(out, std::to_string(rec.size));
    field(out, std::to_string(rec.flags));
    field(out, std::to_string(rec.depth));
    field(out, std::to_string(rec.sequence));
    field(out, time_of_day(rec.ts_recv));
    out += '\n';
  }

  if (ctx.event_mutation) {
    out += "event_mutation";
    append_mbo(out, *ctx.event_mutation);
    out += '\n';
  }

  if (ctx.ours) {
    for (std::size_t level = 0; level < kDepthLevels; ++level) {
      out += "ladder";
      field(out, std::to_string(level));
      field(out, "ours");
      append_level(out, (*ctx.ours)[level]);
      if (ctx.theirs) {
        field(out, "venue");
        append_level(out, (*ctx.theirs)[level]);
      }
      out += '\n';
    }
  }

  if (ctx.mismatch) {
    out += "mismatch";
    field(out, std::to_string(ctx.mismatch->level));
    field(out, ctx.mismatch->field);
    field(out, value_text(ctx.mismatch->ours));
    field(out, value_text(ctx.mismatch->theirs));
    out += '\n';
  }
}

void print_report(const Mbp10DiffReport& report) {
  std::string out;
  row(out, "mbo_records", report.records);
  row(out, "boundaries", report.boundaries);
  row(out, "compared", report.compared);
  row(out, "silent", report.silent);
  row(out, "trades", report.trades);
  row(out, "snapshots", report.snapshots);
  row(out, "compared_top_ten_unchanged", report.compared_unchanged);
  row(out, "deferred", report.deferred);
  row(out, "mbp10_records", report.records_read);
  row(out, "mbp10_records_held", report.records_held);
  row(out, "divergences", report.divergence_count);
  for (std::size_t i = 0; i < kDivergenceKinds; ++i) {
    const auto kind = static_cast<DivergenceKind>(i);
    out += "divergence_";
    out += divergence_name(kind);
    field(out, std::to_string(report.count(kind)));
    out += '\n';
  }
  row(out, "boundaries_reconcile", report.boundaries_reconcile() ? 1 : 0);
  row(out, "records_reconcile", report.records_reconcile() ? 1 : 0);
  row(out, "reconciles", report.reconciles() ? 1 : 0);
  row(out, "truncated", report.finished ? 0 : 1);
  row(out, "ok", report.ok() ? 1 : 0);

  for (const auto& [instrument_id, stats] : report.instruments) {
    out += "instrument";
    field(out, std::to_string(instrument_id));
    field(out, std::to_string(stats.compared));
    field(out, std::to_string(stats.silent));
    field(out, std::to_string(stats.trades));
    field(out, std::to_string(stats.divergences));
    out += '\n';
  }

  for (const Divergence& d : report.divergences) {
    out += "divergence";
    field(out, std::to_string(d.record_index));
    field(out, divergence_name(d.kind));
    field(out, std::to_string(d.instrument_id));
    field(out, std::to_string(d.sequence));
    field(out, std::string{d.action});
    field(out, time_of_day(d.ts_recv));
    field(out, d.what);
    out += '\n';
  }

  print_context(out, report.first_divergence);
  std::fwrite(out.data(), 1, out.size(), stdout);
}

template <typename B>
int replay(B& book, DbnReader& mbo, DbnReader& mbp10, const Options& opts) {
  auto source = [&mbp10]() -> const Mbp10Msg* {
    const RecordHeader* hd = mbp10.next();
    if (hd == nullptr) {
      return nullptr;
    }
    const Mbp10Msg* rec = record_cast<Mbp10Msg>(*hd);
    if (rec == nullptr) {
      throw BookreplayError("the mbp-10 file carries a record of rtype " +
                            std::to_string(hd->rtype));
    }
    return rec;
  };

  Mbp10Diff diff{book, source};

  // A limited run leaves the mbp-10 stream with records the MBO stream never
  // reached, so the end-of-stream check would report every one of them.
  bool truncated = false;
  while (const RecordHeader* hd = mbo.next()) {
    const MboMsg* rec = record_cast<MboMsg>(*hd);
    if (rec == nullptr) {
      std::fprintf(stderr, "book_diff: rtype 0x%02x is not an MBO record\n", hd->rtype);
      return 1;
    }
    book.apply(*rec);
    diff.after(*rec);
    if (opts.limit != 0 && mbo.record_count() >= opts.limit) {
      truncated = true;
      break;
    }
  }
  if (!truncated) {
    diff.finish();
  }

  print_report(diff.report());
  if (diff.report().compared == 0) {
    std::fputs("book_diff: no boundary was compared, so ok proves nothing about the book\n",
               stderr);
  }
  if (!diff.report().finished) {
    std::fputs("book_diff: the run stopped at --limit, so ok covers only the records read\n",
               stderr);
  }
  return diff.ok() ? 0 : 1;
}

int run(const Options& opts) {
  DbnReader mbo{std::filesystem::path{opts.mbo_path}};
  DbnReader mbp10{std::filesystem::path{opts.mbp10_path}};
  require_same_window(mbp10.metadata(), mbo.metadata());

  if (opts.book == BookChoice::kFast) {
    FastBook book;
    set_tick_sizes(book, read_catalog(opts.definition_path, mbo.metadata()));
    return replay(book, mbo, mbp10, opts);
  }
  Book book;
  return replay(book, mbo, mbp10, opts);
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
    if (arg == "--mbp10" && has_value) {
      opts.mbp10_path = argv[++i];
    } else if (arg == "--definition" && has_value) {
      opts.definition_path = argv[++i];
    } else if (arg == "--book" && has_value) {
      if (!parse_book(argv[++i], opts.book)) {
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

  if (opts.mbp10_path.empty()) {
    std::fputs("book_diff: --mbp10 is what there is to diff against\n", stderr);
    return usage();
  }
  if (opts.book == BookChoice::kFast && opts.definition_path.empty()) {
    std::fprintf(stderr, "book_diff: %s\n", kFastNeedsDefinition);
    return usage();
  }
  if (opts.book == BookChoice::kReference && !opts.definition_path.empty()) {
    std::fputs("book_diff: --definition only gives --book fast its ticks\n", stderr);
    return usage();
  }

  try {
    return run(opts);
  } catch (const BookreplayError& e) {
    std::fprintf(stderr, "book_diff: %s\n", e.what());
    return 1;
  }
}
