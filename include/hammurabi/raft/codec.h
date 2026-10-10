#ifndef HAMMURABI_RAFT_CODEC_H
#define HAMMURABI_RAFT_CODEC_H

#include <hammurabi/raft/messages.h>
#include <hammurabi/raft/types.h>
#include <hammurabi/wire/codec.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace hammurabi::raft {

inline constexpr std::uint8_t wire_version = 1;

enum class message_type : std::uint8_t {
    request_vote = 1,
    request_vote_reply = 2,
    append_entries = 3,
    append_entries_reply = 4
};

[[nodiscard]] std::vector<std::byte> encode(const envelope& env);
[[nodiscard]] std::expected<envelope, wire::decode_error> decode_envelope(std::span<const std::byte> datagram);

[[nodiscard]] std::size_t encoded_size(const log_entry& entry); // used for batching in step 4.4

} // namespace hammurabi::raft

#endif // HAMMURABI_RAFT_CODEC_H
