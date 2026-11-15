#ifndef HAMMURABI_KV_STATE_MACHINE_H
#define HAMMURABI_KV_STATE_MACHINE_H

#include <hammurabi/kv/command.h>
#include <hammurabi/wire/codec.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace hammurabi::kv {

enum class operation_type : std::uint8_t { get = 1, put = 2, remove = 3, cas = 4, register_client = 5 };

inline constexpr std::size_t default_max_sessions = 10'000;

class state_machine {
public:
    explicit state_machine(std::size_t max_sessions = default_max_sessions);

    // Applies the committed command at log index `index`. Deduplicates by (client_id, sequence), section 5.8.
    // The index is a plain integer, not raft::log_index: hammurabi_kv does not depend on hammurabi_raft.
    std::expected<reply, rejection> apply(std::uint64_t index, const command& cmd);
    [[nodiscard]] result read(const get& op) const; // used by ReadIndex in phase 8

private:
    struct session {
        std::uint64_t last_sequence = 0;
        result last_result{};
        std::uint64_t last_used = 0; // log index of the last request
    };

    result execute(const get& op) const;
    result execute(const put& op);
    result execute(const remove& op);
    result execute(const cas& op);

    void evict_if_full();

    std::map<std::string, std::string> data_;
    std::map<std::uint64_t, session> sessions_; // keyed by client id
    std::size_t max_sessions_;
};

// The log format: u64 client_id │ u64 sequence │ operation.
[[nodiscard]] std::vector<std::byte> encode(const command& cmd);
[[nodiscard]] std::expected<command, wire::decode_error> decode_command(std::span<const std::byte> bytes);

void write(wire::writer& w, const operation& op);
[[nodiscard]] operation read_operation(wire::reader& r);

} // namespace hammurabi::kv

#endif // HAMMURABI_KV_STATE_MACHINE_H
