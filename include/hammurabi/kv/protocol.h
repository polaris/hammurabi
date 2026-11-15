#ifndef HAMMURABI_KV_PROTOCOL_H
#define HAMMURABI_KV_PROTOCOL_H

#include <hammurabi/kv/command.h>
#include <hammurabi/wire/codec.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace hammurabi::kv {

inline constexpr std::uint8_t client_wire_version = 1;

// Same fields as command, but the client format: it evolves independently of the log format.
// The rules for client_id and sequence are checked by the replica, not by the decoder (shape, not policy).
struct request {
    std::uint64_t client_id = 0;
    std::uint64_t sequence = 0;
    operation op;
    bool operator==(const request&) const = default;
};

struct not_leader {
    std::optional<std::string> leader_address{};
    bool operator==(const not_leader&) const = default;
}; // client address of the leader, if known

using response = std::variant<result, registered, not_leader, rejection>;

[[nodiscard]] std::vector<std::byte> encode(const request& req);
[[nodiscard]] std::vector<std::byte> encode(const response& resp);
[[nodiscard]] std::expected<request, wire::decode_error> decode_request(std::span<const std::byte> bytes);
[[nodiscard]] std::expected<response, wire::decode_error> decode_response(std::span<const std::byte> bytes);

} // namespace hammurabi::kv

#endif // HAMMURABI_KV_PROTOCOL_H
