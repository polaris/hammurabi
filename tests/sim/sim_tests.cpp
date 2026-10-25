#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <hammurabi/raft/messages.h>
#include <hammurabi/raft/node.h>
#include <hammurabi/raft/types.h>
#include <sim/cluster.h>
#include <sim/invariant_checker.h>
#include <sim/network.h>
#include <sim/random.h>
#include <sim/types.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <functional>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

struct test_packet {
    std::uint32_t from = 0;
    std::uint32_t to = 0;
    std::uint32_t id = 0;
    bool operator==(const test_packet&) const = default;
};

// HAMMURABI_SEED=<n> replays a single seed; otherwise seeds 1…50.
std::vector<std::uint64_t> chaos_seeds() {
    if (const char* env = std::getenv("HAMMURABI_SEED")) {
        const std::string_view text{env};
        std::uint64_t seed = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seed);
        if (error != std::errc{} || end != text.data() + text.size()) {
            throw std::invalid_argument(std::format("HAMMURABI_SEED is not a number: '{}'", text));
        }
        return {seed};
    }
    return std::views::iota(std::uint64_t{1}, std::uint64_t{51}) | std::ranges::to<std::vector>();
}

// A new fault configuration for one epoch: maybe a partition, plus some loss, duplication and delay.
void apply_random_faults(hammurabi::sim::network<hammurabi::raft::envelope>& net, std::mt19937_64& rng,
                         std::size_t size) {
    using namespace hammurabi::sim;
    using hammurabi::raft::node_id;

    net.heal();
    const auto layout = rng() % 3;
    if (layout == 1) {
        // Random 2-way split: a coin flip per node. Not std::shuffle, whose result differs between standard libraries.
        std::set<node_id> side;
        for (node_id id = 1; id <= size; ++id) {
            if (rng() % 2 == 0) {
                side.insert(id);
            }
        }
        net.partition({side}); // the nodes not in side form the other half
    } else if (layout == 2) {
        net.isolate(static_cast<node_id>(random_between(rng, 1, size)));
    } // layout == 0: no partition, so leaders can get elected before the next fault

    net.set_loss(static_cast<int>(random_between(rng, 0, 200)));
    net.set_duplication(static_cast<int>(random_between(rng, 0, 100)));
    net.set_delay(1, random_between(rng, 1, 5)); // well below the election timeout of 15 ticks
}

// Heal everything: partitions, loss, duplication and delay.
void calm(hammurabi::sim::network<hammurabi::raft::envelope>& net) {
    net.heal();
    net.set_loss(0);
    net.set_duplication(0);
    net.set_delay(1, 1);
}

hammurabi::raft::node_id require_leader(const hammurabi::sim::cluster& c) {
    if (const auto l = c.leader()) {
        return *l;
    }
    FAIL("no leader");
    return {}; // unreachable: FAIL throws, but the compiler can't see that either
}

} // namespace

template<>
struct Catch::StringMaker<test_packet> {
    static std::string convert(const test_packet& packet) {
        return std::format("{}→{} #{}", packet.from, packet.to, packet.id);
    }
};

TEST_CASE("election within 300 ticks for 1/3/5/7 nodes", "[sim]") {
    using namespace hammurabi::sim;

    auto size = GENERATE(as<std::size_t>{}, 1, 3, 5, 7);
    CAPTURE(size);
    cluster c{size, 42};
    const bool elected = c.run_until([&c]() -> bool { return c.leader() != std::nullopt; }, 300);
    UNSCOPED_INFO(c.history());
    REQUIRE(elected);
}

TEST_CASE("same seed → same leader at the same tick", "[sim]") {
    using namespace hammurabi::sim;

    auto seed = GENERATE(as<std::uint64_t>{}, 42, 67, 123);
    CAPTURE(seed);

    cluster a{3, seed};
    const auto a_elected = a.run_until([&a]() -> bool { return a.leader() != std::nullopt; }, 300);
    UNSCOPED_INFO(a.history());
    REQUIRE(a_elected);

    cluster b{3, seed};
    const auto b_elected = b.run_until([&b]() -> bool { return b.leader() != std::nullopt; }, 300);
    UNSCOPED_INFO(b.history());
    REQUIRE(b_elected);

    UNSCOPED_INFO("a:\n" << a.history());
    UNSCOPED_INFO("b:\n" << b.history());
    CHECK(a.now() == b.now());

    UNSCOPED_INFO("a:\n" << a.history());
    UNSCOPED_INFO("b:\n" << b.history());
    CHECK(a.leader() == b.leader());
}

TEST_CASE("partition: packets only flow inside a group", "[sim]") {
    using namespace hammurabi::sim;

    network<test_packet> net{42};
    net.partition({{1, 2}}); // {1, 2} | everyone else

    net.send({.from = 1, .to = 2}, 0);
    net.send({.from = 3, .to = 4}, 0);
    net.send({.from = 1, .to = 3}, 0);

    const auto delivered = net.deliver(1);
    CHECK(delivered == std::vector<test_packet>{{.from = 1, .to = 2}, {.from = 3, .to = 4}});
}

TEST_CASE("isolate: cuts nodes off from everyone", "[sim]") {
    using namespace hammurabi::sim;

    network<test_packet> net{42};
    net.isolate(1);
    net.isolate(2);

    net.send({.from = 1, .to = 2}, 0);
    net.send({.from = 2, .to = 4}, 0);
    net.send({.from = 3, .to = 4}, 0);
    net.send({.from = 3, .to = 1}, 0);

    const auto delivered = net.deliver(1);
    CHECK(delivered == std::vector<test_packet>{{.from = 3, .to = 4}});
}

TEST_CASE("partition: a packet in flight when it starts still arrives", "[sim]") {
    using namespace hammurabi::sim;

    network<test_packet> net{42};
    net.set_delay(5, 5);
    net.send({.from = 1, .to = 3}, 0);
    net.partition({{1}});

    const auto delivered = net.deliver(5);
    CHECK(delivered == std::vector<test_packet>{{.from = 1, .to = 3}});
}

TEST_CASE("partition: a packet sent during it is lost, even if healed before arrival", "[sim]") {
    using namespace hammurabi::sim;

    network<test_packet> net{42};
    net.set_delay(5, 5);
    net.partition({{1}});
    net.send({.from = 1, .to = 3}, 0);
    net.heal();

    const auto delivered = net.deliver(5);
    CHECK(delivered == std::vector<test_packet>{});
}

TEST_CASE("heal: reconnects everyone", "[sim]") {
    using namespace hammurabi::sim;

    network<test_packet> net{42};
    net.partition({{1}});

    net.send({.from = 1, .to = 3}, 0);
    CHECK(net.deliver(1) == std::vector<test_packet>{});

    net.heal();

    net.send({.from = 1, .to = 3}, 1);
    CHECK(net.deliver(2) == std::vector<test_packet>{{.from = 1, .to = 3}});
}

TEST_CASE("isolate: works on top of a partition", "[sim]") {
    using namespace hammurabi::sim;

    network<test_packet> net{42};
    net.partition({{1, 2}}); // {1, 2} | {3, 4, 5}
    net.isolate(1);          // out of an explicit group
    net.isolate(3);          // out of the implicit group: {2} | {1} | {3} | {4, 5}

    net.send({.from = 1, .to = 2}, 0);
    net.send({.from = 3, .to = 4}, 0);
    net.send({.from = 1, .to = 3}, 0);
    net.send({.from = 4, .to = 5}, 0);

    const auto delivered = net.deliver(1);
    CHECK(delivered == std::vector<test_packet>{{.from = 4, .to = 5}});
}

TEST_CASE("partition: replaces the previous layout", "[sim]") {
    using namespace hammurabi::sim;

    network<test_packet> net{42};
    net.partition({{1}});
    net.partition({{2}});

    net.send({.from = 1, .to = 2}, 0);
    net.send({.from = 3, .to = 1}, 0);

    const auto delivered = net.deliver(1);
    CHECK(delivered == std::vector<test_packet>{{.from = 3, .to = 1}});
}

TEST_CASE("invariant_checker: a second leader in the same term throws", "[sim]") {
    using namespace hammurabi::sim;

    invariant_checker check;
    check.observe_leader(1, 5, 100);
    check.observe_leader(1, 5, 101); // same leader again: fine
    CHECK_THROWS_AS(check.observe_leader(3, 5, 250), invariant_violation);
}

TEST_CASE("cluster: an isolated leader is replaced, and steps down after heal", "[sim]") {
    using namespace hammurabi::sim;
    using hammurabi::raft::role;

    cluster c{5, 42};
    REQUIRE(c.run_until([&c]() -> bool { return c.leader() != std::nullopt; }, 300));
    const auto old_leader = require_leader(c);
    const auto old_term = c.node(old_leader).current_term();

    // Phase 1: isolate → the others elect a new leader in a higher term
    c.net().isolate(old_leader);
    REQUIRE(c.run_until([&]() -> bool { return c.leader() && c.leader() != old_leader; }, 500));
    CHECK(c.node(require_leader(c)).current_term() > old_term);
    CHECK(c.node(old_leader).current_role() == role::leader); // still thinks it leads!

    // Phase 2: heal → the old leader hears the higher term and steps down
    c.net().heal();
    REQUIRE(c.run_until([&]() -> bool { return c.node(old_leader).current_role() != role::leader; }, 500));
}

TEST_CASE("cluster: in a 2/3 split only the majority elects a leader", "[sim]") {
    using namespace hammurabi::sim;
    using hammurabi::raft::role;

    cluster c{5, 42};
    c.net().partition({{1, 2}}); // {1, 2} | {3, 4, 5}, before any election
    c.run_for(1000);
    REQUIRE(c.leader());
    CHECK(require_leader(c) >= 3); // the leader is on the majority side
    CHECK(c.node(1).current_role() != role::leader);
    CHECK(c.node(2).current_role() != role::leader);
}

TEST_CASE("loss: 1000 drops everything", "[sim]") {
    using namespace hammurabi::sim;

    network<test_packet> net{42};
    net.set_loss(1000);

    for (int i = 0; i < 100; ++i) {
        net.send({.from = 1, .to = 2}, 0);
    }

    const auto delivered = net.deliver(1);
    CHECK(delivered.empty());
}

TEST_CASE("loss: 500 drops about half", "[sim]") {
    using namespace hammurabi::sim;

    network<test_packet> net{42};
    net.set_loss(500);

    for (int i = 0; i < 1000; ++i) {
        net.send({.from = 1, .to = 2}, 0);
    }

    const auto delivered = net.deliver(1);
    CHECK(delivered.size() >= 400);
    CHECK(delivered.size() <= 600);
}

TEST_CASE("duplication: 1000 doubles everything", "[sim]") {
    using namespace hammurabi::sim;

    network<test_packet> net{42};
    net.set_duplication(1000);

    for (int i = 0; i < 10; ++i) {
        net.send({.from = 1, .to = 2}, 0);
    }

    const auto delivered = net.deliver(1);
    CHECK(delivered.size() == 20);
}

TEST_CASE("delay: within the range", "[sim]") {
    using namespace hammurabi::sim;

    network<test_packet> net{42};
    net.set_delay(3, 7);

    for (int i = 0; i < 100; ++i) {
        net.send({.from = 1, .to = 2}, 0);
    }

    {
        const auto delivered = net.deliver(2);
        CHECK(delivered.empty());
    }
    {
        const auto delivered = net.deliver(7);
        CHECK(delivered.size() == 100);
    }
}

TEST_CASE("delay: reorders", "[sim]") {
    using namespace hammurabi::sim;

    network<test_packet> net{42};
    net.set_delay(1, 10);

    for (int i = 0; i < 100; ++i) {
        net.send({.from = 1, .to = 2, .id = static_cast<std::uint32_t>(i)}, 0);
    }

    const auto delivered = net.deliver(10);
    CHECK_FALSE(std::ranges::is_sorted(delivered, {}, &test_packet::id));
}

TEST_CASE("same seed → same delivery", "[sim]") {
    using namespace hammurabi::sim;

    network<test_packet> a{42};
    a.set_delay(2, 5);
    a.set_loss(250);
    a.set_duplication(250);

    network<test_packet> b{42};
    b.set_delay(2, 5);
    b.set_loss(250);
    b.set_duplication(250);

    for (int i = 0; i < 100; ++i) {
        const test_packet p{.from = 1, .to = 2, .id = static_cast<std::uint32_t>(i)};
        a.send(p, 0);
        b.send(p, 0);
    }
    const auto a_delivered = a.deliver(5);
    const auto b_delivered = b.deliver(5);
    CHECK_FALSE(a_delivered.empty());
    CHECK(a_delivered == b_delivered);
}

TEST_CASE("cluster: chaos, then heal → a stable leader", "[sim]") {
    using namespace hammurabi::sim;

    constexpr std::size_t size = 5;
    constexpr tick_t chaos_ticks = 5000;

    const auto seed = GENERATE(from_range(chaos_seeds()));
    CAPTURE(seed);

    cluster c{size, seed};
    // Not seeded with seed itself: the cluster's seeder would then produce the same stream.
    std::mt19937_64 chaos{seed ^ 0x9E3779B97F4A7C15};

    while (c.now() < chaos_ticks) {
        apply_random_faults(c.net(), chaos, size);
        // Epochs, not single ticks: a fault must last long enough for elections to happen.
        c.run_for(std::min(random_between(chaos, 50, 300), chaos_ticks - c.now()));
    }

    calm(c.net());
    const bool stable = c.run_until([&c]() -> bool { return c.has_stable_leader(); }, 500);
    UNSCOPED_INFO(c.describe() << c.history());
    REQUIRE(stable);
}
