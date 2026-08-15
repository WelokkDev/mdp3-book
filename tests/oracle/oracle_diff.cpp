// Differential oracle: our decoder against databento-cpp, byte for byte.
//
// This is the CI half. It is stronger than the Python diff in one way — it
// compares the raw bytes of every record rather than an enumerated field
// list, so a field nobody thought to name is still covered — and weaker in
// another, since CI has no licensed market data and can only read the
// committed fixtures.
//
// databento-cpp is an independent C++ implementation of the same published
// format, pinned to a fixed version. It is a development dependency only and
// is never linked into the shipping library.

#include "bookreplay/dbn_reader.hpp"

#include <cstdio>
#include <cstring>
#include <databento/dbn_file_store.hpp>
#include <databento/record.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int fail(const std::string& path, const std::string& what) {
  std::fprintf(stderr, "FAIL %s: %s\n", path.c_str(), what.c_str());
  return 1;
}

int compare_metadata(const std::string& path, const bookreplay::DbnMetadata& ours,
                     const databento::Metadata& theirs) {
  if (ours.version != theirs.version) {
    return fail(path, "metadata version differs");
  }
  if (ours.dataset != theirs.dataset) {
    return fail(path,
                "metadata dataset differs: '" + ours.dataset + "' vs '" + theirs.dataset + "'");
  }
  if (ours.start != theirs.start.time_since_epoch().count()) {
    return fail(path, "metadata start differs");
  }
  if (ours.ts_out != theirs.ts_out) {
    return fail(path, "metadata ts_out differs");
  }
  if (ours.symbol_cstr_len != theirs.symbol_cstr_len) {
    return fail(path, "metadata symbol_cstr_len differs");
  }
  if (ours.symbols != theirs.symbols) {
    return fail(path, "metadata symbol list differs");
  }
  if (ours.not_found != theirs.not_found) {
    return fail(path, "metadata not_found list differs");
  }
  if (ours.mappings.size() != theirs.mappings.size()) {
    return fail(path, "metadata mapping count differs");
  }
  for (std::size_t i = 0; i < ours.mappings.size(); ++i) {
    if (ours.mappings[i].raw_symbol != theirs.mappings[i].raw_symbol) {
      return fail(path, "mapping " + std::to_string(i) + " symbol differs");
    }
    if (ours.mappings[i].intervals.size() != theirs.mappings[i].intervals.size()) {
      return fail(path, "mapping " + std::to_string(i) + " interval count differs");
    }
    for (std::size_t j = 0; j < ours.mappings[i].intervals.size(); ++j) {
      if (ours.mappings[i].intervals[j].symbol != theirs.mappings[i].intervals[j].symbol) {
        return fail(path,
                    "mapping " + std::to_string(i) + " interval " + std::to_string(j) + " differs");
      }
    }
  }
  return 0;
}

int diff_file(const std::filesystem::path& path) {
  const std::string name = path.filename().string();

  bookreplay::DbnReader ours{path};
  databento::DbnFileStore theirs{path};

  if (const int rc = compare_metadata(name, ours.metadata(), theirs.GetMetadata()); rc != 0) {
    return rc;
  }

  std::uint64_t count = 0;
  for (;;) {
    const bookreplay::RecordHeader* mine = ours.next();
    const databento::Record* vendor = theirs.NextRecord();

    if (mine == nullptr && vendor == nullptr) {
      break;
    }
    if (mine == nullptr) {
      return fail(name,
                  "we stopped after " + std::to_string(count) + " records, the oracle had more");
    }
    if (vendor == nullptr) {
      return fail(name, "we produced records past the oracle's end at " + std::to_string(count));
    }

    const std::size_t mine_size = bookreplay::record_bytes(*mine);
    const std::size_t vendor_size = vendor->Size();
    if (mine_size != vendor_size) {
      return fail(name, "record " + std::to_string(count) + " size differs: " +
                            std::to_string(mine_size) + " vs " + std::to_string(vendor_size));
    }

    const auto* mine_bytes = reinterpret_cast<const std::byte*>(mine);
    const auto* vendor_bytes = reinterpret_cast<const std::byte*>(&vendor->Header());
    if (std::memcmp(mine_bytes, vendor_bytes, mine_size) != 0) {
      for (std::size_t i = 0; i < mine_size; ++i) {
        if (mine_bytes[i] != vendor_bytes[i]) {
          return fail(name,
                      "record " + std::to_string(count) + " differs at byte " + std::to_string(i));
        }
      }
    }
    ++count;
  }

  std::printf("ok   %s: %llu records byte-identical\n", name.c_str(),
              static_cast<unsigned long long>(count));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fputs("usage: oracle_diff <file.dbn[.zst]>...\n", stderr);
    return 2;
  }

  int failures = 0;
  for (int i = 1; i < argc; ++i) {
    try {
      failures += diff_file(std::filesystem::path{argv[i]});
    } catch (const std::exception& e) {
      std::fprintf(stderr, "FAIL %s: threw %s\n", argv[i], e.what());
      ++failures;
    }
  }
  return failures == 0 ? 0 : 1;
}
