#include "kv_string_makers.h" // IWYU pragma: keep

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <hammurabi/kv/command.h>
#include <hammurabi/kv/protocol.h>
#include <hammurabi/wire/codec.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

namespace {

std::vector<std::byte> to_bytes(std::initializer_list<std::uint8_t> values) {
    return values | std::views::transform([](std::uint8_t v) { return std::byte{v}; }) | std::ranges::to<std::vector>();
}

std::vector<hammurabi::kv::request> sample_requests() {
    using namespace hammurabi;
    return {
        {kv::get{.key = "foo"}},
        {kv::put{.key = "foo", .value = "bar"}},
        {kv::remove{.key = "foo"}},
        {kv::cas{.key = "foo", .expected = "bar", .desired = "baz"}},
        {kv::cas{.key = "foo", .expected = std::nullopt, .desired = "baz"}},
    };
}

std::vector<hammurabi::kv::response> sample_responses() {
    using namespace hammurabi;
    return {
        kv::result{.code = kv::status::ok, .value = "bar"},
        kv::result{.code = kv::status::ok, .value = std::nullopt},
        kv::result{.code = kv::status::not_found, .value = std::nullopt},
        kv::result{.code = kv::status::cas_failed, .value = "old"},
        kv::not_leader{.leader_address = "127.0.0.1:7001"},
        kv::not_leader{.leader_address = std::nullopt},
        kv::rejection::too_large,
        kv::rejection::malformed,
        kv::rejection::session_expired,
    };
}

} // namespace

TEST_CASE("round trip for every request", "[kv][protocol]") {
    using namespace hammurabi;

    const auto req = GENERATE(from_range(sample_requests()));
    CAPTURE(req.op.index());

    const auto actual = kv::decode_request(kv::encode(req));
    REQUIRE(actual.has_value());
    CHECK(*actual == req);
}

TEST_CASE("round trip for every response", "[kv][protocol]") {
    using namespace hammurabi;

    const auto resp = GENERATE(from_range(sample_responses()));
    CAPTURE(resp.index());

    const auto actual = kv::decode_response(kv::encode(resp));
    REQUIRE(actual.has_value());
    CHECK(*actual == resp);
}

TEST_CASE("every truncation of an encoded request is a truncated error", "[kv][protocol]") {
    using namespace hammurabi;

    const auto req = GENERATE(from_range(sample_requests()));
    CAPTURE(req.op.index());

    const auto bytes = kv::encode(req);
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        CAPTURE(length);
        const auto result = kv::decode_request(std::span{bytes}.first(length));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == wire::decode_error::truncated);
    }
}

TEST_CASE("every truncation of an encoded response is a truncated error", "[kv][protocol]") {
    using namespace hammurabi;

    const auto resp = GENERATE(from_range(sample_responses()));
    CAPTURE(resp.index());

    const auto bytes = kv::encode(resp);
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        CAPTURE(length);
        const auto result = kv::decode_response(std::span{bytes}.first(length));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == wire::decode_error::truncated);
    }
}

TEST_CASE("decoding a request or response with trailing bytes fails", "[kv][protocol]") {
    using namespace hammurabi;

    auto req = kv::encode(kv::request{kv::get{.key = "foo"}});
    req.push_back(std::byte{0});
    const auto req_result = kv::decode_request(req);
    REQUIRE_FALSE(req_result.has_value());
    CHECK(req_result.error() == wire::decode_error::trailing_bytes);

    auto resp = kv::encode(kv::response{kv::rejection::too_large});
    resp.push_back(std::byte{0});
    const auto resp_result = kv::decode_response(resp);
    REQUIRE_FALSE(resp_result.has_value());
    CHECK(resp_result.error() == wire::decode_error::trailing_bytes);
}

TEST_CASE("decoding a request or response with an unknown version fails", "[kv][protocol]") {
    using namespace hammurabi;

    const auto version = GENERATE(as<std::uint8_t>{}, 0, 2, 255);
    CAPTURE(static_cast<int>(version));

    auto req = kv::encode(kv::request{kv::get{.key = "foo"}});
    req.front() = std::byte{version};
    const auto req_result = kv::decode_request(req);
    REQUIRE_FALSE(req_result.has_value());
    CHECK(req_result.error() == wire::decode_error::invalid_value);

    auto resp = kv::encode(kv::response{kv::rejection::too_large});
    resp.front() = std::byte{version};
    const auto resp_result = kv::decode_response(resp);
    REQUIRE_FALSE(resp_result.has_value());
    CHECK(resp_result.error() == wire::decode_error::invalid_value);
}

TEST_CASE("decoding a response with an invalid byte fails with invalid_value", "[kv][protocol]") {
    using namespace hammurabi;

    // version 1, then the bytes below
    const auto input = GENERATE(to_bytes({1, 0, 3}),     // response type 0
                                to_bytes({1, 4, 3}),     // response type 4
                                to_bytes({1, 1, 0, 0}),  // result with status 0
                                to_bytes({1, 1, 99, 0}), // result with status 99
                                to_bytes({1, 1, 1, 2}),  // result with present flag 2
                                to_bytes({1, 2, 2}),     // not_leader with present flag 2
                                to_bytes({1, 3, 0}),     // rejection 0
                                to_bytes({1, 3, 99}));   // rejection 99

    const auto result = kv::decode_response(input);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == wire::decode_error::invalid_value);
}

TEST_CASE("request encoding is stable", "[kv][protocol]") {
    using namespace hammurabi;

    // version, operation type put, key "k", value "v"
    CHECK(kv::encode(kv::request{kv::put{.key = "k", .value = "v"}}) ==
          to_bytes({1, 2, 0, 0, 0, 1, 'k', 0, 0, 0, 1, 'v'}));
}

TEST_CASE("response encoding is stable", "[kv][protocol]") {
    using namespace hammurabi;

    // version, response type result, status ok, value present, value "v"
    CHECK(kv::encode(kv::response{kv::result{.code = kv::status::ok, .value = "v"}}) ==
          to_bytes({1, 1, 1, 1, 0, 0, 0, 1, 'v'}));
    // version, response type not_leader, address absent
    CHECK(kv::encode(kv::response{kv::not_leader{}}) == to_bytes({1, 2, 0}));
    // version, response type rejection, malformed
    CHECK(kv::encode(kv::response{kv::rejection::malformed}) == to_bytes({1, 3, 2}));
}
