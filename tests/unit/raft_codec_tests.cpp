#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <hammurabi/raft/codec.h>
#include <hammurabi/raft/messages.h>
#include <hammurabi/wire/codec.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace {

// One envelope per message kind, with entries and data so truncation also cuts inside nested fields.
std::vector<hammurabi::raft::envelope> sample_envelopes() {
    using namespace hammurabi::raft;
    return {
        envelope{.from = 1, .to = 2, .msg = request_vote{.term = 3, .last_log_index = 4, .last_log_term = 5}},
        envelope{.from = 1, .to = 2, .msg = request_vote_reply{.term = 3, .granted = true}},
        envelope{.from = 1,
                 .to = 2,
                 .msg = append_entries{.term = 3,
                                       .prev_log_index = 4,
                                       .prev_log_term = 5,
                                       .entries = {{.term = 3, .kind = entry_kind::noop},
                                                   {.term = 3,
                                                    .kind = entry_kind::command,
                                                    .data = {std::byte{0xff}, std::byte{0x01}}}},
                                       .leader_commit = 6}},
        envelope{.from = 1,
                 .to = 2,
                 .msg = append_entries_reply{.term = 3, .success = true, .match_index = 4, .last_log_index = 5}},
    };
}

// Byte offsets inside an encoded append_entries, following the layout in codec.cpp.
constexpr std::size_t header_size = 1 + 1 + 4 + 4;                          // version, type, from, to
constexpr std::size_t entry_count_offset = header_size + 8 + 8 + 8;         // after term, prev_log_index, prev_log_term
constexpr std::size_t first_entry_kind_offset = entry_count_offset + 4 + 8; // after the count and the entry's term

} // namespace

TEST_CASE("round trip for append entries", "[raft][codec]") {
    using namespace hammurabi::raft;

    {
        const auto a = envelope{.from = 1, .to = 2, .msg = append_entries{.term = 3, .entries = {}}};
        CHECK(a == decode_envelope(encode(a)));
    }
    {
        const auto a = envelope{
            .from = 1, .to = 2, .msg = append_entries{.term = 3, .entries = {{.term = 3, .kind = entry_kind::noop}}}};
        CHECK(a == decode_envelope(encode(a)));
    }
    {
        const auto a = envelope{
            .from = 1,
            .to = 2,
            .msg = append_entries{.term = 3,
                                  .entries = {{.term = 3, .kind = entry_kind::noop},
                                              {.term = 3, .kind = entry_kind::command, .data = {std::byte(0xff)}}}}};
        CHECK(a == decode_envelope(encode(a)));
    }
}

TEST_CASE("round trip for append entries reply", "[raft][codec]") {
    using namespace hammurabi::raft;

    const auto a =
        envelope{.from = 1,
                 .to = 2,
                 .msg = append_entries_reply{.term = 42, .success = false, .match_index = 123, .last_log_index = 67}};

    CHECK(a == decode_envelope(encode(a)));
}

TEST_CASE("round trip for request vote", "[raft][codec]") {
    using namespace hammurabi::raft;

    const auto a =
        envelope{.from = 1, .to = 2, .msg = request_vote{.term = 123, .last_log_index = 67, .last_log_term = 42}};

    CHECK(a == decode_envelope(encode(a)));
}

TEST_CASE("round trip for request vote reply", "[raft][codec]") {
    using namespace hammurabi::raft;

    const auto a = envelope{.from = 1, .to = 2, .msg = request_vote_reply{.term = 123, .granted = true}};

    CHECK(a == decode_envelope(encode(a)));
}

TEST_CASE("decode datagram with invalid version", "[raft][codec]") {
    using namespace hammurabi::raft;

    const auto a = envelope{.from = 1, .to = 2, .msg = request_vote_reply{.term = 123, .granted = true}};

    auto datagram = encode(a);
    datagram[0] = std::byte(67);

    const auto result = decode_envelope(datagram);
    CHECK(!result);
    CHECK(result.error() == hammurabi::wire::decode_error::invalid_value);
}

TEST_CASE("every truncation of a valid datagram is a truncated error", "[raft][codec]") {
    using hammurabi::raft::decode_envelope;
    using hammurabi::raft::encode;

    for (const auto& env : sample_envelopes()) {
        const auto datagram = encode(env);
        CAPTURE(env.msg.index());
        for (std::size_t length = 0; length < datagram.size(); ++length) {
            CAPTURE(length);
            const auto result = decode_envelope(std::span{datagram}.first(length));
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error() == hammurabi::wire::decode_error::truncated);
        }
    }
}

TEST_CASE("decode datagram with unknown message type", "[raft][codec]") {
    using namespace hammurabi::raft;

    const auto type = GENERATE(as<std::uint8_t>{}, 0, 5, 255);
    CAPTURE(static_cast<int>(type));

    auto datagram = encode(envelope{.from = 1, .to = 2, .msg = request_vote{.term = 3}});
    datagram[1] = std::byte{type};

    const auto result = decode_envelope(datagram);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == hammurabi::wire::decode_error::invalid_value);
}

TEST_CASE("encoded size matches the bytes an entry adds to a datagram", "[raft][codec]") {
    using namespace hammurabi::raft;

    const auto entry = GENERATE(log_entry{.term = 3, .kind = entry_kind::noop},
                                log_entry{.term = 3, .kind = entry_kind::command, .data = {std::byte{0xff}}},
                                log_entry{.term = 3, .kind = entry_kind::command, .data = std::vector<std::byte>(100)});
    CAPTURE(entry.data.size());

    const auto without = encode(envelope{.msg = append_entries{.term = 3}});
    const auto with = encode(envelope{.msg = append_entries{.term = 3, .entries = {entry}}});

    CHECK(with.size() - without.size() == encoded_size(entry));
}

TEST_CASE("decode entry with unknown kind", "[raft][codec]") {
    using namespace hammurabi::raft;

    const auto kind = GENERATE(as<std::uint8_t>{}, 0, 3, 255);
    CAPTURE(static_cast<int>(kind));

    auto datagram =
        encode(envelope{.msg = append_entries{.term = 3, .entries = {{.term = 3, .kind = entry_kind::noop}}}});
    // If the layout changes, fail here instead of overwriting some other field.
    REQUIRE(datagram[first_entry_kind_offset] == std::byte{std::to_underlying(entry_kind::noop)});
    datagram[first_entry_kind_offset] = std::byte{kind};

    const auto result = decode_envelope(datagram);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == hammurabi::wire::decode_error::invalid_value);
}

TEST_CASE("decode entry count larger than the datagram", "[raft][codec]") {
    using namespace hammurabi::raft;

    // Without the guard in read_entries this would reserve memory for 4 billion entries.
    auto datagram = encode(envelope{.msg = append_entries{.term = 3}});
    std::ranges::fill(std::span{datagram}.subspan(entry_count_offset, 4), std::byte{0xff});

    const auto result = decode_envelope(datagram);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == hammurabi::wire::decode_error::truncated);
}

TEST_CASE("decode datagram with trailing bytes", "[raft][codec]") {
    using hammurabi::raft::decode_envelope;
    using hammurabi::raft::encode;

    for (const auto& env : sample_envelopes()) {
        CAPTURE(env.msg.index());
        auto datagram = encode(env);
        datagram.push_back(std::byte{0});

        const auto result = decode_envelope(datagram);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == hammurabi::wire::decode_error::trailing_bytes);
    }
}
