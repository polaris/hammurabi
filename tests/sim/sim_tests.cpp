#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <sim/cluster.h>

#include <cstddef>
#include <cstdint>
#include <optional>

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
