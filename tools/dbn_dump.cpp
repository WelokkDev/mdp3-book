// One tab-separated line per record. tools/oracle_diff.py emits the same
// lines from Databento's own decoder and diffs the two.

#include "bookreplay/dbn_reader.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace bookreplay;

constexpr std::size_t kOutBufSize = 1 << 20;

/// Fixed-width DBN strings are NUL-padded.
template <std::size_t N>
void put_cstr(std::string& out, const std::array<char, N>& field) {
  const auto len =
      static_cast<std::size_t>(std::find(field.begin(), field.end(), '\0') - field.begin());
  out.append(field.data(), len);
}

void field_u(std::string& out, std::uint64_t v) {
  out += '\t';
  out += std::to_string(v);
}

void field_i(std::string& out, std::int64_t v) {
  out += '\t';
  out += std::to_string(v);
}

void field_c(std::string& out, char c) {
  out += '\t';
  out += c;
}

void header_fields(std::string& out, const RecordHeader& hd) {
  out += std::to_string(hd.rtype);
  field_u(out, hd.publisher_id);
  field_u(out, hd.instrument_id);
  field_u(out, hd.ts_event);
}

void dump_mbo(std::string& out, const MboMsg& r) {
  header_fields(out, r.hd);
  field_u(out, r.order_id);
  field_i(out, r.price);
  field_u(out, r.size);
  field_u(out, r.flags);
  field_u(out, r.channel_id);
  field_c(out, r.action);
  field_c(out, r.side);
  field_u(out, r.ts_recv);
  field_i(out, r.ts_in_delta);
  field_u(out, r.sequence);
}

void dump_trade(std::string& out, const TradeMsg& r) {
  header_fields(out, r.hd);
  field_i(out, r.price);
  field_u(out, r.size);
  field_c(out, r.action);
  field_c(out, r.side);
  field_u(out, r.flags);
  field_u(out, r.depth);
  field_u(out, r.ts_recv);
  field_i(out, r.ts_in_delta);
  field_u(out, r.sequence);
}

void dump_mbp10(std::string& out, const Mbp10Msg& r) {
  header_fields(out, r.hd);
  field_i(out, r.price);
  field_u(out, r.size);
  field_c(out, r.action);
  field_c(out, r.side);
  field_u(out, r.flags);
  field_u(out, r.depth);
  field_u(out, r.ts_recv);
  field_i(out, r.ts_in_delta);
  field_u(out, r.sequence);
  for (const BidAskPair& lvl : r.levels) {
    field_i(out, lvl.bid_px);
    field_i(out, lvl.ask_px);
    field_u(out, lvl.bid_sz);
    field_u(out, lvl.ask_sz);
    field_u(out, lvl.bid_ct);
    field_u(out, lvl.ask_ct);
  }
}

void dump_status(std::string& out, const StatusMsg& r) {
  header_fields(out, r.hd);
  field_u(out, r.ts_recv);
  field_u(out, r.action);
  field_u(out, r.reason);
  field_u(out, r.trading_event);
  field_c(out, r.is_trading);
  field_c(out, r.is_quoting);
  field_c(out, r.is_short_sell_restricted);
}

void dump_definition(std::string& out, const InstrumentDefMsg& r) {
  header_fields(out, r.hd);
  field_u(out, r.ts_recv);
  field_i(out, r.min_price_increment);
  field_i(out, r.display_factor);
  field_u(out, r.expiration);
  field_u(out, r.activation);
  field_i(out, r.high_limit_price);
  field_i(out, r.low_limit_price);
  field_i(out, r.max_price_variation);
  field_i(out, r.unit_of_measure_qty);
  field_i(out, r.min_price_increment_amount);
  field_i(out, r.price_ratio);
  field_i(out, r.strike_price);
  field_u(out, r.raw_instrument_id);
  field_i(out, r.leg_price);
  field_i(out, r.leg_delta);
  field_i(out, r.inst_attrib_value);
  field_u(out, r.underlying_id);
  field_i(out, r.market_depth_implied);
  field_i(out, r.market_depth);
  field_u(out, r.market_segment_id);
  field_u(out, r.max_trade_vol);
  field_i(out, r.min_lot_size);
  field_i(out, r.min_lot_size_block);
  field_i(out, r.min_lot_size_round_lot);
  field_u(out, r.min_trade_vol);
  field_i(out, r.contract_multiplier);
  field_i(out, r.decay_quantity);
  field_i(out, r.original_contract_size);
  field_u(out, r.leg_instrument_id);
  field_i(out, r.leg_ratio_price_numerator);
  field_i(out, r.leg_ratio_price_denominator);
  field_i(out, r.leg_ratio_qty_numerator);
  field_i(out, r.leg_ratio_qty_denominator);
  field_u(out, r.leg_underlying_id);
  field_i(out, r.appl_id);
  field_u(out, r.maturity_year);
  field_u(out, r.decay_start_date);
  field_u(out, r.channel_id);
  field_u(out, r.leg_count);
  field_u(out, r.leg_index);
  out += '\t';
  put_cstr(out, r.currency);
  out += '\t';
  put_cstr(out, r.settl_currency);
  out += '\t';
  put_cstr(out, r.secsubtype);
  out += '\t';
  put_cstr(out, r.raw_symbol);
  out += '\t';
  put_cstr(out, r.group);
  out += '\t';
  put_cstr(out, r.exchange);
  out += '\t';
  put_cstr(out, r.asset);
  out += '\t';
  put_cstr(out, r.cfi);
  out += '\t';
  put_cstr(out, r.security_type);
  out += '\t';
  put_cstr(out, r.unit_of_measure);
  out += '\t';
  put_cstr(out, r.underlying);
  out += '\t';
  put_cstr(out, r.strike_price_currency);
  out += '\t';
  put_cstr(out, r.leg_raw_symbol);
  field_c(out, r.instrument_class);
  field_c(out, r.match_algorithm);
  field_u(out, r.main_fraction);
  field_u(out, r.price_display_format);
  field_u(out, r.sub_fraction);
  field_u(out, r.underlying_product);
  field_c(out, r.security_update_action);
  field_u(out, r.maturity_month);
  field_u(out, r.maturity_day);
  field_u(out, r.maturity_week);
  field_c(out, r.user_defined_instrument);
  field_i(out, r.contract_multiplier_unit);
  field_i(out, r.flow_schedule_type);
  field_u(out, r.tick_rule);
  field_c(out, r.leg_instrument_class);
  field_c(out, r.leg_side);
}

int usage() {
  std::fputs("usage: dbn_dump <file.dbn[.zst]> [--limit N] [--metadata]\n", stderr);
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return usage();
  }
  const char* path = argv[1];
  std::uint64_t limit = 0;
  bool want_metadata = false;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--limit" && i + 1 < argc) {
      char* parse_end = nullptr;
      limit = std::strtoull(argv[++i], &parse_end, 10);
      if (parse_end == argv[i] || *parse_end != '\0') {
        return usage();
      }
    } else if (arg == "--metadata") {
      want_metadata = true;
    } else {
      return usage();
    }
  }

  try {
    DbnReader reader{std::filesystem::path{path}};

    if (want_metadata) {
      const DbnMetadata& m = reader.metadata();
      std::string out;
      out += "version\t" + std::to_string(m.version) + "\n";
      out += "dataset\t" + m.dataset + "\n";
      out += "schema\t" + (m.schema ? std::to_string(*m.schema) : "none") + "\n";
      out += "start\t" + std::to_string(m.start) + "\n";
      out += "end\t" + (m.end ? std::to_string(*m.end) : "none") + "\n";
      out += "limit\t" + (m.limit ? std::to_string(*m.limit) : "none") + "\n";
      out += "stype_in\t" + (m.stype_in ? std::to_string(*m.stype_in) : "none") + "\n";
      out += "stype_out\t" + std::to_string(m.stype_out) + "\n";
      out += "ts_out\t" + std::to_string(m.ts_out ? 1 : 0) + "\n";
      out += "symbol_cstr_len\t" + std::to_string(m.symbol_cstr_len) + "\n";
      for (const std::string& s : m.symbols) {
        out += "symbol\t" + s + "\n";
      }
      for (const SymbolMapping& mapping : m.mappings) {
        for (const MappingInterval& iv : mapping.intervals) {
          out += "mapping\t" + mapping.raw_symbol + "\t" + std::to_string(iv.start_date) + "\t" +
                 std::to_string(iv.end_date) + "\t" + iv.symbol + "\n";
        }
      }
      std::fwrite(out.data(), 1, out.size(), stdout);
      return 0;
    }

    // setvbuf's buffer must stay valid until stdout is closed at exit.
    static std::array<char, kOutBufSize> out_buf;
    std::setvbuf(stdout, out_buf.data(), _IOFBF, out_buf.size());

    std::string line;
    line.reserve(4096);
    while (const RecordHeader* hd = reader.next()) {
      line.clear();
      if (const MboMsg* mbo = record_cast<MboMsg>(*hd)) {
        dump_mbo(line, *mbo);
      } else if (const TradeMsg* trade = record_cast<TradeMsg>(*hd)) {
        dump_trade(line, *trade);
      } else if (const Mbp10Msg* mbp10 = record_cast<Mbp10Msg>(*hd)) {
        dump_mbp10(line, *mbp10);
      } else if (const StatusMsg* status = record_cast<StatusMsg>(*hd)) {
        dump_status(line, *status);
      } else if (const InstrumentDefMsg* def = record_cast<InstrumentDefMsg>(*hd)) {
        dump_definition(line, *def);
      } else {
        std::fprintf(stderr, "unhandled rtype 0x%02x\n", hd->rtype);
        return 1;
      }
      line += '\n';
      std::fwrite(line.data(), 1, line.size(), stdout);

      if (limit != 0 && reader.record_count() >= limit) {
        break;
      }
    }
    std::fflush(stdout);
  } catch (const DbnError& e) {
    std::fprintf(stderr, "dbn_dump: %s\n", e.what());
    return 1;
  }
  return 0;
}
