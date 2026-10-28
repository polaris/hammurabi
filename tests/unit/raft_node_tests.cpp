#include "raft_string_makers.h" // IWYU pragma: keep

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <hammurabi/raft/messages.h>
#include <hammurabi/raft/node.h>
#include <hammurabi/raft/types.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <variant>
#include <vector>

namespace {

using namespace hammurabi::raft;

// The election timeout is random in [t, 2t).
constexpr int min_timeout = config{}.election_timeout_ticks;
constexpr int max_timeout = 2 * min_timeout;

config make_config(node_id self, std::vector<node_id> members, std::uint64_t seed = 0) {
    return config{.self = self, .members = std::move(members), .random_seed = seed};
}

int ticks_until_election(node& n, int limit = 100) {
    for (int i = 1; i <= limit; ++i) {
        n.tick();
        if (n.current_role() != role::follower) {
            return i;
        }
    }
    return -1;
}

log_entry entry(term_t term) {
    return log_entry{.term = term};
}

std::vector<log_entry> log_of(const std::vector<term_t>& terms) {
    std::vector<log_entry> entries;
    entries.reserve(terms.size());
    for (const auto term : terms) {
        entries.push_back(entry(term));
    }
    return entries;
}

request_vote_reply ask_for_vote(node& n, const envelope& request) {
    n.receive(request);
    const auto output = n.take_output();
    REQUIRE(output.messages.size() == 1);
    REQUIRE(output.messages[0].from == request.to);
    REQUIRE(output.messages[0].to == request.from);
    REQUIRE(std::holds_alternative<request_vote_reply>(output.messages[0].msg));
    return std::get<request_vote_reply>(output.messages[0].msg);
}

void deliver_all(std::vector<node>& nodes) {
    while (true) {
        std::vector<envelope> in_flight;
        for (auto& n : nodes) {
            auto output = n.take_output();
            in_flight.insert(in_flight.end(), output.messages.begin(), output.messages.end());
        }
        for (const auto& env : in_flight) {
            assert(env.to >= 1 && env.to <= nodes.size()); // node ids are 1..N in vector order
            nodes[env.to - 1].receive(env);
        }
        if (in_flight.empty()) {
            return;
        }
    }
}

struct stable_leader {
    int ticks = 0;
    std::size_t leader{}; // index into nodes
    std::vector<std::size_t> followers{};
};

// Ticks every node and delivers all messages until there is one leader and
// everyone else is a follower. Fails the test if that doesn't happen within
// the limit. (Not an optional + REQUIRE at the call site: clang-tidy can't see
// that a failed REQUIRE ends the test, so it flags every access after it.)
stable_leader ticks_until_stable_leader(std::vector<node>& nodes, int limit = 100) {
    for (int i = 1; i <= limit; ++i) {
        for (auto& n : nodes) {
            n.tick();
        }
        deliver_all(nodes);
        bool leader_elected = false;
        std::vector<std::size_t> followers;
        std::size_t leader = 0;
        for (std::size_t j = 0; j < nodes.size(); ++j) {
            if (nodes[j].current_role() == role::leader) {
                leader_elected = true;
                leader = j;
            } else if (nodes[j].current_role() == role::follower) {
                followers.push_back(j);
            }
        }
        if (leader_elected && followers.size() == nodes.size() - 1) {
            return stable_leader{.ticks = i, .leader = leader, .followers = followers};
        }
    }
    FAIL("no stable leader within " << limit << " ticks");
    return {}; // unreachable: FAIL throws, but the compiler can't see that either
}

// Makes node 1 of {1, 2, 3} leader of term 1 and drops its output (request_votes, first heartbeats).
void elect(node& n) {
    REQUIRE(ticks_until_election(n) != -1);
    n.receive(envelope{.from = 2, .to = 1, .msg = request_vote_reply{.term = n.current_term(), .granted = true}});
    REQUIRE(n.current_role() == role::leader);
    (void)n.take_output(); // take_output is [[nodiscard]]
}

} // namespace

TEST_CASE("memory_storage appends after the last entry", "[storage]") {
    memory_storage store;

    store.append(std::vector{entry(1), entry(1)});
    store.append(std::vector{entry(2)});

    REQUIRE(store.load().log == std::vector{entry(1), entry(1), entry(2)});
}

TEST_CASE("memory_storage truncates from a 1-based index", "[storage]") {
    memory_storage store{persistent_state{.log = {entry(1), entry(2), entry(3), entry(4), entry(5)}}};

    store.truncate(3); // removes entries 3, 4 and 5
    REQUIRE(store.load().log == std::vector{entry(1), entry(2)});

    store.truncate(1); // removes everything
    REQUIRE(store.load().log.empty());
}

TEST_CASE("constructor loads state from storage", "[election]") {
    memory_storage store{persistent_state{.hard = {.term = 5}}};
    node n{make_config(1, {1}), &store};

    ticks_until_election(n);

    REQUIRE(n.current_term() == 6);
    REQUIRE(store.load().hard == hard_state{.term = 6, .voted_for = 1});
}

TEST_CASE("one-node cluster elects itself leader", "[election]") {
    memory_storage store;
    node n{make_config(1, {1}), &store};

    const int ticks = ticks_until_election(n);

    REQUIRE(n.take_output().messages.empty());
    REQUIRE(ticks > 0);
    REQUIRE(ticks < max_timeout);
    REQUIRE(n.current_role() == role::leader);
    REQUIRE(n.current_term() == 1);
}

TEST_CASE("three-node cluster sends exactly two request_vote messages", "[election]") {
    memory_storage store;
    node n{make_config(1, {1, 2, 3}), &store};

    ticks_until_election(n);

    const auto output = n.take_output();

    REQUIRE(n.current_role() == role::candidate);
    REQUIRE(output.messages.size() == 2);

    std::set<node_id> peers;
    for (const envelope& env : output.messages) {
        REQUIRE(env.from == 1);
        peers.insert(env.to);
        REQUIRE(std::holds_alternative<request_vote>(env.msg));
        REQUIRE(std::get<request_vote>(env.msg).term == 1);
    }
    std::set<node_id> expected_peers{2, 3};
    REQUIRE(peers == expected_peers);
}

TEST_CASE("votes are persisted", "[election]") {
    memory_storage store;
    node n{make_config(1, {1, 2, 3}), &store};

    ticks_until_election(n);

    REQUIRE(store.load().hard == hard_state{.term = 1, .voted_for = 1});
}

TEST_CASE("timeout is random but reproducible", "[election]") {
    // Same seed, same count
    {
        memory_storage store1, store2;
        node n1{make_config(1, {1}, 42), &store1};
        node n2{make_config(1, {1}, 42), &store2};
        const auto count1 = ticks_until_election(n1);
        REQUIRE(count1 > 0);
        const auto count2 = ticks_until_election(n2);
        REQUIRE(count2 > 0);
        REQUIRE(count1 == count2);
    }

    // Different seed, different count
    {
        std::set<int> counts;

        for (std::uint64_t seed = 0; seed < 20; seed++) {
            memory_storage store;
            node n{make_config(1, {1}, seed), &store};
            const auto ticks2 = ticks_until_election(n);
            REQUIRE(ticks2 >= min_timeout);
            REQUIRE(ticks2 < max_timeout);
            counts.insert(ticks2);
        }

        REQUIRE(counts.size() > 1);
    }
}

TEST_CASE("already voted another node -> refused", "[election]") {
    memory_storage store{persistent_state{.hard = hard_state{.term = 42, .voted_for = 2}}};
    node n{make_config(1, {1, 2, 3}), &store};

    const auto reply = ask_for_vote(n, envelope{.from = 3, .to = 1, .msg = request_vote{.term = 42}});

    REQUIRE(reply.granted == false);
    REQUIRE(reply.term == 42);
}

TEST_CASE("same node asks again -> granted", "[election]") {
    // The candidate's first reply got lost, so it asks again.
    memory_storage store{persistent_state{.hard = hard_state{.term = 42, .voted_for = std::nullopt}}};
    node n{make_config(1, {1, 2, 3}), &store};
    const envelope request{.from = 2, .to = 1, .msg = request_vote{.term = 42}};

    const auto first = ask_for_vote(n, request);
    const auto second = ask_for_vote(n, request);

    REQUIRE(first.granted == true);
    REQUIRE(second.granted == true);
    REQUIRE(second.term == 42);
    REQUIRE(store.load().hard.voted_for == 2);
}

TEST_CASE("older term -> refused, and the reply carries our term", "[election]") {
    memory_storage store{persistent_state{.hard = hard_state{.term = 42, .voted_for = std::nullopt}}};
    node n{make_config(1, {1, 2, 3}), &store};

    const auto reply = ask_for_vote(n, envelope{.from = 2, .to = 1, .msg = request_vote{.term = 41}});

    REQUIRE(reply.granted == false);
    REQUIRE(reply.term == 42);
}

TEST_CASE("candidate refuses another candidate in the same term", "[election]") {
    // No special case per role: a candidate already voted for itself, so can_vote is false for everyone else.
    memory_storage store{persistent_state{.hard = hard_state{.term = 42, .voted_for = std::nullopt}}};
    node n{make_config(1, {1, 2, 3}), &store};

    ticks_until_election(n);
    static_cast<void>(n.take_output());
    REQUIRE(n.current_role() == role::candidate);
    REQUIRE(n.current_term() == 43);

    const auto reply = ask_for_vote(n, envelope{.from = 2, .to = 1, .msg = request_vote{.term = 43}});

    REQUIRE(reply.granted == false);
    REQUIRE(reply.term == 43);
    REQUIRE(n.current_role() == role::candidate);
    REQUIRE(store.load().hard.voted_for == 1);
}

TEST_CASE("newer term -> becomes follower and grants", "[election]") {
    memory_storage store{persistent_state{.hard = hard_state{.term = 42, .voted_for = std::nullopt}}};
    node n{make_config(1, {1, 2, 3}), &store};

    ticks_until_election(n);
    static_cast<void>(n.take_output());

    const auto reply = ask_for_vote(n, envelope{.from = 2, .to = 1, .msg = request_vote{.term = 44}});

    REQUIRE(n.current_role() == role::follower);
    REQUIRE(reply.granted == true);
    REQUIRE(reply.term == 44);
    REQUIRE(store.load().hard.voted_for == 2);
}

TEST_CASE("after granting -> load() shows voted_for saved", "[election]") {
    memory_storage store{persistent_state{.hard = hard_state{.term = 42, .voted_for = std::nullopt}}};
    node n{make_config(1, {1, 2, 3}), &store};

    n.receive(envelope{.from = 2, .to = 1, .msg = request_vote{.term = 43}});

    const auto ps = store.load();
    REQUIRE(ps.hard.voted_for == 2);
    REQUIRE(ps.hard.term == 43);
}

TEST_CASE("vote only for a candidate whose log is at least as up to date", "[election]") {
    struct row {
        term_t last_log_term;
        log_index last_log_index;
        bool granted;
        const char* why;
    };
    const auto r = GENERATE(values<row>({
        {.last_log_term = 4,
         .last_log_index = 2,
         .granted = true,
         .why = "a higher last term wins, even with a shorter log"},
        {.last_log_term = 3, .last_log_index = 7, .granted = true, .why = "equal"},
        {.last_log_term = 3, .last_log_index = 6, .granted = false, .why = "same term, shorter log"},
        {.last_log_term = 2, .last_log_index = 9, .granted = false, .why = "lower last term, even with a longer log"},
    }));
    INFO(r.why);

    // Our last entry is (term 3, index 7).
    memory_storage store{persistent_state{
        .hard = hard_state{.term = 3, .voted_for = std::nullopt},
        .log = {entry(1), entry(1), entry(1), entry(2), entry(2), entry(2), entry(3)},
    }};
    node n{make_config(1, {1, 2, 3}), &store};

    // A candidate's term is never below its last log term. Otherwise the term stays ours, so only the logs decide.
    const term_t term = std::max<term_t>(3, r.last_log_term);
    const auto reply = ask_for_vote(
        n, envelope{
               .from = 2,
               .to = 1,
               .msg = request_vote{.term = term, .last_log_index = r.last_log_index, .last_log_term = r.last_log_term},
           });

    REQUIRE(reply.granted == r.granted);
    REQUIRE(reply.term == term);
}

TEST_CASE("leader refuses append_entries from an older term", "[election]") {
    memory_storage store{persistent_state{.hard = hard_state{.term = 5}, .log = {}}};
    node n{make_config(1, {1}), &store};

    ticks_until_election(n);

    REQUIRE(n.current_role() == role::leader);
    REQUIRE(n.current_term() == 6);

    n.receive(envelope{.from = 2, .to = 1, .msg = append_entries{.term = 5}});

    REQUIRE(n.take_output().messages ==
            std::vector{envelope{.from = 1, .to = 2, .msg = append_entries_reply{.term = 6, .success = false}}});
}

TEST_CASE("exactly one leader", "[election]") {
    memory_storage store1, store2, store3;
    std::vector<node> nodes = {
        node{make_config(1, {1, 2, 3}, 42), &store1},
        node{make_config(2, {1, 2, 3}, 67), &store2},
        node{make_config(3, {1, 2, 3}, 12), &store3},
    };

    const auto sl = ticks_until_stable_leader(nodes);

    for (const auto f : sl.followers) {
        REQUIRE(nodes[f].leader_hint() == nodes[sl.leader].id());
    }
    REQUIRE(nodes[0].current_term() == nodes[1].current_term());
    REQUIRE(nodes[0].current_term() == nodes[2].current_term());
}

TEST_CASE("heartbeats prevent elections over 1000 ticks", "[election]") {
    memory_storage store1, store2, store3;
    std::vector<node> nodes = {
        node{make_config(1, {1, 2, 3}, 42), &store1},
        node{make_config(2, {1, 2, 3}, 67), &store2},
        node{make_config(3, {1, 2, 3}, 12), &store3},
    };

    const auto sl = ticks_until_stable_leader(nodes);
    const auto& leader = nodes[sl.leader];
    const auto term = leader.current_term();

    // deliver_all drains every message, so a whole election can start and finish
    // within one iteration. Watching for candidates would miss it; a new term can't.
    for (int i = 1; i <= 1000; ++i) {
        for (auto& n : nodes) {
            n.tick();
        }
        deliver_all(nodes);
        CAPTURE(i);
        REQUIRE(leader.current_role() == role::leader);
        for (const auto& n : nodes) {
            REQUIRE(n.current_term() == term);
        }
    }
}

TEST_CASE("leaders and candidates step down when they see a higher term", "[election]") {
    memory_storage store1, store2, store3;
    std::vector<node> nodes = {
        node{make_config(1, {1, 2, 3}, 42), &store1},
        node{make_config(2, {1, 2, 3}, 67), &store2},
        node{make_config(3, {1, 2, 3}, 12), &store3},
    };

    const auto sl = ticks_until_stable_leader(nodes);

    auto& leader = nodes[sl.leader];
    auto& follower = nodes[sl.followers[0]];

    SECTION("leader") {
        // A request_vote, not append_entries: the step-down rule in receive() covers every message type.
        const auto new_term = leader.current_term() + 1;
        leader.receive(envelope{.from = follower.id(), .to = leader.id(), .msg = request_vote{.term = new_term}});

        REQUIRE(leader.current_role() == role::follower);
        REQUIRE(leader.current_term() == new_term);
    }

    SECTION("candidate") {
        REQUIRE(ticks_until_election(follower) != -1);
        const auto new_term = follower.current_term() + 1;
        follower.receive(envelope{.from = leader.id(), .to = follower.id(), .msg = append_entries{.term = new_term}});

        REQUIRE(follower.current_role() == role::follower);
        REQUIRE(follower.current_term() == new_term);
    }
}

TEST_CASE("a reply delivered twice is still one vote", "[election]") {
    memory_storage store;
    node n{make_config(1, {1, 2, 3, 4, 5}, 42), &store};

    ticks_until_election(n);
    REQUIRE(n.current_role() == role::candidate);
    n.receive(envelope{.from = 2, .to = 1, .msg = request_vote_reply{.term = n.current_term(), .granted = true}});
    REQUIRE(n.current_role() == role::candidate);
    n.receive(envelope{.from = 2, .to = 1, .msg = request_vote_reply{.term = n.current_term(), .granted = true}});
    REQUIRE(n.current_role() == role::candidate);
    n.receive(envelope{.from = 3, .to = 1, .msg = request_vote_reply{.term = n.current_term(), .granted = true}});
    REQUIRE(n.current_role() == role::leader);
}

TEST_CASE("follower replicates leader's log", "[replication]") {
    const auto leader_log = log_of({1, 2, 3});
    memory_storage store;
    node n{make_config(1, {1, 2, 3}, 42), &store};
    n.receive(
        envelope{.from = 2,
                 .to = n.id(),
                 .msg = append_entries{
                     .term = 3, .prev_log_index = 0, .prev_log_term = 0, .entries = leader_log, .leader_commit = 3}});
    REQUIRE(n.commit_index() == 3);
    REQUIRE(std::ranges::equal(leader_log, n.log()));
}

TEST_CASE("follower commits only as far as it has verified", "[replication]") {
    memory_storage store{persistent_state{.log = log_of({1, 1, 1, 2, 2, 2})}};
    node n{make_config(1, {1, 2, 3}, 42), &store};
    n.receive(
        envelope{.from = 2,
                 .to = n.id(),
                 .msg = append_entries{
                     .term = 3, .prev_log_index = 3, .prev_log_term = 1, .entries = {entry(2)}, .leader_commit = 6}});
    const auto output = n.take_output();
    REQUIRE(n.commit_index() == 4); // not 6: entries 5–6 were not verified against the leader
    REQUIRE(output.messages ==
            std::vector{envelope{
                .from = 1,
                .to = 2,
                .msg = append_entries_reply{.term = 3, .success = true, .match_index = 4, .last_log_index = 6}}});
}

// Figure 7 of the Raft paper: the leader (term 8) has the log 1 1 1 4 4 5 5 6 6 6 8.
// Followers (a)–(f) hold the logs below.

TEST_CASE("follower accepts append_entries only if it has the entry at prev_log_index (Figure 7)", "[replication]") {
    // The leader sends its newest entry (index 11, term 8), which follows index 10 with term 6.
    struct row {
        std::vector<log_entry> log;
        append_entries_reply reply;
        std::vector<log_entry> final_log;
        const char* why;
    };
    const auto r = GENERATE(values<row>({
        {.log = log_of({1, 1, 1, 4, 4, 5, 5, 6, 6}),
         .reply = {.term = 8, .success = false, .last_log_index = 9},
         .final_log = log_of({1, 1, 1, 4, 4, 5, 5, 6, 6}),
         .why = "(a) log too short (9 < 10)"},
        {.log = log_of({1, 1, 1, 4}),
         .reply = {.term = 8, .success = false, .last_log_index = 4},
         .final_log = log_of({1, 1, 1, 4}),
         .why = "(b) log too short (4 < 10)"},
        {.log = log_of({1, 1, 1, 4, 4, 5, 5, 6, 6, 6, 6}),
         .reply = {.term = 8, .success = true, .match_index = 11, .last_log_index = 11},
         .final_log = log_of({1, 1, 1, 4, 4, 5, 5, 6, 6, 6, 8}),
         .why = "(c) index 10 matches; the term-6 entry at 11 conflicts with the term-8 entry"},
        {.log = log_of({1, 1, 1, 4, 4, 5, 5, 6, 6, 6, 7, 7}),
         .reply = {.term = 8, .success = true, .match_index = 11, .last_log_index = 11},
         .final_log = log_of({1, 1, 1, 4, 4, 5, 5, 6, 6, 6, 8}),
         .why = "(d) index 10 matches; 11–12 (term 7) are dropped"},
        {.log = log_of({1, 1, 1, 4, 4, 4, 4}),
         .reply = {.term = 8, .success = false, .last_log_index = 7},
         .final_log = log_of({1, 1, 1, 4, 4, 4, 4}),
         .why = "(e) log too short (7 < 10)"},
        {.log = log_of({1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 3}),
         .reply = {.term = 8, .success = false, .last_log_index = 11},
         .final_log = log_of({1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 3}),
         .why = "(f) term mismatch: there's an entry at 10, but its term is 3"},
    }));
    INFO(r.why);

    memory_storage store{persistent_state{.log = r.log}};
    node n{make_config(1, {1, 2, 3}, 42), &store};
    n.receive(envelope{.from = 2,
                       .to = n.id(),
                       .msg = append_entries{
                           .term = 8,
                           .prev_log_index = 10,
                           .prev_log_term = 6,
                           .entries = {entry(8)},
                           .leader_commit = 0,
                       }});

    REQUIRE(n.take_output().messages == std::vector{envelope{.from = 1, .to = 2, .msg = r.reply}});
    REQUIRE(std::ranges::equal(r.final_log, n.log()));
}

TEST_CASE("follower ends up with the leader's log once prev_log_index matches (Figure 7)", "[replication]") {
    // The leader has backed up to an entry the follower has, and sends everything after it.
    struct row {
        std::vector<log_entry> log;
        append_entries request;
        append_entries_reply reply;
        const char* why;
    };
    const auto r = GENERATE(values<row>({
        {.log = log_of({1, 1, 1, 4, 4, 5, 5, 6, 6}),
         .request = {.term = 8, .prev_log_index = 9, .prev_log_term = 6, .entries = log_of({6, 8})},
         .reply = {.term = 8, .success = true, .match_index = 11, .last_log_index = 11},
         .why = "(a) appends 10–11"},
        {.log = log_of({1, 1, 1, 4}),
         .request = {.term = 8, .prev_log_index = 4, .prev_log_term = 4, .entries = log_of({4, 5, 5, 6, 6, 6, 8})},
         .reply = {.term = 8, .success = true, .match_index = 11, .last_log_index = 11},
         .why = "(b) appends 5–11"},
        {.log = log_of({1, 1, 1, 4, 4, 4, 4}),
         .request = {.term = 8, .prev_log_index = 5, .prev_log_term = 4, .entries = log_of({5, 5, 6, 6, 6, 8})},
         .reply = {.term = 8, .success = true, .match_index = 11, .last_log_index = 11},
         .why = "(e) truncates 6–7 (the term-4 entries), then appends 6–11"},
        {.log = log_of({1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 3}),
         .request = {.term = 8, .prev_log_index = 3, .prev_log_term = 1, .entries = log_of({4, 4, 5, 5, 6, 6, 6, 8})},
         .reply = {.term = 8, .success = true, .match_index = 11, .last_log_index = 11},
         .why = "(f) truncates 4–11 (all of terms 2 and 3), then appends 4–11"},
    }));
    INFO(r.why);

    memory_storage store{persistent_state{.log = r.log}};
    node n{make_config(1, {1, 2, 3}, 42), &store};
    n.receive(envelope{.from = 2, .to = n.id(), .msg = r.request});

    REQUIRE(n.take_output().messages == std::vector{envelope{.from = 1, .to = 2, .msg = r.reply}});
    REQUIRE(std::ranges::equal(log_of({1, 1, 1, 4, 4, 5, 5, 6, 6, 6, 8}), n.log()));
}

TEST_CASE("a late, older append_entries does not truncate matching entries", "[replication]") {
    // M2 (entries 10–11) arrived first; now the older M1 (entry 10 only) arrives.
    memory_storage store{persistent_state{.log = log_of({1, 1, 1, 4, 4, 5, 5, 6, 6, 6, 8})}};
    node n{make_config(1, {1, 2, 3}), &store};
    n.receive(
        envelope{.from = 2,
                 .to = 1,
                 .msg = append_entries{.term = 8, .prev_log_index = 9, .prev_log_term = 6, .entries = log_of({6})}});

    REQUIRE(n.take_output().messages ==
            std::vector{envelope{
                .from = 1,
                .to = 2,
                .msg = append_entries_reply{.term = 8, .success = true, .match_index = 10, .last_log_index = 11}}});
    REQUIRE(store.load().log == log_of({1, 1, 1, 4, 4, 5, 5, 6, 6, 6, 8})); // entry 11 survived, also on "disk"
}

TEST_CASE("propose on a non-leader returns not_leader and changes nothing", "[replication]") {
    memory_storage store;
    node n{make_config(1, {1, 2, 3}), &store};

    SECTION("follower that knows the leader") {
        n.receive(envelope{.from = 2, .to = 1, .msg = append_entries{.term = 1}});
        (void)n.take_output();
        const auto r = n.propose({std::byte{42}});
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().leader_hint == 2);
    }
    SECTION("candidate") {
        REQUIRE(ticks_until_election(n) != -1);
        (void)n.take_output();
        const auto r = n.propose({std::byte{42}});
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().leader_hint == std::nullopt);
    }

    REQUIRE(n.log().empty());
    REQUIRE(store.load().log.empty());
    REQUIRE(n.take_output().messages.empty());
}

TEST_CASE("leader appends a proposed command, persists it and sends it to every peer", "[replication]") {
    memory_storage store;
    node n{make_config(1, {1, 2, 3}), &store};
    elect(n);

    const std::vector command{std::byte{42}};
    const auto r = n.propose(command);

    const auto e = log_entry{.term = 1, .kind = entry_kind::command, .data = command};
    REQUIRE(r == 1); // the index the client waits on (phase 6)
    REQUIRE(std::ranges::equal(n.log(), std::vector{e}));
    REQUIRE(store.load().log == std::vector{e}); // on "disk" before anything left the node

    const auto ae =
        append_entries{.term = 1, .prev_log_index = 0, .prev_log_term = 0, .entries = {e}, .leader_commit = 0};
    REQUIRE(n.take_output().messages ==
            std::vector{envelope{.from = 1, .to = 2, .msg = ae}, envelope{.from = 1, .to = 3, .msg = ae}});
}

TEST_CASE("a heartbeat resends entries the peers have not confirmed", "[replication]") {
    memory_storage store;
    node n{make_config(1, {1, 2, 3}), &store};
    elect(n);

    const std::vector command{std::byte{42}};
    REQUIRE(n.propose(command) == 1);
    (void)n.take_output(); // the network lost both append_entries

    for (int i = 0; i < config{}.heartbeat_interval_ticks; ++i) {
        n.tick();
    }

    const auto e = log_entry{.term = 1, .kind = entry_kind::command, .data = command};
    const auto ae =
        append_entries{.term = 1, .prev_log_index = 0, .prev_log_term = 0, .entries = {e}, .leader_commit = 0};
    REQUIRE(n.take_output().messages ==
            std::vector{envelope{.from = 1, .to = 2, .msg = ae}, envelope{.from = 1, .to = 3, .msg = ae}});
}

TEST_CASE("a vote from an older term does not count", "[election]") {
    memory_storage store;
    node n{make_config(1, {1, 2, 3}), &store};
    for (int i = 0; i < 2 * max_timeout && n.current_term() < 2; ++i) {
        n.tick(); // the first election times out, the second starts in term 2
    }
    REQUIRE(n.current_term() == 2);
    REQUIRE(n.current_role() == role::candidate);

    n.receive(envelope{.from = 2, .to = 1, .msg = request_vote_reply{.term = 1, .granted = true}});

    REQUIRE(n.current_role() == role::candidate);
}

TEST_CASE("a leader brings lagging and conflicting followers up to date in one heartbeat round", "[replication]") {
    const auto log = log_of({1, 1, 2, 2, 5, 5, 5});
    memory_storage store1{persistent_state{
        .hard = hard_state{.term = 6},
        .log = log,
    }};
    memory_storage store2{persistent_state{
        .hard = hard_state{.term = 3},
        .log = log_of({1, 1, 2, 2, 3}),
    }};
    memory_storage store3{persistent_state{
        .hard = hard_state{.term = 4},
        .log = log_of({1, 1, 2, 2, 3, 4}),
    }};
    std::vector<node> nodes = {
        node{make_config(1, {1, 2, 3}, 42), &store1},
        node{make_config(2, {1, 2, 3}, 67), &store2},
        node{make_config(3, {1, 2, 3}, 12), &store3},
    };

    elect(nodes[0]);

    for (int i = 1; i <= config{}.heartbeat_interval_ticks; ++i) {
        for (auto& n : nodes) {
            n.tick();
        }
        deliver_all(nodes);
    }

    REQUIRE(nodes[0].current_role() == role::leader);

    REQUIRE(std::ranges::equal(nodes[0].log(), log));
    REQUIRE(std::ranges::equal(nodes[1].log(), log));
    REQUIRE(std::ranges::equal(nodes[2].log(), log));
    REQUIRE(store1.load().log == log);
    REQUIRE(store2.load().log == log);
    REQUIRE(store3.load().log == log);
}

TEST_CASE("after a rejection, the leader jumps back to just past the follower's last entry", "[replication]") {
    memory_storage store{persistent_state{.hard = {.term = 3}, .log = log_of({1, 2, 2, 3, 3, 3, 3})}};
    node n{make_config(1, {1, 2, 3}), &store};
    elect(n);

    n.receive(envelope{.from = 2,
                       .to = 1,
                       .msg = append_entries_reply{
                           .term = n.current_term(), .success = false, .match_index = 0, .last_log_index = 2}});

    const auto output = n.take_output();
    REQUIRE(output.messages == std::vector{envelope{.from = 1,
                                                    .to = 2,
                                                    .msg = append_entries{.term = n.current_term(),
                                                                          .prev_log_index = 2,
                                                                          .prev_log_term = 2,
                                                                          .entries = log_of({2, 3, 3, 3, 3}),
                                                                          .leader_commit = 0}}});
}

TEST_CASE("duplicated and reordered replies leave next_index/match_index correct", "[replication]") {
    memory_storage store{persistent_state{.hard = {.term = 3}, .log = log_of({1, 2, 2, 3, 3, 3, 3})}};
    node n{make_config(1, {1, 2, 3}), &store};
    elect(n);

    // Node 2 confirms the whole log. The replies below answer older probes and arrive late.
    n.receive(envelope{
        .from = 2,
        .to = 1,
        .msg = append_entries_reply{.term = n.current_term(), .success = true, .match_index = 7, .last_log_index = 7}});
    (void)n.take_output();

    // An empty probe right after the last entry: nothing to resend.
    const auto probe = append_entries{
        .term = n.current_term(), .prev_log_index = 7, .prev_log_term = 3, .entries = {}, .leader_commit = 0};

    SECTION("an older success does not move match_index or next_index back") {
        n.receive(envelope{.from = 2,
                           .to = 1,
                           .msg = append_entries_reply{
                               .term = n.current_term(), .success = true, .match_index = 3, .last_log_index = 3}});
        (void)n.take_output();

        for (int i = 0; i < config{}.heartbeat_interval_ticks; ++i) {
            n.tick();
        }

        // Node 3 never replied, so its next_index is still the initial last_log_index() + 1.
        REQUIRE(n.take_output().messages ==
                std::vector{envelope{.from = 1, .to = 2, .msg = probe}, envelope{.from = 1, .to = 3, .msg = probe}});
    }

    SECTION("a stale rejection does not move next_index below match_index + 1") {
        n.receive(envelope{.from = 2,
                           .to = 1,
                           .msg = append_entries_reply{
                               .term = n.current_term(), .success = false, .match_index = 0, .last_log_index = 2}});

        REQUIRE(n.take_output().messages == std::vector{envelope{.from = 1, .to = 2, .msg = probe}});
    }
}
