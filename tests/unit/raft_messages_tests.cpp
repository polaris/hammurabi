#include <catch2/catch_test_macros.hpp>
#include <hammurabi/raft/messages.h>

#include <variant>

TEST_CASE("envelopes compare member-wise", "[raft][messages]") {
    using namespace hammurabi::raft;

    const auto a = envelope{
        .from = 1, .to = 2, .msg = append_entries{.term = 3, .entries = {{.term = 3, .kind = entry_kind::noop}}}};

    auto b = a;

    CHECK(a == b);

    std::get<append_entries>(b.msg).entries[0].term = 5;

    CHECK(a != b);
}

TEST_CASE("envelope comparison reaches into nested log entries", "[raft][messages]") {
    using namespace hammurabi::raft;

    CHECK(envelope{.msg = request_vote{}} != envelope{.msg = request_vote_reply{}});
}