#include <hammurabi/raft/codec.h>

#include <utility>
#include <variant>

namespace hammurabi::raft {

namespace {

void write(wire::writer& w, const request_vote& m) {
    w.u64(m.term);
    w.u64(m.last_log_index);
    w.u64(m.last_log_term);
}

void write(wire::writer& w, const request_vote_reply& m) {
    w.u64(m.term);
    w.boolean(m.granted);
}

void write(wire::writer& w, const log_entry& e) {
    w.u64(e.term);
    w.u8(std::to_underlying(e.kind));
    w.bytes(e.data);
}

void write(wire::writer& w, const append_entries& m) {
    w.u64(m.term);
    w.u64(m.prev_log_index);
    w.u64(m.prev_log_term);
    w.u32(static_cast<std::uint32_t>(m.entries.size()));
    for (const auto& entry : m.entries) {
        write(w, entry);
    }
    w.u64(m.leader_commit);
}

void write(wire::writer& w, const append_entries_reply& m) {
    w.u64(m.term);
    w.boolean(m.success);
    w.u64(m.match_index);
    w.u64(m.last_log_index);
}

request_vote read_request_vote(wire::reader& r) {
    return request_vote{.term = r.u64(), .last_log_index = r.u64(), .last_log_term = r.u64()};
}

request_vote_reply read_request_vote_reply(wire::reader& r) {
    return request_vote_reply{.term = r.u64(), .granted = r.boolean()};
}

entry_kind read_entry_kind(wire::reader& r) {
    const auto kind = static_cast<entry_kind>(r.u8());
    if (kind != entry_kind::noop && kind != entry_kind::command) {
        r.fail(wire::decode_error::invalid_value);
    }
    return kind;
}

// The smallest encoded entry: u64 term, u8 kind and the u32 length of empty data.
constexpr std::size_t min_encoded_entry_size = 8 + 1 + 4;

std::vector<log_entry> read_entries(wire::reader& r) {
    std::vector<log_entry> entries;
    const std::uint32_t count = r.u32();
    // The count comes from the peer. Check it against the bytes we actually have before reserving memory for it.
    if (count > r.remaining() / min_encoded_entry_size) {
        r.fail(wire::decode_error::truncated);
        return entries;
    }
    entries.reserve(count);
    for (std::uint32_t i = 0; i < count; i++) {
        entries.push_back(log_entry{.term = r.u64(), .kind = read_entry_kind(r), .data = r.bytes()});
    }
    return entries;
}

append_entries read_append_entries(wire::reader& r) {
    return append_entries{.term = r.u64(),
                          .prev_log_index = r.u64(),
                          .prev_log_term = r.u64(),
                          .entries = read_entries(r),
                          .leader_commit = r.u64()};
}

append_entries_reply read_append_entries_reply(wire::reader& r) {
    return append_entries_reply{
        .term = r.u64(), .success = r.boolean(), .match_index = r.u64(), .last_log_index = r.u64()};
}

constexpr message_type type_of(const request_vote&) {
    return message_type::request_vote;
}

constexpr message_type type_of(const request_vote_reply&) {
    return message_type::request_vote_reply;
}

constexpr message_type type_of(const append_entries&) {
    return message_type::append_entries;
}

constexpr message_type type_of(const append_entries_reply&) {
    return message_type::append_entries_reply;
}

} // namespace

std::vector<std::byte> encode(const envelope& env) {
    wire::writer w;
    w.u8(wire_version);
    w.u8(std::to_underlying(std::visit([](const auto& m) { return type_of(m); }, env.msg)));
    w.u32(env.from);
    w.u32(env.to);
    std::visit([&w](const auto& m) { write(w, m); }, env.msg);
    return std::move(w).take();
}

std::size_t encoded_size(const log_entry& entry) {
    return min_encoded_entry_size + entry.data.size();
}

std::expected<envelope, wire::decode_error> decode_envelope(std::span<const std::byte> datagram) {
    wire::reader r{datagram};
    // The reader keeps its first error, so a datagram too short for its header is reported as truncated,
    // not as having an unknown version or type.
    const auto reject = [&r](wire::decode_error error) {
        r.fail(error);
        return std::unexpected(r.finish().error());
    };
    const auto version = r.u8();
    if (version != wire_version) {
        return reject(wire::decode_error::invalid_value);
    }
    const auto type = static_cast<message_type>(r.u8());
    const auto from = r.u32();
    const auto to = r.u32();
    message m{};
    switch (type) {
        case message_type::request_vote:
            m = read_request_vote(r);
            break;
        case message_type::request_vote_reply:
            m = read_request_vote_reply(r);
            break;
        case message_type::append_entries:
            m = read_append_entries(r);
            break;
        case message_type::append_entries_reply:
            m = read_append_entries_reply(r);
            break;
        default:
            return reject(wire::decode_error::invalid_value);
    }
    const auto result = r.finish();
    if (!result) {
        return std::unexpected(result.error());
    }
    return envelope{.from = from, .to = to, .msg = std::move(m)};
}

} // namespace hammurabi::raft
