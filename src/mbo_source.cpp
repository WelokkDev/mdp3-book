#include "bookreplay/mbo_source.hpp"

#include "bookreplay/order.hpp"

#include <string>

#include "hex_byte.hpp"

namespace bookreplay {

namespace {

void require_mbo_schema(const DbnMetadata& meta) {
  if (meta.schema && *meta.schema != kSchemaMbo) {
    throw ReplayError("expected the mbo schema (" + std::to_string(kSchemaMbo) +
                      "), found schema " + std::to_string(*meta.schema));
  }
}

}  // namespace

MboSource::~MboSource() = default;

DbnMboSource::DbnMboSource(const std::filesystem::path& path) : reader_(path) {
  require_mbo_schema(reader_.metadata());
}

DbnMboSource::DbnMboSource(const std::byte* data, std::size_t size) : reader_(data, size) {
  require_mbo_schema(reader_.metadata());
}

DbnMboSource::~DbnMboSource() = default;

const MboMsg* DbnMboSource::next() {
  const RecordHeader* hd = reader_.next();
  if (hd == nullptr) {
    return nullptr;
  }
  const MboMsg* rec = record_cast<MboMsg>(*hd);
  if (rec == nullptr) {
    throw ReplayError("the stream carries a record of rtype " + hex_byte(hd->rtype) +
                      ", which is not mbo");
  }
  return rec;
}

const DbnMetadata& DbnMboSource::metadata() const noexcept {
  return reader_.metadata();
}

MboSpanSource::MboSpanSource(std::span<const MboMsg> records) : records_(records) {}

MboSpanSource::~MboSpanSource() = default;

const MboMsg* MboSpanSource::next() {
  return pos_ < records_.size() ? &records_[pos_++] : nullptr;
}

}  // namespace bookreplay
