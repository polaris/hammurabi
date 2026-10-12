#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <hammurabi/kv/state_machine.h>
#include <hammurabi/wire/codec.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace {

std::vector<hammurabi::kv::operation> sample_operations() {
    using namespace hammurabi;
    return {
        kv::get{.key = "foo"},
        kv::put{.key = "foo", .value = "bar"},
        kv::remove{.key = "foo"},
        kv::cas{.key = "foo", .expected = "bar", .desired = "baz"},
        kv::cas{.key = "foo", .expected = std::nullopt, .desired = "baz"},
    };
}

} // namespace

TEST_CASE("getting a non-existing key fails with not_found", "[kv]") {
    using namespace hammurabi;

    kv::state_machine sm;
    const auto get_res = sm.apply(kv::get{.key = "foo"});
    CHECK(get_res.code == kv::status::not_found);
    CHECK(get_res.value == std::nullopt);
}

TEST_CASE("reading a non-existing key fails with not_found", "[kv]") {
    using namespace hammurabi;

    kv::state_machine sm;
    const auto get_res = sm.read(kv::get{.key = "foo"});
    CHECK(get_res.code == kv::status::not_found);
    CHECK(get_res.value == std::nullopt);
}

TEST_CASE("put stores a value; get retrieves the same value", "[kv]") {
    using namespace hammurabi;

    kv::state_machine sm;
    const auto put_res = sm.apply(kv::put{.key = "foo", .value = "bar"});
    CHECK(put_res.code == kv::status::ok);
    CHECK(put_res.value == std::nullopt);
    const auto get_res = sm.apply(kv::get{.key = "foo"});
    CHECK(get_res.code == kv::status::ok);
    CHECK(get_res.value == "bar");
}

TEST_CASE("put stores a value; read retrieves the same value", "[kv]") {
    using namespace hammurabi;

    kv::state_machine sm;
    sm.apply(kv::put{.key = "foo", .value = "bar"});
    const auto result = sm.read(kv::get{.key = "foo"});
    CHECK(result.code == kv::status::ok);
    CHECK(result.value == "bar");
}

TEST_CASE("put stores a value; removes erases the key/value", "[kv]") {
    using namespace hammurabi;

    kv::state_machine sm;
    sm.apply(kv::put{.key = "foo", .value = "bar"});
    const auto result = sm.apply(kv::remove{.key = "foo"});
    CHECK(result.code == kv::status::ok);
    CHECK(result.value == std::nullopt);

    const auto get_res = sm.apply(kv::get{.key = "foo"});
    CHECK(get_res.code == kv::status::not_found);
    CHECK(get_res.value == std::nullopt);
}

TEST_CASE("removing a non-existing key fails with not_found", "[kv]") {
    using namespace hammurabi;

    kv::state_machine sm;
    const auto result = sm.apply(kv::remove{.key = "foo"});
    CHECK(result.code == kv::status::not_found);
    CHECK(result.value == std::nullopt);
}

TEST_CASE("removing the same key twice fails the second time with not_found", "[kv]") {
    using namespace hammurabi;

    kv::state_machine sm;
    sm.apply(kv::put{.key = "foo", .value = "bar"});

    const auto first = sm.apply(kv::remove{.key = "foo"});
    CHECK(first.code == kv::status::ok);

    const auto second = sm.apply(kv::remove{.key = "foo"});
    CHECK(second.code == kv::status::not_found);
    CHECK(second.value == std::nullopt);
}

TEST_CASE("cas replaces the expected value with the desired value", "[kv]") {
    using namespace hammurabi;

    kv::state_machine sm;
    sm.apply(kv::put{.key = "foo", .value = "bar"});

    const auto res_cas = sm.apply(kv::cas{.key = "foo", .expected = "bar", .desired = "baz"});
    CHECK(res_cas.code == kv::status::ok);
    CHECK(res_cas.value == std::nullopt);

    const auto res_get = sm.apply(kv::get{.key = "foo"});
    CHECK(res_get.value == "baz");
}

TEST_CASE("cas replaces the expected nullopt value with the desired value", "[kv]") {
    using namespace hammurabi;

    kv::state_machine sm;

    const auto res_cas = sm.apply(kv::cas{.key = "foo", .expected = std::nullopt, .desired = "baz"});
    CHECK(res_cas.code == kv::status::ok);
    CHECK(res_cas.value == std::nullopt);

    const auto res_get = sm.apply(kv::get{.key = "foo"});
    CHECK(res_get.value == "baz");
}

TEST_CASE("cas when the value is not the same as the expected", "[kv]") {
    using namespace hammurabi;

    kv::state_machine sm;
    sm.apply(kv::put{.key = "foo", .value = "bar"});

    const auto res_cas = sm.apply(kv::cas{.key = "foo", .expected = "que", .desired = "baz"});
    CHECK(res_cas.code == kv::status::cas_failed);
    CHECK(res_cas.value == "bar");
}

TEST_CASE("cas when the value is not the same as the expected (nullopt)", "[kv]") {
    using namespace hammurabi;

    kv::state_machine sm;
    sm.apply(kv::put{.key = "foo", .value = "bar"});

    const auto res_cas = sm.apply(kv::cas{.key = "foo", .expected = std::nullopt, .desired = "baz"});
    CHECK(res_cas.code == kv::status::cas_failed);
    CHECK(res_cas.value == "bar");
}

TEST_CASE("cas on a missing key with a non-empty expected fails", "[kv]") {
    using namespace hammurabi;

    kv::state_machine sm;

    const auto res_cas = sm.apply(kv::cas{.key = "foo", .expected = "bar", .desired = "baz"});
    CHECK(res_cas.code == kv::status::cas_failed);
    CHECK(res_cas.value == std::nullopt);

    const auto res_get = sm.apply(kv::get{.key = "foo"});
    CHECK(res_get.code == kv::status::not_found);
    CHECK(res_get.value == std::nullopt);
}

TEST_CASE("round trip for every operation", "[kv]") {
    using namespace hammurabi;

    const auto op = GENERATE(from_range(sample_operations()));
    CAPTURE(op.index());

    const auto actual = kv::decode_operation(kv::encode(op));
    REQUIRE(actual.has_value());
    CHECK(*actual == op);
}

TEST_CASE("every truncation of an encoded operation is a truncated error", "[kv]") {
    using namespace hammurabi;

    const auto op = GENERATE(from_range(sample_operations()));
    CAPTURE(op.index());

    const auto bytes = kv::encode(op);
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        CAPTURE(length);
        const auto result = kv::decode_operation(std::span{bytes}.first(length));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == wire::decode_error::truncated);
    }
}

TEST_CASE("decode operation with unknown type", "[kv]") {
    using namespace hammurabi;

    const auto type = GENERATE(as<std::uint8_t>{}, 0, 5, 255);
    CAPTURE(static_cast<int>(type));

    auto bytes = kv::encode(kv::get{.key = "foo"});
    bytes[0] = std::byte{type};

    const auto result = kv::decode_operation(bytes);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == wire::decode_error::invalid_value);
}

TEST_CASE("decode operation with trailing bytes", "[kv]") {
    using namespace hammurabi;

    const auto op = GENERATE(from_range(sample_operations()));
    CAPTURE(op.index());

    auto bytes = kv::encode(op);
    bytes.push_back(std::byte{0});

    const auto result = kv::decode_operation(bytes);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == wire::decode_error::trailing_bytes);
}
