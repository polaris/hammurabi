#include "kv_string_makers.h" // IWYU pragma: keep

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <hammurabi/kv/command.h>
#include <hammurabi/kv/state_machine.h>
#include <hammurabi/wire/codec.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace hammurabi;

std::vector<std::byte> to_bytes(std::initializer_list<std::uint8_t> values) {
    return values | std::views::transform([](std::uint8_t v) { return std::byte{v}; }) | std::ranges::to<std::vector>();
}

std::vector<kv::command> sample_commands() {
    return {
        {.client_id = 0, .sequence = 0, .op = kv::get{.key = "foo"}},
        {.client_id = 7, .sequence = 1, .op = kv::put{.key = "foo", .value = "bar"}},
        {.client_id = 7, .sequence = 2, .op = kv::remove{.key = "foo"}},
        {.client_id = 7, .sequence = 3, .op = kv::cas{.key = "foo", .expected = "bar", .desired = "baz"}},
        {.client_id = 7, .sequence = 4, .op = kv::cas{.key = "foo", .expected = std::nullopt, .desired = "baz"}},
        {.client_id = 0, .sequence = 0, .op = kv::register_client{}},
        // large values: every byte of both u64 fields must survive
        {.client_id = 0x0102030405060708, .sequence = 0xfffffffffffffffe, .op = kv::get{.key = ""}},
    };
}

// Applies commands the way the replica does: at increasing log indexes, through a registered session,
// numbering the requests 1, 2, 3, ...
struct test_client {
    kv::state_machine* sm;
    std::uint64_t next_index; // a second client on the same machine must start after the first one's indexes
    std::uint64_t id = 0;
    std::uint64_t sequence = 0;

    explicit test_client(kv::state_machine& machine, std::uint64_t first_index = 1)
    : sm{&machine}
    , next_index{first_index} {
        const auto answer = sm->apply(next_index++, kv::command{.op = kv::register_client{}});
        REQUIRE(answer.has_value());
        REQUIRE(std::holds_alternative<kv::registered>(*answer));
        id = std::get<kv::registered>(*answer).client_id;
    }

    // The next request of this client; it must execute and answer with a result.
    kv::result apply(kv::operation op) {
        const auto answer =
            sm->apply(next_index++, kv::command{.client_id = id, .sequence = ++sequence, .op = std::move(op)});
        REQUIRE(answer.has_value());
        REQUIRE(std::holds_alternative<kv::result>(*answer));
        return std::get<kv::result>(*answer);
    }
};

// The answer apply() returns for an executed operation.
std::expected<kv::reply, kv::rejection> answered(kv::result r) {
    return kv::reply{std::move(r)};
}

std::expected<kv::reply, kv::rejection> rejected(kv::rejection r) {
    return std::unexpected(r);
}

const kv::result ok{.code = kv::status::ok};

} // namespace

TEST_CASE("getting a non-existing key fails with not_found", "[kv]") {
    kv::state_machine sm;
    test_client client{sm};
    const auto get_res = client.apply(kv::get{.key = "foo"});
    CHECK(get_res.code == kv::status::not_found);
    CHECK(get_res.value == std::nullopt);
}

TEST_CASE("reading a non-existing key fails with not_found", "[kv]") {
    kv::state_machine sm;
    const auto get_res = sm.read(kv::get{.key = "foo"});
    CHECK(get_res.code == kv::status::not_found);
    CHECK(get_res.value == std::nullopt);
}

TEST_CASE("put stores a value; get retrieves the same value", "[kv]") {
    kv::state_machine sm;
    test_client client{sm};
    const auto put_res = client.apply(kv::put{.key = "foo", .value = "bar"});
    CHECK(put_res.code == kv::status::ok);
    CHECK(put_res.value == std::nullopt);
    const auto get_res = client.apply(kv::get{.key = "foo"});
    CHECK(get_res.code == kv::status::ok);
    CHECK(get_res.value == "bar");
}

TEST_CASE("put stores a value; read retrieves the same value", "[kv]") {
    kv::state_machine sm;
    test_client client{sm};
    client.apply(kv::put{.key = "foo", .value = "bar"});
    const auto result = sm.read(kv::get{.key = "foo"});
    CHECK(result.code == kv::status::ok);
    CHECK(result.value == "bar");
}

TEST_CASE("put stores a value; removes erases the key/value", "[kv]") {
    kv::state_machine sm;
    test_client client{sm};
    client.apply(kv::put{.key = "foo", .value = "bar"});
    const auto result = client.apply(kv::remove{.key = "foo"});
    CHECK(result.code == kv::status::ok);
    CHECK(result.value == std::nullopt);

    const auto get_res = client.apply(kv::get{.key = "foo"});
    CHECK(get_res.code == kv::status::not_found);
    CHECK(get_res.value == std::nullopt);
}

TEST_CASE("removing a non-existing key fails with not_found", "[kv]") {
    kv::state_machine sm;
    test_client client{sm};
    const auto result = client.apply(kv::remove{.key = "foo"});
    CHECK(result.code == kv::status::not_found);
    CHECK(result.value == std::nullopt);
}

TEST_CASE("removing the same key twice fails the second time with not_found", "[kv]") {
    kv::state_machine sm;
    test_client client{sm};
    client.apply(kv::put{.key = "foo", .value = "bar"});

    const auto first = client.apply(kv::remove{.key = "foo"});
    CHECK(first.code == kv::status::ok);

    const auto second = client.apply(kv::remove{.key = "foo"});
    CHECK(second.code == kv::status::not_found);
    CHECK(second.value == std::nullopt);
}

TEST_CASE("cas replaces the expected value with the desired value", "[kv]") {
    kv::state_machine sm;
    test_client client{sm};
    client.apply(kv::put{.key = "foo", .value = "bar"});

    const auto res_cas = client.apply(kv::cas{.key = "foo", .expected = "bar", .desired = "baz"});
    CHECK(res_cas.code == kv::status::ok);
    CHECK(res_cas.value == std::nullopt);

    const auto res_get = client.apply(kv::get{.key = "foo"});
    CHECK(res_get.value == "baz");
}

TEST_CASE("cas replaces the expected nullopt value with the desired value", "[kv]") {
    kv::state_machine sm;
    test_client client{sm};

    const auto res_cas = client.apply(kv::cas{.key = "foo", .expected = std::nullopt, .desired = "baz"});
    CHECK(res_cas.code == kv::status::ok);
    CHECK(res_cas.value == std::nullopt);

    const auto res_get = client.apply(kv::get{.key = "foo"});
    CHECK(res_get.value == "baz");
}

TEST_CASE("cas when the value is not the same as the expected", "[kv]") {
    kv::state_machine sm;
    test_client client{sm};
    client.apply(kv::put{.key = "foo", .value = "bar"});

    const auto res_cas = client.apply(kv::cas{.key = "foo", .expected = "que", .desired = "baz"});
    CHECK(res_cas.code == kv::status::cas_failed);
    CHECK(res_cas.value == "bar");
}

TEST_CASE("cas when the value is not the same as the expected (nullopt)", "[kv]") {
    kv::state_machine sm;
    test_client client{sm};
    client.apply(kv::put{.key = "foo", .value = "bar"});

    const auto res_cas = client.apply(kv::cas{.key = "foo", .expected = std::nullopt, .desired = "baz"});
    CHECK(res_cas.code == kv::status::cas_failed);
    CHECK(res_cas.value == "bar");
}

TEST_CASE("cas on a missing key with a non-empty expected fails", "[kv]") {
    kv::state_machine sm;
    test_client client{sm};

    const auto res_cas = client.apply(kv::cas{.key = "foo", .expected = "bar", .desired = "baz"});
    CHECK(res_cas.code == kv::status::cas_failed);
    CHECK(res_cas.value == std::nullopt);

    const auto res_get = client.apply(kv::get{.key = "foo"});
    CHECK(res_get.code == kv::status::not_found);
    CHECK(res_get.value == std::nullopt);
}

TEST_CASE("register_client answers with its log index as the client id", "[kv]") {
    kv::state_machine sm;
    CHECK(sm.apply(4, kv::command{.op = kv::register_client{}}) == kv::reply{kv::registered{.client_id = 4}});
    CHECK(sm.apply(9, kv::command{.op = kv::register_client{}}) == kv::reply{kv::registered{.client_id = 9}});
}

TEST_CASE("without a session only get runs", "[kv]") {
    kv::state_machine sm;
    CHECK(sm.apply(1, kv::command{.op = kv::get{.key = "x"}}) == answered({.code = kv::status::not_found}));
    CHECK(sm.apply(2, kv::command{.op = kv::put{.key = "x", .value = "1"}}) == rejected(kv::rejection::malformed));
    CHECK(sm.apply(3, kv::command{.op = kv::remove{.key = "x"}}) == rejected(kv::rejection::malformed));
    CHECK(sm.apply(4, kv::command{.op = kv::cas{.key = "x", .expected = std::nullopt, .desired = "1"}}) ==
          rejected(kv::rejection::malformed));
    CHECK(sm.read(kv::get{.key = "x"}) == kv::result{.code = kv::status::not_found});
}

TEST_CASE("register_client with a client id is malformed", "[kv]") {
    kv::state_machine sm;
    test_client client{sm};
    CHECK(sm.apply(5, kv::command{.client_id = client.id, .sequence = 1, .op = kv::register_client{}}) ==
          rejected(kv::rejection::malformed));
}

TEST_CASE("an unknown client id answers session_expired and executes nothing", "[kv]") {
    kv::state_machine sm;
    CHECK(sm.apply(1, kv::command{.client_id = 42, .sequence = 1, .op = kv::put{.key = "x", .value = "1"}}) ==
          rejected(kv::rejection::session_expired));
    CHECK(sm.read(kv::get{.key = "x"}) == kv::result{.code = kv::status::not_found});
}

TEST_CASE("a retry executes once and answers the same result twice", "[kv]") {
    // The cas example of section 5.8: the retry must answer "ok", not "cas failed".
    kv::state_machine sm;
    test_client client{sm}; // index 1
    const auto cas = kv::command{
        .client_id = client.id, .sequence = 1, .op = kv::cas{.key = "x", .expected = std::nullopt, .desired = "1"}};

    CHECK(sm.apply(2, cas) == answered(ok));
    CHECK(sm.apply(3, cas) == answered(ok)); // executed again, it would be cas_failed
    CHECK(sm.read(kv::get{.key = "x"}) == kv::result{.code = kv::status::ok, .value = "1"});
}

TEST_CASE("a retried put does not undo another client's later write", "[kv]") {
    // The put example of section 5.8.
    kv::state_machine sm;
    test_client one{sm}; // index 1
    const auto put_one = kv::command{.client_id = one.id, .sequence = 1, .op = kv::put{.key = "x", .value = "1"}};
    CHECK(sm.apply(2, put_one) == answered(ok));

    const auto two = sm.apply(3, kv::command{.op = kv::register_client{}});
    REQUIRE(two.has_value());
    const auto two_id = std::get<kv::registered>(*two).client_id;
    CHECK(sm.apply(4, kv::command{.client_id = two_id, .sequence = 1, .op = kv::put{.key = "x", .value = "2"}}) ==
          answered(ok));

    CHECK(sm.apply(5, put_one) == answered(ok));
    CHECK(sm.read(kv::get{.key = "x"}) == kv::result{.code = kv::status::ok, .value = "2"});
}

TEST_CASE("a get retry answers the stored result, not the current value", "[kv]") {
    kv::state_machine sm;
    test_client client{sm};
    REQUIRE(client.apply(kv::put{.key = "x", .value = "1"}) == ok);
    const auto get = kv::command{.client_id = client.id, .sequence = 2, .op = kv::get{.key = "x"}};
    CHECK(sm.apply(10, get) == answered({.code = kv::status::ok, .value = "1"}));

    test_client other{sm, 11};
    REQUIRE(other.apply(kv::put{.key = "x", .value = "2"}) == ok);
    CHECK(sm.apply(20, get) == answered({.code = kv::status::ok, .value = "1"}));
}

TEST_CASE("a sequence gap, an old sequence number or sequence 0 is malformed and changes nothing", "[kv]") {
    kv::state_machine sm;
    test_client client{sm};                                         // index 1
    REQUIRE(client.apply(kv::put{.key = "x", .value = "1"}) == ok); // sequence 1, index 2
    REQUIRE(client.apply(kv::put{.key = "x", .value = "2"}) == ok); // sequence 2, index 3

    const auto sequence = GENERATE(as<std::uint64_t>{}, 0, 1, 4);
    CAPTURE(sequence);
    CHECK(sm.apply(
              4, kv::command{.client_id = client.id, .sequence = sequence, .op = kv::put{.key = "x", .value = "3"}}) ==
          rejected(kv::rejection::malformed));
    CHECK(sm.read(kv::get{.key = "x"}) == kv::result{.code = kv::status::ok, .value = "2"});

    // The session is unchanged: the next number still runs, and the last one is still a retry.
    client.next_index = 5;
    CHECK(client.apply(kv::put{.key = "x", .value = "3"}) == ok); // sequence 3
    CHECK(sm.apply(6, kv::command{.client_id = client.id, .sequence = 3, .op = kv::put{.key = "x", .value = "3"}}) ==
          answered(ok));
}

TEST_CASE("sequence 0 right after registering is malformed, not a retry", "[kv]") {
    kv::state_machine sm;
    test_client client{sm};
    CHECK(sm.apply(2, kv::command{.client_id = client.id, .sequence = 0, .op = kv::put{.key = "x", .value = "1"}}) ==
          rejected(kv::rejection::malformed));
    CHECK(sm.read(kv::get{.key = "x"}) == kv::result{.code = kv::status::not_found});
}

TEST_CASE("eviction removes the least recently used session", "[kv]") {
    kv::state_machine sm{2};
    const auto put = [](std::uint64_t client, std::uint64_t sequence) {
        return kv::command{.client_id = client, .sequence = sequence, .op = kv::put{.key = "x", .value = "1"}};
    };
    const auto registered = [](std::uint64_t index) { return kv::reply{kv::registered{.client_id = index}}; };

    REQUIRE(sm.apply(1, kv::command{.op = kv::register_client{}}) == registered(1)); // client a
    REQUIRE(sm.apply(2, kv::command{.op = kv::register_client{}}) == registered(2)); // client b
    REQUIRE(sm.apply(3, put(1, 1)) == answered(ok));                                 // a was used last at 3

    SECTION("an executed request counts as use") {
        REQUIRE(sm.apply(4, kv::command{.op = kv::register_client{}}) == registered(4)); // evicts b (last used at 2)
        CHECK(sm.apply(5, put(2, 1)) == rejected(kv::rejection::session_expired));
        CHECK(sm.apply(6, put(1, 2)) == answered(ok));
        CHECK(sm.apply(7, put(4, 1)) == answered(ok));
    }

    SECTION("a retry counts as use") {
        REQUIRE(sm.apply(4, put(2, 1)) == answered(ok)); // b was used last at 4: now a is the oldest
        REQUIRE(sm.apply(5, put(1, 1)) == answered(ok)); // a's retry: a was used last at 5, b is the oldest again
        REQUIRE(sm.apply(6, kv::command{.op = kv::register_client{}}) == registered(6)); // evicts b
        CHECK(sm.apply(7, put(2, 2)) == rejected(kv::rejection::session_expired));
        CHECK(sm.apply(8, put(1, 2)) == answered(ok));
    }
}

TEST_CASE("round trip for every command", "[kv]") {
    const auto cmd = GENERATE(from_range(sample_commands()));
    CAPTURE(cmd);

    const auto actual = kv::decode_command(kv::encode(cmd));
    REQUIRE(actual.has_value());
    CHECK(*actual == cmd);
}

TEST_CASE("every truncation of an encoded command is a truncated error", "[kv]") {
    const auto cmd = GENERATE(from_range(sample_commands()));
    CAPTURE(cmd);

    const auto bytes = kv::encode(cmd);
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        CAPTURE(length);
        const auto result = kv::decode_command(std::span{bytes}.first(length));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == wire::decode_error::truncated);
    }
}

TEST_CASE("decode command with unknown operation type", "[kv]") {
    const auto type = GENERATE(as<std::uint8_t>{}, 0, 6, 255);
    CAPTURE(static_cast<int>(type));

    auto bytes = kv::encode(kv::command{.op = kv::get{.key = "foo"}});
    bytes.at(16) = std::byte{type}; // after client_id and sequence

    const auto result = kv::decode_command(bytes);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == wire::decode_error::invalid_value);
}

TEST_CASE("decode command with trailing bytes", "[kv]") {
    const auto cmd = GENERATE(from_range(sample_commands()));
    CAPTURE(cmd);

    auto bytes = kv::encode(cmd);
    bytes.push_back(std::byte{0});

    const auto result = kv::decode_command(bytes);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == wire::decode_error::trailing_bytes);
}

TEST_CASE("command encoding is stable", "[kv]") {
    // client 7, sequence 2, operation type cas, key "k", expected present "a", desired "b"
    CHECK(kv::encode(
              kv::command{.client_id = 7, .sequence = 2, .op = kv::cas{.key = "k", .expected = "a", .desired = "b"}}) ==
          to_bytes({0, 0, 0, 0, 0, 0,   0, 7, 0, 0, 0, 0,   0, 0, 0, 2, //
                    4, 0, 0, 0, 1, 'k', 1, 0, 0, 0, 1, 'a', 0, 0, 0, 1, 'b'}));
    // no session, operation type cas, key "k", expected absent, desired "b"
    CHECK(kv::encode(kv::command{.op = kv::cas{.key = "k", .expected = std::nullopt, .desired = "b"}}) ==
          to_bytes({0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0,  0, 0, 0, 0, //
                    4, 0, 0, 0, 1, 'k', 0, 0, 0, 0, 1, 'b'}));
    // no session, operation type register_client: no fields
    CHECK(kv::encode(kv::command{.op = kv::register_client{}}) ==
          to_bytes({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5}));
}
