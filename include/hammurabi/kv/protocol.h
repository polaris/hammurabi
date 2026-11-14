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

struct request {
    operation op;
    bool operator==(const request&) const = default;
}; // step 5.2 adds client_id and sequence

struct not_leader {
    std::optional<std::string> leader_address{};
    bool operator==(const not_leader&) const = default;
}; // client address of the leader, if known

enum class rejection : std::uint8_t { too_large = 1, malformed = 2, session_expired = 3 };

using response = std::variant<result, not_leader, rejection>;

[[nodiscard]] std::vector<std::byte> encode(const request& req);
[[nodiscard]] std::vector<std::byte> encode(const response& resp);
[[nodiscard]] std::expected<request, wire::decode_error> decode_request(std::span<const std::byte> bytes);
[[nodiscard]] std::expected<response, wire::decode_error> decode_response(std::span<const std::byte> bytes);

} // namespace hammurabi::kv

#endif // HAMMURABI_KV_PROTOCOL_H
