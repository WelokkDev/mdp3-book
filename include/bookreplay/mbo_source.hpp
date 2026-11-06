#ifndef BOOKREPLAY_MBO_SOURCE_HPP
#define BOOKREPLAY_MBO_SOURCE_HPP

#include "bookreplay/dbn.hpp"
#include "bookreplay/dbn_reader.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace bookreplay {

/// Every record of an mbo stream, in stream order: what an engine that keeps a
/// book needs, where `TradeSource` hands a trades-only one its prints.
class MboSource {
 public:
  MboSource() = default;
  virtual ~MboSource();
  MboSource(const MboSource&) = delete;
  MboSource& operator=(const MboSource&) = delete;
  MboSource(MboSource&&) = delete;
  MboSource& operator=(MboSource&&) = delete;

  /// nullptr at end of stream; the returned pointer dies at the next call.
  [[nodiscard]] virtual const MboMsg* next() = 0;
};

class DbnMboSource final : public MboSource {
 public:
  /// Throws ReplayError if the file names a schema other than mbo.
  explicit DbnMboSource(const std::filesystem::path& path);
  DbnMboSource(const std::byte* data, std::size_t size);
  ~DbnMboSource() override;

  /// Throws ReplayError on a record that is not an MboMsg.
  [[nodiscard]] const MboMsg* next() override;
  [[nodiscard]] const DbnMetadata& metadata() const noexcept;

 private:
  DbnReader reader_;
};

/// Replays records the caller already holds, so one decoded day can be run
/// under several configurations. Non-owning.
class MboSpanSource final : public MboSource {
 public:
  explicit MboSpanSource(std::span<const MboMsg> records);
  MboSpanSource(std::vector<MboMsg>&&) = delete;
  ~MboSpanSource() override;

  [[nodiscard]] const MboMsg* next() override;

 private:
  std::span<const MboMsg> records_;
  std::size_t pos_ = 0;
};

}  // namespace bookreplay

#endif  // BOOKREPLAY_MBO_SOURCE_HPP
