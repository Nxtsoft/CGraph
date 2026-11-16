#include "cgraph/json_text.hpp"
#include "cgraph/protocol.hpp"

#include <cstring>
#include <limits>
#include <utility>

namespace cgraph {

std::vector<std::uint8_t> encode_frame(const nlohmann::json& payload) {
  const auto body = cgraph::dump_json(payload);
  // Reject at the source so send and receive share one ceiling: a body larger
  // than the shared cap is never framed (the peer would reject it on read).
  if (body.size() > kMaxFrameBodyBytes) {
    return {};
  }

  const auto length = static_cast<std::uint32_t>(body.size());
  std::vector<std::uint8_t> frame;
  frame.reserve(4 + body.size());
  frame.push_back(static_cast<std::uint8_t>(length & 0xffU));
  frame.push_back(static_cast<std::uint8_t>((length >> 8U) & 0xffU));
  frame.push_back(static_cast<std::uint8_t>((length >> 16U) & 0xffU));
  frame.push_back(static_cast<std::uint8_t>((length >> 24U) & 0xffU));
  frame.insert(frame.end(), body.begin(), body.end());
  return frame;
}

std::optional<nlohmann::json> decode_frame(const std::vector<std::uint8_t>& frame) {
  if (frame.size() < 4) {
    return std::nullopt;
  }

  const auto length =
      static_cast<std::uint32_t>(frame[0]) |
      (static_cast<std::uint32_t>(frame[1]) << 8U) |
      (static_cast<std::uint32_t>(frame[2]) << 16U) |
      (static_cast<std::uint32_t>(frame[3]) << 24U);
  if (frame.size() - 4 != length) {
    return std::nullopt;
  }

  try {
    return nlohmann::json::parse(frame.begin() + 4, frame.end());
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

nlohmann::json make_request(std::string op, nlohmann::json params) {
  return nlohmann::json{
      {"protocol_version", kProtocolVersion},
      {"op", std::move(op)},
      {"params", std::move(params)},
  };
}

bool protocol_version_matches(const nlohmann::json& message) {
  const auto version = message.find("protocol_version");
  return version != message.end() && version->is_number_unsigned() && version->get<std::uint32_t>() == kProtocolVersion;
}

}  // namespace cgraph
