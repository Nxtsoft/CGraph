#include "cgraph/protocol.hpp"

#include <cstdint>
#include <vector>

int main() {
  const auto request = cgraph::make_request("status", {{"project_root", "/tmp/project"}});
  if (!cgraph::protocol_version_matches(request)) {
    return 1;
  }

  const auto frame = cgraph::encode_frame(request);
  const auto decoded = cgraph::decode_frame(frame);
  if (!decoded.has_value() || (*decoded)["op"] != "status") {
    return 1;
  }

  auto mismatch = request;
  mismatch["protocol_version"] = cgraph::kProtocolVersion + 1;
  if (cgraph::protocol_version_matches(mismatch)) {
    return 1;
  }

  auto truncated = frame;
  truncated.pop_back();
  if (cgraph::decode_frame(truncated).has_value()) {
    return 1;
  }

  // Wire cap is a shared constant used by both send and receive. A frame whose
  // 4-byte header declares a body far larger than the cap is exactly the DoS
  // vector read_frame guards (it rejects before allocating that body). Assert
  // the cap is a real, sane ceiling here so the send/receive paths agree.
  if (cgraph::kMaxFrameBodyBytes == 0 || cgraph::kMaxFrameBodyBytes > (1U << 30U)) {
    return 1;  // 0 would reject everything; >1 GiB would defeat the DoS guard
  }

  // A hand-built frame declaring an oversized length (bytes 0xFFFFFFFF) but
  // carrying no matching body must not decode -- the length/size mismatch is
  // rejected, never trusted into an allocation. Mirrors the header a hostile
  // peer sends; read_frame rejects it on the length check before ever reaching
  // decode_frame.
  const std::vector<std::uint8_t> oversized_header{0xFF, 0xFF, 0xFF, 0xFF};
  if (cgraph::decode_frame(oversized_header).has_value()) {
    return 1;
  }

  // A reply can carry text that is not valid UTF-8: a label or snippet from a
  // Latin-1 source file, or a file name. It must still frame and decode (each
  // bad byte arrives as U+FFFD); throwing here ended the daemon's serve loop.
  const auto odd = cgraph::encode_frame({{"label", "caf\xE9"}, {"path", "\xFF\xFE.py"}});
  const auto odd_decoded = cgraph::decode_frame(odd);
  if (!odd_decoded.has_value() || (*odd_decoded)["label"] != "caf\xEF\xBF\xBD" ||
      (*odd_decoded)["path"] != "\xEF\xBF\xBD\xEF\xBF\xBD.py") {
    return 1;
  }

  return 0;
}
