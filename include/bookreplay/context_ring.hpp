#ifndef BOOKREPLAY_CONTEXT_RING_HPP
#define BOOKREPLAY_CONTEXT_RING_HPP

#include <array>
#include <cstddef>
#include <vector>

namespace bookreplay {

/// The last few items a checker saw, so the first failure it reports arrives
/// with what led up to it.
template <typename T, std::size_t Depth>
class ContextRing {
 public:
  void push(const T& item) {
    items_[next_] = item;
    next_ = (next_ + 1) % Depth;
    if (filled_ < Depth) {
      ++filled_;
    }
  }

  /// Oldest first, ending with the latest push.
  [[nodiscard]] std::vector<T> items() const {
    std::vector<T> out;
    out.reserve(filled_);
    const std::size_t start = (next_ + Depth - filled_) % Depth;
    for (std::size_t i = 0; i < filled_; ++i) {
      out.push_back(items_[(start + i) % Depth]);
    }
    return out;
  }

 private:
  std::array<T, Depth> items_{};
  std::size_t next_ = 0;
  std::size_t filled_ = 0;
};

}  // namespace bookreplay

#endif  // BOOKREPLAY_CONTEXT_RING_HPP
