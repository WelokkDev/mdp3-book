#include "bookreplay/dbn.hpp"

namespace bookreplay {

bool is_supported_dbn_version(std::uint8_t version) noexcept {
  return version == kSupportedDbnVersion;
}

const char* action_name(Action a) noexcept {
  switch (a) {
    case Action::kAdd:
      return "Add";
    case Action::kCancel:
      return "Cancel";
    case Action::kModify:
      return "Modify";
    case Action::kTrade:
      return "Trade";
    case Action::kFill:
      return "Fill";
    case Action::kNone:
      return "None";
    case Action::kClear:
      return "Clear";
  }
  return "<unknown>";
}

const char* side_name(Side s) noexcept {
  switch (s) {
    case Side::kBid:
      return "Bid";
    case Side::kAsk:
      return "Ask";
    case Side::kNone:
      return "None";
  }
  return "<unknown>";
}

}  // namespace bookreplay
