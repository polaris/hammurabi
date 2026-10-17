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
// everyone else is a follower. Returns nullopt if that doesn't happen within
// the limit.
std::optional<stable_leader> ticks_until_stable_leader(std::vector<node>& nodes, int limit = 100) {
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
    return std::nullopt;
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
    const auto output = n.take_output();
    REQUIRE(output.messages.size() == 1);
    REQUIRE(std::get<append_entries_reply>(output.messages[0].msg).success == false);
    REQUIRE(std::get<append_entries_reply>(output.messages[0].msg).term == 6);
}

TEST_CASE("exactly one leader", "[election]") {
    memory_storage store1, store2, store3;
    std::vector<node> nodes = {
        node{make_config(1, {1, 2, 3}, 42), &store1},
        node{make_config(2, {1, 2, 3}, 67), &store2},
        node{make_config(3, {1, 2, 3}, 12), &store3},
    };

    const auto sl = ticks_until_stable_leader(nodes);
    REQUIRE(sl.has_value());

    for (const auto f : sl->followers) {
        REQUIRE(nodes[f].leader_hint() == nodes[sl->leader].id());
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
    REQUIRE(sl.has_value());
    const auto& leader = nodes[sl->leader];
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
    REQUIRE(sl.has_value());

    auto& leader = nodes[sl->leader];
    auto& follower = nodes[sl->followers[0]];

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
