#include "kv_string_makers.h" // IWYU pragma: keep

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <hammurabi/kv/command.h>
#include <hammurabi/kv/protocol.h>
#include <hammurabi/kv/state_machine.h>
#include <hammurabi/raft/node.h>
#include <hammurabi/raft/types.h>
#include <hammurabi/replica/replica.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace hammurabi;

struct stable_leader {
    int ticks = 0;
    std::size_t leader{}; // index into replicas
    std::vector<std::size_t> followers{};
};

struct test_cluster {
    std::array<raft::memory_storage, 3> stores; // replicas point here: must not move
    std::vector<replica> replicas;
    std::map<request_id, kv::response> responses; // everything any replica answered
    request_id next_id = 1;
    const std::map<raft::node_id, std::string> addresses{{1, "c1"}, {2, "c2"}, {3, "c3"}};

    // Network faults: messages to or from an isolated node, or on a blocked (from, to) link, are dropped.
    std::set<raft::node_id> isolated;
    std::set<std::pair<raft::node_id, raft::node_id>> blocked;

    test_cluster() {
        for (raft::node_id id = 1; id <= 3; ++id) {
            replicas.emplace_back(raft::config{.self = id, .members = {1, 2, 3}, .random_seed = id}, &stores.at(id - 1),
                                  addresses);
        }
    }
    test_cluster(const test_cluster&) = delete;
    test_cluster& operator=(const test_cluster&) = delete;
    test_cluster(test_cluster&&) = delete;
    test_cluster& operator=(test_cluster&&) = delete;
    ~test_cluster() = default;

    void run_for(int ticks) {
        for (int i = 0; i < ticks; ++i) {
            for (auto& r : replicas) {
                r.tick();
            }
            deliver_all();
        }
    }

    // Ticks only replicas[index], delivering after each tick: the others stay quiet.
    void run_one_for(std::size_t index, int ticks) {
        for (int i = 0; i < ticks; ++i) {
            replicas.at(index).tick();
            deliver_all();
        }
    }

    void heal() {
        isolated.clear();
        blocked.clear();
    }

    [[nodiscard]] bool dropped(const raft::envelope& env) const {
        return isolated.contains(env.from) || isolated.contains(env.to) || blocked.contains({env.from, env.to});
    }

    // Delivers messages until none are left (dropping those the faults cut off); collects every response.
    void deliver_all() {
        while (true) {
            std::vector<raft::envelope> in_flight;
            for (auto& r : replicas) {
                auto out = r.take_output();
                in_flight.insert(in_flight.end(), out.messages.begin(), out.messages.end());
                for (auto& [id, resp] : out.responses) {
                    const auto [_, fresh] = responses.try_emplace(id, std::move(resp));
                    REQUIRE(fresh); // every request is answered exactly once
                }
            }
            if (in_flight.empty()) {
                return;
            }
            for (const auto& env : in_flight) {
                if (!dropped(env)) {
                    replicas.at(env.to - 1).receive(env);
                }
            }
        }
    }

    // Ticks and delivers until one replica leads and the others follow. Isolated replicas don't count:
    // an old leader that is cut off still believes it leads.
    stable_leader elect_leader(int limit = 100) {
        for (int i = 1; i <= limit; ++i) {
            for (auto& r : replicas) {
                r.tick();
            }
            deliver_all();
            std::size_t leaders = 0;
            std::vector<std::size_t> followers;
            std::size_t leader = 0;
            for (std::size_t j = 0; j < replicas.size(); ++j) {
                const auto& n = replicas.at(j).node();
                if (isolated.contains(n.id())) {
                    continue;
                }
                if (n.current_role() == raft::role::leader) {
                    ++leaders;
                    leader = j;
                } else if (n.current_role() == raft::role::follower) {
                    followers.push_back(j);
                }
            }
            if (leaders == 1 && followers.size() == replicas.size() - isolated.size() - 1) {
                return stable_leader{.ticks = i, .leader = leader, .followers = followers};
            }
        }
        FAIL("no stable leader within " << limit << " ticks");
        std::unreachable(); // FAIL throws; this tells the compiler too
    }

    // Submits without delivering; the answer arrives in responses once the test delivers.
    request_id submit(replica& r, kv::operation op) {
        const auto id = next_id++;
        r.submit(id, kv::request{std::move(op)});
        return id;
    }

    // Submits, delivers everything, and returns the answer.
    kv::response request(replica& r, kv::operation op) {
        const auto id = submit(r, std::move(op));
        deliver_all();
        REQUIRE(responses.contains(id));
        return responses.at(id);
    }
};

// One operation of each kind whose key and values add up to max_payload_bytes + 1. Each still encodes to
// less than raft's max_append_bytes, so only the replica's own limit can reject it.
std::vector<kv::operation> oversized_operations() {
    constexpr auto over = max_payload_bytes + 1;
    constexpr auto half = over / 2;
    return {
        kv::get{.key = std::string(over, 'k')},
        kv::remove{.key = std::string(over, 'k')},
        kv::put{.key = "k", .value = std::string(over - 1, 'v')},
        kv::cas{.key = "k", .expected = std::nullopt, .desired = std::string(over - 1, 'd')},
        // expected counts too: without it this one would be only about half the limit
        kv::cas{.key = "k", .expected = std::string(half, 'e'), .desired = std::string(over - 1 - half, 'd')},
    };
}

// Ticks a lone replica until it has elected itself: its election timeout is at most 2t ticks.
void single_replica_elect(replica& r, const raft::config& cfg) {
    for (int i = 0; i < 2 * cfg.election_timeout_ticks && r.node().current_role() != raft::role::leader; ++i) {
        r.tick();
    }
    REQUIRE(r.node().current_role() == raft::role::leader);
}

} // namespace

TEST_CASE("constructor rejects client addresses that don't match the members", "[replica]") {
    raft::memory_storage store;
    const raft::config cfg{.self = 1, .members = {1, 2, 3}};

    const std::map<raft::node_id, std::string> missing{{1, "a"}, {2, "b"}};
    REQUIRE_THROWS_AS(replica(cfg, &store, missing), std::runtime_error);

    const std::map<raft::node_id, std::string> extra{{1, "a"}, {2, "b"}, {3, "c"}, {4, "d"}};
    REQUIRE_THROWS_AS(replica(cfg, &store, extra), std::runtime_error);

    const std::map<raft::node_id, std::string> exact{{1, "a"}, {2, "b"}, {3, "c"}};
    REQUIRE_NOTHROW(replica(cfg, &store, exact));
}

TEST_CASE("put then get returns the value", "[replica]") {
    test_cluster c;
    const auto elected = c.elect_leader();
    auto& leader = c.replicas.at(elected.leader);

    REQUIRE(c.request(leader, kv::put{.key = "x", .value = "1"}) == kv::response{kv::result{.code = kv::status::ok}});
    REQUIRE(c.request(leader, kv::get{.key = "x"}) == kv::response{kv::result{.code = kv::status::ok, .value = "1"}});
}

TEST_CASE("followers apply committed entries too", "[replica]") {
    test_cluster c;
    const auto elected = c.elect_leader();
    auto& leader = c.replicas.at(elected.leader);

    REQUIRE(c.request(leader, kv::put{.key = "x", .value = "1"}) == kv::response{kv::result{.code = kv::status::ok}});

    for (const auto i : elected.followers) {
        const auto& r = c.replicas.at(i);
        CHECK(r.machine().read(kv::get{.key = "x"}) ==
              kv::result{.code = kv::status::not_found, .value = std::nullopt});
    }

    // Followers learn the new commit index from the next append_entries: one heartbeat interval.
    c.run_for(raft::config{}.heartbeat_interval_ticks);

    for (const auto& r : c.replicas) {
        CHECK(r.machine().read(kv::get{.key = "x"}) == kv::result{.code = kv::status::ok, .value = "1"});
    }
}

TEST_CASE("results pass through unchanged", "[replica]") {
    test_cluster c;
    const auto elected = c.elect_leader();
    auto& leader = c.replicas.at(elected.leader);

    REQUIRE(c.request(leader, kv::put{.key = "x", .value = "1"}) == kv::response{kv::result{.code = kv::status::ok}});
    REQUIRE(c.request(leader, kv::get{.key = "missing"}) == kv::response{kv::result{.code = kv::status::not_found}});
    REQUIRE(c.request(leader, kv::cas{.key = "x", .expected = "2", .desired = "3"}) ==
            kv::response{kv::result{.code = kv::status::cas_failed, .value = "1"}});
    REQUIRE(c.request(leader, kv::cas{.key = "missing", .expected = "1", .desired = "3"}) ==
            kv::response{kv::result{.code = kv::status::cas_failed, .value = std::nullopt}});
}

TEST_CASE("a single-node cluster answers at the next take_output", "[replica]") {
    // With one node, propose() commits at once; submit() must register the client in pending_ before
    // the next take_output() hands out that entry, or the answer is lost.
    raft::memory_storage store;
    const raft::config cfg{.self = 1, .members = {1}};
    replica r(cfg, &store, {{1, "a"}});

    single_replica_elect(r, cfg);
    const auto startup = r.take_output(); // the leader's committed no-op: nobody waits for it
    REQUIRE(startup.responses.empty());

    r.submit(1, kv::request{kv::put{.key = "x", .value = "42"}});
    const auto out = r.take_output();

    REQUIRE(out.responses.size() == 1);
    CHECK(out.responses.front().first == 1);
    CHECK(out.responses.front().second == kv::response{kv::result{.code = kv::status::ok}});
    CHECK(out.messages.empty()); // no peers, so nothing to send
    CHECK(r.machine().read(kv::get{.key = "x"}) == kv::result{.code = kv::status::ok, .value = "42"});
}

TEST_CASE("a request to a follower answers not_leader with the leader's client address", "[replica]") {
    // Answered by submit() itself: no proposal, no messages, and nothing executed anywhere.
    test_cluster c;
    const auto elected = c.elect_leader();
    const auto& leader = c.replicas.at(elected.leader);
    auto& follower = c.replicas.at(elected.followers.at(0));
    const auto leader_log_before = leader.node().log().size(); // already holds the leader's no-op

    // take_output() directly instead of request(): deliver_all() would hide whether the answer needed messages.
    const auto id = c.submit(follower, kv::put{.key = "x", .value = "1"});
    const auto out = follower.take_output();

    REQUIRE(out.responses.size() == 1);
    CHECK(out.responses.front().first == id);
    CHECK(out.responses.front().second ==
          kv::response{kv::not_leader{.leader_address = c.addresses.at(leader.node().id())}});
    CHECK(out.messages.empty());

    c.deliver_all();
    CHECK(leader.node().log().size() == leader_log_before); // never reached the log
    for (const auto& r : c.replicas) {
        CHECK(r.machine().read(kv::get{.key = "x"}) == kv::result{.code = kv::status::not_found});
    }
}

TEST_CASE("a request at exactly max_payload_bytes is accepted", "[replica]") {
    // Key + value == 1024 bytes: appended to the log and answered with a result.
    raft::memory_storage store;
    const raft::config cfg{.self = 1, .members = {1}};
    replica r(cfg, &store, {{1, "a"}});

    single_replica_elect(r, cfg);

    const auto log_before = r.node().log().size(); // the leader's no-op

    const auto op = kv::put{.key = "x", .value = std::string(max_payload_bytes - 1, 'x')};
    REQUIRE(op.key.size() + op.value.size() == max_payload_bytes);

    r.submit(1, kv::request{.op = op});
    REQUIRE(r.node().log().size() == log_before + 1);

    const auto out = r.take_output();
    REQUIRE(out.responses.size() == 1);
    CHECK(out.responses.front().second == kv::response{kv::result{.code = kv::status::ok}});

    // The log holds the log format (encode(op)), not the client format with its version byte.
    const auto& entry = r.node().log().back();
    CHECK(entry ==
          raft::log_entry{.term = r.node().current_term(), .kind = raft::entry_kind::command, .data = kv::encode(op)});
}

TEST_CASE("a request over max_payload_bytes answers too_large and adds nothing to the log", "[replica]") {
    // Every operation kind, one byte over the limit: covers each branch of the replica's payload count.
    const auto op = GENERATE(from_range(oversized_operations()));
    CAPTURE(op.index()); // not op itself: it would print over 1 KiB

    raft::memory_storage store;
    const raft::config cfg{.self = 1, .members = {1}};
    replica r(cfg, &store, {{1, "a"}});

    single_replica_elect(r, cfg);

    // A copy: log() returns a span, which would show the live log (or dangle), not the log as it was.
    const auto log_before = r.node().log() | std::ranges::to<std::vector>(); // the leader's no-op

    r.submit(1, kv::request{.op = op});
    REQUIRE(std::ranges::equal(r.node().log(), log_before));

    const auto out = r.take_output();
    REQUIRE(out.responses.size() == 1);
    CHECK(out.responses.front().first == 1);
    CHECK(out.responses.front().second == kv::response{kv::rejection::too_large});
}

TEST_CASE("raft's command_too_large answers too_large", "[replica]") {
    // A small max_append_bytes makes propose() reject a command that is well under the replica's 1 KiB limit.
    raft::memory_storage store;
    const raft::config cfg{.self = 1, .members = {1}, .max_append_bytes = 100};
    replica r(cfg, &store, {{1, "a"}});

    single_replica_elect(r, cfg);
    const auto log_before = r.node().log().size(); // the leader's no-op

    r.submit(1, kv::request{kv::put{.key = "k", .value = std::string(200, 'v')}});
    CHECK(r.node().log().size() == log_before);

    const auto out = r.take_output();
    REQUIRE(out.responses.size() == 1);
    CHECK(out.responses.front().first == 1);
    CHECK(out.responses.front().second == kv::response{kv::rejection::too_large});
}

TEST_CASE("a pending request answers not_leader when the leader steps down", "[replica]") {
    // The old leader only learns that a newer term exists. Nothing commits in that batch, so the step-down
    // flush is what answers the client.
    test_cluster c;
    const auto old = c.elect_leader().leader;
    auto& old_leader = c.replicas.at(old);
    const auto old_term = old_leader.node().current_term();

    c.isolated.insert(old_leader.node().id());
    const auto id = c.submit(old_leader, kv::put{.key = "x", .value = "1"});
    c.deliver_all();
    REQUIRE_FALSE(c.responses.contains(id)); // cut off, so it can't commit

    c.elect_leader(); // the other two elect a new leader in a newer term

    // Reconnect, but tick only the old leader: its heartbeat is rejected with the newer term.
    c.heal();
    c.run_one_for(old, raft::config{}.heartbeat_interval_ticks);

    CHECK(old_leader.node().current_role() == raft::role::follower);
    CHECK(old_leader.node().current_term() > old_term);
    REQUIRE(c.responses.contains(id));
    // A rejection tells the old leader the newer term, but not who leads it.
    CHECK(c.responses.at(id) == kv::response{kv::not_leader{.leader_address = std::nullopt}});
    // The entry is still in its log: the client was answered without waiting for the entry's fate.
    CHECK(old_leader.node().log().back().term == old_term);
}

TEST_CASE("an entry overwritten by another leader answers not_leader, not that leader's result", "[replica]") {
    // Without the term check, the old leader would apply the new leader's command at our index and send *its*
    // result to our client: "ok" for a write that never happened.
    test_cluster c;
    const auto old = c.elect_leader().leader;
    auto& old_leader = c.replicas.at(old);

    c.isolated.insert(old_leader.node().id());
    const auto first = c.submit(old_leader, kv::put{.key = "x", .value = "1"});  // index 2
    const auto second = c.submit(old_leader, kv::put{.key = "x", .value = "2"}); // index 3
    c.deliver_all();

    const auto elected = c.elect_leader();
    auto& new_leader = c.replicas.at(elected.leader);
    // The new leader's no-op takes index 2, and another client's put takes index 3.
    REQUIRE(c.request(new_leader, kv::put{.key = "y", .value = "9"}) ==
            kv::response{kv::result{.code = kv::status::ok}});
    REQUIRE(new_leader.node().log().size() == 3);

    // Reconnect and let only the new leader speak: one append_entries replaces indexes 2 and 3 and commits them.
    c.heal();
    c.run_one_for(elected.leader, raft::config{}.heartbeat_interval_ticks);

    const auto redirect = kv::response{kv::not_leader{.leader_address = c.addresses.at(new_leader.node().id())}};
    CHECK(c.responses.at(first) == redirect);  // index 2 now holds the no-op
    CHECK(c.responses.at(second) == redirect); // index 3 now holds put y=9, whose result would be "ok"
    CHECK(old_leader.machine().read(kv::get{.key = "y"}) == kv::result{.code = kv::status::ok, .value = "9"});
    CHECK(old_leader.machine().read(kv::get{.key = "x"}) == kv::result{.code = kv::status::not_found});
}

TEST_CASE("an entry that commits in the same batch as the step-down answers with the result", "[replica]") {
    // One receive() both demotes the old leader and commits its entry. The replica applies first and flushes
    // second, so the client gets the real result instead of a needless not_leader.
    test_cluster c;
    const auto before = c.elect_leader();
    auto& old_leader = c.replicas.at(before.leader);
    const auto old_id = old_leader.node().id();
    const auto keeper_id = c.replicas.at(before.followers.at(0)).node().id(); // will store the entry
    const auto other_id = c.replicas.at(before.followers.at(1)).node().id();  // won't

    // The entry reaches keeper, but keeper's reply and everything to other are lost:
    // the old leader never learns that the entry is on a majority.
    c.blocked = {{keeper_id, old_id}, {old_id, other_id}};
    const auto id = c.submit(old_leader, kv::put{.key = "x", .value = "1"});
    c.deliver_all();
    REQUIRE_FALSE(c.responses.contains(id));
    REQUIRE(c.replicas.at(keeper_id - 1).node().log().size() == old_leader.node().log().size());

    // Cut the old leader off. Only keeper can win: other's log is behind, so keeper won't vote for it.
    c.heal();
    c.isolated.insert(old_id);
    const auto after = c.elect_leader();
    REQUIRE(c.replicas.at(after.leader).node().id() == keeper_id);

    // Reconnect and let only the new leader speak: its append_entries matches the old leader's log
    // and carries a commit index that covers our entry.
    c.heal();
    c.run_one_for(after.leader, raft::config{}.heartbeat_interval_ticks);

    CHECK(old_leader.node().current_role() == raft::role::follower);
    REQUIRE(c.responses.contains(id));
    CHECK(c.responses.at(id) == kv::response{kv::result{.code = kv::status::ok}});
    CHECK(old_leader.machine().read(kv::get{.key = "x"}) == kv::result{.code = kv::status::ok, .value = "1"});
}
