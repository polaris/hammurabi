#include "raft_string_makers.h" // IWYU pragma: keep

#include <catch2/catch_test_macros.hpp>
#include <hammurabi/raft/format.h>
#include <hammurabi/raft/messages.h>
#include <hammurabi/raft/types.h>

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

TEST_CASE("messages are formatted", "[raft][format]") {
    using namespace hammurabi::raft;

    CHECK(std::format(
              "{}",
              envelope{.from = 1, .to = 2, .msg = request_vote{.term = 2, .last_log_index = 5, .last_log_term = 1}}) ==
          "1→2 RequestVote{term=2, last=5/1}");

    CHECK(std::format("{}", envelope{.from = 2, .to = 1, .msg = request_vote_reply{.term = 3, .granted = false}}) ==
          "2→1 RequestVoteReply{term=3, denied}");

    CHECK(std::format("{}", envelope{.from = 2, .to = 1, .msg = request_vote_reply{.term = 3, .granted = true}}) ==
          "2→1 RequestVoteReply{term=3, granted}");

    CHECK(std::format("{}", envelope{.from = 3,
                                     .to = 5,
                                     .msg = append_entries{.term = 3,
                                                           .prev_log_index = 6,
                                                           .prev_log_term = 10,
                                                           .entries = {},
                                                           .leader_commit = 0}}) ==
          "3→5 AppendEntries{term=3, prev=6/10, entries=0, commit=0}");

    CHECK(std::format("{}", envelope{.from = 3,
                                     .to = 5,
                                     .msg = append_entries{.term = 3,
                                                           .prev_log_index = 6,
                                                           .prev_log_term = 10,
                                                           .entries = {log_entry{}, log_entry{}},
                                                           .leader_commit = 0}}) ==
          "3→5 AppendEntries{term=3, prev=6/10, entries=2, commit=0}");

    CHECK(std::format("{}", envelope{.from = 3,
                                     .to = 5,
                                     .msg =
                                         append_entries_reply{
                                             .term = 3, .success = false, .match_index = 0, .last_log_index = 1}}) ==
          "3→5 AppendEntriesReply{term=3, rejected, match=0, last=1}");

    CHECK(std::format("{}", envelope{.from = 3,
                                     .to = 5,
                                     .msg =
                                         append_entries_reply{
                                             .term = 3, .success = true, .match_index = 0, .last_log_index = 1}}) ==
          "3→5 AppendEntriesReply{term=3, ok, match=0, last=1}");
}
