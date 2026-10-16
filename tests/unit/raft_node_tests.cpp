#include <catch2/catch_test_macros.hpp>
#include <hammurabi/raft/messages.h>
#include <hammurabi/raft/node.h>
#include <hammurabi/raft/types.h>

#include <cstdint>
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
